// ESP-NOW Scanner. ESP-NOW has no discovery, so this borrows the radio from
// the Wi-Fi service, sniffs management frames in promiscuous mode and keeps
// the ones that are ESP-NOW (vendor action frames with Espressif's OUI),
// hopping channels unless locked to one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/ui.h"

static const char *TAG = "espnow_app";

#define PEER_MAX        32
#define DWELL_TICKS     3       // 300 ms per channel while hopping
#define STALE_US        (120 * 1000000LL)

// 802.11 action frame carrying ESP-NOW:
// [0] FC=0xD0  [4..9] dest  [10..15] src  [24] category 127 (vendor)
// [25..27] OUI 18:FE:34  [28..31] random  [32] element 0xDD  [33] len
// [34..36] OUI 18:FE:34  [37] type 4 (ESP-NOW)  [38] version  [39..] body
#define HDR_LEN         24
#define MIN_LEN         39

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t version;
    bool bcast;
    uint16_t len;
    uint32_t count;
    int64_t last_us;
} peer_t;

static const uint8_t ch_24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static const uint8_t ch_all[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165};

enum { MODE_HOP_24, MODE_HOP_ALL, MODE_LOCKED };

static peer_t *s_peers;
static int s_n;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_channel = 1;
static volatile uint32_t s_frames, s_mgmt;
static int s_mode, s_hop_idx, s_lock_idx, s_tick;
static int s_sorted[PEER_MAX], s_shown;
static lv_obj_t *s_head, *s_sub;
static ui_list_t *s_list;
static bool s_promisc;

static const uint8_t espressif_oui[3] = {0x18, 0xFE, 0x34};

static void on_pkt(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = buf;
    const uint8_t *f = pkt->payload;
    int len = pkt->rx_ctrl.sig_len - 4;     // drop FCS
    s_mgmt++;
    if (len < MIN_LEN || f[0] != 0xD0 || f[HDR_LEN] != 127 || memcmp(f + 25, espressif_oui, 3) ||
            f[32] != 0xDD || memcmp(f + 34, espressif_oui, 3) || f[37] != 4) {
        return;
    }
    s_frames++;
    int64_t now = esp_timer_get_time();
    const uint8_t *src = f + 10;
    portENTER_CRITICAL(&s_mux);
    peer_t *p = NULL;
    for (int i = 0; i < s_n; i++) {
        if (!memcmp(s_peers[i].mac, src, 6)) {
            p = &s_peers[i];
            break;
        }
    }
    if (!p) {
        if (s_n < PEER_MAX) {
            p = &s_peers[s_n++];
        } else {
            p = &s_peers[0];
            for (int i = 1; i < s_n; i++) {
                if (s_peers[i].last_us < p->last_us) {
                    p = &s_peers[i];
                }
            }
        }
        memset(p, 0, sizeof(*p));
        memcpy(p->mac, src, 6);
    }
    p->rssi = pkt->rx_ctrl.rssi;
    p->channel = s_channel;
    p->version = f[38];
    p->bcast = f[4] == 0xFF && f[5] == 0xFF && f[6] == 0xFF;
    p->len = f[33] > 5 ? f[33] - 5 : 0;
    p->count++;
    p->last_us = now;
    portEXIT_CRITICAL(&s_mux);
}

static void set_channel(uint8_t ch)
{
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        s_channel = ch;
    }
}

// ---------------------------------------------------------------------------

static int by_rssi(const void *a, const void *b)
{
    return s_peers[*(const int *)b].rssi - s_peers[*(const int *)a].rssi;
}

static void row(int i, ui_row_t *r, void *ctx)
{
    portENTER_CRITICAL(&s_mux);
    peer_t p = s_peers[s_sorted[i]];
    portEXIT_CRITICAL(&s_mux);
    int ago = (esp_timer_get_time() - p.last_us) / 1000000;
    snprintf(r->title, sizeof(r->title), "%02X:%02X:%02X:%02X:%02X:%02X", p.mac[0], p.mac[1], p.mac[2], p.mac[3],
             p.mac[4], p.mac[5]);
    snprintf(r->sub, sizeof(r->sub), "ch %d  %lu pkt  %s  %u B  v%d  %ds ago", p.channel, (unsigned long)p.count,
             p.bcast ? "bcast" : "unicast", p.len, p.version, ago);
    snprintf(r->right, sizeof(r->right), "%d dBm", p.rssi);
    r->right_color = p.rssi >= -60 ? COL_GREEN : p.rssi >= -80 ? COL_YELLOW : COL_ORANGE;
    if (ago > 10) {
        r->title_color = COL_DIM;
    }
}

static void render(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_shown = 0;
    for (int i = 0; i < s_n; i++) {
        if (now - s_peers[i].last_us < STALE_US) {
            s_sorted[s_shown++] = i;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    qsort(s_sorted, s_shown, sizeof(int), by_rssi);
    ui_list_set_count(s_list, s_shown);

    uint8_t ch = s_channel;
    const char *band = ch > 14 ? "5 GHz" : "2.4 GHz";
    if (s_mode == MODE_LOCKED) {
        lv_label_set_text_fmt(s_head, LV_SYMBOL_EYE_OPEN " locked on ch %d (%s)", ch, band);
    } else {
        lv_label_set_text_fmt(s_head, LV_SYMBOL_SHUFFLE " hopping %s  ch %d", s_mode == MODE_HOP_24 ? "2.4 GHz" : "all bands", ch);
    }
    lv_label_set_text_fmt(s_sub, "%d sender%s  %lu ESP-NOW / %lu mgmt frames", s_shown, s_shown == 1 ? "" : "s",
                          (unsigned long)s_frames, (unsigned long)s_mgmt);
}

static void enter(lv_obj_t *root)
{
    s_peers = calloc(PEER_MAX, sizeof(peer_t));
    s_n = s_shown = 0;
    s_frames = s_mgmt = 0;
    s_mode = MODE_HOP_24;
    s_hop_idx = s_tick = 0;

    s_head = ui_label(root, &sol_font_14, COL_TEXT, "");
    lv_obj_set_pos(s_head, 8, 3);
    s_sub = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_sub, 8, 21);
    s_list = ui_list_create(root, 38, OS_ROOT_H - 38 - UI_HINT_H, row, NULL);
    ui_list_set_empty(s_list, "Listening for ESP-NOW frames...\nAny ESP32 sending ESP-NOW nearby will show up here.");
    ui_hint(root, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " lock channel   SELECT hop mode   CANCEL exit");

    // Borrow the radio: Wi-Fi reconnects when we give it back in on_exit.
    os_wifi_suspend();
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(on_pkt);
    s_promisc = esp_wifi_set_promiscuous(true) == ESP_OK;
    if (!s_promisc) {
        ESP_LOGE(TAG, "promiscuous mode failed");
        ui_list_set_empty(s_list, "Could not enable promiscuous mode");
    }
    set_channel(ch_24[0]);
    render();
}

static void on_leave(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    os_wifi_resume();
    free(s_peers);
    s_peers = NULL;
    s_n = 0;
}

static int all_index_of(uint8_t ch)
{
    for (int i = 0; i < sizeof(ch_all); i++) {
        if (ch_all[i] == ch) {
            return i;
        }
    }
    return 0;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    switch (k) {
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT: {
        int n = sizeof(ch_all);
        if (s_mode != MODE_LOCKED) {
            s_lock_idx = all_index_of(s_channel);
            s_mode = MODE_LOCKED;
        }
        s_lock_idx = (s_lock_idx + (k == OS_KEY_RIGHT ? 1 : n - 1)) % n;
        set_channel(ch_all[s_lock_idx]);
        render();
        return true;
    }
    case OS_KEY_SELECT:
        s_mode = s_mode == MODE_HOP_24 ? MODE_HOP_ALL : MODE_HOP_24;
        s_hop_idx = 0;
        os_toast(s_mode == MODE_HOP_24 ? "Hopping 2.4 GHz channels" : "Hopping 2.4 + 5 GHz channels");
        render();
        return true;
    default:
        return false;
    }
}

static void on_tick(void)
{
    s_tick++;
    if (s_mode != MODE_LOCKED && s_tick % DWELL_TICKS == 0) {
        const uint8_t *list = s_mode == MODE_HOP_24 ? ch_24 : ch_all;
        int n = s_mode == MODE_HOP_24 ? sizeof(ch_24) : sizeof(ch_all);
        s_hop_idx = (s_hop_idx + 1) % n;
        set_channel(list[s_hop_idx]);
    }
    if (s_tick % 5 == 0) {
        render();
    }
}

const os_app_t app_espnow = {
    .id = "espnow",
    .name = "ESP-NOW",
    .symbol = LV_SYMBOL_SHUFFLE,
    .accent = COL_PINK,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
