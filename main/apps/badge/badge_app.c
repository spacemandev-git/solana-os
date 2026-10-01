// My Badge: this badge's identity on the server (core/os_cloud). While the
// badge is unclaimed it shows a QR code of the claim link, so the owner can
// scan it with a phone and link the badge to their account (or type its ID
// and claim code into <host>/claim). Once claimed it
// shows the owner, !LAMPs, Level, pages and unopened packs.
//
// The cloud task does the hello; this app renders snapshots of
// os_cloud_get_info() (kept in NVS, so the claimed view works offline) and,
// when online, adds GET /api/device/me (pages, next bounty) and the first
// unopened pack's name, cached in /sdcard/hh/cache/me.json.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#define CONTENT_H   (OS_ROOT_H - UI_HINT_H)
#define QR_BOX      164         // square behind the code: its quiet zone
#define QR_SIZE     148         // canvas; lv_qrcode centres the code at an integer scale
#define COL_X       (10 + QR_BOX + 12)
#define COL_W       (OS_ROOT_W - COL_X - 8)
#define TILE_W      74
#define TILE_GAP    6
#define CACHE       "cache/me.json"
#define CACHE_MAX   1024

typedef enum { V_NONE, V_OFFLINE, V_NO_SERVER, V_CONNECTING, V_UNCLAIMED, V_CLAIMED, V_ERROR, V_LINKED } view_t;

// GET /api/device/me (+ the first unopened pack), live or from the cache.
typedef struct {
    int owned, total;           // pages; total 0 = unknown
    int next_bounty;            // 0 = unknown
    bool needs_x;               // X Dex bounties wait for the owner to link X
    char pack[40];              // first unopened pack's name, "" = unknown
    int64_t at;                 // server time of the data (unix ms), 0 = unknown
    bool fetching;
} me_t;

static lv_obj_t *s_root, *s_view, *s_hint, *s_live, *s_offline;
static view_t s_shown;
static os_cloud_info_t *s_last;         // what s_view was built from
static me_t s_me, s_me_shown;
static ui_net_t s_net_shown;
static bool s_asked;                    // fetched /me since the view opened (or since SELECT)

// "https://host:port/path" -> "host:port"
static void host_of(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t len = strcspn(p, "/?#");
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), p);
}

// The claim link's code ("...&c=ABCD2345"), shown as "ABCD-2345" for typing.
static void claim_code(const char *url, char *out, size_t n)
{
    const char *q = strchr(url, '?');
    const char *c = q ? strstr(q, "c=") : NULL;
    while (c && c[-1] != '?' && c[-1] != '&') {
        c = strstr(c + 1, "c=");
    }
    size_t len = c ? strcspn(c + 2, "&#") : 0;
    if (len == 8) {
        snprintf(out, n, "%.4s-%.4s", c + 2, c + 6);
    } else {
        snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), c ? c + 2 : "");
    }
}

// "https://host/claim?b=..." -> "host/claim"
static void claim_page(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t len = strcspn(p, "?#");
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), p);
}

static lv_obj_t *label_at(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text, int x, int y, int w)
{
    lv_obj_t *l = ui_label(parent, font, color, text);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    return l;
}

// Round badge with an icon (or a spinner), then a centred title and body.
static void message(const char *symbol, uint32_t color, const char *title, const char *body)
{
    lv_obj_t *ring;
    if (symbol) {
        ring = lv_obj_create(s_view);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 48, 48);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ring, ui_col(COL_PANEL_HI), 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, ui_col(color), 0);
        lv_obj_t *icon = ui_label(ring, &lv_font_montserrat_20, color, symbol);
        lv_obj_center(icon);
    } else {
        ring = lv_spinner_create(s_view);
        lv_obj_set_size(ring, 48, 48);
        lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring, ui_col(COL_PANEL_HI), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring, ui_col(color), LV_PART_INDICATOR);
    }
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *t = ui_label(s_view, &sol_font_20, COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_t *b = ui_label(s_view, &sol_font_14, COL_DIM, body);
    lv_obj_set_width(b, OS_ROOT_W - 40);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 112);
}

// One-line mono footnote (server, badge ID, retry timer, sync state).
// row 0 is the bottom line, row 1 the one above it.
static lv_obj_t *footnote(int row, uint32_t color, const char *text)
{
    lv_obj_t *l = ui_label(s_view, &sol_mono_12, color, text);
    lv_obj_set_size(l, OS_ROOT_W - 20, lv_font_get_line_height(&sol_mono_12));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, CONTENT_H - 24 - row * 16);
    return l;
}

// ---------------------------------------------------------------------------
// /api/device/me and its cache

static void cache_load(void)
{
    char *raw = os_data_read(CACHE, CACHE_MAX, NULL);
    cJSON *j = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!j) {
        return;
    }
    const cJSON *pages = cJSON_GetObjectItem(j, "pages");
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(pages, "owned"))) {
        s_me.owned = v->valueint;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(pages, "total"))) {
        s_me.total = v->valueint;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "nextBounty"))) {
        s_me.next_bounty = v->valueint;
    }
    s_me.needs_x = cJSON_IsTrue(cJSON_GetObjectItem(j, "needsX"));
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "time"))) {
        s_me.at = (int64_t)v->valuedouble;
    }
    const char *pack = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pack"));
    strlcpy(s_me.pack, pack ? pack : "", sizeof(s_me.pack));
    cJSON_Delete(j);
}

static void cache_save(void)
{
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"pages\":{\"owned\":%d,\"total\":%d},\"nextBounty\":%d,\"needsX\":%s,\"pack\":\"%s\",\"time\":%lld}",
             s_me.owned, s_me.total, s_me.next_bounty, s_me.needs_x ? "true" : "false", s_me.pack, (long long)s_me.at);
    os_data_write(CACHE, buf, strlen(buf));
}

// The pack's name goes into JSON and onto the screen: printable ASCII, no quotes.
static void clean_name(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p < 0x7F && *p != '"' && *p != '\\') {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

static void on_packs(int status, const char *body, void *ctx)
{
    s_me.fetching = false;
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "items"), 0);
    clean_name(s_me.pack, sizeof(s_me.pack), cJSON_GetStringValue(cJSON_GetObjectItem(first, "name")));
    if (j) {
        os_cloud_note_packs(cJSON_GetArraySize(cJSON_GetObjectItem(j, "items")));
    }
    cJSON_Delete(j);
    cache_save();
}

static void on_me(int status, const char *body, void *ctx)
{
    s_me.fetching = false;
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    if (!j) {
        if (status && status != 200) {
            os_toast("The server answered %d", status);
        }
        return;
    }
    const cJSON *pages = cJSON_GetObjectItem(j, "pages");
    const cJSON *lamps = cJSON_GetObjectItem(j, "lamps"), *level = cJSON_GetObjectItem(j, "level");
    const cJSON *packs = cJSON_GetObjectItem(j, "packs"), *v;
    s_me.owned = cJSON_IsNumber(v = cJSON_GetObjectItem(pages, "owned")) ? v->valueint : 0;
    s_me.total = cJSON_IsNumber(v = cJSON_GetObjectItem(pages, "total")) ? v->valueint : 0;
    s_me.next_bounty = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "nextBounty")) ? v->valueint : 0;
    s_me.needs_x = cJSON_IsTrue(cJSON_GetObjectItem(j, "needsX"));
    s_me.at = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "time")) ? (int64_t)v->valuedouble : os_cloud_time_ms();
    if (cJSON_IsNumber(lamps) && cJSON_IsNumber(level)) {
        os_cloud_note_lamps(lamps->valueint, level->valueint);
    }
    int n = cJSON_IsNumber(packs) ? packs->valueint : 0;
    os_cloud_note_packs(n);
    cJSON_Delete(j);
    if (n > 0 && os_api_call("GET", "/api/device/packs", NULL, 0, on_packs, &s_me) == ESP_OK) {
        s_me.fetching = true;
        return;
    }
    if (!n) {
        s_me.pack[0] = 0;
    }
    cache_save();
}

static void fetch_me(void)
{
    if (!s_me.fetching && os_api_call("GET", "/api/device/me", NULL, 0, on_me, &s_me) == ESP_OK) {
        s_me.fetching = true;
        s_asked = true;
    }
}

// ---------------------------------------------------------------------------
// Views

static void build_unclaimed(const os_cloud_info_t *in)
{
    // The code follows the theme: white modules on black in Dark (default),
    // classic black on white in Light, for scanners that can't read inverted
    // codes.
    bool light = ui_light();
    lv_color_t fg = light ? lv_color_black() : lv_color_white();
    lv_color_t bg = light ? lv_color_white() : lv_color_black();

    lv_obj_t *box = lv_obj_create(s_view);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, QR_BOX, QR_BOX);
    lv_obj_set_pos(box, 10, (CONTENT_H - QR_BOX) / 2);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, bg, 0);
    lv_obj_set_style_radius(box, 8, 0);
    if (!light) {
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, ui_col(COL_BORDER), 0);
    }
    lv_obj_set_scrollable(box, false);

    lv_obj_t *qr = lv_qrcode_create(box);
    lv_qrcode_set_size(qr, QR_SIZE);
    lv_qrcode_set_dark_color(qr, fg);
    lv_qrcode_set_light_color(qr, bg);
    if (lv_qrcode_update(qr, in->claim_url, strlen(in->claim_url)) != LV_RESULT_OK) {
        lv_obj_delete(qr);
        lv_obj_t *l = label_at(box, &sol_font_14, light ? COL_BG : COL_TEXT, "Link too long\nfor a QR code", 0, 0, 0);
        lv_obj_center(l);
    } else {
        lv_obj_center(qr);
    }

    // The ID and claim code are also typed in by hand at <host>/claim, for a
    // phone that can't scan the code.
    char code[16], url[96], line[128];
    claim_code(in->claim_url, code, sizeof(code));
    claim_page(in->claim_url, url, sizeof(url));
    int y = (CONTENT_H - QR_BOX) / 2 + 2;
    label_at(s_view, &sol_mono_12, COL_DIM, "BADGE ID", COL_X, y, 0);
    lv_obj_t *id = label_at(s_view, &sol_font_32, COL_TEXT, in->id, COL_X, y + 12, 0);
    lv_obj_update_layout(id);
    if (lv_obj_get_width(id) > COL_W) {
        lv_obj_set_style_text_font(id, &sol_font_20, 0);
    }
    label_at(s_view, &sol_mono_12, COL_DIM, "CLAIM CODE", COL_X, y + 48, 0);
    lv_obj_t *c = label_at(s_view, &sol_font_20, COL_TEXT, code, COL_X, y + 61, 0);
    lv_obj_update_layout(c);
    if (lv_obj_get_width(c) > COL_W) {
        lv_obj_set_style_text_font(c, &sol_font_14, 0);
    }
    ui_gradient_bar(s_view, COL_X, y + 89, COL_W, 3);
    label_at(s_view, &sol_font_14, COL_TEXT, "Scan to link", COL_X, y + 98, COL_W);
    snprintf(line, sizeof(line), "or enter both at %s", url);
    lv_obj_t *how = label_at(s_view, &sol_mono_12, COL_DIM, line, COL_X, y + 117, COL_W);
    // A long host (a run.app URL) can run past the bottom: drop the lead-in.
    lv_obj_update_layout(how);
    if (lv_obj_get_y(how) + lv_obj_get_height(how) > CONTENT_H) {
        lv_label_set_text(how, url);
    }
}

static void tile(int i, const char *value, const char *name, uint32_t color)
{
    lv_obj_t *t = ui_panel(s_view, 8 + i * (TILE_W + TILE_GAP), 74, TILE_W, 54);
    lv_obj_set_style_pad_all(t, 4, 0);
    lv_obj_t *v = ui_label(t, &sol_font_20, color, value);
    lv_obj_set_width(v, TILE_W - 10);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *l = ui_label(t, &sol_mono_12, COL_DIM, name);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -1);
}

// "synced 5 min ago  *  host" or the offline state; updated live.
static void sync_line(char *out, size_t n, const os_cloud_info_t *in)
{
    char ago[24], host[64];
    host_of(in->server, host, sizeof(host));
    int64_t now = os_cloud_time_ms();
    if (s_me.fetching) {
        snprintf(out, n, "syncing...  " LV_SYMBOL_BULLET "  %s", host);
    } else if (os_api_online()) {
        if (now && s_me.at) {
            snprintf(out, n, "synced %s  " LV_SYMBOL_BULLET "  %s", ui_fmt_ago(ago, sizeof(ago), now - s_me.at), host);
        } else {
            snprintf(out, n, "online  " LV_SYMBOL_BULLET "  %s", host);
        }
    } else if (os_wifi_enabled()) {
        snprintf(out, n, "connecting...  " LV_SYMBOL_BULLET "  saved data");
    } else {
        snprintf(out, n, "Wi-Fi is off  " LV_SYMBOL_BULLET "  saved data");
    }
}

static void build_claimed(const os_cloud_info_t *in)
{
    char buf[48], val[24];
    lv_obj_t *who = ui_label(s_view, &sol_font_32, COL_TEXT, in->owner[0] ? in->owner : "Linked");
    lv_obj_update_layout(who);
    if (lv_obj_get_width(who) > OS_ROOT_W - 24) {
        lv_obj_set_style_text_font(who, &sol_font_20, 0);
        lv_obj_set_width(who, OS_ROOT_W - 24);
        lv_label_set_long_mode(who, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *id = ui_label(s_view, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(id, "BADGE %s  " LV_SYMBOL_BULLET "  Lv %d", in->id, in->level);
    lv_obj_align(id, LV_ALIGN_TOP_MID, 0, 48);
    ui_gradient_bar(s_view, (OS_ROOT_W - 160) / 2, 64, 160, 3);

    tile(0, ui_fmt_num(val, sizeof(val), in->lamps), "!LAMPs", COL_LAMPS);
    snprintf(buf, sizeof(buf), "%d", in->level);
    tile(1, buf, "Level", COL_TEXT);
    if (s_me.total) {
        snprintf(buf, sizeof(buf), "%d/%d", s_me.owned, s_me.total);
    } else {
        strlcpy(buf, "-", sizeof(buf));
    }
    tile(2, buf, "Pages", COL_TEXT);
    snprintf(buf, sizeof(buf), "%d", in->packs);
    tile(3, buf, "Packs", in->packs ? COL_GREEN : COL_TEXT);

    lv_obj_t *action;
    if (in->welcome_needs_x && !in->packs) {
        // The welcome (Starter Pack + !LAMPs) waits for a linked X account.
        char host[64];
        host_of(in->server, host, sizeof(host));
        action = ui_label(s_view, &sol_mono_12, COL_YELLOW, "");
        lv_label_set_text_fmt(action, "Link X at %s/profile to get your Starter Pack and 100 !LAMPs", host);
        lv_obj_set_width(action, OS_ROOT_W - 16);
        lv_label_set_long_mode(action, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(action, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(action, LV_ALIGN_TOP_MID, 0, 132);
    } else if (in->packs > 0) {
        action = ui_panel(s_view, 8, 138, OS_ROOT_W - 16, 30);
        lv_obj_set_style_border_color(action, ui_col(COL_GREEN), 0);
        lv_obj_set_style_pad_ver(action, 5, 0);
        lv_obj_t *l = ui_label(action, &sol_font_14, COL_TEXT, "");
        lv_label_set_text_fmt(l, "SELECT: open your %s", s_me.pack[0] ? s_me.pack : in->packs > 1 ? "packs" : "pack");
        lv_obj_center(l);
    } else if (s_me.needs_x) {
        // Bounties only pay when both owners have linked X (server/API.md).
        action = ui_label(s_view, &sol_font_14, COL_YELLOW, "Link X on the web to earn bounties");
        lv_obj_align(action, LV_ALIGN_TOP_MID, 0, 144);
    } else if (s_me.next_bounty) {
        char amount[32];
        action = ui_label(s_view, &sol_font_14, COL_DIM, "");
        lv_label_set_text_fmt(action, "Next X Dex bounty: %s", ui_fmt_lamps(amount, sizeof(amount), s_me.next_bounty));
        lv_obj_align(action, LV_ALIGN_TOP_MID, 0, 144);
    }
    char line[112];
    sync_line(line, sizeof(line), in);
    s_live = footnote(0, COL_DIM, line);
}

static view_t view_for(const os_cloud_info_t *in)
{
    if (!in->server[0]) {
        return V_NO_SERVER;
    }
    if (in->state == OS_CLOUD_LINKED) {
        return V_LINKED;
    }
    if (in->claimed) {
        return V_CLAIMED;           // works offline: os_cloud keeps the figures in NVS
    }
    switch (in->state) {
    case OS_CLOUD_UNCLAIMED:  return V_UNCLAIMED;
    case OS_CLOUD_ERROR:      return V_ERROR;
    case OS_CLOUD_CONNECTING: return V_CONNECTING;
    default:
        break;
    }
    // The cloud task picks a fresh connection up within a second.
    return os_wifi_state() == OS_WIFI_CONNECTED ? V_CONNECTING : V_OFFLINE;
}

// Only the parts that change without a rebuild.
static void update_live(const os_cloud_info_t *in)
{
    if (!s_live) {
        return;
    }
    char line[112];
    if (s_shown == V_CLAIMED) {
        sync_line(line, sizeof(line), in);
        lv_label_set_text(s_live, line);
        return;
    }
    if (s_shown != V_ERROR) {
        return;
    }
    char st[20] = "no response";
    if (in->http_status) {
        snprintf(st, sizeof(st), "HTTP %d", in->http_status);
    }
    if (in->busy || in->retry_s == 0) {
        lv_label_set_text_fmt(s_live, "%s  " LV_SYMBOL_BULLET "  retrying now", st);
    } else if (in->retry_s > 0) {
        lv_label_set_text_fmt(s_live, "%s  " LV_SYMBOL_BULLET "  retry in %d s", st, in->retry_s);
    } else {
        lv_label_set_text(s_live, st);
    }
}

static const char *claimed_hint(const os_cloud_info_t *in)
{
    if (in->packs > 0) {
        return "SELECT open pack  CANCEL exit";
    }
    switch (ui_net_state(false)) {
    case UI_NET_OK:         return "SELECT refresh  CANCEL exit";
    case UI_NET_WIFI_OFF:
    case UI_NET_FAILED:     return "SELECT connect Wi-Fi  CANCEL exit";
    case UI_NET_NO_NETWORK: return "SELECT open Wi-Fi  CANCEL exit";
    default:                return "CANCEL exit";
    }
}

static void build(const os_cloud_info_t *in, view_t v)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);
    s_live = s_offline = NULL;
    s_shown = v;
    *s_last = *in;
    s_me_shown = s_me;
    s_net_shown = ui_net_state(false);

    char host[64], line[96];
    host_of(in->server, host, sizeof(host));
    const char *hint = "SELECT refresh  CANCEL exit";
    switch (v) {
    case V_OFFLINE:
        // The shared panel: "Wi-Fi is off. SELECT to connect to <ssid>" and friends.
        s_offline = ui_offline_create(s_view, 0, CONTENT_H, false);
        hint = "SELECT connect  CANCEL exit";
        break;
    case V_NO_SERVER:
        message(LV_SYMBOL_SETTINGS, COL_YELLOW, "No server set",
                "This build has no badge server. Set one over USB with the console:");
        footnote(0, COL_TEXT, "server https://<host>");
        hint = "CANCEL exit";
        break;
    case V_CONNECTING:
        snprintf(line, sizeof(line), "Saying hello to %s", host);
        message(NULL, COL_PURPLE, "Connecting", line);
        if (in->id[0]) {
            snprintf(line, sizeof(line), "BADGE %s", in->id);
            footnote(0, COL_DIM, line);
        }
        break;
    case V_ERROR:
        message(LV_SYMBOL_WARNING, COL_ERR, in->error ? in->error : "Something went wrong",
                "The badge keeps retrying on its own. Check the Wi-Fi and the server.");
        footnote(1, COL_DIM, host);
        s_live = footnote(0, COL_DIM, "");
        update_live(in);
        hint = "SELECT retry now  CANCEL exit";
        break;
    case V_LINKED:
        // Someone owns this badge but it lost its secret (erased): only a
        // release on the web frees it. The cloud task keeps asking.
        // The server's text runs to four lines: no footnote under it.
        message(LV_SYMBOL_WARNING, COL_YELLOW, "Linked to an account", in->message);
        hint = "SELECT check again  CANCEL exit";
        break;
    case V_UNCLAIMED:
        build_unclaimed(in);
        break;
    case V_CLAIMED:
        build_claimed(in);
        hint = claimed_hint(in);
        break;
    default:
        break;
    }
    lv_label_set_text(s_hint, hint);
}

static bool same_me(const me_t *a, const me_t *b)
{
    return a->owned == b->owned && a->total == b->total && a->next_bounty == b->next_bounty && a->needs_x == b->needs_x
        && !strcmp(a->pack, b->pack);
}

static void render(bool force)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    view_t v = view_for(in);
    bool same = !force && v == s_shown && !strcmp(in->id, s_last->id) && !strcmp(in->claim_url, s_last->claim_url)
             && !strcmp(in->owner, s_last->owner) && in->error == s_last->error && !strcmp(in->server, s_last->server)
             && !strcmp(in->message, s_last->message)
             && in->lamps == s_last->lamps && in->level == s_last->level && in->packs == s_last->packs
             && in->welcome_needs_x == s_last->welcome_needs_x
             && same_me(&s_me, &s_me_shown) && (v != V_CLAIMED || ui_net_state(false) == s_net_shown);
    if (!same) {
        build(in, v);
    } else {
        update_live(in);
    }
    if (s_offline) {
        ui_offline_update(s_offline);
    }
    // Claimed and online: fetch the details once per visit.
    if (v == V_CLAIMED && !s_asked && os_api_online()) {
        fetch_me();
    }
    free(in);
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_shown = V_NONE;
    s_asked = false;
    memset(&s_me, 0, sizeof(s_me));
    cache_load();
    s_last = lv_malloc_zeroed(sizeof(*s_last));
    s_hint = ui_hint(root, "");
    render(true);
}

static void on_leave(void)
{
    os_api_cancel(&s_me);
    s_me.fetching = false;
    lv_free(s_last);
    s_last = NULL;
    s_view = s_live = s_hint = s_offline = NULL;    // deleted with the root
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k != OS_KEY_SELECT) {
        return false;
    }
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (s_shown == V_CLAIMED) {
        if (s_last->packs > 0) {
            os_launch_id("guide");
            return true;
        }
        switch (ui_net_state(false)) {
        case UI_NET_OK:
            os_cloud_refresh();
            fetch_me();
            break;
        case UI_NET_WIFI_OFF:
        case UI_NET_FAILED:
            os_wifi_set_enabled(true);
            os_toast("Connecting to Wi-Fi...");
            break;
        case UI_NET_NO_NETWORK:
            os_launch_id("wifi");
            break;
        default:
            break;
        }
        return true;
    }
    if (s_shown != V_NO_SERVER && s_shown != V_OFFLINE) {
        os_cloud_refresh();
        os_toast("Checking with the server...");
    }
    return true;
}

static void on_tick(void)
{
    static int n;
    if (++n % 5 == 0) {
        render(false);
    }
}

const os_app_t app_badge = {
    .id = "badge",
    .name = "My Badge",
    .symbol = LV_SYMBOL_LOOP,
    .accent = COL_LPURPLE,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
