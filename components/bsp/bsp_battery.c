// Battery voltage on GPIO1 (ADC1, 1:1 divider). Mirrors the stock firmware:
// 8 calibrated samples at 12 dB, averaged, times two.

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "bsp.h"

static const char *TAG = "bsp_battery";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

esp_err_t bsp_battery_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC_GPIO, &unit, &s_chan), TAG, "io");
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "unit");
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "chan");
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali), TAG, "cali");
    return ESP_OK;
}

int bsp_battery_mv(void)
{
    if (!s_adc) {
        return -1;
    }
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK ||
                adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return -1;
        }
        sum += mv;
    }
    return sum / 8 * BOARD_BAT_DIVIDER;
}

int bsp_battery_percent(int mv)
{
    // Linear Li-ion approximation, 3.30 V empty .. 4.15 V full.
    if (mv <= 3300) {
        return 0;
    }
    if (mv >= 4150) {
        return 100;
    }
    return (mv - 3300) * 100 / (4150 - 3300);
}
