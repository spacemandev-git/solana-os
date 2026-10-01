// os_radio on the badge: esp_now on the Wi-Fi STA interface. Peers are added
// unencrypted with channel 0 ("whatever the radio is on"), so they keep
// working when the radio moves between the mesh channel and an AP's channel.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "os_radio.h"

static const char *TAG = "os_radio";

// Unicast peers we keep registered, besides broadcast (driver limit: 20).
#define PEERS   12

static const uint8_t s_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static os_radio_rx_cb_t s_rx;
static SemaphoreHandle_t s_lock;
static struct {
    uint8_t mac[6];
    uint32_t used;          // LRU stamp, 0 = free
} s_peers[PEERS];
static uint32_t s_stamp;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (s_rx && info && info->src_addr) {
        s_rx(info->src_addr, data, len, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
    }
}

static esp_err_t add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t p = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(p.peer_addr, mac, 6);
    esp_err_t err = esp_now_add_peer(&p);
    return err == ESP_ERR_ESPNOW_EXIST ? ESP_OK : err;
}

esp_err_t os_radio_init(os_radio_rx_cb_t cb)
{
    s_lock = xSemaphoreCreateMutex();
    s_rx = cb;
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_recv), TAG, "recv cb");
    ESP_RETURN_ON_ERROR(add_peer(s_bcast), TAG, "broadcast peer");
    return ESP_OK;
}

int os_radio_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    return esp_wifi_get_channel(&primary, &second) == ESP_OK ? primary : 0;
}

esp_err_t os_radio_set_channel(int channel)
{
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

// Register a unicast peer, evicting the least recently used one when full.
static esp_err_t ensure_peer(const uint8_t *mac)
{
    int free_i = -1, lru = 0;
    for (int i = 0; i < PEERS; i++) {
        if (s_peers[i].used && !memcmp(s_peers[i].mac, mac, 6)) {
            s_peers[i].used = ++s_stamp;
            return ESP_OK;
        }
        if (!s_peers[i].used) {
            free_i = free_i < 0 ? i : free_i;
        } else if (s_peers[i].used < s_peers[lru].used || !s_peers[lru].used) {
            lru = i;
        }
    }
    int i = free_i;
    if (i < 0) {
        i = lru;
        esp_now_del_peer(s_peers[i].mac);
    }
    esp_err_t err = add_peer(mac);
    if (err == ESP_OK) {
        memcpy(s_peers[i].mac, mac, 6);
        s_peers[i].used = ++s_stamp;
    } else {
        s_peers[i].used = 0;
        ESP_LOGW(TAG, "add peer: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_radio_send(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len <= 0 || len > OS_RADIO_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mac && memcmp(mac, s_bcast, 6)) {
        err = ensure_peer(mac);
    }
    if (err == ESP_OK) {
        err = esp_now_send(mac ? mac : s_bcast, data, len);
    }
    xSemaphoreGive(s_lock);
    return err == ESP_ERR_ESPNOW_NO_MEM ? ESP_ERR_NO_MEM : err;
}

void os_radio_mac(uint8_t mac[6])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

void os_radio_stay_awake(bool on)
{
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power save: %s", esp_err_to_name(err));
    }
}
