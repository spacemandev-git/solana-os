// Emulator counterpart of core/os_radio.c: ESP-NOW between browser tabs.
//
// Tabs of one origin share a radio through new BroadcastChannel("solana-os-mesh")
// (docs/MESH.md "Emulator transport"); the web beacon simulator speaks the
// same messages: {from, to, ch, data} with 12-hex MACs and a base64 frame.
// Frames from our own MAC, for other MACs or on another channel are not
// heard. The channel is the emulated radio's (esp_wifi_emu.c), which the
// emulated Wi-Fi moves to its access point's channel while joined, exactly
// like the badge. A "radio" task polls the page and hands frames to the rx
// callback, as the badge's Wi-Fi task does.

#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "core/os_radio.h"
#include "esp_random.h"
#include "emu.h"

static const char *TAG = "os_radio";

#define POLL_MS     10

EM_JS(int, radio_js_open, (const char *me), {
    if (Module.mesh) return 1;
    const m = (Module.mesh = { me: UTF8ToString(me), rx: [], bc: null });
    if (typeof BroadcastChannel === "undefined") return 0;
    m.bc = new BroadcastChannel("solana-os-mesh");
    m.bc.onmessage = (e) => {
        let msg = e.data;
        if (typeof msg === "string") {
            try { msg = JSON.parse(msg); } catch (x) { return; }
        }
        if (!msg || typeof msg.from !== "string" || typeof msg.data !== "string") return;
        const from = msg.from.toLowerCase();
        const to = String(msg.to || "").toLowerCase();
        if (!/^[0-9a-f]{12}$/.test(from) || from === m.me) return;
        if (to !== m.me && to !== "ffffffffffff") return;
        if (m.rx.length >= 32) m.rx.shift();
        m.rx.push({ from, ch: Number(msg.ch), data: msg.data });
    };
    return 1;
});

// Next frame heard on channel ch: its length (bytes in buf, MAC in mac), or -1.
EM_JS(int, radio_js_take, (int ch, uint8_t *mac, uint8_t *buf, int cap), {
    const m = Module.mesh;
    while (m && m.rx.length) {
        const f = m.rx.shift();
        if (f.ch !== ch) continue;
        let bin;
        try { bin = atob(f.data); } catch (e) { continue; }
        if (!bin.length || bin.length > cap) continue;
        for (let i = 0; i < 6; i++) HEAPU8[mac + i] = parseInt(f.from.substr(i * 2, 2), 16);
        for (let i = 0; i < bin.length; i++) HEAPU8[buf + i] = bin.charCodeAt(i);
        return bin.length;
    }
    return -1;
});

EM_JS(void, radio_js_send, (const char *to, int ch, const uint8_t *data, int len), {
    const m = Module.mesh;
    if (!m || !m.bc) return;
    let s = "";
    for (let i = 0; i < len; i++) s += String.fromCharCode(HEAPU8[data + i]);
    m.bc.postMessage({ from: m.me, to: UTF8ToString(to), ch, data: btoa(s) });
});

static os_radio_rx_cb_t s_rx;

static void hex_mac(const uint8_t *mac, char out[13])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2] = hex[mac[i] >> 4];
        out[i * 2 + 1] = hex[mac[i] & 15];
    }
    out[12] = 0;
}

// A stable signal strength per sender (so "strongest first" lists stay put)
// with a little jitter.
static int fake_rssi(const uint8_t *mac)
{
    return -42 - (mac[5] % 32) - (int)(esp_random() % 4);
}

static void radio_task(void *arg)
{
    uint8_t mac[6], buf[OS_RADIO_FRAME_MAX];
    for (;;) {
        int n;
        while ((n = radio_js_take(os_radio_channel(), mac, buf, sizeof(buf))) > 0) {
            if (s_rx) {
                s_rx(mac, buf, n, fake_rssi(mac));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t os_radio_init(os_radio_rx_cb_t cb)
{
    char me[13];
    hex_mac(emu_mac(), me);
    s_rx = cb;
    if (!radio_js_open(me)) {
        ESP_LOGW(TAG, "no BroadcastChannel in this browser: the mesh stays silent");
        return ESP_ERR_NOT_SUPPORTED;
    }
    xTaskCreate(radio_task, "radio", 3072, NULL, 23, NULL);
    return ESP_OK;
}

int os_radio_channel(void)
{
    return emu_radio_channel();
}

esp_err_t os_radio_set_channel(int channel)
{
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

esp_err_t os_radio_send(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len <= 0 || len > OS_RADIO_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char to[13] = "ffffffffffff";
    if (mac) {
        hex_mac(mac, to);
    }
    radio_js_send(to, os_radio_channel(), data, len);
    return ESP_OK;
}

void os_radio_mac(uint8_t mac[6])
{
    memcpy(mac, emu_mac(), 6);
}

void os_radio_stay_awake(bool on)
{
    (void)on;               // the emulated radio never naps
}
