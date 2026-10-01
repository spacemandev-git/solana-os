// Device API client and outbox, see os_api.h and server/API.md.
//
// Requests wait in a short list. The cloud task (os_cloud.c) is the worker:
// between hellos it takes one request at a time, makes the blocking os_http
// call with the badge's bearer and posts the answer to the OS task, where the
// callback runs with the LVGL lock held. A request stays in the list until
// it is delivered, so os_api_cancel() can mark it wherever it is; delivery
// and cancel both run on the OS task, so a cancelled callback never runs.
//
// The outbox holds POSTs that must reach the server eventually. Lines go to
// /sdcard/hh/outbox.jsonl ({"path":..., "body":...}); the worker renames that
// file to outbox.sending, sends its lines in order and deletes it when done,
// so adding never waits for a send. Without a card the lines stay in RAM.
//
// Portable: the emulator runs it over its fetch-based os_http.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "os.h"
#include "os_api.h"
#include "os_cloud.h"
#include "os_cmd.h"
#include "os_data.h"
#include "os_http.h"
#include "os_net.h"
#include "os_wifi.h"

static const char *TAG = "os_api";

#define QUEUE_MAX       8               // requests waiting for the worker
#define BODY_MAX        4096            // API.md: request bodies are <= 4 KB
#define MANIFEST_B64    2048            // X-App-Manifest header, base64
#define OUTBOX_FILE     "outbox.jsonl"
#define OUTBOX_SENDING  "outbox.sending"
#define OUTBOX_LINE_MAX (BODY_MAX + 160)
#define OUTBOX_RAM_MAX  20
#define OUTBOX_RESP     1024
#define RETRY_MIN_MS    15000
#define RETRY_MAX_MS    (5 * 60 * 1000)
#define POST_TRIES      50              // OS queue full: wait up to ~0.5 s to deliver

typedef struct req {
    struct req *next;
    char method[8];
    const char *path, *body, *dest;     // point into the same allocation
    size_t cap;                         // response cap, or download max bytes
    os_api_cb_t cb;
    void *ctx;
    bool download, taken, cancelled;
    int status;
    char *resp;                         // NUL-terminated answer (or manifest), NULL if none
} req_t;

typedef struct line {
    struct line *next;
    char text[];
} line_t;

static SemaphoreHandle_t s_lock;        // request list
static SemaphoreHandle_t s_box_lock;    // outbox file / RAM list
static req_t *s_reqs;
static line_t *s_ram;                   // outbox without a card
static int s_box_n = -1;                // entries pending, -1 = not counted yet
static long s_sending_off;              // bytes of outbox.sending already sent
static int64_t s_box_retry_at;
static int s_box_backoff = RETRY_MIN_MS;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool os_api_online(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED && os_cloud_registered();
}

// ---------------------------------------------------------------------------
// Requests

static esp_err_t enqueue(const char *method, const char *path, const char *body, const char *dest, size_t cap,
                         bool download, os_api_cb_t cb, void *ctx)
{
    if (!method || !path || path[0] != '/' || strlen(method) >= sizeof(((req_t *)0)->method) ||
            (body && strlen(body) > BODY_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!os_api_online()) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t lp = strlen(path) + 1, lb = body ? strlen(body) + 1 : 0, ld = dest ? strlen(dest) + 1 : 0;
    req_t *r = calloc(1, sizeof(*r) + lp + lb + ld);
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    char *p = (char *)(r + 1);
    strlcpy(r->method, method, sizeof(r->method));
    r->path = memcpy(p, path, lp);
    if (body) {
        r->body = memcpy(p + lp, body, lb);
    }
    if (dest) {
        r->dest = memcpy(p + lp + lb, dest, ld);
    }
    r->cap = cap;
    r->download = download;
    r->cb = cb;
    r->ctx = ctx;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    req_t **tail = &s_reqs;
    for (; *tail; tail = &(*tail)->next) {
        n += !(*tail)->taken;
    }
    if (n >= QUEUE_MAX) {
        xSemaphoreGive(s_lock);
        free(r);
        return ESP_ERR_NO_MEM;
    }
    *tail = r;
    xSemaphoreGive(s_lock);
    os_cloud_kick();
    return ESP_OK;
}

esp_err_t os_api_call(const char *method, const char *path, const char *body, size_t resp_cap,
                      os_api_cb_t cb, void *ctx)
{
    return enqueue(method, path, body, NULL, resp_cap ? resp_cap : OS_API_RESP_DEFAULT, false, cb, ctx);
}

esp_err_t os_api_download(const char *path, const char *dest, size_t max_bytes, os_api_cb_t cb, void *ctx)
{
    if (!dest || !dest[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue("GET", path, NULL, dest, max_bytes, true, cb, ctx);
}

void os_api_prewarm(void)
{
    if (os_api_online() && !os_http_connected()) {
        os_cloud_refresh();
    }
}

void os_api_cancel(void *ctx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (req_t *r = s_reqs; r; r = r->next) {
        if (r->ctx == ctx) {
            r->cancelled = true;
        }
    }
    xSemaphoreGive(s_lock);
}

static void unlink_req(req_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (req_t **pp = &s_reqs; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

// OS task.
static void deliver(void *arg)
{
    req_t *r = arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cancelled = r->cancelled;
    xSemaphoreGive(s_lock);
    unlink_req(r);
    if (!cancelled && r->cb) {
        r->cb(r->status, r->status ? (r->resp ? r->resp : "") : NULL, r->ctx);
    }
    free(r->resp);
    free(r);
}

static void post_result(req_t *r)
{
    for (int i = 0; i < POST_TRIES; i++) {
        if (os_post(deliver, r)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "OS queue full: dropped the answer to %s %s", r->method, r->path);
    unlink_req(r);
    free(r->resp);
    free(r);
}

// The oldest request nobody took yet; cancelled ones are dropped on the way.
static req_t *take_next(void)
{
    req_t *r = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (req_t **pp = &s_reqs; *pp;) {
        req_t *q = *pp;
        if (q->taken) {
            pp = &q->next;
        } else if (q->cancelled) {
            *pp = q->next;
            free(q);
        } else {
            q->taken = true;
            r = q;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

static void url_for(const char *path, char *out, size_t n)
{
    os_cloud_server(out, n);
    strlcat(out, path, n);
}

static void run_download(req_t *r, const char *url, const char *bearer)
{
    char tmp[168];
    char *b64 = malloc(MANIFEST_B64);
    snprintf(tmp, sizeof(tmp), "%s.part", r->dest);
    if (!b64 || !os_data_ready()) {
        free(b64);
        return;                             // status 0: no card or no memory
    }
    size_t root = strlen(OS_DATA_DIR);
    if (!strncmp(r->dest, OS_DATA_DIR "/", root + 1)) {
        os_data_mkdirs(r->dest + root + 1);     // apps/<id>/ may not exist yet
    }
    esp_err_t err = os_http_download(url, bearer, tmp, r->cap, "X-App-Manifest", b64, MANIFEST_B64, &r->status);
    if (err != ESP_OK || r->status / 100 != 2) {
        unlink(tmp);
        if (err != ESP_OK) {
            r->status = 0;
        }
    } else {
        unlink(r->dest);                    // FatFs rename() won't replace a file
        if (rename(tmp, r->dest) != 0) {
            unlink(tmp);
            r->status = 0;
        }
    }
    // The manifest header is base64 of app.json (API.md).
    size_t n = 0;
    r->resp = calloc(1, MANIFEST_B64);
    if (r->resp && b64[0] &&
            mbedtls_base64_decode((unsigned char *)r->resp, MANIFEST_B64 - 1, &n, (unsigned char *)b64, strlen(b64))) {
        r->resp[0] = 0;
    }
    free(b64);
}

static void run(req_t *r, bool online)
{
    char url[192], bearer[96];
    if (!online || !os_cloud_bearer(bearer, sizeof(bearer))) {
        return;                             // status 0
    }
    url_for(r->path, url, sizeof(url));
    if (r->download) {
        run_download(r, url, bearer);
        return;
    }
    r->resp = malloc(r->cap);
    if (!r->resp) {
        return;
    }
    if (os_http_request(r->method, url, bearer, r->body, r->resp, r->cap, &r->status) != ESP_OK) {
        r->status = 0;
    }
    if (r->status == 401) {
        os_cloud_refresh();                 // the secret may have been rotated: hello sorts it out
    }
}

// ---------------------------------------------------------------------------
// Outbox

// RAM or card: where entries go right now.
static bool box_on_card(void)
{
    return os_data_ready();
}

static int count_lines(const char *rel)
{
    char path[160];
    os_data_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "r");
    int n = 0, c, prev = '\n';
    if (!f) {
        return 0;
    }
    while ((c = fgetc(f)) != EOF) {
        n += c == '\n';
        prev = c;
    }
    fclose(f);
    return n + (prev != '\n');
}

// Lazily: the card is mounted before os_api_init, but counting can wait.
static void box_count_locked(void)
{
    if (s_box_n >= 0) {
        return;
    }
    s_box_n = 0;
    for (line_t *l = s_ram; l; l = l->next) {
        s_box_n++;
    }
    if (box_on_card()) {
        s_box_n += count_lines(OUTBOX_FILE) + count_lines(OUTBOX_SENDING);
    }
}

esp_err_t os_outbox_add(const char *path, const char *json)
{
    if (!path || strncmp(path, "/api/device/", 12) || strpbrk(path, "\"\\ \n") || !json || !json[0] ||
            strlen(json) > BODY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    // One entry per line: JSON never needs a raw newline outside strings.
    size_t n = strlen(path) + strlen(json) + 32;
    char *line = malloc(n);
    if (!line) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(line, n, "{\"path\":\"%s\",\"body\":%s}", path, json);
    for (char *c = line; *c; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_box_lock, portMAX_DELAY);
    box_count_locked();
    if (box_on_card()) {
        if (!os_data_append(OUTBOX_FILE, line)) {
            err = ESP_FAIL;
        }
    } else {
        int ram = 0;
        line_t **tail = &s_ram;
        for (; *tail; tail = &(*tail)->next) {
            ram++;
        }
        line_t *l = ram < OUTBOX_RAM_MAX ? malloc(sizeof(line_t) + strlen(line) + 1) : NULL;
        if (l) {
            l->next = NULL;
            strcpy(l->text, line);
            *tail = l;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        s_box_n++;
        s_box_retry_at = 0;                 // try now if online
    }
    xSemaphoreGive(s_box_lock);
    free(line);
    if (err == ESP_OK) {
        os_cloud_kick();
    } else {
        ESP_LOGW(TAG, "outbox add %s: %s", path, esp_err_to_name(err));
    }
    return err;
}

int os_outbox_count(void)
{
    xSemaphoreTake(s_box_lock, portMAX_DELAY);
    box_count_locked();
    int n = s_box_n;
    xSemaphoreGive(s_box_lock);
    return n;
}

void os_outbox_clear(void)
{
    xSemaphoreTake(s_box_lock, portMAX_DELAY);
    while (s_ram) {
        line_t *l = s_ram;
        s_ram = l->next;
        free(l);
    }
    if (box_on_card()) {
        os_data_remove(OUTBOX_FILE);
        os_data_remove(OUTBOX_SENDING);
    }
    s_sending_off = 0;
    s_box_n = 0;
    xSemaphoreGive(s_box_lock);
}

// POST one stored line. Returns the HTTP status, 0 if nothing came back.
static int send_line(const char *line)
{
    cJSON *j = cJSON_Parse(line);
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(j, "path"));
    char *body = path ? cJSON_PrintUnformatted(cJSON_GetObjectItem(j, "body")) : NULL;
    int status = 0;
    char url[192], bearer[96];
    char *resp = body ? malloc(OUTBOX_RESP) : NULL;
    if (!path || !body) {
        status = 400;                       // unreadable: drop it like a refused request
        ESP_LOGW(TAG, "outbox: dropping a bad line");
    } else if (resp && os_cloud_bearer(bearer, sizeof(bearer))) {
        url_for(path, url, sizeof(url));
        if (os_http_request("POST", url, bearer, body, resp, OUTBOX_RESP, &status) != ESP_OK) {
            status = 0;
        }
        // X Dex uploads answer with the new balance and Level.
        cJSON *a = status / 100 == 2 ? cJSON_Parse(resp) : NULL;
        const cJSON *lamps = cJSON_GetObjectItem(a, "lamps"), *level = cJSON_GetObjectItem(a, "level");
        if (cJSON_IsNumber(lamps) && cJSON_IsNumber(level)) {
            os_cloud_note_lamps((int)lamps->valuedouble, (int)level->valuedouble);
        }
        cJSON_Delete(a);
        ESP_LOGI(TAG, "outbox: POST %s -> %d", path, status);
    }
    free(resp);
    cJSON_free(body);
    cJSON_Delete(j);
    return status;
}

// Sent for good (2xx) or refused for good (4xx). 401, 408 and 429 are
// temporary: a re-hello fixes the bearer, the others clear up on their own.
static bool done_with(int status)
{
    return status / 100 == 2 || (status / 100 == 4 && status != 401 && status != 408 && status != 429);
}

static void box_backoff(void)
{
    s_box_retry_at = now_ms() + s_box_backoff;
    s_box_backoff = s_box_backoff * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_box_backoff * 2;
}

static void box_sent(void)
{
    s_box_backoff = RETRY_MIN_MS;
    xSemaphoreTake(s_box_lock, portMAX_DELAY);
    if (s_box_n > 0) {
        s_box_n--;
    }
    xSemaphoreGive(s_box_lock);
}

// Send the next outbox entry. Returns true if one was attempted.
static bool flush_one(void)
{
    if (now_ms() < s_box_retry_at) {
        return false;
    }
    // RAM entries first (no card); they are older than anything on a card inserted later.
    xSemaphoreTake(s_box_lock, portMAX_DELAY);
    line_t *head = s_ram;
    xSemaphoreGive(s_box_lock);
    if (head) {
        int status = send_line(head->text);
        if (!done_with(status)) {
            box_backoff();
            return true;
        }
        xSemaphoreTake(s_box_lock, portMAX_DELAY);
        s_ram = head->next;
        xSemaphoreGive(s_box_lock);
        free(head);
        box_sent();
        return true;
    }
    if (!box_on_card()) {
        return false;
    }

    char sending[160], pending[160];
    os_data_path(sending, sizeof(sending), OUTBOX_SENDING);
    os_data_path(pending, sizeof(pending), OUTBOX_FILE);
    FILE *f = fopen(sending, "r");
    if (!f) {
        // Start a new batch: what was added so far, in order.
        xSemaphoreTake(s_box_lock, portMAX_DELAY);
        bool have = rename(pending, sending) == 0;
        xSemaphoreGive(s_box_lock);
        s_sending_off = 0;
        f = have ? fopen(sending, "r") : NULL;
        if (!f) {
            return false;
        }
    }
    char *line = malloc(OUTBOX_LINE_MAX);
    bool got = false;
    if (line && fseek(f, s_sending_off, SEEK_SET) == 0) {
        // Skip blank lines; a line longer than the buffer can't be valid.
        while (!got && fgets(line, OUTBOX_LINE_MAX, f)) {
            size_t n = strlen(line);
            long next = ftell(f);
            bool whole = n && line[n - 1] == '\n';
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = 0;
            }
            if (!whole && !feof(f)) {
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') {
                }
                next = ftell(f);
                ESP_LOGW(TAG, "outbox: dropping an over-long line");
                s_sending_off = next;
                box_sent();
                continue;
            }
            if (n) {
                got = true;
                fclose(f);
                f = NULL;
                int status = send_line(line);
                if (done_with(status)) {
                    s_sending_off = next;
                    box_sent();
                } else {
                    box_backoff();
                }
            } else {
                s_sending_off = next;
            }
        }
    }
    if (f) {
        fclose(f);
    }
    if (line && !got) {
        unlink(sending);                    // batch finished
        s_sending_off = 0;
    }
    free(line);
    return got;
}

// ---------------------------------------------------------------------------
// Worker entry (cloud task)

bool os_api_service(bool online)
{
    req_t *r = take_next();
    if (r) {
        run(r, online);
        post_result(r);
        return true;
    }
    return online && flush_one();
}

// ---------------------------------------------------------------------------
// Console: api <GET|POST|PATCH|DELETE> <path> [json, ' for "] / outbox

static void print_result(int status, const char *body, void *ctx)
{
    printf("api: HTTP %d\n%s\n", status, body ? body : "(no response)");
}

static int cmd_api(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "download")) {
        // api download <path> <file under /sdcard/hh>: body to the file, manifest header printed
        char dest[160];
        os_data_path(dest, sizeof(dest), argv[3]);
        esp_err_t err = os_api_download(argv[2], dest, 64 * 1024, print_result, NULL);
        if (err != ESP_OK) {
            printf("api: %s\n", esp_err_to_name(err));
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 3) {
        printf("usage: api <GET|POST|PATCH|DELETE> <path> [json]   (write ' for \" in json)\n"
               "       api download <path> <file under " OS_DATA_DIR ">\n");
        return 1;
    }
    char *body = NULL;
    if (argc > 3) {
        body = calloc(1, BODY_MAX + 1);
        if (!body) {
            return 1;
        }
        for (int i = 3; i < argc; i++) {
            strlcat(body, argv[i], BODY_MAX + 1);
            if (i + 1 < argc) {
                strlcat(body, " ", BODY_MAX + 1);
            }
        }
        for (char *c = body; *c; c++) {
            if (*c == '\'') {
                *c = '"';                   // the badge console eats double quotes
            }
        }
    }
    esp_err_t err = os_api_call(argv[1], argv[2], body, 0, print_result, NULL);
    free(body);
    if (err != ESP_OK) {
        printf("api: %s\n", err == ESP_ERR_INVALID_STATE ? "offline (Wi-Fi off or badge not registered)"
                                                          : esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_outbox(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "add")) {
        // Queue an empty POST, to watch the flush (the server answers 4xx: dropped).
        return os_outbox_add(argv[2], "{}") == ESP_OK ? 0 : 1;
    }
    int64_t wait = s_box_retry_at - now_ms();
    printf("outbox: %d pending%s, %s", os_outbox_count(), box_on_card() ? " on the card" : " in RAM (no card)",
           os_api_online() ? "online" : "offline");
    if (wait > 0) {
        printf(", next try in %lld s", (long long)(wait / 1000));
    }
    printf("\n");
    return 0;
}

void os_api_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_box_lock = xSemaphoreCreateMutex();
    os_cmd_register("api", "api <GET|POST|PATCH|DELETE> <path> [json] | download <path> <file> - device API (' for \")",
                    cmd_api);
    os_cmd_register("outbox", "outbox [add <path>] - pending uploads", cmd_outbox);
}
