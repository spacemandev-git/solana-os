// Emulated raw radio for promiscuous apps (the ESP-NOW scanner). While
// promiscuous mode is on, a radio task delivers real 802.11 frames to the rx
// callback: ESP-NOW vendor action frames from a handful of simulated senders,
// plus beacons as background management traffic. A frame is only heard when
// the app is tuned to its channel, exactly like channel hopping on the badge.
// Every sender MAC is locally administered (1A:DE:40:...): demo data.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "emu.h"

typedef struct {
    uint8_t mac[6];
    uint8_t channel;
    int8_t rssi;
    bool bcast;
    uint8_t body;           // payload bytes after the ESP-NOW header
    uint8_t version;
    uint16_t period_ms;
    int64_t next_us;
} sender_t;

static sender_t s_senders[] = {
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x01}, 1, -52, true, 24, 1, 400},
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x02}, 1, -67, false, 10, 1, 1200},
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x03}, 6, -58, true, 200, 2, 250},
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x04}, 11, -74, true, 48, 1, 800},
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x05}, 11, -86, false, 6, 1, 2000},
    {{0x1A, 0xDE, 0x40, 0x00, 0x00, 0x06}, 36, -63, true, 32, 2, 500},
};
#define N_SENDERS (int)(sizeof(s_senders) / sizeof(s_senders[0]))

// Channels with a simulated access point beaconing.
static const uint8_t s_beacon_ch[] = {1, 3, 6, 9, 11, 36, 44, 149};

static bool s_promisc;
static wifi_promiscuous_cb_t s_cb;
static uint32_t s_filter = WIFI_PROMIS_FILTER_MASK_ALL;
static uint8_t s_channel = 1;
static TaskHandle_t s_task;
static uint8_t s_pkt[sizeof(wifi_promiscuous_pkt_t) + 320] __attribute__((aligned(4)));

uint8_t emu_radio_channel(void)
{
    return s_channel;
}

bool emu_radio_promiscuous(void)
{
    return s_promisc;
}

static void deliver(const uint8_t *frame, int len, int rssi)
{
    wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)s_pkt;
    memset(&p->rx_ctrl, 0, sizeof(p->rx_ctrl));
    p->rx_ctrl.rssi = rssi;
    p->rx_ctrl.channel = s_channel;
    p->rx_ctrl.sig_len = len + 4;           // the driver counts the FCS
    p->rx_ctrl.noise_floor = -96;
    memcpy(p->payload, frame, len);
    memset(p->payload + len, 0, 4);
    if (s_cb && (s_filter & WIFI_PROMIS_FILTER_MASK_MGMT)) {
        s_cb(p, WIFI_PKT_MGMT);
    }
}

static void send_espnow(const sender_t *s)
{
    static const uint8_t oui[3] = {0x18, 0xFE, 0x34};
    uint8_t f[40 + 255];
    int len = 39 + s->body;
    memset(f, 0, sizeof(f));
    f[0] = 0xD0;                            // action frame
    if (s->bcast) {
        memset(f + 4, 0xFF, 6);
    } else {
        const uint8_t *me = emu_mac();
        memcpy(f + 4, me, 6);
    }
    memcpy(f + 10, s->mac, 6);
    memset(f + 16, 0xFF, 6);
    f[24] = 127;                            // vendor-specific category
    memcpy(f + 25, oui, 3);
    esp_fill_random(f + 28, 4);
    f[32] = 0xDD;                           // vendor element
    f[33] = s->body + 5;
    memcpy(f + 34, oui, 3);
    f[37] = 4;                              // ESP-NOW
    f[38] = s->version;
    esp_fill_random(f + 39, s->body);
    deliver(f, len, s->rssi + (int)(esp_random() % 9) - 4);
}

static void send_beacon(uint8_t ch)
{
    uint8_t f[48] = {0x80, 0x00};
    memset(f + 4, 0xFF, 6);
    uint8_t bssid[6] = {0x02, 0xde, 0x40, 0x00, ch, 0x01};
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[36] = 0;                              // SSID element, hidden: payload is filler
    deliver(f, sizeof(f), -60 - (int)(esp_random() % 30));
}

static void radio_task(void *arg)
{
    for (;;) {
        if (!s_promisc || !s_cb) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = emu_now_us();
        for (int i = 0; i < N_SENDERS; i++) {
            sender_t *s = &s_senders[i];
            if (now < s->next_us) {
                continue;
            }
            s->next_us = now + (s->period_ms + (int)(esp_random() % (s->period_ms / 4 + 1))) * 1000LL;
            if (s->channel == s_channel) {
                send_espnow(s);
            }
        }
        for (int i = 0; i < (int)sizeof(s_beacon_ch); i++) {
            if (s_beacon_ch[i] == s_channel && esp_random() % 5 == 0) {
                send_beacon(s_channel);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t esp_wifi_set_promiscuous(bool en)
{
    s_promisc = en;
    if (en && !s_task) {
        xTaskCreate(radio_task, "wifi_rx", 3072, NULL, 23, &s_task);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb)
{
    s_cb = cb;
    return ESP_OK;
}

esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter)
{
    s_filter = filter ? filter->filter_mask : WIFI_PROMIS_FILTER_MASK_ALL;
    return ESP_OK;
}

esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second)
{
    (void)second;
    if (!primary || primary > 177) {
        return ESP_ERR_INVALID_ARG;
    }
    s_channel = primary;
    return ESP_OK;
}

esp_err_t esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
    *primary = s_channel;
    if (second) {
        *second = WIFI_SECOND_CHAN_NONE;
    }
    return ESP_OK;
}
