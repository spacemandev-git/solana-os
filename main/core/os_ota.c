// Over-the-air updates, see os_ota.h and docs/HHGTS.md "Software updates".
//
// The download is an ordinary os_api_download (the cloud task streams it to
// the card), so it shares the badge's one HTTPS connection and never runs
// beside another request. The image goes to /sdcard/ota/, outside
// OS_DATA_DIR, so an owner change wiping /sdcard/hh can't take it away
// mid-install. Checking and writing block for tens of seconds: a worker task
// does them and the update app polls os_ota_get() from on_tick.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "bsp.h"
#include "lua/lua_install.h"
#include "os.h"
#include "os_api.h"
#include "os_cloud.h"
#include "os_cmd.h"
#include "os_data.h"
#include "os_http.h"
#include "os_ota.h"
#include "os_ota_flash.h"

static const char *TAG = "os_ota";

#define OTA_DIR         BSP_SD_MOUNT "/ota"
#define OTA_FILE        OTA_DIR "/solana-os.bin"
#define NVS_NS          "ota"           // "to": the version an update restarted into
#define CHUNK           4096
#define RESTART_MS      2000            // "Restarting..." stays readable

static SemaphoreHandle_t s_lock;
static os_ota_info_t s_info;            // under s_lock
static volatile int s_pct;              // the flash write's progress
static void *s_ctx;                     // the download's os_api ctx
static struct {
    char version[32];
    size_t size;
    uint8_t sha[32];
} s_job;
static char s_why[64];                  // os_ota_blocker()'s battery text

static void set(os_ota_phase_t phase, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.phase = phase;
    s_info.error = error;
    s_info.pct = 0;
    xSemaphoreGive(s_lock);
    if (error) {
        ESP_LOGW(TAG, "update to %s failed: %s", s_job.version, error);
    }
}

static bool hex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = i % 2 ? (out[i / 2] | v) : v << 4;
    }
    return true;
}

static bool sha256_file(const char *path, uint8_t out[32], size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = f ? malloc(CHUNK) : NULL;
    if (!buf) {
        if (f) {
            fclose(f);
        }
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    size_t total = 0, got;
    while ((got = fread(buf, 1, CHUNK, f)) > 0) {
        mbedtls_sha256_update(&c, buf, got);
        total += got;
    }
    bool ok = !ferror(f);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    fclose(f);
    free(buf);
    *size = total;
    return ok;
}

int os_ota_battery(void)
{
    int mv = bsp_battery_mv();
    if (mv < 0) {
        return -1;
    }
    return mv > 4300 ? 100 : bsp_battery_percent(mv);   // over 4.3 V: USB power (the status bar's rule)
}

// ---------------------------------------------------------------------------
// Worker: check, write, switch, restart

static void worker(void *arg)
{
    uint8_t got[32];
    size_t size = 0;
    const char *why = NULL;
    if (!sha256_file(OTA_FILE, got, &size) || size != s_job.size || memcmp(got, s_job.sha, sizeof(got))) {
        why = "The download is damaged. Try again.";
    } else {
        int b = os_ota_battery();
        if (b < OS_OTA_MIN_BATTERY) {
            why = b < 0 ? "Can't read the battery." : "The battery dropped below 70 %. Charge it, then try again.";
        }
    }
    if (!why) {
        set(OS_OTA_INSTALLING, NULL);
        s_pct = 0;
        ESP_LOGI(TAG, "writing Solana OS %s (%u bytes)", s_job.version, (unsigned)s_job.size);
        if (os_ota_flash(OTA_FILE, s_job.size, s_job.version, &s_pct, &why) != ESP_OK && !why) {
            why = "Couldn't write the update.";
        }
    }
    unlink(OTA_FILE);
    if (why) {
        set(OS_OTA_FAILED, why);
        vTaskDelete(NULL);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "to", s_job.version);
        nvs_commit(h);
        nvs_close(h);
    }
    set(OS_OTA_RESTARTING, NULL);
    ESP_LOGI(TAG, "restarting into Solana OS %s", s_job.version);
    vTaskDelay(pdMS_TO_TICKS(RESTART_MS));
    esp_restart();
}

static void downloaded(int status, const char *body, void *ctx)
{
    if (ctx != s_ctx || s_info.phase != OS_OTA_DOWNLOADING) {
        return;
    }
    s_ctx = NULL;
    if (status / 100 != 2) {
        if (status == 404) {
            os_cloud_refresh();             // hello says what's on offer now
        }
        set(OS_OTA_FAILED, status == 404 ? "This update isn't offered any more."
            : status == 0 ? "The download stopped. Check Wi-Fi and the card, then try again."
            : "The server refused the download.");
        return;
    }
    set(OS_OTA_VERIFYING, NULL);
    // 4 KB: a FatFs read, SHA-256, then esp_ota_write with its flash calls.
    if (xTaskCreate(worker, "ota", 4096, NULL, 3, NULL) != pdPASS) {
        unlink(OTA_FILE);
        set(OS_OTA_FAILED, "Not enough memory. Restart the badge and try again.");
    }
}

// ---------------------------------------------------------------------------
// API

const char *os_ota_blocker(void)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return "Not enough memory right now.";
    }
    os_cloud_get_info(in);
    bool offer = in->update[0] != 0;
    free(in);
    if (!offer) {
        return "No update on offer.";
    }
    if (!os_api_online()) {
        return "Turn Wi-Fi on first.";
    }
    if (!os_data_ready()) {
        return "Insert a microSD card: the update downloads to it.";
    }
    if (lua_install_busy(NULL)) {
        return "Wait for the app install to finish.";
    }
    int b = os_ota_battery();
    if (b < 0) {
        return "Can't read the battery.";
    }
    if (b < OS_OTA_MIN_BATTERY) {
        snprintf(s_why, sizeof(s_why), "Charge the badge to %d %% first (now %d %%).", OS_OTA_MIN_BATTERY, b);
        return s_why;
    }
    return NULL;
}

esp_err_t os_ota_start(void)
{
    os_ota_phase_t p = s_info.phase;
    if ((p != OS_OTA_IDLE && p != OS_OTA_FAILED) || os_ota_blocker()) {
        return ESP_ERR_INVALID_STATE;
    }
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return ESP_ERR_NO_MEM;
    }
    os_cloud_get_info(in);
    strlcpy(s_job.version, in->update, sizeof(s_job.version));
    s_job.size = in->update_size;
    bool ok = hex32(in->update_sha, s_job.sha);
    free(in);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }

    // The version goes into the query as is, but for anything URL-special ("0.4.1+rc").
    char path[96] = "/api/device/firmware?v=";
    size_t n = strlen(path);
    for (const char *c = s_job.version; *c && n + 4 < sizeof(path); c++) {
        bool plain = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || strchr(".-_~", *c);
        n += snprintf(path + n, sizeof(path) - n, plain ? "%c" : "%%%02X", (unsigned char)*c);
    }
    mkdir(OTA_DIR, 0775);
    unlink(OTA_FILE);
    s_ctx = os_api_token();
    esp_err_t err = os_api_download(path, OTA_FILE, s_job.size, downloaded, s_ctx);
    if (err != ESP_OK) {
        s_ctx = NULL;
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_info.version, s_job.version, sizeof(s_info.version));
    xSemaphoreGive(s_lock);
    set(OS_OTA_DOWNLOADING, NULL);
    ESP_LOGI(TAG, "downloading Solana OS %s (%u bytes)", s_job.version, (unsigned)s_job.size);
    return ESP_OK;
}

void os_ota_cancel(void)
{
    if (s_info.phase != OS_OTA_DOWNLOADING) {
        return;
    }
    // The transfer itself runs to its end on the cloud task; its file is
    // replaced by the next try and deleted at boot.
    os_api_cancel(s_ctx);
    s_ctx = NULL;
    set(OS_OTA_IDLE, NULL);
}

void os_ota_get(os_ota_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
    if (out->phase == OS_OTA_DOWNLOADING && s_job.size) {
        size_t done, total;
        os_http_progress(&done, &total);
        out->pct = total == s_job.size ? (int)(done * 100 / total) : 0;
    } else if (out->phase == OS_OTA_INSTALLING) {
        out->pct = s_pct;
    }
}

// ---------------------------------------------------------------------------
// Boot

static void confirm_boot(void *arg)
{
    // The OS task runs and the launcher is up: good enough to keep this build.
    if (os_ota_flash_confirm()) {
        ESP_LOGI(TAG, "Solana OS %s confirmed after an update", esp_app_get_description()->version);
    }
}

static int cmd_ota(int argc, char **argv)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return 1;
    }
    os_cloud_get_info(in);
    os_ota_info_t o;
    os_ota_get(&o);
    static const char *const phases[] = { "idle", "downloading", "verifying", "installing", "restarting", "failed" };
    const char *why = os_ota_blocker();
    printf("running %s, offered %s (%d bytes), battery %d%%\n%s %d%%%s%s\n%s%s\n",
           esp_app_get_description()->version, in->update[0] ? in->update : "-", in->update_size, os_ota_battery(),
           phases[o.phase], o.pct, o.error ? ": " : "", o.error ? o.error : "",
           why ? "can't start: " : "ready: open update", why ? why : "");
    free(in);
    return 0;
}

void os_ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    unlink(OTA_FILE);                   // an update that never got written
    unlink(OTA_FILE ".part");

    nvs_handle_t h;
    char to[32] = "";
    size_t len = sizeof(to);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_str(h, "to", to, &len) == ESP_OK) {
            nvs_erase_key(h, "to");
            nvs_commit(h);
        }
        nvs_close(h);
    }
    const char *running = esp_app_get_description()->version;
    if (to[0] && !strcmp(to, running)) {
        os_toast("Updated to Solana OS %s", running);
    } else if (to[0]) {
        // The bootloader went back to the old slot: the new build never got to the launcher.
        ESP_LOGW(TAG, "update to %s rolled back", to);
        os_toast("The update didn't start: still on Solana OS %s", running);
    }
    if (!os_post(confirm_boot, NULL)) {
        confirm_boot(NULL);             // never leave a good build on trial
    }
    os_cmd_register("ota", "ota - software update: offer, battery, progress", cmd_ota);
}
