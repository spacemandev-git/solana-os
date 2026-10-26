// Software update (hidden app "update", opened from Settings): shows the
// Solana OS build hello offers, checks battery, Wi-Fi and card, asks in an OS
// confirm, then follows os_ota through download, check, install and restart
// (docs/HHGTS.md "Software updates"). From the download on, os_hold() keeps
// the badge on this screen: CANCEL stops a download, nothing stops an install.

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_ota.h"
#include "core/ui.h"

extern const os_app_t app_wifi;

typedef struct {
    lv_obj_t *info, *title, *body, *bar, *hint;
    int64_t checking_ms;                // SELECT asked the server again (esp_timer ms), 0 = no
} state_t;

static state_t *s;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Ticks redraw only what changed.
static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void render(void)
{
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    os_ota_info_t o;
    os_ota_get(&o);
    const char *running = esp_app_get_description()->version;
    int batt = os_ota_battery();

    char info[160], title[64], body[160], hint[64], ago[24];
    char batt_txt[16];
    if (batt < 0) {
        strlcpy(batt_txt, "--", sizeof(batt_txt));
    } else {
        snprintf(batt_txt, sizeof(batt_txt), "%d %%", batt);
    }
    if (in->update[0]) {
        snprintf(info, sizeof(info), "Installed  %s\nAvailable  %s  (%d.%d MB)\nBattery    %s", running, in->update,
                 in->update_size / 1000000, in->update_size / 100000 % 10, batt_txt);
    } else {
        snprintf(info, sizeof(info), "Installed  %s\nBattery    %s", running, batt_txt);
    }
    uint32_t body_col = COL_DIM;
    int bar = -1;
    const char *hold = NULL;

    switch (o.phase) {
    case OS_OTA_DOWNLOADING:
        snprintf(title, sizeof(title), "Downloading %s... %d %%", o.version, o.pct);
        strlcpy(body, "Keep Wi-Fi on. It goes to the microSD card first, then into the flash.", sizeof(body));
        strlcpy(hint, "CANCEL stop", sizeof(hint));
        bar = o.pct;
        hold = "Updating Solana OS: CANCEL stops the download";
        break;
    case OS_OTA_VERIFYING:
        strlcpy(title, "Checking the download...", sizeof(title));
        strlcpy(body, "Keep the badge on.", sizeof(body));
        hint[0] = 0;
        bar = 100;
        hold = "Installing Solana OS: keep the badge on";
        break;
    case OS_OTA_INSTALLING:
        snprintf(title, sizeof(title), "Installing %s... %d %%", o.version, o.pct);
        strlcpy(body, "Keep the badge on. Don't switch it off or press RST until it restarts.", sizeof(body));
        body_col = COL_WARN;
        hint[0] = 0;
        bar = o.pct;
        hold = "Installing Solana OS: keep the badge on";
        break;
    case OS_OTA_RESTARTING:
        snprintf(title, sizeof(title), "Done. Restarting into %s...", o.version);
        strlcpy(body, "Your apps, data and !LAMPs stay as they are.", sizeof(body));
        body_col = COL_OK;
        hint[0] = 0;
        bar = 100;
        hold = "Restarting...";
        break;
    case OS_OTA_FAILED:
    case OS_OTA_IDLE:
        if (in->update[0]) {
            const char *why = os_ota_blocker();
            if (o.phase == OS_OTA_FAILED) {
                strlcpy(title, "The update didn't finish", sizeof(title));
                snprintf(body, sizeof(body), "%s Nothing changed: still on %s.", o.error ? o.error : "", running);
                body_col = COL_ERR;
            } else {
                snprintf(title, sizeof(title), "Solana OS %s is ready", in->update);
                snprintf(body, sizeof(body), "%s", why ? why : "About a minute. Needs 70 % battery or USB power.");
                body_col = why ? COL_WARN : COL_DIM;
            }
            snprintf(hint, sizeof(hint), "%sCANCEL exit", why ? "" : o.phase == OS_OTA_FAILED ? "SELECT try again  " : "SELECT update  ");
        } else if (!os_api_online()) {
            strlcpy(title, "Turn Wi-Fi on to check", sizeof(title));
            snprintf(body, sizeof(body), "Solana OS %s is installed. Updates come from the server.", running);
            strlcpy(hint, "SELECT Wi-Fi  CANCEL exit", sizeof(hint));
        } else {
            bool checking = s->checking_ms && (in->busy || in->synced_ms < s->checking_ms);
            snprintf(title, sizeof(title), "%s", checking ? "Checking..." : "Solana OS is up to date");
            if (in->synced_ms) {
                snprintf(body, sizeof(body), "%s is the latest for this badge. Checked %s.", running,
                         ui_fmt_ago(ago, sizeof(ago), now_ms() - in->synced_ms));
            } else {
                snprintf(body, sizeof(body), "%s is installed.", running);
            }
            strlcpy(hint, "SELECT check again  CANCEL exit", sizeof(hint));
        }
        break;
    }
    lv_free(in);
    os_hold(hold);

    set_text(s->info, info);
    set_text(s->title, title);
    set_text(s->body, body);
    set_text(s->hint, hint);
    lv_obj_set_style_text_color(s->body, ui_col(body_col), 0);
    lv_obj_set_hidden(s->bar, bar < 0);
    if (bar >= 0 && lv_bar_get_value(s->bar) != bar) {
        lv_bar_set_value(s->bar, bar, LV_ANIM_OFF);
    }
}

static void answer(bool yes, void *ctx)
{
    if (!yes || !s) {
        return;
    }
    esp_err_t err = os_ota_start();
    if (err != ESP_OK) {
        const char *why = os_ota_blocker();
        os_toast("%s", why ? why : "Couldn't start the update. Try again.");
    }
    render();
}

static void ask(void)
{
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    char text[200];
    snprintf(text, sizeof(text), "Update Solana OS %s to %s? It downloads %d.%d MB and takes about a minute. "
             "Keep the badge on until it restarts.", esp_app_get_description()->version, in->update,
             in->update_size / 1000000, in->update_size / 100000 % 10);
    lv_free(in);
    os_confirm(text, answer, NULL);
}

static void enter(lv_obj_t *root)
{
    s = lv_malloc(sizeof(*s));
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->info = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s->info, 12, 10);

    s->title = ui_label(root, &sol_font_20, COL_TEXT, "");
    lv_obj_set_pos(s->title, 12, 72);
    lv_obj_set_width(s->title, OS_ROOT_W - 24);
    lv_label_set_long_mode(s->title, LV_LABEL_LONG_DOT);

    s->bar = lv_bar_create(root);
    lv_obj_set_pos(s->bar, 12, 104);
    lv_obj_set_size(s->bar, OS_ROOT_W - 24, 8);
    lv_bar_set_range(s->bar, 0, 100);
    lv_obj_set_style_bg_color(s->bar, ui_col(COL_PANEL_HI), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->bar, ui_col(COL_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s->bar, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s->bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_hidden(s->bar, true);

    s->body = ui_label(root, &sol_font_14, COL_DIM, "");
    lv_obj_set_pos(s->body, 12, 122);
    lv_obj_set_width(s->body, OS_ROOT_W - 24);
    lv_label_set_long_mode(s->body, LV_LABEL_LONG_WRAP);

    s->hint = ui_hint(root, "");
    render();
}

static void leave(void)
{
    os_hold(NULL);
    lv_free(s);
    s = NULL;
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s) {
        return false;
    }
    if (repeat) {
        return true;
    }
    os_ota_info_t o;
    os_ota_get(&o);
    bool busy = o.phase == OS_OTA_DOWNLOADING || o.phase == OS_OTA_VERIFYING || o.phase == OS_OTA_INSTALLING ||
                o.phase == OS_OTA_RESTARTING;
    if (k == OS_KEY_BACK) {
        if (o.phase == OS_OTA_DOWNLOADING) {
            os_ota_cancel();
            render();
        }
        return busy;                    // otherwise CANCEL exits
    }
    if (k != OS_KEY_SELECT || busy) {
        return true;
    }
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (!in) {
        return true;
    }
    os_cloud_get_info(in);
    bool offer = in->update[0] != 0;
    lv_free(in);
    if (offer) {
        const char *why = os_ota_blocker();
        if (why) {
            os_toast("%s", why);
        } else {
            ask();
        }
    } else if (!os_api_online()) {
        os_launch(&app_wifi);
    } else {
        s->checking_ms = now_ms();
        os_cloud_refresh();
    }
    render();
    return true;
}

static void on_tick(void)
{
    if (s) {
        render();
    }
}

const os_app_t app_update = {
    .id = "update",
    .name = "Software update",
    .symbol = LV_SYMBOL_DOWNLOAD,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = on_key,
    .on_tick = on_tick,
};
