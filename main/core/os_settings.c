// User settings: a small table of NVS-backed integers with defaults. Add a
// setting by extending os_setting_t and s_defs, then a row in the Settings app.

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs.h"
#include "os_settings.h"

static const char *TAG = "settings";

static const struct {
    const char *key;        // NVS key, max 15 chars
    int32_t def;
} s_defs[OS_SET_COUNT] = {
    [OS_SET_THEME] = {"theme", OS_THEME_DARK},
};

static int32_t s_val[OS_SET_COUNT];
static bool s_loaded;

static void load(void)
{
    nvs_handle_t h;
    bool open = nvs_open("settings", NVS_READONLY, &h) == ESP_OK;
    for (int i = 0; i < OS_SET_COUNT; i++) {
        s_val[i] = s_defs[i].def;
        if (open) {
            nvs_get_i32(h, s_defs[i].key, &s_val[i]);
        }
    }
    if (open) {
        nvs_close(h);
    }
    s_loaded = true;
}

int os_setting(os_setting_t s)
{
    if (!s_loaded) {
        load();
    }
    return s < OS_SET_COUNT ? s_val[s] : 0;
}

void os_setting_set(os_setting_t s, int value)
{
    if (s >= OS_SET_COUNT) {
        return;
    }
    if (!s_loaded) {
        load();
    }
    s_val[s] = value;
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, s_defs[s].key, value);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "can't save %s", s_defs[s].key);
    }
}
