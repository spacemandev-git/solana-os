// The flash side of os_ota (os_ota_flash.h): ESP-IDF's OTA API, badge-only.
//
// Sequential writes erase each sector just before writing it, so the
// progress bar moves from the start instead of after a ~15 s erase. The
// image's own header is checked first (a Solana OS build of the version hello
// offered); esp_ota_end() then validates segments and checksum, and only
// esp_ota_set_boot_partition() touches otadata, which is the one write that
// changes what boots.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "os_ota_flash.h"

static const char *TAG = "os_ota";

#define CHUNK 4096

esp_err_t os_ota_flash(const char *path, size_t size, const char *version, volatile int *pct, const char **why)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next || size > next->size) {
        *why = "The update doesn't fit this badge's flash.";
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(path, "rb");
    uint8_t *buf = f ? malloc(CHUNK) : NULL;
    if (!buf) {
        if (f) {
            fclose(f);
        }
        *why = f ? "Not enough memory. Restart the badge and try again." : "The download is gone from the card.";
        return ESP_FAIL;
    }

    // esp_app_desc_t sits right after the image header and the first segment header.
    size_t n = fread(buf, 1, CHUNK, f);
    const size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + at);
    const esp_app_desc_t *me = esp_app_get_description();
    esp_err_t err = ESP_OK;
    if (n < at + sizeof(*d) || buf[0] != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strncmp(d->project_name, me->project_name, sizeof(d->project_name))) {
        *why = "That isn't a Solana OS image.";
        err = ESP_ERR_INVALID_VERSION;
    } else if (strncmp(d->version, version, sizeof(d->version))) {
        *why = "The image is another version than offered.";
        err = ESP_ERR_INVALID_VERSION;
    }

    esp_ota_handle_t h = 0;
    if (err == ESP_OK) {
        err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
            *why = "Couldn't prepare the flash.";
        }
    }
    size_t done = 0;
    while (err == ESP_OK && n > 0) {
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write at %u: %s", (unsigned)done, esp_err_to_name(err));
            *why = "Couldn't write the flash.";
            break;
        }
        done += n;
        *pct = (int)(done * 100 / size);
        n = fread(buf, 1, CHUNK, f);
    }
    if (err == ESP_OK && (ferror(f) || done != size)) {
        *why = "Couldn't read the download from the card.";
        err = ESP_FAIL;
    }
    fclose(f);
    free(buf);

    if (err != ESP_OK) {
        if (h) {
            esp_ota_abort(h);
        }
        return err;
    }
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        *why = "The image didn't pass its checks.";
        return err;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        *why = "Couldn't switch to the new version.";
        return err;
    }
    ESP_LOGI(TAG, "%s written to %s", version, next->label);
    return ESP_OK;
}

bool os_ota_flash_confirm(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
            (state != ESP_OTA_IMG_PENDING_VERIFY && state != ESP_OTA_IMG_NEW)) {
        return false;                   // flashed over the wire, or already confirmed
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't confirm this build: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}
