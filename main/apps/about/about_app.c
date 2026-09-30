// About: Solana OS version, badge identity, credits.

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "core/os.h"
#include "core/ui.h"

static void enter(lv_obj_t *root)
{
    lv_obj_t *logo = lv_image_create(root);
    lv_image_set_src(logo, &img_logomark_40);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *name = ui_label(root, &sol_font_20, COL_TEXT, "Solana OS");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 60);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *d = esp_app_get_description();
    lv_obj_t *ver = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(ver, "v%s  built %s\nbadge %02X%02X%02X  " LV_SYMBOL_BULLET "  %d apps installed",
                          d->version, d->date, mac[3], mac[4], mac[5], os_app_count);
    lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 88);

    ui_gradient_bar(root, 90, 126, OS_ROOT_W - 180, 3);

    lv_obj_t *blurb = ui_label(root, &sol_mono_12, COL_TEXT, "ESP32-C5 " LV_SYMBOL_BULLET " Wi-Fi 6 2.4/5 GHz "
                               LV_SYMBOL_BULLET " ESP-NOW");
    lv_obj_align(blurb, LV_ALIGN_TOP_MID, 0, 138);

    lv_obj_t *foundation = lv_image_create(root);
    lv_image_set_src(foundation, ui_light() ? &img_foundation_black : &img_foundation);
    lv_obj_align(foundation, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 12);

    ui_hint(root, "CANCEL exit");
}

const os_app_t app_about = {
    .id = "about",
    .name = "About",
    .icon = &img_logomark_40,
    .accent = COL_GREEN,
    .on_enter = enter,
};
