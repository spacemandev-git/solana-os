// ESP-NOW Scanner. ESP-NOW has no discovery, so this borrows the radio from
// the Wi-Fi service, sniffs management frames in promiscuous mode and keeps
// the ones that are ESP-NOW (vendor action frames with Espressif's OUI),
// hopping channels.
//
// Mesh tab: the HH GtS HELLOs among them (docs/MESH.md), sorted by channel.
// A beacon's radio follows its access point, so at a venue with several
// networks a beacon can sit on a channel the badge isn't listening on. The
// tab sweeps every channel slowly enough to catch a HELLO (they come every
// 2 s), lists the beacons and badges per channel, and SELECT moves this
// badge's mesh to that channel (os_mesh_set_channel) so Quests, X Dex, Trade
// and Lua apps find them.
//
// Frames tab: every ESP-NOW sender, hopping fast; SELECT locks onto a
// sender's channel.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "core/os.h"
#include "core/os_mesh.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game/game.h"

static const char *TAG = "espnow_app";

#define PEER_MAX        32
#define DWELL_TICKS     3       // Frames: 300 ms per channel
#define MESH_DWELL      26      // Mesh: 2.6 s per channel, longer than a HELLO period (2 s +- 0.5)
#define STALE_US        (120 * 1000000LL)
#define DEV_MAX         48
#define DEV_TTL_US      (150 * 1000000LL)   // a few sweeps
#define RING            8
#define HELLO_MAX       232     // one mesh fragment (MESH.md)

// 802.11 action frame carrying ESP-NOW:
// [0] FC=0xD0  [4..9] dest  [10..15] src  [24] category 127 (vendor)
// [25..27] OUI 18:FE:34  [28..31] random  [32] element 0xDD  [33] len
// [34..36] OUI 18:FE:34  [37] type 4 (ESP-NOW)  [38] version  [39..] body
#define HDR_LEN         24
#define MIN_LEN         39
// A mesh frame in the body (docs/MESH.md "Frame"): "HG", version, type,
// 6-char sender id, msg (2), fragment index, count, total length (2), data.
#define MESH_HDR        16

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t version;
    bool bcast, mesh;
    uint16_t len;
    uint32_t count;
    int64_t last_us;
} peer_t;

// A HELLO heard in the Wi-Fi task, parsed later on the OS task.
typedef struct {
    uint8_t channel;
    int8_t rssi;
    char src[7];
    uint8_t len;
    char data[HELLO_MAX + 1];
} hello_t;

// A beacon or badge seen in the Mesh sweep.
typedef struct {
    char id[7];
    bool beacon;
    char name[25];
    uint32_t color;
    uint8_t channel;
    int8_t rssi;
    int64_t last_us;
} dev_t;

typedef struct {
    uint8_t ch;
    int beacons, badges;
    int8_t rssi;
    char names[56];
} chan_t;

// 2.4 GHz is the first N_24 entries.
static const uint8_t ch_all[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165};
#define N_ALL (int)sizeof(ch_all)
#define N_24 13

enum { TAB_MESH, TAB_FRAMES };
enum { HOP_24, HOP_ALL, HOP_LOCKED };

typedef struct {
    peer_t peers[PEER_MAX];
    int n;
    hello_t ring[RING];
    int ring_head, ring_count;
    dev_t devs[DEV_MAX];
    int ndev;
    chan_t chans[N_ALL + 1];
    int nchan;
    int sorted[PEER_MAX], shown;
} state_t;

static state_t *s;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_channel = 1;
static volatile uint32_t s_frames, s_mgmt, s_hellos;
static int s_tab, s_hop, s_hop_idx, s_tick, s_locked_ch;
static bool s_mesh_all;
static int64_t s_sweep_start_us;
static lv_obj_t *s_tabs, *s_head, *s_sub, *s_hint;
static ui_list_t *s_list;
static bool s_promisc;

static const uint8_t espressif_oui[3] = {0x18, 0xFE, 0x34};
static const char *const tab_names[] = {"Mesh", "Frames"};

// ---------------------------------------------------------------------------
// Wi-Fi task: never blocks, only copies.

static void keep_hello(const uint8_t *body, int blen, int8_t rssi)
{
    if (blen <= MESH_HDR || body[0] != 'H' || body[1] != 'G' || body[2] != 1 || body[3] != OS_MESH_HELLO ||
        body[12] != 0 || body[13] != 1) {
        return;
    }
    int len = body[14] | body[15] << 8;
    if (len != blen - MESH_HDR || len > HELLO_MAX) {
        return;
    }
    for (int i = 4; i < 10; i++) {
        if (body[i] < 0x21 || body[i] > 0x7E) {
            return;
        }
    }
    hello_t *h = &s->ring[(s->ring_head + s->ring_count) % RING];
    if (s->ring_count == RING) {
        s->ring_head = (s->ring_head + 1) % RING;   // drop the oldest
    } else {
        s->ring_count++;
    }
    h->channel = s_channel;
    h->rssi = rssi;
    memcpy(h->src, body + 4, 6);
    h->src[6] = 0;
    h->len = len;
    memcpy(h->data, body + MESH_HDR, len);
    h->data[len] = 0;
    s_hellos++;
}

static void on_pkt(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !s) {
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
    int blen = f[33] > 5 ? f[33] - 5 : 0;
    blen = blen > len - MIN_LEN ? len - MIN_LEN : blen;
    const uint8_t *body = f + MIN_LEN;
    portENTER_CRITICAL(&s_mux);
    peer_t *p = NULL;
    for (int i = 0; i < s->n; i++) {
        if (!memcmp(s->peers[i].mac, src, 6)) {
            p = &s->peers[i];
            break;
        }
    }
    if (!p) {
        if (s->n < PEER_MAX) {
            p = &s->peers[s->n++];
        } else {
            p = &s->peers[0];
            for (int i = 1; i < s->n; i++) {
                if (s->peers[i].last_us < p->last_us) {
                    p = &s->peers[i];
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
    p->len = blen;
    p->mesh = blen >= 2 && body[0] == 'H' && body[1] == 'G';
    p->count++;
    p->last_us = now;
    if (p->mesh) {
        keep_hello(body, blen, pkt->rx_ctrl.rssi);
    }
    portEXIT_CRITICAL(&s_mux);
}

static void set_channel(uint8_t ch)
{
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        s_channel = ch;
    }
}

// ---------------------------------------------------------------------------
// Mesh: HELLOs into beacons and badges per channel (OS task).

static void absorb(const hello_t *h, int64_t now)
{
    // Mesh input is hostile: a flat object of short strings and numbers, or nothing.
    cJSON *o = cJSON_ParseWithLength(h->data, h->len);
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(o, "k"));
    if (!cJSON_IsObject(o) || !k || (strcmp(k, "c") && strcmp(k, "b"))) {
        cJSON_Delete(o);
        return;
    }
    dev_t *d = NULL;
    for (int i = 0; i < s->ndev; i++) {
        if (!strcmp(s->devs[i].id, h->src)) {
            d = &s->devs[i];
            break;
        }
    }
    if (!d) {
        if (s->ndev < DEV_MAX) {
            d = &s->devs[s->ndev++];
        } else {
            d = &s->devs[0];
            for (int i = 1; i < s->ndev; i++) {
                if (s->devs[i].last_us < d->last_us) {
                    d = &s->devs[i];
                }
            }
        }
        memset(d, 0, sizeof(*d));
        strlcpy(d->id, h->src, sizeof(d->id));
    }
    d->beacon = k[0] == 'c';
    game_ascii(d->name, sizeof(d->name), cJSON_GetStringValue(cJSON_GetObjectItem(o, "n")));
    const char *col = cJSON_GetStringValue(cJSON_GetObjectItem(o, "col"));
    d->color = col && strlen(col) == 6 ? strtoul(col, NULL, 16) : 0;
    d->channel = h->channel;
    d->rssi = h->rssi;
    d->last_us = now;
    cJSON_Delete(o);
}

static int chan_rank(const void *a, const void *b)
{
    const chan_t *x = a, *y = b;
    if (x->beacons != y->beacons) {
        return y->beacons - x->beacons;
    }
    if (x->badges != y->badges) {
        return y->badges - x->badges;
    }
    return x->ch - y->ch;
}

// One row per channel where something was heard, plus the badge's own.
static void group(int64_t now)
{
    int mine = os_mesh_channel();
    s->nchan = 0;
    for (int i = 0; i < N_ALL + 1; i++) {
        uint8_t ch = i < N_ALL ? ch_all[i] : (uint8_t)mine;
        if (i == N_ALL) {
            bool listed = false;
            for (int j = 0; j < s->nchan; j++) {
                listed |= s->chans[j].ch == ch;
            }
            if (listed || !ch) {
                continue;
            }
        }
        chan_t c = { .ch = ch, .rssi = -127 };
        for (int j = 0; j < s->ndev; j++) {
            const dev_t *d = &s->devs[j];
            if (d->channel != ch || now - d->last_us > DEV_TTL_US) {
                continue;
            }
            if (d->beacon) {
                c.beacons++;
                size_t at = strlen(c.names);
                snprintf(c.names + at, sizeof(c.names) - at, "%s%s", at ? ", " : "", d->name[0] ? d->name : d->id);
            } else {
                c.badges++;
            }
            c.rssi = d->rssi > c.rssi ? d->rssi : c.rssi;
        }
        if (c.beacons || c.badges || ch == mine) {
            s->chans[s->nchan++] = c;
        }
    }
    qsort(s->chans, s->nchan, sizeof(chan_t), chan_rank);
}

// ---------------------------------------------------------------------------
// Rows

static void mesh_row(int i, ui_row_t *r)
{
    int mine = os_mesh_channel();
    if (i == 0) {
        snprintf(r->title, sizeof(r->title), "Scan: %s", s_mesh_all ? "2.4 + 5 GHz" : "2.4 GHz");
        snprintf(r->sub, sizeof(r->sub), "SELECT for %s", s_mesh_all ? "2.4 GHz only (faster)" : "5 GHz too (slower)");
        snprintf(r->right, sizeof(r->right), "ch %d", s_channel);
        return;
    }
    const chan_t *c = &s->chans[i - 1];
    bool here = c->ch == mine;
    // The badge fonts are printable ASCII (plus a bullet): no middle dots.
    snprintf(r->title, sizeof(r->title), "Channel %d%s", c->ch,
             !here ? "" : os_mesh_channel_overridden() ? "  (yours, chosen)" : "  (yours)");
    if (c->beacons) {
        char badges[24] = "";
        if (c->badges) {
            snprintf(badges, sizeof(badges), " + %d badge%s", c->badges, c->badges == 1 ? "" : "s");
        }
        snprintf(r->sub, sizeof(r->sub), "%s%s", c->names, badges);
    } else if (c->badges) {
        snprintf(r->sub, sizeof(r->sub), "%d badge%s, no beacon", c->badges, c->badges == 1 ? "" : "s");
    } else {
        snprintf(r->sub, sizeof(r->sub), "Nothing heard here yet");
    }
    if (c->rssi > -127) {
        snprintf(r->right, sizeof(r->right), "%d dBm", c->rssi);
        r->right_color = c->rssi >= -60 ? COL_GREEN : c->rssi >= -80 ? COL_YELLOW : COL_ORANGE;
    }
    if (here) {
        r->title_color = COL_GREEN;
    }
}

static void frames_row(int i, ui_row_t *r)
{
    if (i == 0) {
        if (s_hop == HOP_LOCKED) {
            snprintf(r->title, sizeof(r->title), "Locked on channel %d", s_locked_ch);
            snprintf(r->sub, sizeof(r->sub), "SELECT to hop again");
        } else {
            snprintf(r->title, sizeof(r->title), "Hopping %s", s_hop == HOP_24 ? "2.4 GHz" : "2.4 + 5 GHz");
            snprintf(r->sub, sizeof(r->sub), "SELECT for %s; SELECT a sender to lock", s_hop == HOP_24 ? "5 GHz too" : "2.4 GHz only");
        }
        snprintf(r->right, sizeof(r->right), "ch %d", s_channel);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    peer_t p = s->peers[s->sorted[i - 1]];
    portEXIT_CRITICAL(&s_mux);
    int ago = (esp_timer_get_time() - p.last_us) / 1000000;
    snprintf(r->title, sizeof(r->title), "%02X:%02X:%02X:%02X:%02X:%02X%s", p.mac[0], p.mac[1], p.mac[2], p.mac[3],
             p.mac[4], p.mac[5], p.mesh ? "  HH" : "");
    snprintf(r->sub, sizeof(r->sub), "ch %d  %lu pkt  %s  %u B  v%d  %ds ago", p.channel, (unsigned long)p.count,
             p.bcast ? "bcast" : "unicast", p.len, p.version, ago);
    snprintf(r->right, sizeof(r->right), "%d dBm", p.rssi);
    r->right_color = p.rssi >= -60 ? COL_GREEN : p.rssi >= -80 ? COL_YELLOW : COL_ORANGE;
    if (ago > 10) {
        r->title_color = COL_DIM;
    }
}

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s_tab == TAB_MESH) {
        mesh_row(i, r);
    } else {
        frames_row(i, r);
    }
}

static int by_rssi(const void *a, const void *b)
{
    return s->peers[*(const int *)b].rssi - s->peers[*(const int *)a].rssi;
}

static void render(void)
{
    int64_t now = esp_timer_get_time();
    if (s_tab == TAB_MESH) {
        group(now);
        ui_list_set_count(s_list, 1 + s->nchan);
        int n = s_mesh_all ? N_ALL : N_24;
        int left = (n * MESH_DWELL) / 10 - (int)((now - s_sweep_start_us) / 1000000);
        lv_label_set_text_fmt(s_head, LV_SYMBOL_SHUFFLE " Looking for beacons on ch %d", s_channel);
        lv_label_set_text_fmt(s_sub, "Your mesh: ch %d%s  sweep ~%d s left  %lu HELLOs", os_mesh_channel(),
                              os_mesh_channel_overridden() ? " (chosen)" : "", left > 0 ? left : 0,
                              (unsigned long)s_hellos);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s->shown = 0;
    for (int i = 0; i < s->n; i++) {
        if (now - s->peers[i].last_us < STALE_US) {
            s->sorted[s->shown++] = i;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    qsort(s->sorted, s->shown, sizeof(int), by_rssi);
    ui_list_set_count(s_list, 1 + s->shown);
    const char *band = s_channel > 14 ? "5 GHz" : "2.4 GHz";
    if (s_hop == HOP_LOCKED) {
        lv_label_set_text_fmt(s_head, LV_SYMBOL_EYE_OPEN " locked on ch %d (%s)", s_channel, band);
    } else {
        lv_label_set_text_fmt(s_head, LV_SYMBOL_SHUFFLE " hopping %s  ch %d", s_hop == HOP_24 ? "2.4 GHz" : "all bands",
                              s_channel);
    }
    lv_label_set_text_fmt(s_sub, "%d sender%s  %lu ESP-NOW / %lu mgmt frames", s->shown, s->shown == 1 ? "" : "s",
                          (unsigned long)s_frames, (unsigned long)s_mgmt);
}

static void show_tab(int tab)
{
    s_tab = tab;
    ui_tabs_select(s_tabs, tab);
    s_hop_idx = 0;
    s_tick = 0;
    s_sweep_start_us = esp_timer_get_time();
    if (tab == TAB_FRAMES && s_hop == HOP_LOCKED) {
        set_channel(s_locked_ch);
    } else {
        set_channel(ch_all[0]);
    }
    ui_list_select(s_list, 0);
    lv_label_set_text(s_hint, tab == TAB_MESH ? LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT use channel  " LV_SYMBOL_RIGHT " Frames"
                                              : LV_SYMBOL_LEFT " Mesh  SELECT lock / hop  CANCEL exit");
    render();
}

// ---------------------------------------------------------------------------
// App

static void enter(lv_obj_t *root)
{
    s = calloc(1, sizeof(state_t));
    s_frames = s_mgmt = s_hellos = 0;
    s_hop = HOP_24;
    s_mesh_all = false;

    s_tabs = ui_tabs(root, tab_names, 2);
    s_head = ui_label(root, &sol_font_14, COL_TEXT, "");
    lv_obj_set_pos(s_head, 8, UI_TABS_H + 2);
    s_sub = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_sub, 8, UI_TABS_H + 20);
    s_list = ui_list_create(root, UI_TABS_H + 36, OS_ROOT_H - UI_TABS_H - 36 - UI_HINT_H, row, NULL);
    s_hint = ui_hint(root, "");

    // Borrow the radio: Wi-Fi reconnects (or parks on the mesh channel) when we give it back in on_exit.
    os_wifi_suspend();
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(on_pkt);
    s_promisc = esp_wifi_set_promiscuous(true) == ESP_OK;
    if (!s_promisc) {
        ESP_LOGE(TAG, "promiscuous mode failed");
        ui_list_set_empty(s_list, "Could not enable promiscuous mode");
    }
    show_tab(TAB_MESH);
}

static void on_leave(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    os_wifi_resume();
    portENTER_CRITICAL(&s_mux);
    state_t *old = s;
    s = NULL;
    portEXIT_CRITICAL(&s_mux);
    free(old);
}

static void use_channel(const chan_t *c)
{
    int server = os_mesh_server_channel();
    if (c->ch == os_mesh_channel() && !os_mesh_channel_overridden()) {
        os_toast("Your badge is already on this channel");
        return;
    }
    char msg[80];
    if (c->ch == server || (c->ch == os_mesh_channel() && os_mesh_channel_overridden())) {
        os_mesh_set_channel(0);
        snprintf(msg, sizeof(msg), "Back on the event's channel %d", server);
    } else {
        os_mesh_set_channel(c->ch);
        snprintf(msg, sizeof(msg), "Mesh on channel %d until restart", c->ch);
    }
    os_toast(msg);
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s_list, k, repeat)) {
        return true;
    }
    switch (k) {
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT:
        if ((k == OS_KEY_RIGHT) != (s_tab == TAB_FRAMES)) {
            show_tab(k == OS_KEY_RIGHT ? TAB_FRAMES : TAB_MESH);
        }
        return true;
    case OS_KEY_SELECT: {
        int sel = ui_list_selected(s_list);
        if (s_tab == TAB_MESH) {
            if (sel == 0) {
                s_mesh_all = !s_mesh_all;
                show_tab(TAB_MESH);
            } else if (sel > 0 && sel <= s->nchan) {
                use_channel(&s->chans[sel - 1]);
            }
        } else if (sel == 0) {
            s_hop = s_hop == HOP_LOCKED ? HOP_24 : s_hop == HOP_24 ? HOP_ALL : HOP_24;
            show_tab(TAB_FRAMES);
        } else if (sel > 0 && sel <= s->shown) {
            uint8_t ch = s->peers[s->sorted[sel - 1]].channel;
            if (s_hop == HOP_LOCKED && s_locked_ch == ch) {
                s_hop = HOP_24;
                os_toast("Hopping again");
            } else {
                s_hop = HOP_LOCKED;
                s_locked_ch = ch;
                set_channel(ch);
            }
            render();
        }
        return true;
    }
    default:
        return false;
    }
}

static void on_tick(void)
{
    s_tick++;
    if (s_tab == TAB_MESH) {
        // Parse what the Wi-Fi task kept since the last tick.
        int64_t now = esp_timer_get_time();
        for (;;) {
            hello_t h;
            portENTER_CRITICAL(&s_mux);
            bool any = s->ring_count > 0;
            if (any) {
                h = s->ring[s->ring_head];
                s->ring_head = (s->ring_head + 1) % RING;
                s->ring_count--;
            }
            portEXIT_CRITICAL(&s_mux);
            if (!any) {
                break;
            }
            absorb(&h, now);
        }
        int n = s_mesh_all ? N_ALL : N_24;
        if (s_tick % MESH_DWELL == 0) {
            s_hop_idx = (s_hop_idx + 1) % n;
            if (!s_hop_idx) {
                s_sweep_start_us = now;
            }
            set_channel(ch_all[s_hop_idx]);
        }
    } else if (s_hop != HOP_LOCKED && s_tick % DWELL_TICKS == 0) {
        int n = s_hop == HOP_24 ? N_24 : N_ALL;
        s_hop_idx = (s_hop_idx + 1) % n;
        set_channel(ch_all[s_hop_idx]);
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
