// Quests over beacon relays, see quests.h, docs/MESH.md §4/5 and
// server/API.md "Beacon relay".
//
// One relay request is in flight at a time. A dialog action (open, choose)
// goes first, then the next chunk of an app download, then a keep-alive
// ping every 10 s while a conversation is open (the server lapses a session
// 30 s after the last request, and leaving a talk step unfinished that way
// loses progress).
//
// Anything can claim to be a beacon, so an answer counts only with the
// server's tag over it, under this badge's mesh key (MESH.md §5): only the
// server could have made it. Anything else is dropped and we keep waiting.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_random.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "lua/lua_install.h"
#include "quests.h"

static const char *TAG = "quests";

#define RETRY_MS        3000        // MESH.md: one retry after 3 s
#define TRIES           2
#define PING_MS         10000
#define REQ_MAX         96
#define APP_MAX         (64 * 1024)

typedef enum { RQ_NONE, RQ_DIALOG, RQ_PING, RQ_LOG, RQ_APP } rq_kind_t;

typedef struct {
    rq_kind_t kind;
    int rid;
    char beacon[OS_MESH_ID_LEN + 1];
    char req[REQ_MAX];
    int64_t sent;
    int tries;
    char nonce[17];                 // the beacon nonce the request carries
    char tag[OS_MESH_TAG_LEN + 1];  // our tag over it
    bool busy;                      // the beacon answered "busy" without a tag
} relay_t;

static relay_t s_rq;                    // in flight
static char s_want[REQ_MAX];            // dialog request waiting for its turn
static int s_rid;
static quest_dialog_t *s_dlg;
static int64_t s_last_req;

static quest_download_t s_dl;
static char s_dl_beacon[OS_MESH_ID_LEN + 1];
static char *s_dl_manifest;

static quest_entry_t *s_log;
static int s_nlog;
static int64_t s_log_at;
static bool s_log_saved;
static bool s_log_print;                // the console asked: print the answer when it comes
static bool s_log_busy;
static char s_log_error[48];


// ---------------------------------------------------------------------------
// Relay client

// The first try signs with the beacon's current nonce. The retry resends the
// identical frame: the beacon still accepts that nonce (it keeps the previous
// one for 30 s) and answers a repeat from its cache, tagged over that nonce.
static bool send_relay(relay_t *r)
{
    os_mesh_peer_t p;
    if (!os_mesh_peer(r->beacon, &p) || p.kind != OS_PEER_BEACON || !p.nonce[0]) {
        return false;
    }
    if (!r->tries) {
        char *tagged = malloc(REQ_MAX + 64);
        if (!tagged) {
            return false;
        }
        strlcpy(r->nonce, p.nonce, sizeof(r->nonce));
        snprintf(tagged, REQ_MAX + 64, "relay|%s|%s|%s|%d|%s", game_badge_id(), r->beacon, r->nonce, r->rid, r->req);
        bool ok = os_mesh_sign(tagged, r->tag);
        free(tagged);
        if (!ok) {
            return false;
        }
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "rid", r->rid);
    cJSON_AddStringToObject(m, "nonce", r->nonce);
    cJSON_AddStringToObject(m, "req", r->req);
    cJSON_AddStringToObject(m, "tag", r->tag);
    char *json = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    bool ok = json && os_mesh_send(OS_MESH_RELAY, r->beacon, json) == ESP_OK;
    cJSON_free(json);
    r->sent = game_now_ms();
    r->tries++;
    s_last_req = r->sent;
    return ok;
}

static void failed(rq_kind_t kind, const char *error);

static void start(rq_kind_t kind, const char *beacon, const char *req)
{
    memset(&s_rq, 0, sizeof(s_rq));
    s_rq.kind = kind;
    s_rq.rid = ++s_rid;
    strlcpy(s_rq.beacon, beacon, sizeof(s_rq.beacon));
    strlcpy(s_rq.req, req, sizeof(s_rq.req));
    game_expect_relay(beacon, s_rq.rid);
    if (!send_relay(&s_rq)) {
        s_rq.kind = RQ_NONE;
        failed(kind, "The beacon is out of range");
    }
}

// Next request, by priority.
static void pump(int64_t now)
{
    if (s_rq.kind != RQ_NONE) {
        return;
    }
    if (s_dlg && s_want[0]) {
        char req[REQ_MAX];
        strlcpy(req, s_want, sizeof(req));
        s_want[0] = 0;
        start(RQ_DIALOG, s_dlg->beacon, req);
    } else if (s_dl.state == QDL_RUNNING) {
        char req[REQ_MAX];
        snprintf(req, sizeof(req), "{\"op\":\"app\",\"id\":\"%s\",\"off\":%d}", s_dl.id, s_dl.off);
        start(RQ_APP, s_dl_beacon, req);
    } else if (s_dlg && s_dlg->shown && !s_dlg->end && now - s_last_req > PING_MS) {
        start(RQ_PING, s_dlg->beacon, "{\"op\":\"ping\"}");
    }
}

// ---------------------------------------------------------------------------
// Dialog

static void dl_start(const char *app, const char *beacon);

// fx: what this answer just did (toast, LED, !LAMPs, app, page).
static void apply_fx(const cJSON *fx)
{
    if (!cJSON_IsObject(fx)) {
        return;
    }
    char msg[96] = "";
    size_t len = 0;
    const char *toast = game_jstr(fx, "toast");
    int lamps = game_jint(fx, "lamps", 0);
    if (toast[0]) {
        len += snprintf(msg, sizeof(msg), "%s", toast);
    }
    if (lamps > 0 && len < sizeof(msg)) {
        len += snprintf(msg + len, sizeof(msg) - len, "%s+%d !LAMPs", len ? "  " : "", lamps);
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
            os_cloud_note_lamps(in->lamps + lamps, in->level);
            free(in);
        }
    }
    if (game_jstr(fx, "page")[0] && len < sizeof(msg)) {
        len += snprintf(msg + len, sizeof(msg) - len, "%sNew Guide page", len ? "  " : "");
    }
    if (msg[0]) {
        os_toast("%s", msg);
    }
    const char *led = game_jstr(fx, "led");
    if (led[0]) {
        game_led_flash(game_parse_hex(led, COL_ORANGE));
    } else if (lamps > 0) {
        game_led_flash(COL_LAMPS);
    }
    const char *app = game_jstr(fx, "app");
    if (app[0] && s_dlg) {
        dl_start(app, s_dlg->beacon);
    }
}

static void on_dialog(const cJSON *res)
{
    if (!s_dlg) {
        return;
    }
    quest_dialog_t *d = s_dlg;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(res, "c");
    if (cJSON_IsObject(c)) {
        game_ascii(d->name, sizeof(d->name), game_jstr(c, "n"));
        game_ascii(d->title, sizeof(d->title), game_jstr(c, "t"));
        d->color = game_parse_hex(game_jstr(c, "col"), d->color);
    }
    game_ascii(d->quest_title, sizeof(d->quest_title), game_jstr(res, "qt"));
    game_ascii(d->text, sizeof(d->text), game_jstr(res, "text"));
    d->nopts = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(res, "opts")) {
        if (d->nopts < QUEST_OPTS_MAX && cJSON_IsString(o)) {
            game_ascii(d->opts[d->nopts++], sizeof(d->opts[0]), o->valuestring);
        }
    }
    d->end = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "end")) || !d->nopts;
    d->shown = true;
    d->error[0] = 0;
    apply_fx(cJSON_GetObjectItemCaseSensitive(res, "fx"));
}

bool quests_talk(const char *beacon_id, const char **why)
{
    os_mesh_peer_t p;
    *why = NULL;
    if (!game_claimed()) {
        *why = "Not linked yet: claim this badge first";
    } else if (!game_can_sign()) {
        *why = "The badge isn't ready to talk yet";
    } else if (!os_mesh_peer(beacon_id, &p) || p.kind != OS_PEER_BEACON) {
        *why = "The beacon is out of range";
    } else if (!s_dlg && !(s_dlg = malloc(sizeof(quest_dialog_t)))) {
        *why = "Out of memory";
    }
    if (*why) {
        return false;
    }
    memset(s_dlg, 0, sizeof(*s_dlg));
    strlcpy(s_dlg->beacon, p.id, sizeof(s_dlg->beacon));
    game_ascii(s_dlg->name, sizeof(s_dlg->name), p.name);
    game_ascii(s_dlg->title, sizeof(s_dlg->title), p.title);
    s_dlg->color = p.color;
    s_dlg->waiting = true;
    strlcpy(s_want, "{\"op\":\"open\"}", sizeof(s_want));
    if (s_rq.kind == RQ_PING) {
        s_rq.kind = RQ_NONE;            // a late pong is ignored
    }
    pump(game_now_ms());
    return true;
}

void quests_choose(int option)
{
    if (!s_dlg || s_dlg->waiting || s_dlg->end || option < 0 || option >= s_dlg->nopts) {
        return;
    }
    s_dlg->waiting = true;
    snprintf(s_want, sizeof(s_want), "{\"op\":\"choose\",\"i\":%d}", option);
    if (s_rq.kind == RQ_PING) {
        s_rq.kind = RQ_NONE;
    }
    pump(game_now_ms());
}

void quests_hangup(void)
{
    free(s_dlg);
    s_dlg = NULL;
    s_want[0] = 0;
    if (s_rq.kind == RQ_DIALOG || s_rq.kind == RQ_PING) {
        s_rq.kind = RQ_NONE;
    }
}

const quest_dialog_t *quests_dialog(void)
{
    return s_dlg;
}

// ---------------------------------------------------------------------------
// Temporary app download: {"op":"app","id","off"} until off == total.
// Written to main.lua.part and renamed when complete, then app.json, so the
// Apps library never sees half an app.

static void dl_path(char *out, size_t n, const char *file)
{
    char rel[64];
    snprintf(rel, sizeof(rel), "apps/%s/%s", s_dl.id, file);
    os_data_path(out, n, rel);
}

static void dl_fail(const char *error)
{
    char path[128];
    dl_path(path, sizeof(path), "main.lua.part");
    unlink(path);
    free(s_dl_manifest);
    s_dl_manifest = NULL;
    s_dl.state = QDL_FAILED;
    strlcpy(s_dl.error, error, sizeof(s_dl.error));
    os_toast("Download stopped: %s", error);
    ESP_LOGW(TAG, "app %s: %s", s_dl.id, error);
}

// A quest may hand out (or refresh) a temporary app, but never replace an
// app the owner installed.
static bool app_replaceable(const char *id)
{
    char rel[64];
    snprintf(rel, sizeof(rel), "apps/%s/app.json", id);
    char *m = os_data_read(rel, 4096, NULL);
    if (!m) {
        return true;                    // not installed
    }
    cJSON *j = cJSON_Parse(m);
    free(m);
    bool temp = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "temp"));
    cJSON_Delete(j);
    return temp;
}

static void dl_start(const char *app, const char *beacon)
{
    if (s_dl.state == QDL_RUNNING && !strcmp(s_dl.id, app)) {
        return;
    }
    bool ok_id = strlen(app) >= 3 && strlen(app) < sizeof(s_dl.id);
    for (const char *p = app; *p; p++) {
        ok_id = ok_id && ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-');
    }
    if (!ok_id) {
        return;
    }
    free(s_dl_manifest);
    s_dl_manifest = NULL;
    memset(&s_dl, 0, sizeof(s_dl));
    strlcpy(s_dl.id, app, sizeof(s_dl.id));
    strlcpy(s_dl_beacon, beacon, sizeof(s_dl_beacon));
    if (!os_data_ready()) {
        dl_fail("insert a microSD card");
        return;
    }
    if (!app_replaceable(app)) {
        dl_fail("an app with that name is installed");
        return;
    }
    if (lua_install_busy(app)) {
        dl_fail("the App Store is installing it");
        return;
    }
    char rel[64];
    snprintf(rel, sizeof(rel), "apps/%s/main.lua.part", app);
    os_data_mkdirs(rel);
    s_dl.state = QDL_RUNNING;
}

static void on_app_chunk(const cJSON *res)
{
    const char *data = game_jstr(res, "data");
    int off = game_jint(res, "off", -1), total = game_jint(res, "total", -1);
    if (strcmp(game_jstr(res, "id"), s_dl.id) || off != s_dl.off || total < 0) {
        dl_fail("bad answer from the beacon");
        return;
    }
    if (off == 0) {
        const char *m = game_jstr(res, "m");
        free(s_dl_manifest);
        s_dl_manifest = m[0] ? strdup(m) : NULL;
    }
    char path[128];
    dl_path(path, sizeof(path), "main.lua.part");
    FILE *f = fopen(path, off == 0 ? "wb" : "ab");
    size_t len = strlen(data);
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    s_dl.total = total;
    s_dl.off += len;                    // API.md: off and total count bytes
    if (!ok) {
        dl_fail("couldn't write to the microSD card");
        return;
    }
    if ((!len && s_dl.off < total) || s_dl.off > total || s_dl.off > APP_MAX) {
        dl_fail("bad answer from the beacon");
        return;
    }
    if (s_dl.off < total) {
        return;                         // pump() asks for the next chunk
    }
    if (!app_replaceable(s_dl.id)) {
        dl_fail("an app with that name is installed");
        return;
    }
    char dest[128];
    dl_path(dest, sizeof(dest), "code.bin");
    unlink(dest);                       // the store's compiled code of an earlier copy: it runs from source now
    dl_path(dest, sizeof(dest), "main.lua");
    unlink(dest);
    ok = rename(path, dest) == 0;
    char rel[64];
    snprintf(rel, sizeof(rel), "apps/%s/app.json", s_dl.id);
    ok = ok && s_dl_manifest && os_data_write(rel, s_dl_manifest, strlen(s_dl_manifest));
    free(s_dl_manifest);
    s_dl_manifest = NULL;
    if (!ok) {
        dl_fail("couldn't save the app");
        return;
    }
    s_dl.state = QDL_DONE;
    os_toast("New app. Open it from Apps");
    game_led_flash(COL_ORANGE);
    ESP_LOGI(TAG, "app %s saved (%d bytes)", s_dl.id, total);
}

const quest_download_t *quests_download(void)
{
    return &s_dl;
}

// ---------------------------------------------------------------------------
// Quest log

static void log_set(const cJSON *arr, bool compact)
{
    quest_entry_t *log = calloc(QUEST_LOG_MAX, sizeof(quest_entry_t));
    if (!log) {
        return;
    }
    int n = 0;
    const cJSON *q;
    cJSON_ArrayForEach(q, arr) {
        if (n == QUEST_LOG_MAX) {
            break;
        }
        quest_entry_t *e = &log[n++];
        game_ascii(e->id, sizeof(e->id), game_jstr(q, "id"));
        if (compact) {
            game_ascii(e->title, sizeof(e->title), game_jstr(q, "t"));
            game_ascii(e->status, sizeof(e->status), game_jstr(q, "s"));
            game_ascii(e->step, sizeof(e->step), game_jstr(q, "st"));
            game_ascii(e->at, sizeof(e->at), game_jstr(q, "at"));
        } else {
            const cJSON *step = cJSON_GetObjectItemCaseSensitive(q, "step");
            game_ascii(e->title, sizeof(e->title), game_jstr(q, "title"));
            game_ascii(e->status, sizeof(e->status), game_jstr(q, "status"));
            game_ascii(e->step, sizeof(e->step), game_jstr(step, "title"));
            // Where to go: the current step's character, else the quest giver.
            const char *at = game_jstr(step, "at");
            game_ascii(e->at, sizeof(e->at), at[0] ? at : game_jstr(cJSON_GetObjectItemCaseSensitive(q, "giver"), "name"));
        }
    }
    free(s_log);
    s_log = log;
    s_nlog = n;
}

// Cache the compact relay shape, whatever the source.
static void log_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "q");
    for (int i = 0; i < s_nlog; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", s_log[i].id);
        cJSON_AddStringToObject(e, "t", s_log[i].title);
        cJSON_AddStringToObject(e, "s", s_log[i].status);
        cJSON_AddStringToObject(e, "st", s_log[i].step);
        cJSON_AddStringToObject(e, "at", s_log[i].at);
        cJSON_AddItemToArray(arr, e);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body) {
        game_cache_put("quests", body);
    }
    cJSON_free(body);
    s_log_at = os_cloud_time_ms();
    s_log_saved = false;
}

static void log_from_cache(void)
{
    int64_t at;
    cJSON *body = game_cache_get("quests", &at);
    if (body) {
        log_set(cJSON_GetObjectItemCaseSensitive(body, "q"), true);
        s_log_at = at;
        s_log_saved = true;
    }
    cJSON_Delete(body);
}

static void print_log(void)
{
    if (!s_nlog) {
        printf("quest log: %s\n", s_log_busy ? "checking..." : "nothing saved");
    }
    for (int i = 0; i < s_nlog; i++) {
        printf("  quest %s [%s] %s: %s%s%s\n", s_log[i].id, s_log[i].status, s_log[i].title, s_log[i].step,
               s_log[i].at[0] ? " @ " : "", s_log[i].at);
    }
    if (s_log_error[0]) {
        printf("  log: %s\n", s_log_error);
    }
}

// The log belongs to the Quests app; after the console used it, let it go.
static void log_release(void)
{
    const os_app_t *app = os_current();
    if (!app || strcmp(app->id, "quests")) {
        quests_log_free();
    }
}

// A fetch the console started has answered: print it.
static void log_answered(void)
{
    if (s_log_print) {
        s_log_print = false;
        print_log();
        log_release();
    }
}

static void on_log_api(int status, const char *body, void *ctx)
{
    s_log_busy = false;
    if (status != 200) {
        snprintf(s_log_error, sizeof(s_log_error), status ? "The server said no (%d)" : "Can't reach the server", status);
        log_answered();
        return;
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(j, "items");
    if (cJSON_IsArray(items)) {
        log_set(items, false);
        log_save();
        s_log_error[0] = 0;
    }
    cJSON_Delete(j);
    log_answered();
}

void quests_log_refresh(void)
{
    if (s_log_busy) {
        return;
    }
    if (!s_log) {
        log_from_cache();
    }
    s_log_error[0] = 0;
    if (os_api_online()) {
        if (os_api_call("GET", "/api/device/quests", NULL, 0, on_log_api, &s_log_busy) == ESP_OK) {
            s_log_busy = true;
        }
        return;
    }
    os_mesh_peer_t *peers = malloc(8 * sizeof(os_mesh_peer_t));
    int n = peers ? os_mesh_peers(peers, 8) : 0;
    int beacon = -1;
    for (int i = 0; i < n && beacon < 0; i++) {
        beacon = peers[i].kind == OS_PEER_BEACON ? i : -1;
    }
    if (beacon >= 0 && game_can_sign() && game_claimed() && (s_rq.kind == RQ_NONE || s_rq.kind == RQ_PING)) {
        s_log_busy = true;
        start(RQ_LOG, peers[beacon].id, "{\"op\":\"log\"}");
    }
    free(peers);
    if (beacon < 0 && !s_log) {
        strlcpy(s_log_error, "Wi-Fi is off and no beacon is near", sizeof(s_log_error));
    }
}

bool quests_log_busy(void)
{
    return s_log_busy;
}

int quests_log(const quest_entry_t **out)
{
    *out = s_log;
    return s_nlog;
}

bool quests_log_saved(void)
{
    return s_log_saved;
}

int64_t quests_log_at(void)
{
    return s_log_at;
}

const char *quests_log_error(void)
{
    return s_log_error;
}

void quests_log_free(void)
{
    os_api_cancel(&s_log_busy);
    if (s_rq.kind == RQ_LOG) {
        s_rq.kind = RQ_NONE;            // a late answer is ignored
    }
    s_log_busy = false;
    free(s_log);
    s_log = NULL;
    s_nlog = 0;
    s_log_at = 0;
}

// ---------------------------------------------------------------------------
// Answers

static void failed(rq_kind_t kind, const char *error)
{
    switch (kind) {
    case RQ_DIALOG:
        if (s_dlg) {
            s_dlg->waiting = false;
            strlcpy(s_dlg->error, error, sizeof(s_dlg->error));
        }
        break;
    case RQ_LOG:
        s_log_busy = false;
        strlcpy(s_log_error, error, sizeof(s_log_error));
        log_answered();
        break;
    case RQ_APP:
        dl_fail(error);
        break;
    default:
        break;                          // a lost ping is harmless
    }
}

// tag(key, "resp|" + me + "|" + beacon + "|" + nonce + "|" + rid + "|" + ("1|" + res or "0|" + error))
static bool answer_signed(const relay_t *r, bool ok, const char *payload, const char *tag)
{
    size_t cap = strlen(payload) + 64;
    char *s = malloc(cap);
    char want[OS_MESH_TAG_LEN + 1];
    bool good = s && snprintf(s, cap, "resp|%s|%s|%s|%d|%s|%s", game_badge_id(), r->beacon, r->nonce, r->rid,
                              ok ? "1" : "0", payload) < (int)cap
             && os_mesh_sign(s, want) && !strcmp(want, tag);
    free(s);
    return good;
}

void quests_on_mesh(const os_mesh_peer_t *from, cJSON *msg)
{
    if (s_rq.kind == RQ_NONE || strcmp(from->id, s_rq.beacon) || game_jint(msg, "rid", -1) != s_rq.rid) {
        return;
    }
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "ok"));
    const char *tag = game_jstr(msg, "tag");
    if (!tag[0]) {
        // The beacon's own trouble (no server answer): worth a word, not trust.
        s_rq.busy = s_rq.busy || !ok;
        return;
    }
    const char *payload = game_jstr(msg, ok ? "res" : "error");
    if (!answer_signed(&s_rq, ok, payload, tag)) {
        ESP_LOGW(TAG, "rid %d: answer from %s without a valid tag, dropped", s_rq.rid, from->id);
        return;
    }
    relay_t rq = s_rq;
    s_rq.kind = RQ_NONE;
    if (!ok) {
        failed(rq.kind, payload[0] ? payload : "The beacon said no");
        return;
    }
    cJSON *res = cJSON_Parse(payload);
    switch (rq.kind) {
    case RQ_DIALOG:
        if (s_dlg) {
            s_dlg->waiting = false;
            on_dialog(res);
        }
        break;
    case RQ_LOG:
        s_log_busy = false;
        log_set(cJSON_GetObjectItemCaseSensitive(res, "q"), true);
        log_save();
        log_answered();
        break;
    case RQ_APP:
        if (s_dl.state == QDL_RUNNING) {
            on_app_chunk(res);
        }
        break;
    default:
        break;
    }
    cJSON_Delete(res);
}

// ---------------------------------------------------------------------------

void quests_tick(int64_t now)
{
    if (s_rq.kind != RQ_NONE && now - s_rq.sent > RETRY_MS) {
        if (s_rq.tries < TRIES && send_relay(&s_rq)) {
            ESP_LOGD(TAG, "rid %d: retry", s_rq.rid);
        } else {
            rq_kind_t kind = s_rq.kind;
            s_rq.kind = RQ_NONE;
            failed(kind, s_rq.busy ? "The beacon is busy" : "The beacon didn't answer");
        }
    }
    pump(now);
}

// ---------------------------------------------------------------------------
// Console: quest [status|talk <beacon>|choose <i>|bye|log]. Runs on the OS
// task like everything else here.

static void cmd_status(void)
{
    os_mesh_peer_t *peers = malloc(8 * sizeof(os_mesh_peer_t));
    int n = peers ? os_mesh_peers(peers, 8) : 0;
    for (int i = 0; i < n; i++) {
        if (peers[i].kind == OS_PEER_BEACON) {
            printf("beacon %s '%s' (%s) %d dBm nonce %s\n", peers[i].id, peers[i].name, peers[i].character,
                   peers[i].rssi, peers[i].nonce);
        }
    }
    free(peers);
    if (s_rq.kind != RQ_NONE) {
        printf("rid %d in flight to %s (try %d)%s\n", s_rq.rid, s_rq.beacon, s_rq.tries, s_rq.busy ? ", beacon busy" : "");
    }
    const quest_dialog_t *d = s_dlg;
    if (d) {
        printf("talking to %s (%s)%s%s %s\n  %s\n", d->name, d->beacon, d->waiting ? " waiting" : "",
               d->end ? " end" : "", d->error, d->text);
        for (int i = 0; i < d->nopts; i++) {
            printf("  [%d] %s\n", i, d->opts[i]);
        }
    }
    if (s_dl.state != QDL_NONE) {
        static const char *const st[] = {"-", "running", "done", "failed"};
        printf("app %s: %s %d/%d %s\n", s_dl.id, st[s_dl.state], s_dl.off, s_dl.total, s_dl.error);
    }
    if (s_log) {
        print_log();
    }
}

static void cmd_run(void *arg)
{
    char *line = arg;
    char *op = strtok(line, " "), *val = strtok(NULL, " ");
    const char *why = NULL;
    if (!op || !strcmp(op, "status")) {
        cmd_status();
    } else if (!strcmp(op, "talk") && val) {
        quests_talk(val, &why);
    } else if (!strcmp(op, "choose") && val) {
        quests_choose(atoi(val));
    } else if (!strcmp(op, "bye")) {
        quests_hangup();
    } else if (!strcmp(op, "log")) {
        quests_log_refresh();
        print_log();                    // what's saved now; a fresh answer follows if one is on its way
        if (s_log_busy) {
            s_log_print = true;
        } else {
            log_release();
        }
    } else {
        why = "usage: quest [status|talk <beacon>|choose <i>|bye|log]";
    }
    if (why) {
        printf("quest: %s\n", why);
    }
    free(line);
}

static int cmd_quest(int argc, char **argv)
{
    char *line = malloc(48);
    if (line) {
        snprintf(line, 48, "%s %s", argc > 1 ? argv[1] : "status", argc > 2 ? argv[2] : "");
        if (!os_post(cmd_run, line)) {
            free(line);
        }
    }
    return 0;
}

void quests_init(void)
{
    s_rid = esp_random() % 100000;      // rids don't repeat across a quick reboot
    os_cmd_register("quest", "quest [status|talk <beacon>|choose <i>|bye|log]", cmd_quest);
}
