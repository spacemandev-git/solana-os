// Emulator entry point. main() plays the part of the FreeRTOS scheduler and
// the esp_lvgl_port task: it starts app_main() on a "main" task, as ESP-IDF
// does, then loops forever: run every runnable task until it blocks, let
// LVGL render, and sleep (yielding to the browser) until the next deadline.

#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/os.h"
#include "emu.h"

#define MAX_SLEEP_MS 8      // input latency bound: page events land between sleeps

static volatile bool s_halt;

static void main_task(void *arg)
{
    app_main();
}

int main(void)
{
    emu_sched_init();
    xTaskCreate(main_task, "main", CONFIG_ESP_MAIN_TASK_STACK_SIZE, NULL, 1, NULL);
    while (!s_halt) {
        emu_sched_run();
        emu_display_pump();
        int64_t now = emu_now_us();
        int64_t wake = emu_sched_next_wake_us();
        int64_t ms = wake == INT64_MAX ? MAX_SLEEP_MS : (wake - now) / 1000;
        if (ms > emu_display_next_ms()) {
            ms = emu_display_next_ms();
        }
        if (ms > MAX_SLEEP_MS) {
            ms = MAX_SLEEP_MS;
        }
        emscripten_sleep(ms > 0 ? (unsigned)ms : 0);
    }
    return 0;
}

// ---- Page API ----

// Stop this instance (the page is about to boot a fresh one).
EMSCRIPTEN_KEEPALIVE void emu_halt(void)
{
    s_halt = true;
}

// Which raw button line each front-panel key is wired to (os_keys.h
// defaults), so the page presses the same lines the physical buttons would.
EMSCRIPTEN_KEEPALIVE uint32_t emu_button_line(int key)
{
    static const uint32_t wiring[OS_KEY_COUNT] = {
        [OS_KEY_UP] = OS_BTN_UP, [OS_KEY_DOWN] = OS_BTN_DOWN, [OS_KEY_LEFT] = OS_BTN_LEFT,
        [OS_KEY_RIGHT] = OS_BTN_RIGHT, [OS_KEY_SELECT] = OS_BTN_SELECT, [OS_KEY_BACK] = OS_BTN_BACK,
        [OS_KEY_HOME] = OS_BTN_HOME,
    };
    return key >= 0 && key < OS_KEY_COUNT ? wiring[key] : 0;
}

EMSCRIPTEN_KEEPALIVE const char *emu_current_app(void)
{
    const os_app_t *app = os_current();
    return app ? app->id : "";
}
