// My Badge: this badge's identity on the server (core/os_cloud). While the
// badge is unclaimed it shows a QR code of the claim link, so the owner can
// scan it with a phone and link the badge to their account.
//
// The cloud task does all the talking; this app only renders snapshots of
// os_cloud_get_info() and rebuilds the view when the state changes.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#define CONTENT_H   (OS_ROOT_H - UI_HINT_H)
#define QR_BOX      164         // square behind the code: its quiet zone
#define QR_SIZE     148         // canvas; lv_qrcode centres the code at an integer scale
#define COL_X       (10 + QR_BOX + 12)
#define COL_W       (OS_ROOT_W - COL_X - 8)

extern const os_app_t app_wifi;

typedef enum { V_NONE, V_NO_WIFI, V_JOINING, V_NO_SERVER, V_CONNECTING, V_UNCLAIMED, V_CLAIMED, V_ERROR } view_t;

static lv_obj_t *s_root, *s_view, *s_hint, *s_live;
static view_t s_shown;
static os_cloud_info_t *s_last;         // what s_view was built from

// "https://host:port/path" -> "host:port"
static void host_of(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t len = strcspn(p, "/?#");
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
// Returns the body label so callers can add to it.
static lv_obj_t *message(const char *symbol, uint32_t color, const char *title, const char *body)
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
    return b;
}

// One-line mono footnote under a message (server, badge ID, retry timer).
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

    int y = (CONTENT_H - QR_BOX) / 2 + 2;
    label_at(s_view, &sol_mono_12, COL_DIM, "BADGE ID", COL_X, y, 0);
    lv_obj_t *id = label_at(s_view, &sol_font_32, COL_TEXT, in->id, COL_X, y + 14, 0);
    lv_obj_update_layout(id);
    if (lv_obj_get_width(id) > COL_W) {
        lv_obj_set_style_text_font(id, &sol_font_20, 0);
    }
    ui_gradient_bar(s_view, COL_X, y + 58, COL_W, 3);
    label_at(s_view, &sol_font_20, COL_TEXT, "Scan to link\nyour badge", COL_X, y + 72, COL_W);
    label_at(s_view, &sol_mono_12, COL_DIM, "Open it on your phone and sign in with your wallet.", COL_X, y + 124, COL_W);
}

static void build_claimed(const os_cloud_info_t *in)
{
    lv_obj_t *ring = lv_obj_create(s_view);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 48, 48);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ring, ui_col(COL_GREEN), 0);
    lv_obj_t *ok = ui_label(ring, &lv_font_montserrat_20, COL_BG, LV_SYMBOL_OK);
    lv_obj_center(ok);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *lead = ui_label(s_view, &sol_font_14, COL_DIM, "This badge belongs to");
    lv_obj_align(lead, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_t *who = ui_label(s_view, &sol_font_32, COL_TEXT, in->owner);
    lv_obj_update_layout(who);
    if (lv_obj_get_width(who) > OS_ROOT_W - 24) {
        lv_obj_set_style_text_font(who, &sol_font_20, 0);
        lv_obj_set_width(who, OS_ROOT_W - 24);
        lv_label_set_long_mode(who, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 94);
    ui_gradient_bar(s_view, (OS_ROOT_W - 160) / 2, 142, 160, 3);

    lv_obj_t *id = ui_label(s_view, &sol_mono_12, COL_TEXT, "");
    lv_label_set_text_fmt(id, "BADGE %s  " LV_SYMBOL_BULLET "  LINKED", in->id);
    lv_obj_align(id, LV_ALIGN_TOP_MID, 0, 156);
    char host[64];
    host_of(in->server, host, sizeof(host));
    footnote(0, COL_DIM, host);
}

static view_t view_for(const os_cloud_info_t *in)
{
    if (!in->server[0]) {
        return V_NO_SERVER;
    }
    switch (in->state) {
    case OS_CLOUD_UNCLAIMED:  return V_UNCLAIMED;
    case OS_CLOUD_CLAIMED:    return V_CLAIMED;
    case OS_CLOUD_ERROR:      return V_ERROR;
    case OS_CLOUD_CONNECTING: return V_CONNECTING;
    default:
        break;
    }
    os_wifi_state_t w = os_wifi_state();
    if (w == OS_WIFI_CONNECTED) {
        return V_CONNECTING;    // the cloud task picks the connection up within a second
    }
    return w == OS_WIFI_CONNECTING ? V_JOINING : V_NO_WIFI;
}

// Only the parts that change without a rebuild: the retry countdown.
static void update_live(const os_cloud_info_t *in)
{
    if (!s_live || s_shown != V_ERROR) {
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

static void build(const os_cloud_info_t *in, view_t v)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);
    s_live = NULL;
    s_shown = v;
    *s_last = *in;

    char host[64], line[96];
    host_of(in->server, host, sizeof(host));
    const char *hint = "SELECT refresh  CANCEL exit";
    switch (v) {
    case V_NO_WIFI:
        message(LV_SYMBOL_WIFI, COL_ERR, "No Wi-Fi",
                "My Badge talks to the badge server over the internet. Join a network first.");
        hint = "SELECT open Wi-Fi  CANCEL exit";
        break;
    case V_JOINING: {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        snprintf(line, sizeof(line), "Connecting to %s...", w.ssid);
        message(NULL, COL_AQUA, "Joining Wi-Fi", line);
        hint = "SELECT open Wi-Fi  CANCEL exit";
        break;
    }
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
    case V_UNCLAIMED:
        build_unclaimed(in);
        break;
    case V_CLAIMED:
        build_claimed(in);
        break;
    default:
        break;
    }
    lv_label_set_text(s_hint, hint);
}

static void render(bool force)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    view_t v = view_for(&in);
    bool same = !force && v == s_shown && !strcmp(in.id, s_last->id) && !strcmp(in.claim_url, s_last->claim_url)
             && !strcmp(in.owner, s_last->owner) && in.error == s_last->error && !strcmp(in.server, s_last->server);
    if (!same) {
        build(&in, v);
    } else {
        update_live(&in);
    }
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_view = NULL;
    s_shown = V_NONE;
    s_last = lv_malloc_zeroed(sizeof(*s_last));
    s_hint = ui_hint(root, "");
    render(true);
}

static void on_leave(void)
{
    lv_free(s_last);
    s_last = NULL;
    s_view = s_live = s_hint = NULL;    // deleted with the root
}

static bool on_key(os_key_t k, bool repeat)
{
    if (k != OS_KEY_SELECT) {
        return false;
    }
    if (s_shown == V_NO_WIFI || s_shown == V_JOINING) {
        os_launch(&app_wifi);
    } else if (s_shown != V_NO_SERVER) {
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
