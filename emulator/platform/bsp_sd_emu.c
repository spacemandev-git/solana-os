// Emulated microSD: a 64 MB card that lives in the page's memory (Emscripten
// MEMFS), so the debug app can mount it, list it and speed-test it with the
// same stdio/dirent calls it uses on the badge. Unmounting moves the files
// out of /sdcard; reloading the page gives a fresh card.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp.h"
#include "emu.h"

static const char *TAG = "bsp_sd";

#define STORE       "/.sdcard"          // where the card's files wait while unmounted
#define CARD_MB     64
#define FAT_BYTES   ((uint64_t)(CARD_MB - 1) << 20)

static bsp_sd_state_t s_state = BSP_SD_NOT_MOUNTED;
static esp_err_t s_last_err;
static uint64_t s_used;
static bool s_created;

esp_err_t bsp_spi_bus_init(void)
{
    return ESP_OK;      // no bus to set up; the SD-before-LCD order still holds in app_main
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void create_card(void)
{
    mkdir(STORE, 0777);
    write_file(STORE "/README.TXT",
               "This is the emulator's pretend microSD card.\n"
               "It lives in browser memory and is gone when you reload the page.\n");
    mkdir(STORE "/APPS", 0777);
    s_created = true;
}

static uint64_t du(const char *dir)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st)) {
            continue;
        }
        // FAT allocates whole 4 KB clusters.
        total += S_ISDIR(st.st_mode) ? 4096 + du(path) : ((uint64_t)st.st_size + 4095) / 4096 * 4096;
    }
    closedir(d);
    return total;
}

static void rm_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (!stat(path, &st) && S_ISDIR(st.st_mode)) {
            rm_tree(path);
            rmdir(path);
        } else {
            unlink(path);
        }
    }
    closedir(d);
}

static void card_delay(int ms)
{
    // Card init and FAT work take a moment on the badge; let the UI show it.
    if (emu_in_task()) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

esp_err_t bsp_sd_mount(void)
{
    if (s_state == BSP_SD_MOUNTED) {
        return ESP_OK;
    }
    card_delay(120);
    if (!s_created) {
        create_card();
    }
    if (rename(STORE, BSP_SD_MOUNT) != 0) {
        s_state = BSP_SD_ERROR;
        s_last_err = ESP_FAIL;
        ESP_LOGW(TAG, "mount failed");
        return ESP_FAIL;
    }
    s_state = BSP_SD_MOUNTED;
    s_last_err = ESP_OK;
    bsp_sd_refresh_usage();
    ESP_LOGI(TAG, "mounted EMUSD (emulated, in memory)");
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (s_state == BSP_SD_MOUNTED) {
        rename(BSP_SD_MOUNT, STORE);
    }
    s_state = BSP_SD_NOT_MOUNTED;
    return ESP_OK;
}

esp_err_t bsp_sd_format(void)
{
    ESP_LOGW(TAG, "formatting emulated card");
    card_delay(600);
    if (s_state != BSP_SD_MOUNTED) {
        if (!s_created) {
            create_card();
        }
        rm_tree(STORE);
        return bsp_sd_mount();
    }
    rm_tree(BSP_SD_MOUNT);
    bsp_sd_refresh_usage();
    return ESP_OK;
}

void bsp_sd_refresh_usage(void)
{
    s_used = s_state == BSP_SD_MOUNTED ? du(BSP_SD_MOUNT) : 0;
}

void bsp_sd_get_info(bsp_sd_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->last_err = s_last_err;
    if (s_state == BSP_SD_MOUNTED) {
        strlcpy(out->name, "EMUSD", sizeof(out->name));
        out->type = "SDHC/SDXC";
        out->card_mb = CARD_MB;
        out->total_bytes = FAT_BYTES;
        out->free_bytes = FAT_BYTES > s_used ? FAT_BYTES - s_used : 0;
    }
}

void bsp_sd_print_card(void)
{
    if (s_state == BSP_SD_MOUNTED) {
        printf("Name: EMUSD (emulated)\nType: SDHC/SDXC\nSize: %dMB\n", CARD_MB);
    } else {
        printf("SD: %s (last error %s)\n", bsp_sd_state_str(s_state), esp_err_to_name(s_last_err));
    }
}

const char *bsp_sd_state_str(bsp_sd_state_t s)
{
    switch (s) {
    case BSP_SD_MOUNTED:     return "mounted";
    case BSP_SD_NO_CARD:     return "no card";
    case BSP_SD_BAD_FS:      return "unsupported format";
    case BSP_SD_ERROR:       return "card error";
    case BSP_SD_INCOMPATIBLE: return "incompatible card";
    default:                 return "not mounted";
    }
}

// shim/dirent.h routes readdir here; skip MEMFS's "." and ".." like FatFs.
#undef readdir
struct dirent *emu_readdir(DIR *d)
{
    struct dirent *de;
    while ((de = readdir(d)) && (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))) {
    }
    return de;
}
