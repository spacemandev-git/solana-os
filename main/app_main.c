// Solana OS entry point: bring up the hardware in the order it needs, then
// hand over to the app manager.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/os_cloud.h"
#include "core/os_bridge.h"
#include "core/os_mesh.h"
#include "core/os_ota.h"
#include "core/console.h"
#include "lua/lua_host.h"
#include "game/game.h"

static const char *TAG = "solana_os";

// Purple -> green sweep across the strip, then off.
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
    ESP_LOGI(TAG, "Solana OS starting");
    esp_log_level_set("wifi", ESP_LOG_ERROR);       // the driver's PHY tables flood the console
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Order matters: the SD card must enter SPI mode before the LCD starts
    // using the shared bus (see components/bsp/bsp_sd.c).
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
    os_bridge_init();
    os_wifi_boot();     // after the mesh: the radio parks on its channel
    game_init();
    boot_sweep();
    lua_host_init();

    os_start();
    os_ota_init();
    console_start();    // last: the device console only sees commands registered before it
}
