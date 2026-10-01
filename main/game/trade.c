// Badge-to-badge trades, see trade.h and docs/MESH.md §3.
//
// A invites, B joins. Both send offers (each edit is a new revision) and lock
// once happy with a pair of revisions; a lock only counts while both
// revisions are current, so any new offer clears both. When both locks hold,
// A's user confirms and A sends the canonical terms with its tag; B rebuilds
// the terms from the offers it saw, checks they match and signs at once (B's
// lock was its commitment), A answers "done". Whoever holds both signatures
// records the trade and uploads it; the server settles it.
//
// The server never hears about a cancel: once A's tag is out, B can still
// sign and settle. So the apps confirm the commitment (B's lock, A's sign)
// and offer no cancel after it.
//
// Unicast frames can get lost, so while a trade is open each side repeats its
// offer and lock every few seconds (receivers ignore revisions they have),
// A repeats invite / terms until answered, and duplicates get the same answer.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "trade.h"

static const char *TAG = "trade";

#define TRADES_FILE     "trades.jsonl"
#define TERMS_MAX       1100
#define RESEND_MS       3000        // invite / terms repeat, offer + lock sync
#define OFFER_DEBOUNCE  400         // LEFT/RIGHT on !LAMPs repeats fast
#define INVITE_WAIT_MS  30000
#define POLL_EVERY_MS   60000
#define POLL_IDLE_MS    (5 * 60000)
#define POLL_MAX        8

typedef struct {
    trade_view_t v;
    int my_lock_mine, my_lock_theirs;       // revisions our lock covers, -1 = none
    int their_lock_mine, their_lock_theirs; // their lock as sent: mine = their rev, theirs = ours
    bool dirty;                             // edited since the last offer went out
    char terms[TERMS_MAX];
    char tag_a[OS_MESH_TAG_LEN + 1], tag_b[OS_MESH_TAG_LEN + 1];
    int64_t at, last_offer, last_sync;
} trade_t;

static trade_t *s_t;
static trade_invite_t s_inv[TRADE_INVITES_MAX];
static int64_t s_inv_at[TRADE_INVITES_MAX];
static int s_ninv;

static int64_t s_poll_at;
static bool s_poll_busy;
static char s_poll_tids[POLL_MAX][17];
static int s_npoll;

// ---------------------------------------------------------------------------
// Helpers

static bool is_hex16(const char *s)
{
    if (strlen(s) != 16) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

// Ids end up in the canonical terms, joined with ',' and '|'.
static bool good_id(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > TRADE_ID_LEN) {
        return false;
    }
    for (; *s; s++) {
        if (*s <= 0x20 || *s >= 0x7F || *s == '|' || *s == ',') {
            return false;
        }
    }
    return true;
}

static void send(cJSON *msg)
{
    char *json = cJSON_PrintUnformatted(msg);
    if (json && s_t) {
        esp_err_t err = os_mesh_send(OS_MESH_TRADE, s_t->v.peer, json);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send to %s: %s", s_t->v.peer, esp_err_to_name(err));
        }
    }
    cJSON_free(json);
    cJSON_Delete(msg);
}

static void send_to(const char *peer, cJSON *msg)
{
    char *json = cJSON_PrintUnformatted(msg);
    if (json) {
        os_mesh_send(OS_MESH_TRADE, peer, json);
    }
    cJSON_free(json);
    cJSON_Delete(msg);
}

static cJSON *msg_new(const char *op)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "op", op);
    if (s_t) {
        cJSON_AddStringToObject(m, "tid", s_t->v.tid);
    }
    return m;
}

static void refresh_locks(void)
{
    trade_t *t = s_t;
    t->v.i_locked = !t->dirty && t->my_lock_mine == t->v.my_rev && t->my_lock_theirs == t->v.their_rev;
    t->v.they_locked = !t->dirty && t->their_lock_mine == t->v.their_rev && t->their_lock_theirs == t->v.my_rev;
    t->v.can_sign = t->v.inviter && t->v.phase == TRADE_OPEN && t->v.i_locked && t->v.they_locked;
}

static void set_phase(trade_phase_t p)
{
    s_t->v.phase = p;
    s_t->at = game_now_ms();
}

static void cancelled(const char *fmt, const char *who)
{
    s_t->v.can_sign = false;
    snprintf(s_t->v.reason, sizeof(s_t->v.reason), fmt, who);
    set_phase(TRADE_CANCELLED);
    ESP_LOGI(TAG, "%s cancelled: %s", s_t->v.tid, s_t->v.reason);
}

static void record(const trade_t *t);

// A's tag is out but B's signature never came. The server doesn't hear
// cancels, so B may still sign and settle: record it as unconfirmed and keep
// polling for the outcome.
static void unconfirmed(const char *fmt)
{
    record(s_t);
    s_t->v.unconfirmed = true;
    cancelled(fmt, s_t->v.label);
}

static trade_t *start(const char *tid, const char *peer, const char *name, bool inviter)
{
    trade_t *t = calloc(1, sizeof(trade_t));
    if (!t) {
        return NULL;
    }
    free(s_t);
    s_t = t;
    strlcpy(t->v.tid, tid, sizeof(t->v.tid));
    strlcpy(t->v.peer, peer, sizeof(t->v.peer));
    game_peer_label(name, peer, t->v.label, sizeof(t->v.label));
    t->v.inviter = inviter;
    t->my_lock_mine = t->their_lock_mine = -1;
    game_expect_trade(tid);
    return t;
}

// ---------------------------------------------------------------------------
// Bundles and canonical terms

static cJSON *bundle_json(const trade_bundle_t *b)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "l", b->lamps);
    cJSON *items = cJSON_AddArrayToObject(j, "i");
    for (int i = 0; i < b->nitems; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(b->items[i].id));
        cJSON_AddItemToArray(pair, cJSON_CreateString(b->items[i].name));
        cJSON_AddItemToArray(items, pair);
    }
    cJSON *pages = cJSON_AddArrayToObject(j, "p");
    for (int i = 0; i < b->npages; i++) {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateString(b->pages[i].id));
        cJSON_AddItemToArray(pair, cJSON_CreateString(b->pages[i].name));
        cJSON_AddItemToArray(pages, pair);
    }
    return j;
}

static bool things_parse(const cJSON *arr, trade_thing_t *out, int max, int *n)
{
    *n = 0;
    if (!arr) {
        return true;
    }
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > max) {
        return false;
    }
    const cJSON *pair;
    cJSON_ArrayForEach(pair, arr) {
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(pair, 0));
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(pair, 1));
        if (!id || !good_id(id)) {
            return false;
        }
        strlcpy(out[*n].id, id, sizeof(out[*n].id));
        game_ascii(out[*n].name, sizeof(out[*n].name), name ? name : id);
        (*n)++;
    }
    return true;
}

static bool bundle_parse(const cJSON *j, trade_bundle_t *b)
{
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(j, "l");
    if (!cJSON_IsObject(j) || (l && (!cJSON_IsNumber(l) || l->valuedouble < 0 || l->valuedouble > 1e9))) {
        return false;
    }
    b->lamps = l ? (int)l->valuedouble : 0;
    return things_parse(cJSON_GetObjectItemCaseSensitive(j, "i"), b->items, TRADE_MAX_ITEMS, &b->nitems)
        && things_parse(cJSON_GetObjectItemCaseSensitive(j, "p"), b->pages, TRADE_MAX_PAGES, &b->npages);
}

static int by_id(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Ids sorted ascending (byte order, like the server's string sort), joined with ','.
static void join_ids(const trade_thing_t *things, int n, char *out, size_t cap)
{
    const char *ids[TRADE_MAX_ITEMS > TRADE_MAX_PAGES ? TRADE_MAX_ITEMS : TRADE_MAX_PAGES];
    for (int i = 0; i < n; i++) {
        ids[i] = things[i].id;
    }
    qsort(ids, n, sizeof(ids[0]), by_id);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len < cap; i++) {
        len += snprintf(out + len, cap - len, "%s%s", i ? "," : "", ids[i]);
    }
}

// T = tid|A|B|aLamps|aItems|aPages|bLamps|bItems|bPages (MESH.md §3)
static bool build_terms(const trade_t *t, char *out, size_t cap)
{
    const trade_bundle_t *a = t->v.inviter ? &t->v.mine : &t->v.theirs;
    const trade_bundle_t *b = t->v.inviter ? &t->v.theirs : &t->v.mine;
    const char *ida = t->v.inviter ? game_badge_id() : t->v.peer;
    const char *idb = t->v.inviter ? t->v.peer : game_badge_id();
    char ai[TRADE_MAX_ITEMS * (TRADE_ID_LEN + 1) + 1], ap[TRADE_MAX_PAGES * (TRADE_ID_LEN + 1) + 1];
    char bi[sizeof(ai)], bp[sizeof(ap)];
    join_ids(a->items, a->nitems, ai, sizeof(ai));
    join_ids(a->pages, a->npages, ap, sizeof(ap));
    join_ids(b->items, b->nitems, bi, sizeof(bi));
    join_ids(b->pages, b->npages, bp, sizeof(bp));
    int n = snprintf(out, cap, "%s|%s|%s|%d|%s|%s|%d|%s|%s", t->v.tid, ida, idb, a->lamps, ai, ap, b->lamps, bi, bp);
    return n > 0 && (size_t)n < cap;
}

static void summary(const trade_bundle_t *b, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    if (b->lamps) {
        len += snprintf(out, cap, "%d !LAMPs", b->lamps);
    }
    for (int i = 0; i < b->nitems + b->npages && len < cap; i++) {
        const char *name = i < b->nitems ? b->items[i].name : b->pages[i - b->nitems].name;
        len += snprintf(out + len, cap - len, "%s%s", len ? ", " : "", name);
    }
    if (!out[0]) {
        strlcpy(out, "nothing", cap);
    }
}

// ---------------------------------------------------------------------------
// Records and uploads

static void append_status(const char *tid, const char *status, const char *reason)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "tid", tid);
    cJSON_AddStringToObject(j, "status", status);
    cJSON_AddStringToObject(j, "reason", reason ? reason : "");
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (line) {
        os_data_append(TRADES_FILE, line);
    }
    cJSON_free(line);
}

// Server answer {tid, status, reason}, from the upload or a poll.
static void outcome(const char *body, bool toast)
{
    cJSON *j = cJSON_Parse(body);
    const char *tid = game_jstr(j, "tid"), *status = game_jstr(j, "status");
    if (is_hex16(tid) && (!strcmp(status, "settled") || !strcmp(status, "rejected"))) {
        const char *reason = game_jstr(j, "reason");
        append_status(tid, status, reason);
        if (s_t && !strcmp(s_t->v.tid, tid)) {
            s_t->v.settled = status[0] == 's';
            s_t->v.rejected = !s_t->v.settled;
            if (!s_t->v.unconfirmed) {
                strlcpy(s_t->v.reason, reason, sizeof(s_t->v.reason));   // keep why it was left open
            }
        }
        if (toast) {
            if (status[0] == 's') {
                os_toast("Trade settled");
            } else {
                os_toast("Trade rejected: %s", reason[0] ? reason : "the server said no");
            }
        }
        if (status[0] == 's') {
            os_cloud_refresh();         // new balance and Level
        }
    }
    cJSON_Delete(j);
}

static void uploaded(int status, const char *body, void *ctx)
{
    char *req = ctx;
    if (status == 0 || status >= 500) {
        os_outbox_add("/api/device/trades", req);
    } else if (status / 100 == 2) {
        outcome(body, true);
    } else {
        cJSON *j = cJSON_Parse(body);
        os_toast("Trade upload failed: %s", game_jstr(j, "error"));
        cJSON_Delete(j);
    }
    free(req);
}

static void upload(const trade_t *t)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "tid", t->v.tid);
    cJSON_AddStringToObject(j, "terms", t->terms);
    cJSON_AddStringToObject(j, "a", t->tag_a);
    cJSON_AddStringToObject(j, "b", t->tag_b);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return;
    }
    bool sent = false;
    if (os_api_online()) {
        char *copy = strdup(body);
        sent = copy && os_api_call("POST", "/api/device/trades", body, 0, uploaded, copy) == ESP_OK;
        if (!sent) {
            free(copy);
        }
    }
    if (!sent) {
        os_outbox_add("/api/device/trades", body);
        os_toast("Trade signed. Saved; uploads when Wi-Fi is on");
    }
    cJSON_free(body);
}

// Full record, once we hold our signature (tag_b may still be missing).
static void record(const trade_t *t)
{
    char gave[64], got[64];
    summary(&t->v.mine, gave, sizeof(gave));
    summary(&t->v.theirs, got, sizeof(got));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "tid", t->v.tid);
    cJSON_AddStringToObject(j, "peer", t->v.peer);
    cJSON_AddStringToObject(j, "name", t->v.label);
    cJSON_AddStringToObject(j, "terms", t->terms);
    cJSON_AddStringToObject(j, "a", t->tag_a);
    cJSON_AddStringToObject(j, "b", t->tag_b);
    cJSON_AddNumberToObject(j, "at", (double)os_cloud_time_ms());
    cJSON_AddStringToObject(j, "gave", gave);
    cJSON_AddStringToObject(j, "got", got);
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (line && !os_data_append(TRADES_FILE, line)) {
        ESP_LOGW(TAG, "%s not recorded (no microSD card)", t->v.tid);
    }
    cJSON_free(line);
    s_poll_at = game_now_ms() + POLL_EVERY_MS;
}

// ---------------------------------------------------------------------------
// Sending

static void send_offer(void)
{
    cJSON *m = msg_new("offer");
    cJSON_AddNumberToObject(m, "rev", s_t->v.my_rev);
    cJSON_AddItemToObject(m, "give", bundle_json(&s_t->v.mine));
    send(m);
    s_t->last_offer = game_now_ms();
}

static void send_lock(void)
{
    cJSON *m = msg_new("lock");
    cJSON_AddNumberToObject(m, "mine", s_t->my_lock_mine);
    cJSON_AddNumberToObject(m, "theirs", s_t->my_lock_theirs);
    send(m);
}

static void send_terms(void)
{
    cJSON *m = msg_new("terms");
    cJSON_AddStringToObject(m, "terms", s_t->terms);
    cJSON_AddStringToObject(m, "tag", s_t->tag_a);
    send(m);
}

static void send_sign(void)
{
    cJSON *m = msg_new("sign");
    cJSON_AddStringToObject(m, "tag", s_t->tag_b);
    send(m);
}

static bool sign(const char *role_id, char out[OS_MESH_TAG_LEN + 1])
{
    char *msg = malloc(TERMS_MAX + 16);
    bool ok = msg && snprintf(msg, TERMS_MAX + 16, "trade|%s|%s", role_id, s_t->terms) < TERMS_MAX + 16
           && os_mesh_sign(msg, out);
    free(msg);
    return ok;
}

// A: both locks hold and the user confirmed -> terms.
void trade_sign(void)
{
    trade_t *t = s_t;
    if (!t || !t->v.can_sign) {
        return;
    }
    if (!build_terms(t, t->terms, sizeof(t->terms)) || !sign(game_badge_id(), t->tag_a)) {
        cancelled("Couldn't sign the trade", NULL);
        send(msg_new("cancel"));
        return;
    }
    set_phase(TRADE_SIGNING);
    t->v.can_sign = false;
    t->last_sync = t->at;
    send_terms();
}

// ---------------------------------------------------------------------------
// Receiving

static void on_invite(const os_mesh_peer_t *from, const char *tid)
{
    if (s_t && !strcmp(s_t->v.tid, tid)) {
        if (!s_t->v.inviter && s_t->v.phase == TRADE_OPEN) {
            cJSON *m = msg_new("join");
            cJSON_AddStringToObject(m, "to", s_t->v.peer);
            send(m);                    // our join got lost
        }
        return;
    }
    // os_mesh raises the "wants to trade" toast.
    int i;
    for (i = 0; i < s_ninv && strcmp(s_inv[i].peer, from->id); i++) {
    }
    if (i == s_ninv) {
        if (s_ninv == TRADE_INVITES_MAX) {
            s_ninv--;                   // drop the oldest
        }
        i = s_ninv++;
    }
    // Newest first.
    trade_invite_t inv = {0};
    strlcpy(inv.tid, tid, sizeof(inv.tid));
    strlcpy(inv.peer, from->id, sizeof(inv.peer));
    game_peer_label(from->name, from->id, inv.label, sizeof(inv.label));
    memmove(&s_inv[1], &s_inv[0], i * sizeof(s_inv[0]));
    memmove(&s_inv_at[1], &s_inv_at[0], i * sizeof(s_inv_at[0]));
    s_inv[0] = inv;
    s_inv_at[0] = game_now_ms();
}

static void on_offer(const cJSON *msg)
{
    trade_t *t = s_t;
    int rev = game_jint(msg, "rev", -1);
    if (t->v.phase != TRADE_OPEN || rev <= t->v.their_rev) {
        return;                         // after A's tag is out, the offers are fixed
    }
    trade_bundle_t *b = malloc(sizeof(*b));
    if (!b) {
        return;
    }
    if (bundle_parse(cJSON_GetObjectItemCaseSensitive(msg, "give"), b)) {
        t->v.theirs = *b;
        t->v.their_rev = rev;
        refresh_locks();
    } else {
        ESP_LOGW(TAG, "bad offer from %s", t->v.peer);
    }
    free(b);
}

static void on_terms(const cJSON *msg)
{
    trade_t *t = s_t;
    const char *terms = game_jstr(msg, "terms"), *tag = game_jstr(msg, "tag");
    if (t->v.inviter || strlen(tag) != OS_MESH_TAG_LEN) {
        return;
    }
    if (t->v.phase == TRADE_DONE) {
        if (!strcmp(terms, t->terms)) {
            send_sign();                // our sign got lost
        }
        return;
    }
    if (t->v.phase != TRADE_OPEN || !t->v.i_locked || !t->v.they_locked) {
        return;                         // not settled on our side yet; A repeats the terms
    }
    // Check the terms against the offers we saw, byte for byte.
    if (!build_terms(t, t->terms, sizeof(t->terms)) || strcmp(terms, t->terms)) {
        ESP_LOGW(TAG, "terms mismatch:\n  got  %s\n  want %s", terms, t->terms);
        send(msg_new("cancel"));
        cancelled("The terms didn't match", NULL);
        return;
    }
    strlcpy(t->tag_a, tag, sizeof(t->tag_a));
    if (!sign(game_badge_id(), t->tag_b)) {
        send(msg_new("cancel"));
        cancelled("Couldn't sign the trade", NULL);
        return;
    }
    send_sign();
    set_phase(TRADE_DONE);
    game_led_flash(COL_AQUA);
    record(t);
    upload(t);
}

static void on_sign(const cJSON *msg)
{
    trade_t *t = s_t;
    const char *tag = game_jstr(msg, "tag");
    if (!t->v.inviter || strlen(tag) != OS_MESH_TAG_LEN) {
        return;
    }
    if (t->v.phase == TRADE_DONE) {
        send(msg_new("done"));          // our done got lost
        return;
    }
    if (t->v.phase != TRADE_SIGNING) {
        return;
    }
    strlcpy(t->tag_b, tag, sizeof(t->tag_b));
    set_phase(TRADE_DONE);
    send(msg_new("done"));
    game_led_flash(COL_AQUA);
    record(t);
    upload(t);
}

void trade_on_mesh(const os_mesh_peer_t *from, cJSON *msg)
{
    const char *op = game_jstr(msg, "op"), *tid = game_jstr(msg, "tid");
    if (from->kind != OS_PEER_BADGE || !is_hex16(tid) || !game_badge_id()[0]) {
        return;
    }
    if (!strcmp(op, "invite")) {
        if (!strcmp(game_jstr(msg, "to"), game_badge_id())) {
            on_invite(from, tid);
        }
        return;
    }
    trade_t *t = s_t;
    if (!t || strcmp(t->v.tid, tid) || strcmp(t->v.peer, from->id)) {
        return;
    }
    if (!strcmp(op, "join") || !strcmp(op, "decline")) {
        if (t->v.inviter && t->v.phase == TRADE_INVITING) {
            if (op[0] == 'j') {
                set_phase(TRADE_OPEN);
                os_toast("%s joined", t->v.label);
            } else {
                cancelled("%s said no", t->v.label);
            }
        }
    } else if (!strcmp(op, "offer")) {
        on_offer(msg);
    } else if (!strcmp(op, "lock")) {
        t->their_lock_mine = game_jint(msg, "mine", -1);
        t->their_lock_theirs = game_jint(msg, "theirs", -1);
        refresh_locks();
    } else if (!strcmp(op, "terms")) {
        on_terms(msg);
    } else if (!strcmp(op, "sign")) {
        on_sign(msg);
    } else if (!strcmp(op, "cancel")) {
        if (t->v.phase == TRADE_SIGNING) {
            unconfirmed("%s cancelled after you signed");
        } else if (t->v.phase == TRADE_INVITING || t->v.phase == TRADE_OPEN) {
            cancelled("%s cancelled", t->v.label);
        }
    }
}

// ---------------------------------------------------------------------------
// Outcome polling: GET /api/device/trades/:tid for pending records.

static void polled(int status, const char *body, void *ctx)
{
    s_poll_busy = false;
    if (status == 200) {
        outcome(body, true);
    }
}

static void poll_step(int64_t now)
{
    if (s_poll_busy || now < s_poll_at || !os_api_online()) {
        return;
    }
    if (!s_npoll) {
        trade_rec_t *recs;
        int n = trade_records_load(&recs, 20);
        for (int i = 0; i < n && s_npoll < POLL_MAX; i++) {
            if (recs[i].status == TRADE_REC_PENDING || recs[i].status == TRADE_REC_UNCONFIRMED) {
                strlcpy(s_poll_tids[s_npoll++], recs[i].tid, sizeof(s_poll_tids[0]));
            }
        }
        free(recs);
        if (!s_npoll) {
            s_poll_at = now + POLL_IDLE_MS;
            return;
        }
    }
    char path[48];
    snprintf(path, sizeof(path), "/api/device/trades/%s", s_poll_tids[--s_npoll]);
    if (os_api_call("GET", path, NULL, 0, polled, &s_poll_busy) == ESP_OK) {
        s_poll_busy = true;
    }
    if (!s_npoll) {
        s_poll_at = now + POLL_EVERY_MS;
    }
}

void trade_poll_now(void)
{
    s_poll_at = 0;
}

// ---------------------------------------------------------------------------

void trade_tick(int64_t now)
{
    for (int i = s_ninv - 1; i >= 0; i--) {
        if (now - s_inv_at[i] > TRADE_INVITE_TTL_MS) {
            s_ninv = i;                 // newest first: everything from here on is older
        }
    }
    poll_step(now);

    trade_t *t = s_t;
    if (!t) {
        return;
    }
    switch (t->v.phase) {
    case TRADE_INVITING:
        if (now - t->at > INVITE_WAIT_MS) {
            send(msg_new("cancel"));
            cancelled("%s didn't answer", t->v.label);
        } else if (now - t->last_sync > RESEND_MS) {
            cJSON *m = msg_new("invite");
            cJSON_AddStringToObject(m, "to", t->v.peer);
            send(m);
            t->last_sync = now;
        }
        break;
    case TRADE_OPEN:
    case TRADE_SIGNING:
        os_mesh_peer_t p;
        if (!os_mesh_peer(t->v.peer, &p)) {
            if (t->v.phase == TRADE_SIGNING) {
                unconfirmed("%s went out of range after you signed");
            } else {
                cancelled("%s is out of range", t->v.label);
            }
            break;
        }
        if (t->dirty && now - t->last_offer > OFFER_DEBOUNCE) {
            t->dirty = false;
            t->v.my_rev++;
            refresh_locks();
            send_offer();
        }
        if (now - t->last_sync > RESEND_MS) {
            t->last_sync = now;
            if (t->v.phase == TRADE_SIGNING) {
                send_terms();
            } else {
                if (t->v.my_rev) {
                    send_offer();
                }
                if (t->v.i_locked) {
                    send_lock();
                }
            }
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// App-facing API

int trade_invite_count(void)
{
    return s_ninv;
}

const trade_invite_t *trade_invite(int i)
{
    return i >= 0 && i < s_ninv ? &s_inv[i] : NULL;
}

static bool busy(void)
{
    return s_t && s_t->v.phase != TRADE_CANCELLED && s_t->v.phase != TRADE_DONE;
}

static const char *cant_trade(void)
{
    if (!game_claimed()) {
        return "Not linked yet: claim this badge first";
    }
    if (!game_can_sign()) {
        return "The badge isn't ready to trade yet";
    }
    if (busy()) {
        return "Finish this trade first";
    }
    return NULL;
}

// A trade id is used once: an invite naming one we already signed is stale or forged.
static bool tid_recorded(const char *tid)
{
    char path[96], want[32], line[256];
    os_data_path(path, sizeof(path), TRADES_FILE);
    snprintf(want, sizeof(want), "\"tid\":\"%s\"", tid);
    FILE *f = os_data_ready() ? fopen(path, "r") : NULL;
    bool found = false;
    while (f && !found && fgets(line, sizeof(line), f)) {
        found = strstr(line, want) != NULL;     // the tid leads each line
    }
    if (f) {
        fclose(f);
    }
    return found;
}

bool trade_join(const char *tid, const char **why)
{
    int i;
    for (i = 0; i < s_ninv && strcmp(s_inv[i].tid, tid); i++) {
    }
    if (i == s_ninv) {
        *why = "That invite expired";
        return false;
    }
    if ((*why = cant_trade())) {
        return false;
    }
    if (tid_recorded(tid)) {
        *why = "That trade already happened";
        return false;
    }
    os_mesh_peer_t p;
    if (!os_mesh_peer(s_inv[i].peer, &p)) {
        *why = "That badge is out of range";
        return false;
    }
    trade_invite_t inv = s_inv[i];
    memmove(&s_inv[i], &s_inv[i + 1], (s_ninv - i - 1) * sizeof(s_inv[0]));
    memmove(&s_inv_at[i], &s_inv_at[i + 1], (s_ninv - i - 1) * sizeof(s_inv_at[0]));
    s_ninv--;
    trade_t *t = start(inv.tid, inv.peer, inv.label, false);
    if (!t) {
        *why = "Out of memory";
        return false;
    }
    set_phase(TRADE_OPEN);
    t->last_sync = t->at;
    cJSON *m = msg_new("join");
    cJSON_AddStringToObject(m, "to", inv.peer);
    send(m);
    return true;
}

void trade_decline(const char *tid)
{
    for (int i = 0; i < s_ninv; i++) {
        if (!strcmp(s_inv[i].tid, tid)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "op", "decline");
            cJSON_AddStringToObject(m, "to", s_inv[i].peer);
            cJSON_AddStringToObject(m, "tid", tid);
            send_to(s_inv[i].peer, m);
            memmove(&s_inv[i], &s_inv[i + 1], (s_ninv - i - 1) * sizeof(s_inv[0]));
            memmove(&s_inv_at[i], &s_inv_at[i + 1], (s_ninv - i - 1) * sizeof(s_inv_at[0]));
            s_ninv--;
            return;
        }
    }
}

bool trade_invite_peer(const char *peer_id, const char **why)
{
    os_mesh_peer_t p;
    if ((*why = cant_trade())) {
        return false;
    }
    if (!os_mesh_peer(peer_id, &p) || p.kind != OS_PEER_BADGE) {
        *why = "That badge is out of range";
        return false;
    }
    if (!p.name[0]) {
        *why = "That badge isn't linked yet";
        return false;
    }
    char tid[17];
    os_mesh_nonce(tid);
    if (!start(tid, p.id, p.name, true)) {
        *why = "Out of memory";
        return false;
    }
    set_phase(TRADE_INVITING);
    s_t->last_sync = 0;             // the tick sends the invite
    trade_tick(game_now_ms());
    return true;
}

const trade_view_t *trade_current(void)
{
    return s_t ? &s_t->v : NULL;
}

void trade_dismiss(void)
{
    if (s_t && busy()) {
        trade_cancel();
    }
    if (s_t && s_t->v.phase == TRADE_SIGNING) {
        record(s_t);                    // committed: we already signed
    }
    free(s_t);
    s_t = NULL;
    game_expect_trade("");
}

void trade_cancel(void)
{
    if (!s_t || (s_t->v.phase != TRADE_INVITING && s_t->v.phase != TRADE_OPEN)) {
        return;
    }
    send(msg_new("cancel"));
    cancelled("You cancelled", NULL);
}

static bool editable(void)
{
    return s_t && s_t->v.phase == TRADE_OPEN;
}

static void edited(void)
{
    s_t->dirty = true;
    refresh_locks();
}

bool trade_set_lamps(int lamps)
{
    if (!editable() || lamps < 0 || lamps == s_t->v.mine.lamps) {
        return false;
    }
    s_t->v.mine.lamps = lamps;
    edited();
    return true;
}

int trade_count_of(bool page, const char *id)
{
    if (!s_t) {
        return 0;
    }
    const trade_bundle_t *b = &s_t->v.mine;
    int n = 0;
    for (int i = 0; i < (page ? b->npages : b->nitems); i++) {
        n += !strcmp((page ? b->pages : b->items)[i].id, id);
    }
    return n;
}

bool trade_add(bool page, const char *id, const char *name, const char **why)
{
    *why = NULL;
    if (!editable() || !good_id(id)) {
        *why = "Can't add that";
        return false;
    }
    trade_bundle_t *b = &s_t->v.mine;
    int *n = page ? &b->npages : &b->nitems;
    if (*n == (page ? TRADE_MAX_PAGES : TRADE_MAX_ITEMS)) {
        *why = page ? "Up to 6 pages per trade" : "Up to 6 items per trade";
        return false;
    }
    trade_thing_t *th = &(page ? b->pages : b->items)[(*n)++];
    strlcpy(th->id, id, sizeof(th->id));
    game_ascii(th->name, sizeof(th->name), name);
    edited();
    return true;
}

void trade_remove(bool page, int index)
{
    if (!editable()) {
        return;
    }
    trade_bundle_t *b = &s_t->v.mine;
    int *n = page ? &b->npages : &b->nitems;
    trade_thing_t *arr = page ? b->pages : b->items;
    if (index < 0 || index >= *n) {
        return;
    }
    memmove(&arr[index], &arr[index + 1], (*n - index - 1) * sizeof(arr[0]));
    (*n)--;
    edited();
}

void trade_flush(void)
{
    trade_t *t = s_t;
    if (editable() && t->dirty) {
        t->dirty = false;
        t->v.my_rev++;
        refresh_locks();
        send_offer();
    }
}

void trade_lock(bool on)
{
    trade_t *t = s_t;
    if (!editable() || on == t->v.i_locked) {
        return;
    }
    if (!on) {
        edited();                       // unlocking = a new revision of the same offer
        return;
    }
    trade_flush();                      // the peer must see this revision before the lock that names it
    t->my_lock_mine = t->v.my_rev;
    t->my_lock_theirs = t->v.their_rev;
    refresh_locks();
    send_lock();
}

// ---------------------------------------------------------------------------
// Records file: a full line per signed trade, then status lines.

int trade_records_load(trade_rec_t **out, int max)
{
    *out = NULL;
    char path[96];
    os_data_path(path, sizeof(path), TRADES_FILE);
    FILE *f = os_data_ready() ? fopen(path, "r") : NULL;
    if (!f) {
        return 0;
    }
    trade_rec_t *recs = calloc(max, sizeof(trade_rec_t));
    char *line = malloc(2048);
    int n = 0;
    while (recs && line && fgets(line, 2048, f)) {
        cJSON *j = cJSON_Parse(line);
        const char *tid = game_jstr(j, "tid");
        int i;
        for (i = 0; i < n && strcmp(recs[i].tid, tid); i++) {
        }
        const char *status = game_jstr(j, "status");
        if (status[0] && i < n) {
            recs[i].status = !strcmp(status, "settled") ? TRADE_REC_SETTLED : TRADE_REC_REJECTED;
            strlcpy(recs[i].reason, game_jstr(j, "reason"), sizeof(recs[i].reason));
        } else if (!status[0] && i == n && is_hex16(tid)) {
            if (n == max) {
                // Keep the newest: drop the oldest record.
                memmove(&recs[0], &recs[1], (max - 1) * sizeof(trade_rec_t));
                n--;
            }
            trade_rec_t *r = &recs[n++];
            memset(r, 0, sizeof(*r));
            strlcpy(r->tid, tid, sizeof(r->tid));
            strlcpy(r->label, game_jstr(j, "name"), sizeof(r->label));
            strlcpy(r->gave, game_jstr(j, "gave"), sizeof(r->gave));
            strlcpy(r->got, game_jstr(j, "got"), sizeof(r->got));
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "at");
            r->at = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
            r->status = game_jstr(j, "b")[0] ? TRADE_REC_PENDING : TRADE_REC_UNCONFIRMED;
        }
        cJSON_Delete(j);
    }
    fclose(f);
    free(line);
    if (!n) {
        free(recs);
        return 0;
    }
    // Newest first.
    for (int i = 0; i < n / 2; i++) {
        trade_rec_t tmp = recs[i];
        recs[i] = recs[n - 1 - i];
        recs[n - 1 - i] = tmp;
    }
    *out = recs;
    return n;
}

// ---------------------------------------------------------------------------
// Console: trade [status|invite <id>|join|lamps <n>|item <id>|page <id>|lock|unlock|sign|cancel|dismiss].
// Runs on the OS task like everything else here.

static void cmd_status(void)
{
    static const char *const phases[] = {"idle", "inviting", "open", "signing", "done", "cancelled"};
    const trade_t *t = s_t;
    if (t) {
        printf("trade %s with %s (%s): %s %s\n  mine rev %d: %d !LAMPs, %d items, %d pages%s\n"
               "  theirs rev %d: %d !LAMPs, %d items, %d pages%s\n",
               t->v.tid, t->v.peer, t->v.inviter ? "A" : "B", phases[t->v.phase], t->v.reason,
               t->v.my_rev, t->v.mine.lamps, t->v.mine.nitems, t->v.mine.npages, t->v.i_locked ? " LOCKED" : "",
               t->v.their_rev, t->v.theirs.lamps, t->v.theirs.nitems, t->v.theirs.npages,
               t->v.they_locked ? " LOCKED" : "");
        if (t->terms[0]) {
            printf("  terms %s\n", t->terms);
        }
    } else {
        printf("trade: idle\n");
    }
    for (int i = 0; i < s_ninv; i++) {
        printf("  invite %s from %s (%s)\n", s_inv[i].tid, s_inv[i].peer, s_inv[i].label);
    }
    static const char *const st[] = {"pending", "unconfirmed", "settled", "rejected"};
    trade_rec_t *recs;
    int n = trade_records_load(&recs, 20);
    for (int i = 0; i < n; i++) {
        printf("  %s %-11s %s: gave %s / got %s %s\n", recs[i].tid, st[recs[i].status], recs[i].label, recs[i].gave,
               recs[i].got, recs[i].reason);
    }
    free(recs);
}

static void cmd_run(void *arg)
{
    char *line = arg;
    char *op = strtok(line, " "), *val = strtok(NULL, " ");
    const char *why = NULL;
    if (!op || !strcmp(op, "status")) {
        cmd_status();
    } else if (!strcmp(op, "invite") && val) {
        trade_invite_peer(val, &why);
    } else if (!strcmp(op, "join") && s_ninv) {
        trade_join(s_inv[0].tid, &why);
    } else if (!strcmp(op, "cancel")) {
        trade_cancel();
    } else if (!strcmp(op, "dismiss")) {
        trade_dismiss();
    } else if (!strcmp(op, "lamps") && val) {
        trade_set_lamps(atoi(val));
    } else if (!strcmp(op, "lock") || !strcmp(op, "unlock")) {
        trade_lock(op[0] == 'l');
    } else if (!strcmp(op, "sign")) {
        trade_sign();
    } else if ((!strcmp(op, "page") || !strcmp(op, "item")) && val) {
        trade_add(op[0] == 'p', val, val, &why);
    } else {
        why = "usage: trade [status|invite <id>|join|lamps <n>|item <id>|page <id>|lock|unlock|sign|cancel|dismiss]";
    }
    if (why) {
        printf("trade: %s\n", why);
    }
    free(line);
}

static int cmd_trade(int argc, char **argv)
{
    char *line = malloc(64);
    if (line) {
        snprintf(line, 64, "%s %s", argc > 1 ? argv[1] : "status", argc > 2 ? argv[2] : "");
        if (!os_post(cmd_run, line)) {
            free(line);
        }
    }
    return 0;
}

void trade_init(void)
{
    s_poll_at = 15000;                  // first check shortly after boot
    os_cmd_register("trade", "trade [status|invite <id>|join|lamps <n>|item <id>|page <id>|lock|unlock|sign|cancel|dismiss]",
                    cmd_trade);
}
