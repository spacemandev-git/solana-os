// Emulated battery: a 1000 mAh LiPo that starts fairly full and drains slowly
// while the page is open, with a little ADC noise. Plausible, not measured.

#include "esp_random.h"
#include "esp_timer.h"
#include "bsp.h"

static bool s_ready;

esp_err_t bsp_battery_init(void)
{
    s_ready = true;
    return ESP_OK;
}

int bsp_battery_mv(void)
{
    if (!s_ready) {
        return -1;
    }
    int minutes = (int)(esp_timer_get_time() / 60000000LL);
    int mv = 4040 - minutes * 3;            // ~180 mV per hour of use
    if (mv < 3620) {
        mv = 3620;
    }
    return mv + (int)(esp_random() % 9) - 4;
}

int bsp_battery_percent(int mv)
{
    // Same linear Li-ion approximation as bsp_battery.c: 3.30 V empty .. 4.15 V full.
    if (mv <= 3300) {
        return 0;
    }
    if (mv >= 4150) {
        return 100;
    }
    return (mv - 3300) * 100 / (4150 - 3300);
}
