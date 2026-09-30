// Emulated TCA9534 buttons + BOOT. The page sets the raw port state (PB1..PB6
// as bits 0..5, BOOT as bit 6, 1 = pressed) the way the expander and GPIO28
// read on the badge; the rest mirrors bsp_buttons.c: the "interrupt" wakes a
// button task early, a 50 ms poll covers anything missed, and the callback
// sees (pressed, changed). Key mapping and calibration stay in the firmware.

#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bsp.h"

#define POLL_MS     50
#define ALL_BUTTONS (((1u << BOARD_BTN_COUNT) - 1) | BSP_BTN_BOOT)

static SemaphoreHandle_t s_irq;
static bsp_btn_cb_t s_cb;
static volatile uint32_t s_pressed, s_port;

static void button_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(POLL_MS));
        uint32_t now = s_port;
        uint32_t changed = now ^ s_pressed;
        if (changed) {
            s_pressed = now;
            if (s_cb) {
                s_cb(now, changed);
            }
        }
    }
}

esp_err_t bsp_buttons_start(bsp_btn_cb_t cb)
{
    s_cb = cb;
    s_irq = xSemaphoreCreateBinary();
    s_pressed = s_port;
    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
    return ESP_OK;
}

uint32_t bsp_buttons_get(void)
{
    return s_pressed;
}

// Page API: raw button lines, 1 = pressed.
EMSCRIPTEN_KEEPALIVE void emu_buttons_set(uint32_t pressed)
{
    s_port = pressed & ALL_BUTTONS;
    if (s_irq) {
        xSemaphoreGive(s_irq);      // the expander's INT line
    }
}
