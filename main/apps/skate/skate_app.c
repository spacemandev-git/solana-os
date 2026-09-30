// Skate Monkey: Flappy Bird on a skateboard. UP or SELECT ollies the monkey
// over and under rows of mushrooms; touching a cap, a stem or the ground ends
// the run. The best score survives reboots (NVS "skate/best").
//
// The OS tick (100 ms) is too coarse for a game, so the app runs its own
// ~30 fps lv_timer. LVGL timers fire in the LVGL task with the lock held, the
// same guarantee as app callbacks, and the emulator drives them the same way.
// Physics is fixed point (1/16 px) because the C5 has no FPU. There is no
// canvas: a 320x218 RGB565 buffer would eat 140 KB, so the world is a few
// dozen plain objects that are moved, never recreated, while playing.

#include <stdarg.h>
#include "esp_random.h"
#include "nvs.h"
#include "bsp.h"
#include "core/os.h"
#include "core/ui.h"

#define FRAME_MS    33
#define FP          16                      // fixed point: 1 px = 16

#define W           OS_ROOT_W
#define H           OS_ROOT_H
#define GROUND_Y    (H - 16)

#define COLS        3
#define SPACING     130                     // px between mushroom columns
#define CAP_W       46
#define CAP_H       20
#define STEM_W      18
#define STEM_X      ((CAP_W - STEM_W) / 2)
#define GAP         90                      // opening between the two caps
#define MARGIN      20                      // smallest cap-to-edge distance

#define MONKEY_X    64
#define MONKEY_W    32
#define MONKEY_H    37

#define GRAVITY     7                       // 1/16 px per frame^2
#define OLLIE       (-80)                   // 1/16 px per frame
#define MAX_FALL    (9 * FP)

#define DASHES      6
#define DASH_GAP    (W / DASHES + 10)

// True colours for the critters; the brand palette has no brown or cream.
#define FUR         0x7A4A26
#define FACE        0xE9C39B
#define STEM        0xEFE3C8
#define STEM_EDGE   0xB9A98A
#define SPOT        0xFFFFFF

typedef enum { ST_READY, ST_PLAY, ST_DEAD } state_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *top_stem, *top_cap;
    lv_obj_t *bot_stem, *bot_cap;
    int x;                                  // px, left edge of the caps
    int gap;                                // px, top of the opening
    bool scored;
} column_t;

static lv_timer_t *s_timer;
static lv_obj_t *s_world, *s_monkey, *s_score, *s_msg;
static lv_obj_t *s_dash[DASHES];
static int s_dash_x[DASHES];         // tracked here: lv_obj_get_x() lags until layout
static column_t s_col[COLS];
static state_t s_state;
static int32_t s_y, s_vy;                   // monkey top, fixed point
static int s_speed_fp, s_carry, s_score_n, s_best, s_frame, s_dead_frames, s_led_frames;

// ---- Persistence ----

static void best_load(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("skate", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "best", &v);
        nvs_close(h);
    }
    s_best = v;
}

static void best_save(void)
{
    nvs_handle_t h;
    if (nvs_open("skate", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "best", s_best);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- LEDs: a short green blip per point, orange on a wipe-out ----

static void leds(uint8_t r, uint8_t g, uint8_t b, int frames)
{
    bsp_rgb_fill(r, g, b);
    bsp_rgb_show();
    s_led_frames = frames;
}

static void leds_tick(void)
{
    if (s_led_frames > 0 && --s_led_frames == 0) {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
    }
}

// ---- Drawing helpers ----

static lv_obj_t *blob(lv_obj_t *parent, int x, int y, int w, int h, uint32_t hex, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *group(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *stem(lv_obj_t *parent)
{
    lv_obj_t *o = blob(parent, STEM_X, 0, STEM_W, 1, STEM, 4);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(STEM_EDGE), 0);
    return o;
}

// A mushroom cap: a dome with a flat underside. LVGL rounds every corner, so
// the dome is a pill twice the cap's height, half of it clipped away by its
// parent. `up` = dome on top (a mushroom growing from the ground).
static lv_obj_t *cap(lv_obj_t *parent, uint32_t col, bool up)
{
    lv_obj_t *g = group(parent, CAP_W, CAP_H);
    lv_obj_t *dome = blob(g, 0, up ? 0 : -CAP_H, CAP_W, 2 * CAP_H, 0, CAP_H);
    lv_obj_set_style_bg_color(dome, ui_col(col), 0);
    int y0 = up ? 0 : 4;
    blob(g, 8, y0 + 7, 7, 6, SPOT, LV_RADIUS_CIRCLE);
    blob(g, 20, y0 + 2, 6, 5, SPOT, LV_RADIUS_CIRCLE);
    blob(g, 31, y0 + 8, 6, 5, SPOT, LV_RADIUS_CIRCLE);
    return g;
}

static lv_obj_t *build_monkey(lv_obj_t *parent)
{
    lv_obj_t *m = group(parent, MONKEY_W, MONKEY_H);
    blob(m, 1, 18, 8, 4, FUR, 2);                   // tail
    blob(m, 7, 16, 17, 14, FUR, 6);                 // body
    blob(m, 5, 4, 7, 7, FACE, LV_RADIUS_CIRCLE);    // ear
    blob(m, 9, 0, 20, 19, FUR, LV_RADIUS_CIRCLE);   // head
    blob(m, 16, 6, 13, 11, FACE, LV_RADIUS_CIRCLE); // face
    blob(m, 22, 7, 3, 4, 0x000000, 1);              // eye
    blob(m, 21, 13, 6, 2, 0x5A3218, 1);             // grin
    blob(m, 20, 19, 9, 4, FUR, 2);                  // arm, reaching forward

    // The deck wears the Solana gradient.
    lv_obj_t *deck = blob(m, 0, 29, MONKEY_W, 4, 0, 2);
    lv_obj_set_style_bg_color(deck, lv_color_hex(COL_PURPLE), 0);
    lv_obj_set_style_bg_grad_color(deck, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_bg_grad_dir(deck, LV_GRAD_DIR_HOR, 0);
    blob(m, 4, 32, 5, 5, 0x3A3A44, LV_RADIUS_CIRCLE);
    blob(m, MONKEY_W - 9, 32, 5, 5, 0x3A3A44, LV_RADIUS_CIRCLE);
    return m;
}

static void build_column(column_t *c, int i)
{
    static const uint32_t caps[COLS] = { COL_ORANGE, COL_PINK, COL_PURPLE };
    c->obj = group(s_world, CAP_W, GROUND_Y);
    c->top_stem = stem(c->obj);
    c->top_cap = cap(c->obj, caps[i], false);
    c->bot_stem = stem(c->obj);
    c->bot_cap = cap(c->obj, caps[i], true);
}

// Pick a fresh opening for a column and lay out its two mushrooms: one hangs
// from the top with its cap facing down, one grows from the ground.
static void column_reset(column_t *c, int x)
{
    int span = GROUND_Y - 2 * MARGIN - GAP;
    c->x = x;
    c->gap = MARGIN + (int)(esp_random() % (uint32_t)(span + 1));
    c->scored = false;

    int top_cap_y = c->gap - CAP_H;
    lv_obj_set_pos(c->top_stem, STEM_X, -4);
    lv_obj_set_height(c->top_stem, top_cap_y + 8);
    lv_obj_set_pos(c->top_cap, 0, top_cap_y);

    int bot_cap_y = c->gap + GAP;
    lv_obj_set_pos(c->bot_stem, STEM_X, bot_cap_y + CAP_H - 4);
    lv_obj_set_height(c->bot_stem, GROUND_Y - bot_cap_y - CAP_H + 8);
    lv_obj_set_pos(c->bot_cap, 0, bot_cap_y);

    lv_obj_set_x(c->obj, x);
}

// ---- Game ----

static void show_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[96];
    lv_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(s_msg, buf);
    lv_obj_set_hidden(s_msg, false);
}

static void set_score(int n)
{
    s_score_n = n;
    lv_label_set_text_fmt(s_score, "%d", n);
}

static void reset(void)
{
    s_state = ST_READY;
    s_y = (GROUND_Y / 2 - MONKEY_H) * FP;
    s_vy = 0;
    s_speed_fp = 2 * FP;
    s_carry = 0;
    s_frame = 0;
    for (int i = 0; i < COLS; i++) {
        column_reset(&s_col[i], W + 40 + i * SPACING);
    }
    lv_obj_set_pos(s_monkey, MONKEY_X, s_y / FP);
    set_score(0);
    show_msg("SKATE MONKEY\n\n" LV_SYMBOL_UP " or SELECT to ollie\nbest %d", s_best);
}

static void ollie(void)
{
    if (s_state == ST_READY) {
        s_state = ST_PLAY;
        lv_obj_set_hidden(s_msg, true);
    }
    s_vy = OLLIE;
}

static void wipe_out(void)
{
    s_state = ST_DEAD;
    s_dead_frames = 0;
    bool record = s_score_n > s_best;
    if (record) {
        s_best = s_score_n;
        best_save();
    }
    leds(OS_LED_MAX, OS_LED_MAX / 3, 0, 20);
    show_msg("%s\n\nscore %d   best %d\nSELECT again  CANCEL exit",
             record ? "NEW BEST!" : "WIPED OUT", s_score_n, s_best);
}

static bool overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static bool hits(const column_t *c, int mx, int my, int mw, int mh)
{
    int top_cap_y = c->gap - CAP_H, bot_cap_y = c->gap + GAP;
    return overlaps(mx, my, mw, mh, c->x, top_cap_y, CAP_W, CAP_H)
        || overlaps(mx, my, mw, mh, c->x + STEM_X, 0, STEM_W, top_cap_y)
        || overlaps(mx, my, mw, mh, c->x, bot_cap_y, CAP_W, CAP_H)
        || overlaps(mx, my, mw, mh, c->x + STEM_X, bot_cap_y + CAP_H, STEM_W, GROUND_Y);
}

static void scroll_ground(int dx)
{
    for (int i = 0; i < DASHES; i++) {
        s_dash_x[i] -= dx;
        if (s_dash_x[i] < -12) {
            s_dash_x[i] += DASHES * DASH_GAP;
        }
        lv_obj_set_x(s_dash[i], s_dash_x[i]);
    }
}

static void frame(lv_timer_t *t)
{
    (void)t;
    s_frame++;
    leds_tick();

    if (s_state == ST_READY) {
        // Idle bob so the monkey looks alive before the first ollie.
        int bob = (s_frame / 6) % 4;
        lv_obj_set_y(s_monkey, s_y / FP + (bob == 3 ? 1 : bob == 1 ? -1 : 0));
        scroll_ground(1);
        return;
    }
    if (s_state == ST_DEAD) {
        s_dead_frames++;
        // Tumble to the ground.
        if (s_y / FP < GROUND_Y - MONKEY_H) {
            s_vy = s_vy + GRAVITY * 2 > MAX_FALL ? MAX_FALL : s_vy + GRAVITY * 2;
            s_y += s_vy;
            if (s_y / FP > GROUND_Y - MONKEY_H) {
                s_y = (GROUND_Y - MONKEY_H) * FP;
            }
            lv_obj_set_y(s_monkey, s_y / FP);
        }
        return;
    }

    s_vy = s_vy + GRAVITY > MAX_FALL ? MAX_FALL : s_vy + GRAVITY;
    s_y += s_vy;
    if (s_y < 0) {
        s_y = 0;
        s_vy = 0;
    }
    int my = s_y / FP;
    lv_obj_set_y(s_monkey, my);

    // Sub-pixel scrolling: move by whole pixels, carry the remainder.
    s_carry += s_speed_fp;
    int dx = s_carry / FP;
    s_carry %= FP;
    scroll_ground(dx);

    // A slightly forgiving hitbox: the drawing's corners are round.
    int hx = MONKEY_X + 3, hy = my + 2, hw = MONKEY_W - 6, hh = MONKEY_H - 3;
    for (int i = 0; i < COLS; i++) {
        column_t *c = &s_col[i];
        c->x -= dx;
        if (c->x < -CAP_W) {
            column_reset(c, c->x + COLS * SPACING);
        } else {
            lv_obj_set_x(c->obj, c->x);
        }
        if (!c->scored && c->x + CAP_W < MONKEY_X) {
            c->scored = true;
            set_score(s_score_n + 1);
            leds(0, OS_LED_MAX, OS_LED_MAX / 2, 3);
            // Speed up a little every 5 points, up to 3.5 px/frame.
            if (s_score_n % 5 == 0 && s_speed_fp < 3 * FP + FP / 2) {
                s_speed_fp += FP / 4;
            }
        }
        if (hits(c, hx, hy, hw, hh)) {
            wipe_out();
            return;
        }
    }
    if (my + MONKEY_H >= GROUND_Y) {
        wipe_out();
    }
}

// ---- App ----

static void enter(lv_obj_t *root)
{
    best_load();

    // Own clipping container, so mushrooms sliding off the edges never make
    // the OS root scrollable.
    s_world = group(root, W, H);

    for (int i = 0; i < COLS; i++) {
        build_column(&s_col[i], i);
    }

    lv_obj_t *ground = blob(s_world, 0, GROUND_Y, W, H - GROUND_Y, 0, 0);
    lv_obj_set_style_bg_color(ground, ui_col(COL_PANEL_HI), 0);
    ui_gradient_bar(s_world, 0, GROUND_Y, W, 2);
    for (int i = 0; i < DASHES; i++) {
        s_dash_x[i] = i * DASH_GAP;
        s_dash[i] = blob(s_world, s_dash_x[i], GROUND_Y + 8, 12, 2, 0, 1);
        lv_obj_set_style_bg_color(s_dash[i], ui_col(COL_DIM), 0);
    }

    s_monkey = build_monkey(s_world);

    s_score = ui_label(s_world, &sol_font_32, COL_TEXT, "0");
    lv_obj_align(s_score, LV_ALIGN_TOP_MID, 0, 4);

    s_msg = ui_label(s_world, &sol_font_14, COL_TEXT, "");
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(s_msg, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_msg, ui_col(COL_PANEL), 0);
    lv_obj_set_style_pad_all(s_msg, 8, 0);
    lv_obj_set_style_radius(s_msg, 6, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, 6);

    reset();
    s_timer = lv_timer_create(frame, FRAME_MS, NULL);
}

static void on_leave(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
    s_led_frames = 0;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (repeat) {
        return k != OS_KEY_BACK;
    }
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_SELECT:
        if (s_state == ST_DEAD) {
            // Ignore a panicked mash right after the crash.
            if (s_dead_frames > 12) {
                reset();
            }
        } else {
            ollie();
        }
        return true;
    default:
        return false;       // CANCEL exits to the launcher
    }
}

const os_app_t app_skate = {
    .id = "skate",
    .name = "Skate Monkey",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_ORANGE,
    .on_enter = enter,
    .on_exit = on_leave,
    .on_key = on_key,
};
