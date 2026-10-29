// Installed Lua apps on the microSD card, see lua_pkg.h.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "lvgl.h"
#include "mbedtls/sha256.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "lua_install.h"
#include "lua_pkg.h"
#include "vendor/lua.h"

#define HASH_CHUNK  1024

static const char *const s_perm_names[LUA_PERM_COUNT] = {
    "led", "wifi", "mesh", "lamps", "items", "profile", "quest", "http", "rpc",
};

static const char *const s_perm_texts[LUA_PERM_COUNT] = {
    "Light the LEDs",
    "Turn Wi-Fi on and off",
    "Talk to nearby badges",
    "Spend and earn your !LAMPs",
    "Create and read your items",
    "See your profile",
    "Report quest progress",
    "Talk to servers on the internet",
    "Read and send to Solana",
};

const char *lua_perm_name(int i)
{
    return i >= 0 && i < LUA_PERM_COUNT ? s_perm_names[i] : "";
}

const char *lua_perm_text(int i)
{
    return i >= 0 && i < LUA_PERM_COUNT ? s_perm_texts[i] : "";
}

uint32_t lua_perm_parse(const char *name)
{
    for (int i = 0; name && i < LUA_PERM_COUNT; i++) {
        if (!strcmp(name, s_perm_names[i])) {
            return 1u << i;
        }
    }
    return 0;
}

void lua_pkg_hosts_line(const cJSON *hosts, char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    const cJSON *h;
    cJSON_ArrayForEach(h, hosts) {
        char one[65];
        lua_pkg_plain(one, sizeof(one), cJSON_IsString(h) ? h->valuestring : "");
        size_t len = strlen(one);
        if (!len) {
            continue;
        }
        if (o + (o ? 2 : 0) + len + 1 > n) {
            strlcpy(o + 6 <= n ? out + o : out + n - 4, o + 6 <= n && o ? ", ..." : "...", 6);
            return;
        }
        o += snprintf(out + o, n - o, "%s%s", o ? ", " : "", one);
    }
}

bool lua_pkg_hosts(const char *id, char *out, size_t n)
{
    char rel[64];
    out[0] = 0;
    if (!lua_pkg_valid_id(id)) {
        return false;
    }
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    cJSON *m = json ? cJSON_Parse(json) : NULL;
    free(json);
    lua_pkg_hosts_line(cJSON_GetObjectItemCaseSensitive(m, "hosts"), out, n);
    cJSON_Delete(m);
    return out[0] != 0;
}

// LVGL's symbol font has no star, heart, gift or bulb: use the closest glyph.
const char *lua_pkg_symbol(const char *icon)
{
    static const struct { const char *name, *sym; } map[] = {
        {"play", LV_SYMBOL_PLAY}, {"star", LV_SYMBOL_OK}, {"heart", LV_SYMBOL_BELL},
        {"bell", LV_SYMBOL_BELL}, {"gift", LV_SYMBOL_DOWNLOAD}, {"home", LV_SYMBOL_HOME},
        {"image", LV_SYMBOL_IMAGE}, {"list", LV_SYMBOL_LIST}, {"wifi", LV_SYMBOL_WIFI},
        {"shuffle", LV_SYMBOL_SHUFFLE}, {"eye", LV_SYMBOL_EYE_OPEN}, {"edit", LV_SYMBOL_EDIT},
        {"charge", LV_SYMBOL_CHARGE}, {"gps", LV_SYMBOL_GPS}, {"settings", LV_SYMBOL_SETTINGS},
        {"audio", LV_SYMBOL_AUDIO}, {"bulb", LV_SYMBOL_CHARGE},
    };
    for (size_t i = 0; icon && i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(icon, map[i].name)) {
            return map[i].sym;
        }
    }
    return LV_SYMBOL_PLAY;
}

void lua_pkg_plain(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 1 < n; in++) {
        if (*in >= 0x20 && *in <= 0x7E) {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

bool lua_pkg_valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 3 || n > LUA_PKG_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

void lua_pkg_rel(char *out, size_t n, const char *id, const char *file)
{
    snprintf(out, n, "apps/%s/%s", id, file);
}

bool lua_pkg_valid_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    if (n == 0 || n > LUA_PKG_PATH_MAX) {
        return false;
    }
    int depth = 1;
    char prev = '/';                // the start of a segment
    for (size_t i = 0; i < n; i++) {
        char c = path[i];
        if (c == '/') {
            // No empty segment and none ending in '.'.
            if (prev == '/' || prev == '.' || ++depth > LUA_PKG_PATH_DEPTH) {
                return false;
            }
        } else if (c == '.') {
            if (prev == '/' || prev == '.') {
                return false;       // a leading dot, or ".."
            }
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        prev = c;
    }
    return prev != '.' && prev != '/';
}

bool lua_pkg_file_rel(char *rel, size_t n, const char *id, const char *file, size_t *max)
{
    if (!lua_pkg_valid_id(id) || !file) {
        return false;
    }
    if (!strcmp(file, "main.lua") || !strcmp(file, "app.json")) {
        lua_pkg_rel(rel, n, id, file);
        if (max) {
            *max = file[0] == 'm' ? LUA_PKG_SRC_MAX : LUA_PKG_JSON_MAX;
        }
        return true;
    }
    if (!lua_pkg_valid_path(file)) {
        return false;
    }
    snprintf(rel, n, "apps/%s/files/%s", id, file);
    if (max) {
        *max = LUA_PKG_FILE_MAX;
    }
    return true;
}

bool lua_pkg_sha256_file(const char *path, uint8_t out[32], long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = f ? malloc(HASH_CHUNK) : NULL;
    if (!buf) {
        if (f) {
            fclose(f);
        }
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    long total = 0;
    size_t got;
    while ((got = fread(buf, 1, HASH_CHUNK, f)) > 0) {
        mbedtls_sha256_update(&c, buf, got);
        total += got;
    }
    bool ok = !ferror(f);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    fclose(f);
    free(buf);
    if (size) {
        *size = total;
    }
    return ok;
}

bool lua_pkg_unhex(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] << 4 | v : v);
    }
    return s[64] == 0;
}

void lua_pkg_hex(char *out, const uint8_t *bin, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[bin[i] >> 4];
        out[2 * i + 1] = hex[bin[i] & 15];
    }
    out[2 * n] = 0;
}

// Manifest text shows in titles, lists and dialogs: drop control characters
// (a newline in a name must not add a line anywhere), and don't cut a UTF-8
// character in half.
static void copy_str(const cJSON *obj, const char *key, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *in = cJSON_IsString(v) ? v->valuestring : "";
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        if ((unsigned char)*in >= 0x20 && *in != 0x7F) {
            out[o++] = *in;
        }
    }
    size_t start = o;
    while (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0x80) {
        start--;
    }
    if (start > 0 && (unsigned char)out[start - 1] >= 0xC0) {
        unsigned char lead = out[start - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (o - (start - 1) < need) {
            o = start - 1;      // the last character didn't fit
        }
    }
    out[o] = 0;
}

bool lua_pkg_parse(const char *json, const char *want_id, lua_pkg_t *out)
{
    cJSON *m = cJSON_Parse(json);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return false;
    }
    memset(out, 0, sizeof(*out));
    copy_str(m, "id", out->id, sizeof(out->id));
    bool ok = lua_pkg_valid_id(out->id) && (!want_id || !strcmp(want_id, out->id));
    copy_str(m, "name", out->name, sizeof(out->name));
    if (!out->name[0]) {
        strlcpy(out->name, out->id, sizeof(out->name));
    }
    copy_str(m, "version", out->version, sizeof(out->version));
    copy_str(m, "author", out->author, sizeof(out->author));
    copy_str(m, "summary", out->summary, sizeof(out->summary));
    copy_str(m, "category", out->category, sizeof(out->category));
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(m, "icon");
    out->symbol = lua_pkg_symbol(cJSON_IsString(icon) ? icon->valuestring : NULL);
    const cJSON *accent = cJSON_GetObjectItemCaseSensitive(m, "accent");
    out->accent = cJSON_IsString(accent) ? strtoul(accent->valuestring, NULL, 16) & 0xFFFFFF : 0xB6B0FF;
    const cJSON *perm;
    cJSON_ArrayForEach(perm, cJSON_GetObjectItemCaseSensitive(m, "permissions")) {
        out->perms |= cJSON_IsString(perm) ? lua_perm_parse(perm->valuestring) : 0;
    }
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(m, "files");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(m, "filesSize");
    out->files = cJSON_IsNumber(files) && files->valuedouble > 0 && files->valuedouble <= LUA_PKG_FILES_MAX
                 ? (int)files->valuedouble : 0;
    out->files_size = cJSON_IsNumber(bytes) && bytes->valuedouble > 0 && bytes->valuedouble <= LUA_PKG_FILES_TOTAL
                      ? (int32_t)bytes->valuedouble : 0;
    out->temp = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "temp"));
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(m, "expires");
    out->expires = cJSON_IsNumber(exp) ? (int64_t)exp->valuedouble : 0;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(m, "code");
    const cJSON *luac = cJSON_GetObjectItemCaseSensitive(code, "luac");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(code, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(code, "size");
    out->code = cJSON_IsString(luac) && !strcmp(luac->valuestring, LUA_SOLANA_LUAC) && cJSON_IsString(sha)
                && lua_pkg_unhex(sha->valuestring, out->code_sha) && cJSON_IsNumber(size)
                && size->valuedouble > 0 && size->valuedouble <= LUA_PKG_CODE_MAX;
    out->code_size = out->code ? (int32_t)size->valuedouble : 0;
    cJSON_Delete(m);
    return ok;
}

bool lua_pkg_load(const char *id, lua_pkg_t *out)
{
    char rel[64], path[160];
    struct stat st;
    if (!lua_pkg_valid_id(id) || !os_data_ready()) {
        return false;
    }
    lua_pkg_rel(rel, sizeof(rel), id, "main.lua");
    os_data_path(path, sizeof(path), rel);
    if (stat(path, &st) != 0) {
        return false;
    }
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    bool ok = json && lua_pkg_parse(json, id, out);
    free(json);
    return ok;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const lua_pkg_t *)a)->name, ((const lua_pkg_t *)b)->name);
}

int lua_pkg_scan(lua_pkg_t *out, int max)
{
    if (!os_data_ready()) {
        return -1;
    }
    lua_install_recover();          // an update cut off mid-swap
    char path[160];
    os_data_path(path, sizeof(path), "apps");
    DIR *d = opendir(path);
    if (!d) {
        return 0;       // nothing installed yet
    }
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (lua_pkg_valid_id(e->d_name) && lua_pkg_load(e->d_name, &out[n])) {
            n++;
        }
    }
    closedir(d);
    qsort(out, n, sizeof(*out), by_name);
    return n;
}

bool lua_pkg_remove(const char *id)
{
    char rel[40];
    if (!lua_pkg_valid_id(id) || lua_install_busy(id)) {
        return false;
    }
    snprintf(rel, sizeof(rel), "apps/%s", id);
    return os_data_remove(rel);
}

bool lua_pkg_expired(const lua_pkg_t *p)
{
    int64_t now = os_cloud_time_ms();
    return p->temp && p->expires && now && now >= p->expires;
}

static bool set_expires(const char *id, int64_t expires)
{
    char rel[64];
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    cJSON *m = json ? cJSON_Parse(json) : NULL;
    free(json);
    char *out = NULL;
    if (cJSON_IsObject(m)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "expires");
        cJSON_AddNumberToObject(m, "expires", (double)expires);
        out = cJSON_PrintUnformatted(m);
    }
    cJSON_Delete(m);
    bool ok = out && os_data_write(rel, out, strlen(out));
    cJSON_free(out);
    return ok;
}

int lua_pkg_apply_library(const char *body)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *it;
    int changed = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(it, "expires");
        lua_pkg_t pkg;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "temp")) || !cJSON_IsString(id) || !cJSON_IsNumber(exp)
                || !lua_pkg_load(id->valuestring, &pkg) || !pkg.temp || pkg.expires == (int64_t)exp->valuedouble) {
            continue;
        }
        changed += set_expires(pkg.id, (int64_t)exp->valuedouble);
    }
    cJSON_Delete(j);
    return changed;
}
