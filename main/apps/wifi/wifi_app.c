// Wi-Fi: scan both bands, pick a network, type the password on the d-pad
// keyboard, connect. The system service (os_wifi) saves it and reconnects on
// boot.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/ui.h"

#define SCAN_MAX    32
#define NET_MAX     24

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t auth;
    uint8_t aps;        // BSSIDs merged into this row
} net_t;

static net_t *s_nets;
static int s_count;
static bool s_scanning;
static int64_t s_retry_scan_at;
static lv_obj_t *s_root, *s_status, *s_detail;
static ui_list_t *s_list;
static ui_kb_t *s_kb;
static char s_kb_ssid[33];
static wifi_auth_mode_t s_kb_auth;

static const char *auth_str(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_OWE:             return "OWE";
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return "enterprise";
    default:                        return "secured";
    }
}

static const char *band(int ch)
{
    return ch > 14 ? "5 GHz" : "2.4 GHz";
}

static uint32_t rssi_color(int rssi)
{
    return rssi >= -60 ? COL_GREEN : rssi >= -75 ? COL_YELLOW : COL_ORANGE;
}

static void row(int i, ui_row_t *r, void *ctx)
{
    const net_t *n = &s_nets[i];
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    char saved[33] = "";
    os_wifi_saved_ssid(saved, sizeof(saved));
    bool current = w.state == OS_WIFI_CONNECTED && !strcmp(w.ssid, n->ssid);

    strlcpy(r->title, n->ssid[0] ? n->ssid : "(hidden network)", sizeof(r->title));
    snprintf(r->sub, sizeof(r->sub), "%s  ch %d  %s%s%s", band(n->channel), n->channel, auth_str(n->auth),
             current ? "  connected" : "", !current && saved[0] && !strcmp(saved, n->ssid) ? "  saved" : "");
    snprintf(r->right, sizeof(r->right), "%d dBm", n->rssi);
    r->right_color = rssi_color(n->rssi);
    if (current) {
        r->title_color = COL_GREEN;
    } else if (!n->ssid[0]) {
        r->title_color = COL_DIM;
    }
}

static void render_status(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    uint32_t c = COL_DIM;
    switch (w.state) {
    case OS_WIFI_CONNECTED:
        lv_label_set_text_fmt(s_status, LV_SYMBOL_WIFI "  %s", w.ssid);
        lv_label_set_text_fmt(s_detail, "IP %s  %d dBm  ch %d (%s)", w.ip, w.rssi, w.channel, band(w.channel));
        c = COL_GREEN;
        break;
    case OS_WIFI_CONNECTING:
        lv_label_set_text_fmt(s_status, LV_SYMBOL_REFRESH "  Connecting to %s", w.ssid);
        lv_label_set_text(s_detail, "Waiting for the access point...");
        c = COL_YELLOW;
        break;
    case OS_WIFI_FAILED:
        lv_label_set_text_fmt(s_status, LV_SYMBOL_WARNING "  %s: %s", w.ssid, w.fail_reason ? w.fail_reason : "failed");
        lv_label_set_text(s_detail, "Pick the network again to retry");
        c = COL_ERR;
        break;
    default:
        lv_label_set_text(s_status, LV_SYMBOL_CLOSE "  Not connected");
        lv_label_set_text(s_detail, "Pick a network below");
        break;
    }
    lv_obj_set_style_text_color(s_status, ui_col(c), 0);
    if (s_scanning) {
        lv_label_set_text(s_detail, "Scanning 2.4 + 5 GHz...");
    }
}

static void start_scan(void)
{
    if (os_wifi_scan_start() == ESP_OK) {
        s_scanning = true;
        s_retry_scan_at = 0;
        ui_list_set_empty(s_list, "Scanning 2.4 + 5 GHz...");
    } else {
        // Busy connecting; the driver refuses scans until that settles.
        s_retry_scan_at = esp_timer_get_time() + 1500000;
    }
    render_status();
}

static void collect_results(void)
{
    wifi_ap_record_t *recs = malloc(SCAN_MAX * sizeof(*recs));
    int n = recs ? os_wifi_scan_results(recs, SCAN_MAX) : 0;
    s_count = 0;
    // Results are sorted by RSSI, so the first hit per SSID+band is the strongest.
    for (int i = 0; i < n && s_count < NET_MAX; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool five = recs[i].primary > 14;
        net_t *dup = NULL;
        for (int j = 0; ssid[0] && j < s_count; j++) {
            if (!strcmp(s_nets[j].ssid, ssid) && (s_nets[j].channel > 14) == five) {
                dup = &s_nets[j];
                break;
            }
        }
        if (dup) {
            dup->aps++;
            continue;
        }
        net_t *t = &s_nets[s_count++];
        strlcpy(t->ssid, ssid, sizeof(t->ssid));
        t->rssi = recs[i].rssi;
        t->channel = recs[i].primary;
        t->auth = recs[i].authmode;
        t->aps = 1;
    }
    free(recs);
    ui_list_set_empty(s_list, "No networks found.\nPress RIGHT to scan again.");
    ui_list_set_count(s_list, s_count);
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_nets = calloc(NET_MAX, sizeof(net_t));
    s_count = 0;
    s_kb = NULL;
    lv_obj_t *head = ui_panel(root, 4, 4, OS_ROOT_W - 8, 42);
    lv_obj_set_style_pad_ver(head, 3, 0);
    s_status = ui_label(head, &sol_font_14, COL_TEXT, "");
    lv_obj_set_width(s_status, OS_ROOT_W - 24);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    s_detail = ui_label(head, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_detail, 0, 18);
    s_list = ui_list_create(root, 50, OS_ROOT_H - 50 - UI_HINT_H, row, NULL);
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT join  " LV_SYMBOL_RIGHT " rescan  " LV_SYMBOL_LEFT " forget");
    start_scan();
}

static void on_leave(void)
{
    free(s_nets);
    s_nets = NULL;
    s_kb = NULL;        // deleted with the root
}

static void join(const net_t *n)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    if (w.state == OS_WIFI_CONNECTED && !strcmp(w.ssid, n->ssid)) {
        os_toast("Already connected");
        return;
    }
    if (!n->ssid[0]) {
        os_toast("Hidden networks need their name; use the console: wifi connect");
        return;
    }
    char saved[33] = "", pass[65] = "";
    os_wifi_saved_ssid(saved, sizeof(saved));
    if (n->auth == WIFI_AUTH_OPEN || n->auth == WIFI_AUTH_OWE) {
        os_wifi_connect(n->ssid, "");
    } else if (saved[0] && !strcmp(saved, n->ssid) && os_wifi_saved_pass(pass, sizeof(pass))) {
        os_wifi_connect(n->ssid, pass);
    } else {
        strlcpy(s_kb_ssid, n->ssid, sizeof(s_kb_ssid));
        s_kb_auth = n->auth;
        char title[64];
        snprintf(title, sizeof(title), "Password for %s", n->ssid);
        s_kb = ui_kb_create(s_root, title, 63);
        return;
    }
    os_toast("Connecting to %s", n->ssid);
    render_status();
}

static bool on_key(os_key_t k, bool repeat)
{
    if (s_kb) {
        switch (ui_kb_key(s_kb, k, repeat)) {
        case UI_KB_DONE:
            if (s_kb_auth != WIFI_AUTH_WEP && strlen(ui_kb_text(s_kb)) < 8) {
                os_toast("WPA passwords are at least 8 characters");
                return true;
            }
            os_wifi_connect(s_kb_ssid, ui_kb_text(s_kb));
            os_toast("Connecting to %s", s_kb_ssid);
            ui_kb_delete(s_kb);
            s_kb = NULL;
            render_status();
            break;
        case UI_KB_CANCEL:
            ui_kb_delete(s_kb);
            s_kb = NULL;
            break;
        default:
            break;
        }
        return true;
    }
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    switch (k) {
    case OS_KEY_SELECT: {
        int i = ui_list_selected(s_list);
        if (i >= 0) {
            join(&s_nets[i]);
        }
        return true;
    }
    case OS_KEY_RIGHT:
        if (!repeat && !s_scanning) {
            start_scan();
        }
        return true;
    case OS_KEY_LEFT: {
        char saved[33];
        if (!repeat && os_wifi_saved_ssid(saved, sizeof(saved))) {
            os_wifi_forget();
            os_toast("Forgot %s", saved);
            ui_list_refresh(s_list);
            render_status();
        }
        return true;
    }
    default:
        return false;
    }
}

static void on_tick(void)
{
    static int n;
    if (s_scanning && os_wifi_scan_done()) {
        s_scanning = false;
        collect_results();
    }
    if (s_retry_scan_at && esp_timer_get_time() > s_retry_scan_at) {
        start_scan();
    }
    if (!s_kb && ++n % 5 == 0) {
        render_status();
        ui_list_refresh(s_list);
    }
}

const os_app_t app_wifi = {
    .id = "wifi",
    .name = "Wi-Fi",
    .symbol = LV_SYMBOL_WIFI,
    .accent = COL_AQUA,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
