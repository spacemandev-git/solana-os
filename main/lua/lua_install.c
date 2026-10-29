// Store installs and updates of Lua apps (server/API.md "Device API",
// docs/LUA_API.md "The package"). A worker task does the whole job one
// download at a time (os_api's callback hands each answer over), so the OS
// task never waits on the card or the network:
//   1. main.lua into apps/<id>.new/, the manifest from X-App-Manifest
//   2. with bundled files, the version's index (files?v=), then each file:
//      copied from the installed app when its SHA-256 matches, downloaded
//      otherwise, and its hash checked either way
//   3. with compiled code for this badge (the manifest's "code"), code.bin
//      (server/API.md "Compiled code"), kept or downloaded like a file
//   4. app.json, and the installed app's data.json copied over
//   5. the swap: apps/<id> -> apps/<id>.old, apps/<id>.new -> apps/<id>, and
//      .old is removed
// The installed version keeps working until the swap, and a power cut in the
// middle of it leaves .old complete for lua_install_recover() to put back.
// Every request is pinned to the manifest's version, so a version published
// meanwhile can't mix in. One install at a time; it goes on after the store
// closes and toasts how it ended.
//
// The card takes 5 open files in all (bsp_sd.c), so the worker holds at most
// two at a time: the index is reopened for each line.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_data.h"
#include "lua_host.h"
#include "lua_install.h"

static const char *TAG = "lua_install";

#define WORKER_STACK    5120
#define WORKER_PRIO     3
#define ANSWER_WAIT_MS  (3 * 60 * 1000)     // longest wait for one download
#define QUEUE_TRIES     25                  // os_api's queue is full: retry every 200 ms
#define INDEX_MAX       (LUA_PKG_FILES_MAX * 160)
#define INDEX_LINE      (64 + 1 + 7 + 1 + LUA_PKG_PATH_MAX + 3)
#define RUNNING_WAIT_MS 3000                // the app's task must end before the swap
#define COPY_CHUNK      1024

static SemaphoreHandle_t s_mx;              // s_info, s_name, s_busy and the answer below
static SemaphoreHandle_t s_swap;            // the renames vs lua_install_recover()
static SemaphoreHandle_t s_answered;        // a download's callback ran
static lua_install_info_t s_info;
static char s_name[21];
static bool s_busy;
static void *s_ctx;                         // os_api ctx of the download in flight
static int s_status;
static char *s_manifest;                    // its X-App-Manifest, decoded

static void lock(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mx);
}

static void app_dir(char *out, size_t n, const char *id, const char *suffix)
{
    char rel[48];
    snprintf(rel, sizeof(rel), "apps/%s%s", id, suffix);
    os_data_path(out, n, rel);
}

// ---------------------------------------------------------------------------
// Downloads: os_api_download, waited for on the worker

// OS task: hand the answer to the worker, unless it gave up on it.
static void answered(int status, const char *body, void *ctx)
{
    lock();
    if (ctx == s_ctx) {
        s_ctx = NULL;
        s_status = status;
        free(s_manifest);
        s_manifest = body && body[0] ? strdup(body) : NULL;
        xSemaphoreGive(s_answered);
    }
    unlock();
}

// GET path into dest (a full path), at most max bytes. The HTTP status, 0
// without an answer.
static int fetch(const char *path, const char *dest, size_t max)
{
    void *ctx = os_api_token();
    lock();
    s_ctx = ctx;
    s_status = 0;
    free(s_manifest);
    s_manifest = NULL;
    unlock();
    xSemaphoreTake(s_answered, 0);
    esp_err_t err;
    for (int i = 0; (err = os_api_download(path, dest, max, answered, ctx)) == ESP_ERR_NO_MEM && i < QUEUE_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    bool got = err == ESP_OK && xSemaphoreTake(s_answered, pdMS_TO_TICKS(ANSWER_WAIT_MS)) == pdTRUE;
    lock();
    int status = got ? s_status : 0;
    s_ctx = NULL;                           // a late answer is ignored
    unlock();
    if (err == ESP_OK && !got) {
        os_api_cancel(ctx);
    }
    return status;
}

static char *take_manifest(void)
{
    lock();
    char *m = s_manifest;
    s_manifest = NULL;
    unlock();
    return m;
}

static void fail_text(char *why, size_t n, int status)
{
    if (status == 0) {
        strlcpy(why, "Download failed. Check Wi-Fi and the card", n);
    } else if (status == 403) {
        strlcpy(why, "You don't own this app", n);
    } else {
        snprintf(why, n, "Download failed (%d)", status);
    }
}

static bool card_fail(char *why, size_t n)
{
    strlcpy(why, "Couldn't write to the microSD card", n);
    return false;
}

// The version in a query string: anything but unreserved characters escaped.
static void urlenc(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------------------
// Bundled files

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(to, "wb");
    char *buf = out ? malloc(COPY_CHUNK) : NULL;
    bool ok = buf != NULL;
    size_t got;
    while (ok && (got = fread(buf, 1, COPY_CHUNK, in)) > 0) {
        ok = fwrite(buf, 1, got, out) == got;
    }
    ok = ok && !ferror(in);
    free(buf);
    fclose(in);
    if (out) {
        ok = fclose(out) == 0 && ok;
    }
    if (!ok) {
        unlink(to);
    }
    return ok;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// "<sha256 hex> <bytes> <path>\n" (server/API.md). path points into line.
static bool parse_line(char *line, uint8_t sha[32], long *size, const char **path)
{
    line[strcspn(line, "\r\n")] = 0;
    if (strlen(line) < 64 + 4 || line[64] != ' ') {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hexval(line[2 * i]), lo = hexval(line[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        sha[i] = hi << 4 | lo;
    }
    char *end;
    long n = strtol(line + 65, &end, 10);
    if (end == line + 65 || *end != ' ' || n < 0 || n > LUA_PKG_FILE_MAX) {
        return false;
    }
    *size = n;
    *path = end + 1;
    return lua_pkg_valid_path(*path);
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    int n = 0, c, prev = '\n';
    while ((c = fgetc(f)) != EOF) {
        n += c == '\n' && prev != '\n';
        prev = c;
    }
    fclose(f);
    return n + (prev != '\n');
}

// One bundled file into the staging folder: the installed copy if it's the
// same file, else a download. Either way its hash must match.
// dest from the installed copy (have) when it has the same contents, else
// downloaded from url; either way its size and SHA-256 must match.
static bool fetch_checked(const char *have, const char *dest, const char *url, long size, const uint8_t want[32],
                          bool *kept, char *why, size_t n)
{
    uint8_t got[32];
    long got_size = -1;
    struct stat st;
    *kept = stat(have, &st) == 0 && st.st_size == size && lua_pkg_sha256_file(have, got, NULL)
            && !memcmp(got, want, 32) && copy_file(have, dest);
    if (*kept) {
        return true;
    }
    int status = fetch(url, dest, size);
    if (status != 200) {
        fail_text(why, n, status);
        return false;
    }
    if (!lua_pkg_sha256_file(dest, got, &got_size) || got_size != size || memcmp(got, want, 32)) {
        strlcpy(why, "A file didn't download right. Try again", n);
        return false;
    }
    return true;
}

static bool fetch_file(const char *id, const char *ver, const char *p, long size, const uint8_t want[32],
                       bool *kept, char *why, size_t n)
{
    char rel[128], have[160], dest[160], url[200];
    snprintf(rel, sizeof(rel), "apps/%s.new/files/%s", id, p);
    if (!os_data_mkdirs(rel)) {
        return card_fail(why, n);
    }
    os_data_path(dest, sizeof(dest), rel);
    snprintf(rel, sizeof(rel), "apps/%s/files/%s", id, p);
    os_data_path(have, sizeof(have), rel);
    snprintf(url, sizeof(url), "/api/device/apps/%s/files/%s?v=%s", id, p, ver);
    return fetch_checked(have, dest, url, size, want, kept, why, n);
}

// code.bin: only for code compiled for this badge (lua_pkg_t.code).
static bool install_code(const char *id, const char *ver, const lua_pkg_t *pkg, char *why, size_t n)
{
    char rel[64], have[160], dest[160], url[160];
    bool kept;
    snprintf(rel, sizeof(rel), "apps/%s.new/code.bin", id);
    os_data_path(dest, sizeof(dest), rel);
    lua_pkg_rel(rel, sizeof(rel), id, "code.bin");
    os_data_path(have, sizeof(have), rel);
    snprintf(url, sizeof(url), "/api/device/apps/%s/code?v=%s", id, ver);
    if (!fetch_checked(have, dest, url, pkg->code_size, pkg->code_sha, &kept, why, n)) {
        return false;
    }
    ESP_LOGI(TAG, "%s: compiled code, %ld bytes%s", id, (long)pkg->code_size, kept ? " (kept)" : "");
    return true;
}

static bool install_files(const char *id, const char *ver, char *why, size_t n)
{
    char rel[64], idx[160], url[160], line[INDEX_LINE + 1];
    snprintf(rel, sizeof(rel), "apps/%s.new/files.idx", id);
    os_data_path(idx, sizeof(idx), rel);
    snprintf(url, sizeof(url), "/api/device/apps/%s/files?v=%s", id, ver);
    int status = fetch(url, idx, INDEX_MAX);
    if (status != 200) {
        fail_text(why, n, status);
        return false;
    }
    int total = count_lines(idx);
    if (total < 0 || total > LUA_PKG_FILES_MAX) {
        strlcpy(why, total < 0 ? "Couldn't read the microSD card" : "The app has too many files", n);
        unlink(idx);
        return false;
    }
    lock();
    s_info.files_total = total;
    unlock();
    long pos = 0, sum = 0;
    int kept_n = 0;
    bool ok = true;
    while (ok) {
        FILE *f = fopen(idx, "r");
        if (!f) {
            ok = card_fail(why, n);
            break;
        }
        bool got = fseek(f, pos, SEEK_SET) == 0 && fgets(line, sizeof(line), f);
        pos = ftell(f);
        fclose(f);
        if (!got) {
            break;                          // the end of the index
        }
        if (line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        uint8_t want[32];
        long size;
        const char *p;
        bool kept = false;
        if (!parse_line(line, want, &size, &p)) {
            strlcpy(why, "The store sent a bad file list", n);
            ok = false;
        } else if ((sum += size) > LUA_PKG_FILES_TOTAL) {
            strlcpy(why, "The app is too big", n);
            ok = false;
        } else {
            ok = fetch_file(id, ver, p, size, want, &kept, why, n);
        }
        kept_n += kept;
        lock();
        s_info.files_done += ok;
        unlock();
    }
    unlink(idx);
    if (ok) {
        ESP_LOGI(TAG, "%s: %d files, %d kept from the installed version", id, total, kept_n);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// The swap

static bool commit(const char *id, const char *manifest, char *why, size_t n)
{
    char rel[48], cur[160], old[160], nw[160], from[176], to[176];
    struct stat st;
    snprintf(rel, sizeof(rel), "apps/%s.new/app.json", id);
    if (!os_data_write(rel, manifest, strlen(manifest))) {
        return card_fail(why, n);
    }
    // The host doesn't start an app that's being installed; one that was
    // already running (or is still closing) gets a moment to end.
    for (int ms = 0; lua_host_active(id) && ms < RUNNING_WAIT_MS; ms += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (lua_host_active(id)) {
        strlcpy(why, "Close the app to update it", n);
        return false;
    }
    app_dir(cur, sizeof(cur), id, "");
    app_dir(old, sizeof(old), id, ".old");
    app_dir(nw, sizeof(nw), id, ".new");
    snprintf(from, sizeof(from), "%s/data.json", cur);
    snprintf(to, sizeof(to), "%s/data.json", nw);
    if (stat(from, &st) == 0 && !copy_file(from, to)) {
        return card_fail(why, n);           // the app's storage moves along
    }
    snprintf(rel, sizeof(rel), "apps/%s.old", id);
    xSemaphoreTake(s_swap, portMAX_DELAY);
    os_data_remove(rel);                    // left over from a failed removal
    bool had = stat(cur, &st) == 0;
    // FatFs rename() won't replace: move the installed folder aside first.
    bool ok = (!had || rename(cur, old) == 0) && rename(nw, cur) == 0;
    if (!ok && had && stat(cur, &st) != 0) {
        rename(old, cur);                   // keep the installed version
    }
    xSemaphoreGive(s_swap);
    if (!ok) {
        return card_fail(why, n);
    }
    os_data_remove(rel);
    return true;
}

// ---------------------------------------------------------------------------
// The worker

static bool install(const char *id, lua_pkg_t *pkg, char *why, size_t n)
{
    char rel[64], path[160], url[64], ver[64];
    snprintf(rel, sizeof(rel), "apps/%s.new", id);
    os_data_remove(rel);                    // an install that never finished
    snprintf(rel, sizeof(rel), "apps/%s.new/main.lua", id);
    os_data_path(path, sizeof(path), rel);
    snprintf(url, sizeof(url), "/api/device/apps/%s/source", id);
    int status = fetch(url, path, LUA_PKG_SRC_MAX);
    char *manifest = take_manifest();
    bool ok = status == 200 && manifest && lua_pkg_parse(manifest, id, pkg);
    if (!ok) {
        if (status == 200) {
            strlcpy(why, "The store sent a bad manifest", n);
        } else {
            fail_text(why, n, status);
        }
    }
    urlenc(ver, sizeof(ver), pkg->version);
    if (ok && pkg->files > 0) {
        ok = install_files(id, ver, why, n);
    }
    if (ok && pkg->code) {
        ok = install_code(id, ver, pkg, why, n);
    }
    ok = ok && commit(id, manifest, why, n);
    free(manifest);
    if (!ok) {
        snprintf(rel, sizeof(rel), "apps/%s.new", id);
        os_data_remove(rel);
    }
    return ok;
}

static void worker(void *arg)
{
    char id[LUA_PKG_ID_MAX + 1], name[sizeof(s_name)], why[sizeof(s_info.error)] = "";
    lock();
    strlcpy(id, s_info.id, sizeof(id));
    strlcpy(name, s_name, sizeof(name));
    unlock();
    lua_pkg_t pkg;
    bool update = lua_pkg_load(id, &pkg);
    bool ok = install(id, &pkg, why, sizeof(why));
    if (ok) {
        os_toast("%s %s", update ? "Updated" : "Installed", name);
        ESP_LOGI(TAG, "%s %s", id, pkg.version);
    } else {
        os_toast("%s", why);
        ESP_LOGW(TAG, "%s: %s", id, why);
    }
    lock();
    s_info.state = ok ? LUA_INSTALL_DONE : LUA_INSTALL_FAILED;
    strlcpy(s_info.version, ok ? pkg.version : "", sizeof(s_info.version));
    strlcpy(s_info.error, why, sizeof(s_info.error));
    s_info.seq++;
    s_busy = false;
    unlock();
    vTaskDelete(NULL);
}

esp_err_t lua_install_start(const char *id, const char *name)
{
    if (!lua_pkg_valid_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!os_data_ready()) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    if (s_busy) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seq = s_info.seq;
    s_info = (lua_install_info_t){ .state = LUA_INSTALL_RUNNING, .seq = seq };
    strlcpy(s_info.id, id, sizeof(s_info.id));
    strlcpy(s_name, name && name[0] ? name : id, sizeof(s_name));
    s_busy = true;
    unlock();
    if (xTaskCreate(worker, "lua_inst", WORKER_STACK, NULL, WORKER_PRIO, NULL) != pdPASS) {
        lock();
        s_busy = false;
        s_info.state = LUA_INSTALL_IDLE;
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void lua_install_get(lua_install_info_t *out)
{
    lock();
    *out = s_info;
    unlock();
}

bool lua_install_busy(const char *id)
{
    if (!s_mx) {
        return false;
    }
    lock();
    bool busy = s_busy && (!id || !strcmp(id, s_info.id));
    unlock();
    return busy;
}

// Paths for the walk: static (it runs under s_swap), since the console and
// the scan workers that call it have small stacks.
static char s_dir[160], s_cur[160], s_old[160];

void lua_install_recover(void)
{
    if (!s_swap) {
        return;
    }
    xSemaphoreTake(s_swap, portMAX_DELAY);
    os_data_path(s_dir, sizeof(s_dir), "apps");
    DIR *d = opendir(s_dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        struct stat st;
        if (len < 4 + 3 || len - 4 > LUA_PKG_ID_MAX || strcmp(e->d_name + len - 4, ".old")) {
            continue;
        }
        char id[LUA_PKG_ID_MAX + 1];
        memcpy(id, e->d_name, len - 4);
        id[len - 4] = 0;
        if (!lua_pkg_valid_id(id)) {
            continue;
        }
        app_dir(s_cur, sizeof(s_cur), id, "");
        app_dir(s_old, sizeof(s_old), id, ".old");
        if (stat(s_cur, &st) != 0 && rename(s_old, s_cur) == 0) {
            ESP_LOGW(TAG, "%s: put back the version an update had moved aside", id);
        }
    }
    if (d) {
        closedir(d);
    }
    xSemaphoreGive(s_swap);
}

void lua_install_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_swap = xSemaphoreCreateMutex();
    s_answered = xSemaphoreCreateBinary();
}
