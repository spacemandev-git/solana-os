// Wi-Fi at boot (docs/HHGTS.md "Badge behaviour"): with a saved network the
// badge tries to join it for 30 s, then turns Wi-Fi off again, so a badge away
// from its network neither keeps scanning nor keeps the radio off the mesh
// channel. Portable: only the os_wifi API, from a short-lived task.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "os_wifi.h"

static const char *TAG = "wifi_boot";

#define BOOT_JOIN_MS    30000

static void boot_join_task(void *arg)
{
    char ssid[33] = "";
    os_wifi_saved_ssid(ssid, sizeof(ssid));
    os_wifi_info_t in;
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        os_wifi_get_info(&in);
        if (in.state == OS_WIFI_CONNECTED || !in.enabled || strcmp(in.ssid, ssid)) {
            break;          // joined, or the user turned it off or picked another network
        }
        bool late = xTaskGetTickCount() - start >= pdMS_TO_TICKS(BOOT_JOIN_MS);
        if (late && in.state != OS_WIFI_SUSPENDED) {
            ESP_LOGI(TAG, "'%s' not found in %d s: Wi-Fi off", ssid, BOOT_JOIN_MS / 1000);
            os_wifi_set_enabled(false);
            break;
        }
        if (late) {
            break;          // lent to a mesh app: resuming rejoins, as after any join
        }
        if (in.state == OS_WIFI_FAILED) {
            os_wifi_set_enabled(true);      // gave up early: keep listening until 30 s
        }
    }
    vTaskDelete(NULL);
}

void os_wifi_boot(void)
{
    char ssid[33];
    if (!os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        return;             // nothing saved: Wi-Fi stays off until the user joins one
    }
    ESP_LOGI(TAG, "looking for '%s'", ssid);
    os_wifi_set_enabled(true);
    if (xTaskCreate(boot_join_task, "wifi_boot", 2560, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no task: Wi-Fi stays on");
    }
}
