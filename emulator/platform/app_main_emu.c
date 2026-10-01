// Emulator counterpart of main/app_main.c: the same boot order over the
// emulated hardware. Keep it in step with app_main.c when the boot sequence
// changes (a new service to start, a new init step).

#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/os_cloud.h"
#include "core/os_mesh.h"
#include "core/console.h"
#include "lua/lua_host.h"
#include "game/game.h"

static const char *TAG = "solana_os";

// The emulated badge talks to the server that serves the page (server/API.md:
// one origin in production, Vite proxies /api in dev).
EM_JS(void, page_origin, (char *out, int cap), { stringToUTF8(location.origin, out, cap); });

// Purple -> green sweep across the strip, then off (as in app_main.c).
static void boot_sweep(void)
{
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        int t = i * 255 / (BOARD_RGB_COUNT - 1);
        uint8_t r = (0x99 + (0x14 - 0x99) * t / 255) * OS_LED_MAX / 255;
        uint8_t g = (0x45 + (0xF1 - 0x45) * t / 255) * OS_LED_MAX / 255;
        uint8_t b = (0xFF + (0x95 - 0xFF) * t / 255) * OS_LED_MAX / 255;
        bsp_rgb_set(i, r, g, b);
        bsp_rgb_show();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(400));
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Solana OS starting (browser emulator)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spi_bus_init());
    bsp_sd_mount();

    bsp_display_start();
    os_splash();

    ESP_ERROR_CHECK(bsp_rgb_init());
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable");
    }
    if (os_wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "wifi unavailable");
    }
    os_cloud_init();
    os_mesh_init();
    game_init();
    char origin[96];
    page_origin(origin, sizeof(origin));
    os_cloud_set_server(origin);
    boot_sweep();
    lua_host_init();

    console_start();
    os_start();
}
