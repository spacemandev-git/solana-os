// Button calibration: asks for each front button in turn, records which
// expander line it is on, and saves the map to NVS. BOOT aborts.

#include <string.h>
#include "esp_timer.h"
#include "os.h"
#include "ui.h"

static const char *const prompts[OS_KEY_FRONT_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "SELECT", "CANCEL"};

// Layout of the front panel, roughly as on the board (d-pad left, SELECT and
// CANCEL on a diagonal to the right).
static const int16_t pos[OS_KEY_FRONT_COUNT][2] = {
    {70, 100}, {70, 164}, {30, 132}, {110, 132}, {206, 160}, {256, 118},
};

static lv_obj_t *s_prompt, *s_step, *s_keys[OS_KEY_FRONT_COUNT];
static uint32_t s_map[OS_KEY_FRONT_COUNT];
static int s_cur;
static int64_t s_done_at;

static void style_key(int i)
{
    lv_obj_t *k = s_keys[i];
    bool cur = i == s_cur, done = i < s_cur;
    lv_obj_set_style_bg_color(k, ui_col(cur ? COL_PURPLE : done ? COL_PANEL_HI : COL_PANEL), 0);
    lv_obj_set_style_bg_grad_color(k, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_grad_dir(k, cur ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(k, ui_col(done ? COL_GREEN : COL_BORDER), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(k, 0), ui_col(cur ? COL_BG : done ? COL_GREEN : COL_DIM), 0);
}

static void render(void)
{
    for (int i = 0; i < OS_KEY_FRONT_COUNT; i++) {
        style_key(i);
    }
    if (s_cur < OS_KEY_FRONT_COUNT) {
        lv_label_set_text_fmt(s_prompt, "Press %s", prompts[s_cur]);
        lv_label_set_text_fmt(s_step, "Step %d of %d", s_cur + 1, OS_KEY_FRONT_COUNT);
    } else {
        lv_label_set_text(s_prompt, "Saved");     // sol_font_32 has no symbol fallback
        lv_label_set_text(s_step, "Buttons calibrated");
    }
}

static void enter(lv_obj_t *root)
{
    s_cur = 0;
    s_done_at = 0;
    memset(s_map, 0, sizeof(s_map));
    s_prompt = ui_label(root, &sol_font_32, COL_TEXT, "");
    lv_obj_align(s_prompt, LV_ALIGN_TOP_MID, 0, 10);
    s_step = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_align(s_step, LV_ALIGN_TOP_MID, 0, 50);
    for (int i = 0; i < OS_KEY_FRONT_COUNT; i++) {
        lv_obj_t *k = ui_panel(root, pos[i][0], pos[i][1] - 20, i >= OS_KEY_SELECT ? 64 : 44, 26);
        lv_obj_set_style_pad_all(k, 0, 0);
        lv_obj_set_style_radius(k, 13, 0);
        lv_obj_t *l = ui_label(k, &sol_mono_12, COL_DIM, prompts[i]);
        lv_obj_center(l);
        s_keys[i] = k;
    }
    ui_hint(root, "Press each button when asked   BOOT cancel");
    render();
}

static void on_button(uint32_t bit)
{
    if (s_cur >= OS_KEY_FRONT_COUNT) {
        os_launch(&os_launcher);
        return;
    }
    for (int i = 0; i < s_cur; i++) {
        if (s_map[i] == bit) {
            os_toast("That one is %s already", prompts[i]);
            return;
        }
    }
    s_map[s_cur++] = bit;
    if (s_cur == OS_KEY_FRONT_COUNT) {
        os_keys_save(s_map);
        s_done_at = esp_timer_get_time();
    }
    render();
}

static void on_tick(void)
{
    if (s_done_at && esp_timer_get_time() - s_done_at > 1500000) {
        s_done_at = 0;
        os_launch(&os_launcher);
    }
}

const os_app_t os_calibrate = {
    .id = "calib",
    .name = "Calibrate buttons",
    .on_enter = enter,
    .on_button = on_button,
    .on_tick = on_tick,
};
