// X Dex card swaps, see xdex.h and docs/MESH.md §2.
//
// B (requester)                     A (accepter)
//   req {nonce nB, cardB}  ------>    pending for 60 s, toast
//                          <------    ok {nonce nA, tag(kA, xdex|A|B|nB), cardA}
//   fin {tag(kB, xdex|B|A|nA)} --->
// Each side then stores the other's card and uploads it with the *other*
// badge's tag, which the server checks against that badge's mesh key.
//
// Frames can get lost, so B repeats "req" until it hears back, A repeats "ok"
// until "fin" arrives, and B answers a repeated "ok" with the same "fin".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "xdex.h"

static const char *TAG = "xdex";

#define CARDS_FILE      "xdex.jsonl"
#define RESEND_MS       4000        // req / ok repeat
#define OK_TRIES        3
#define RAM_CARDS       8           // without a microSD card
#define RESULT_SHOW_MS  8000

static xdex_req_t s_pending[XDEX_PENDING_MAX];
static int s_npending;

static xdex_swap_t s_last;
static char s_out_nonce[17];        // our nonce while asking
static int64_t s_out_sent;          // last req send
static char s_fin_peer[OS_MESH_ID_LEN + 1], s_fin_nonce[17], s_fin_tag[OS_MESH_TAG_LEN + 1];

static xdex_card_t *s_ram;          // collected cards when there's no SD card
static int s_nram;

// ---------------------------------------------------------------------------
// Helpers

static bool is_hex(const char *s, size_t len)
{
    if (strlen(s) != len) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void send(const char *to, cJSON *msg)
{
    char *json = cJSON_PrintUnformatted(msg);
    if (json) {
        esp_err_t err = os_mesh_send(OS_MESH_XDEX, to, json);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send to %s: %s", to, esp_err_to_name(err));
        }
        cJSON_free(json);
    }
    cJSON_Delete(msg);
}

static cJSON *msg_new(const char *op, const char *to)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "op", op);
    cJSON_AddStringToObject(m, "to", to);
    return m;
}

static void card_from_json(xdex_card_t *c, const char *id, const cJSON *card)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->id, id, sizeof(c->id));
    game_ascii(c->n, sizeof(c->n), game_jstr(card, "n"));
    game_ascii(c->x, sizeof(c->x), game_jstr(card, "x"));
    c->lv = game_jint(card, "lv", 0);
    c->bounty = -1;
}

static cJSON *card_to_json(const xdex_card_t *c)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "n", c->n);
    cJSON_AddStringToObject(j, "x", c->x);
    cJSON_AddNumberToObject(j, "lv", c->lv);
    return j;
}

static void set_phase(xdex_phase_t phase, const char *peer, const char *label)
{
    s_last.phase = phase;
    s_last.at = game_now_ms();
    if (peer) {
        strlcpy(s_last.peer, peer, sizeof(s_last.peer));
    }
    if (label) {
        game_peer_label(label, s_last.peer, s_last.label, sizeof(s_last.label));
    }
}

// ---------------------------------------------------------------------------
// Storing and uploading a collected card

static void store_card(xdex_card_t *c)
{
    c->at = os_cloud_time_ms();
    cJSON *j = card_to_json(c);
    cJSON_AddStringToObject(j, "badge", c->id);
    cJSON_AddNumberToObject(j, "at", (double)c->at);
    char *line = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    bool saved = line && os_data_append(CARDS_FILE, line);
    cJSON_free(line);
    if (saved) {
        return;
    }
    // No card (or it failed): keep the newest few in RAM until reboot.
    if (!s_ram && !(s_ram = calloc(RAM_CARDS, sizeof(xdex_card_t)))) {
        return;
    }
    for (int i = 0; i < s_nram; i++) {
        if (!strcmp(s_ram[i].id, c->id)) {
            s_ram[i] = *c;
            return;
        }
    }
    if (s_nram == RAM_CARDS) {
        memmove(s_ram, s_ram + 1, (RAM_CARDS - 1) * sizeof(xdex_card_t));
        s_nram--;
    }
    s_ram[s_nram++] = *c;
}

typedef struct {
    char peer[OS_MESH_ID_LEN + 1];
    char body[];
} upload_t;

static void toast_result(void)
{
    switch (s_last.upload) {
    case XDEX_UP_DONE:
        if (s_last.bounty > 0) {
            os_toast("%s's card: +%d !LAMPs", s_last.label, s_last.bounty);
        } else {
            os_toast("%s's card updated", s_last.label);
        }
        break;
    case XDEX_UP_QUEUED:
        os_toast("Got %s's card. Saved; uploads when Wi-Fi is on", s_last.label);
        break;
    case XDEX_UP_REFUSED:
        os_toast("%s's card: %s", s_last.label, s_last.error);
        break;
    default:
        break;
    }
}

static void uploaded(int status, const char *body, void *ctx)
{
    upload_t *u = ctx;
    bool mine = !strcmp(u->peer, s_last.peer) && s_last.upload == XDEX_UP_SENDING;
    if (status == 0 || status >= 500) {
        os_outbox_add("/api/device/xdex", u->body);
        if (mine) {
            s_last.upload = XDEX_UP_QUEUED;
            toast_result();
        }
        free(u);
        return;
    }
    cJSON *j = cJSON_Parse(body);
    if (status / 100 == 2) {
        cJSON *res = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "results"), 0);
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
            os_cloud_note_lamps(game_jint(j, "lamps", in->lamps), game_jint(j, "level", in->level));
            free(in);
        }
        if (mine) {
            bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "ok"));
            s_last.upload = ok ? XDEX_UP_DONE : XDEX_UP_REFUSED;
            s_last.bounty = game_jint(res, "bounty", 0);
            s_last.fresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "new"));
            strlcpy(s_last.error, ok ? "" : game_jstr(res, "error"), sizeof(s_last.error));
            if (!ok && !s_last.error[0]) {
                strlcpy(s_last.error, "not accepted", sizeof(s_last.error));
            }
        }
    } else if (mine) {
        s_last.upload = XDEX_UP_REFUSED;
        const char *err = game_jstr(j, "error");
        strlcpy(s_last.error, err[0] ? err : "the server said no", sizeof(s_last.error));
    }
    cJSON_Delete(j);
    if (mine) {
        toast_result();
    }
    free(u);
}

// POST /api/device/xdex {entries:[{peer, nonce, tag, card}]}: right away when
// online so the bounty shows, else (or on a network error) via the outbox.
static void upload(const xdex_card_t *c, const char *nonce, const char *tag)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "peer", c->id);
    cJSON_AddStringToObject(e, "nonce", nonce);
    cJSON_AddStringToObject(e, "tag", tag);
    cJSON_AddItemToObject(e, "card", card_to_json(c));
    cJSON_AddItemToArray(entries, e);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return;
    }
    s_last.upload = XDEX_UP_QUEUED;
    if (os_api_online()) {
        size_t len = strlen(body);
        upload_t *u = malloc(sizeof(upload_t) + len + 1);
        if (u) {
            strlcpy(u->peer, c->id, sizeof(u->peer));
            memcpy(u->body, body, len + 1);
            if (os_api_call("POST", "/api/device/xdex", body, 0, uploaded, u) == ESP_OK) {
                s_last.upload = XDEX_UP_SENDING;
            } else {
                free(u);
            }
        }
    }
    if (s_last.upload == XDEX_UP_QUEUED) {
        if (os_outbox_add("/api/device/xdex", body) != ESP_OK) {
            s_last.upload = XDEX_UP_REFUSED;
            strlcpy(s_last.error, "outbox full", sizeof(s_last.error));
        }
        toast_result();
    }
    cJSON_free(body);
}

static void swapped(xdex_card_t *c, const char *nonce, const char *tag)
{
    store_card(c);
    set_phase(XDEX_SWAPPED, c->id, c->n);
    s_last.bounty = 0;
    s_last.fresh = false;
    s_last.error[0] = 0;
    game_led_flash(COL_PINK);
    ESP_LOGI(TAG, "collected %s (%s)", c->id, c->n);
    upload(c, nonce, tag);
}

// ---------------------------------------------------------------------------
// Pending requests (we are A)

static int find_pending(const char *id)
{
    for (int i = 0; i < s_npending; i++) {
        if (!strcmp(s_pending[i].card.id, id)) {
            return i;
        }
    }
    return -1;
}

static void drop_pending(int i)
{
    memmove(&s_pending[i], &s_pending[i + 1], (s_npending - i - 1) * sizeof(xdex_req_t));
    s_npending--;
}

int xdex_pending_count(void)
{
    return s_npending;
}

const xdex_req_t *xdex_pending(int i)
{
    return i >= 0 && i < s_npending ? &s_pending[i] : NULL;
}

static void send_ok(xdex_req_t *r)
{
    char msg[64], tag[OS_MESH_TAG_LEN + 1];
    snprintf(msg, sizeof(msg), "xdex|%s|%s|%s", game_badge_id(), r->card.id, r->nonce);
    if (!os_mesh_sign(msg, tag)) {
        return;
    }
    cJSON *m = msg_new("ok", r->card.id);
    cJSON_AddStringToObject(m, "nonce", r->mine);
    cJSON_AddStringToObject(m, "tag", tag);
    cJSON *card = cJSON_AddObjectToObject(m, "card");
    game_add_card(card);
    send(r->card.id, m);
    r->tries++;
    r->at = game_now_ms();
}

void xdex_accept(const char *peer_id)
{
    int i = find_pending(peer_id);
    if (i < 0 || s_pending[i].accepted) {
        return;
    }
    if (!game_can_sign()) {
        os_toast("The badge isn't ready to swap yet");
        return;
    }
    xdex_req_t *r = &s_pending[i];
    r->accepted = true;
    r->tries = 0;
    os_mesh_nonce(r->mine);
    set_phase(XDEX_FINISHING, r->card.id, r->card.n);
    s_last.upload = XDEX_UP_NONE;
    send_ok(r);
}

void xdex_decline(const char *peer_id)
{
    int i = find_pending(peer_id);
    if (i < 0) {
        return;
    }
    send(peer_id, msg_new("no", peer_id));
    drop_pending(i);
}

static void on_req(const os_mesh_peer_t *from, const cJSON *msg)
{
    const char *nonce = game_jstr(msg, "nonce");
    const cJSON *card = cJSON_GetObjectItemCaseSensitive(msg, "card");
    if (!is_hex(nonce, 16) || !cJSON_IsObject(card)) {
        return;
    }
    int i = find_pending(from->id);
    if (i >= 0 && s_pending[i].accepted) {
        return;                         // a repeat of the one we're finishing
    }
    bool fresh = i < 0;
    if (fresh) {
        if (s_npending == XDEX_PENDING_MAX) {
            drop_pending(XDEX_PENDING_MAX - 1);     // the oldest
        }
        memmove(&s_pending[1], &s_pending[0], s_npending * sizeof(xdex_req_t));
        s_npending++;
        i = 0;
    }
    xdex_req_t *r = &s_pending[i];
    memset(r, 0, sizeof(*r));
    card_from_json(&r->card, from->id, card);
    strlcpy(r->nonce, nonce, sizeof(r->nonce));
    r->at = game_now_ms();
    if (fresh) {
        char who[GAME_CARD_NAME + 8];
        game_peer_label(r->card.n, r->card.id, who, sizeof(who));
        os_toast("%s wants to swap cards", who);
    }
}

static void on_fin(const os_mesh_peer_t *from, const cJSON *msg)
{
    int i = find_pending(from->id);
    const char *tag = game_jstr(msg, "tag");
    if (i < 0 || !s_pending[i].accepted || !is_hex(tag, OS_MESH_TAG_LEN)) {
        return;
    }
    xdex_req_t r = s_pending[i];
    drop_pending(i);
    swapped(&r.card, r.mine, tag);
}

// ---------------------------------------------------------------------------
// Our request (we are B)

bool xdex_request(const char *peer_id, const char **why)
{
    os_mesh_peer_t p;
    *why = NULL;
    if (!game_claimed()) {
        *why = "Not linked yet: claim this badge first";
    } else if (!game_can_sign()) {
        *why = "The badge isn't ready to swap yet";
    } else if (!os_mesh_peer(peer_id, &p) || p.kind != OS_PEER_BADGE) {
        *why = "That badge is out of range";
    } else if (!p.name[0]) {
        *why = "That badge isn't linked yet";
    } else if (s_last.phase == XDEX_ASKING || s_last.phase == XDEX_FINISHING) {
        *why = "One swap at a time";
    }
    if (*why) {
        return false;
    }
    os_mesh_nonce(s_out_nonce);
    set_phase(XDEX_ASKING, p.id, p.name);
    s_last.upload = XDEX_UP_NONE;
    s_out_sent = 0;                     // the tick sends it
    xdex_tick(game_now_ms());
    return true;
}

static void send_req(void)
{
    cJSON *m = msg_new("req", s_last.peer);
    cJSON_AddStringToObject(m, "nonce", s_out_nonce);
    cJSON *card = cJSON_AddObjectToObject(m, "card");
    game_add_card(card);
    send(s_last.peer, m);
    s_out_sent = game_now_ms();
}

static void send_fin(void)
{
    cJSON *m = msg_new("fin", s_fin_peer);
    cJSON_AddStringToObject(m, "tag", s_fin_tag);
    send(s_fin_peer, m);
}

static void on_ok(const os_mesh_peer_t *from, const cJSON *msg)
{
    const char *nonce = game_jstr(msg, "nonce");
    const char *tag = game_jstr(msg, "tag");
    const cJSON *card = cJSON_GetObjectItemCaseSensitive(msg, "card");
    if (!is_hex(nonce, 16) || !is_hex(tag, OS_MESH_TAG_LEN) || !cJSON_IsObject(card)) {
        return;
    }
    if (!strcmp(from->id, s_fin_peer) && !strcmp(nonce, s_fin_nonce)) {
        send_fin();                     // our fin got lost: say it again
        return;
    }
    if (s_last.phase != XDEX_ASKING || strcmp(from->id, s_last.peer)) {
        return;
    }
    char m[64];
    snprintf(m, sizeof(m), "xdex|%s|%s|%s", game_badge_id(), from->id, nonce);
    if (!os_mesh_sign(m, s_fin_tag)) {
        return;
    }
    strlcpy(s_fin_peer, from->id, sizeof(s_fin_peer));
    strlcpy(s_fin_nonce, nonce, sizeof(s_fin_nonce));
    send_fin();
    xdex_card_t c;
    card_from_json(&c, from->id, card);
    swapped(&c, s_out_nonce, tag);
}

static void on_no(const os_mesh_peer_t *from)
{
    if (s_last.phase == XDEX_ASKING && !strcmp(from->id, s_last.peer)) {
        set_phase(XDEX_DECLINED, NULL, NULL);
    }
}

// ---------------------------------------------------------------------------

void xdex_on_mesh(const os_mesh_peer_t *from, cJSON *msg)
{
    const char *op = game_jstr(msg, "op");
    if (from->kind != OS_PEER_BADGE || strcmp(game_jstr(msg, "to"), game_badge_id()) || !game_badge_id()[0]) {
        return;
    }
    if (!strcmp(op, "req")) {
        on_req(from, msg);
    } else if (!strcmp(op, "ok")) {
        on_ok(from, msg);
    } else if (!strcmp(op, "no")) {
        on_no(from);
    } else if (!strcmp(op, "fin")) {
        on_fin(from, msg);
    }
}

void xdex_tick(int64_t now)
{
    for (int i = s_npending - 1; i >= 0; i--) {
        xdex_req_t *r = &s_pending[i];
        if (!r->accepted) {
            if (now - r->at > XDEX_REQ_TTL_MS) {
                drop_pending(i);
            }
        } else if (now - r->at > RESEND_MS) {
            if (r->tries < OK_TRIES) {
                send_ok(r);
            } else {
                if (s_last.phase == XDEX_FINISHING && !strcmp(s_last.peer, r->card.id)) {
                    set_phase(XDEX_FAILED, NULL, NULL);
                }
                drop_pending(i);
            }
        }
    }
    if (s_last.phase == XDEX_ASKING) {
        if (now - s_last.at > XDEX_REQ_TTL_MS) {
            set_phase(XDEX_NO_ANSWER, NULL, NULL);
        } else if (!s_out_sent || now - s_out_sent > RESEND_MS) {
            send_req();
        }
    }
    // Finished swaps fade back to idle once their result has been seen.
    bool done = s_last.phase == XDEX_DECLINED || s_last.phase == XDEX_NO_ANSWER || s_last.phase == XDEX_FAILED
             || (s_last.phase == XDEX_SWAPPED && s_last.upload != XDEX_UP_SENDING);
    if (done && now - s_last.at > RESULT_SHOW_MS) {
        s_last.phase = XDEX_IDLE;
    }
}

const xdex_swap_t *xdex_last(void)
{
    return &s_last;
}

// ---------------------------------------------------------------------------
// Collected cards

static void add_card(xdex_card_t *out, int *n, int max, const xdex_card_t *c)
{
    for (int i = 0; i < *n; i++) {
        if (!strcmp(out[i].id, c->id)) {
            memmove(&out[1], &out[0], i * sizeof(xdex_card_t));     // newest first
            out[0] = *c;
            return;
        }
    }
    int keep = *n < max ? *n : max - 1;
    memmove(&out[1], &out[0], keep * sizeof(xdex_card_t));
    out[0] = *c;
    *n = keep + 1;
}

int xdex_cards_load(xdex_card_t **out, int max)
{
    *out = NULL;
    xdex_card_t *cards = calloc(max, sizeof(xdex_card_t));
    if (!cards) {
        return 0;
    }
    int n = 0;
    char path[96];
    os_data_path(path, sizeof(path), CARDS_FILE);
    FILE *f = os_data_ready() ? fopen(path, "r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            cJSON *j = cJSON_Parse(line);
            const char *id = game_jstr(j, "badge");
            if (id[0]) {
                xdex_card_t c;
                card_from_json(&c, id, j);
                const cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "at");
                c.at = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
                add_card(cards, &n, max, &c);
            }
            cJSON_Delete(j);
        }
        fclose(f);
    }
    for (int i = 0; i < s_nram; i++) {
        add_card(cards, &n, max, &s_ram[i]);
    }
    if (!n) {
        free(cards);
        return 0;
    }
    *out = cards;
    return n;
}

// ---------------------------------------------------------------------------
// Console: xdex [status|req <id>|accept <id>|no <id>]. Runs on the OS task
// like everything else here.

static void cmd_run(void *arg)
{
    static const char *const phases[] = {"idle", "asking", "finishing", "declined", "no answer", "failed", "swapped"};
    static const char *const uploads[] = {"-", "sending", "done", "queued", "refused"};
    char *line = arg;
    char *op = strtok(line, " "), *id = strtok(NULL, " ");
    const char *why = NULL;
    if (op && id && !strcmp(op, "req")) {
        xdex_request(id, &why);
    } else if (op && id && !strcmp(op, "accept")) {
        xdex_accept(id);
    } else if (op && id && !strcmp(op, "no")) {
        xdex_decline(id);
    } else if (op && strcmp(op, "status")) {
        why = "usage: xdex [status|req <id>|accept <id>|no <id>]";
    }
    if (why) {
        printf("xdex: %s\n", why);
    }
    printf("xdex: %s %s upload=%s bounty=%d %s\n", phases[s_last.phase], s_last.peer, uploads[s_last.upload],
           s_last.bounty, s_last.error);
    for (int i = 0; i < s_npending; i++) {
        printf("  pending %s '%s' lv%d%s\n", s_pending[i].card.id, s_pending[i].card.n, s_pending[i].card.lv,
               s_pending[i].accepted ? " (accepted)" : "");
    }
    free(line);
}

static int cmd_xdex(int argc, char **argv)
{
    char *line = malloc(40);
    if (line) {
        snprintf(line, 40, "%s %s", argc > 1 ? argv[1] : "status", argc > 2 ? argv[2] : "");
        if (!os_post(cmd_run, line)) {
            free(line);
        }
    }
    return 0;
}

void xdex_init(void)
{
    os_cmd_register("xdex", "xdex [status|req <id>|accept <id>|no <id>]", cmd_xdex);
}
