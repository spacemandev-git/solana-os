// Solana OS data on the microSD card, see os_data.h.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "os_data.h"

static const char *TAG = "os_data";

bool os_data_ready(void)
{
    bsp_sd_info_t sd;
    bsp_sd_get_info(&sd);
    if (sd.state != BSP_SD_MOUNTED) {
        return false;
    }
    struct stat st;
    if (stat(OS_DATA_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(OS_DATA_DIR, 0775) == 0;
}

void os_data_path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", OS_DATA_DIR, rel ? rel : "");
}

bool os_data_mkdirs(const char *rel)
{
    char path[160];
    os_data_path(path, sizeof(path), rel);
    char *slash = strrchr(path, '/');
    if (!slash) {
        return false;
    }
    *slash = 0;
    for (char *p = path + strlen(OS_DATA_DIR) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0775);
            *p = '/';
        }
    }
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

char *os_data_read(const char *rel, size_t max, size_t *len_out)
{
    char path[160];
    os_data_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (len >= 0 && (size_t)len <= max) ? malloc(len + 1) : NULL;
    if (buf && fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[len] = 0;
        if (len_out) {
            *len_out = len;
        }
    }
    return buf;
}

bool os_data_write(const char *rel, const char *data, size_t len)
{
    char path[160], tmp[168];
    if (!os_data_ready() || !os_data_mkdirs(rel)) {
        return false;
    }
    os_data_path(path, sizeof(path), rel);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGW(TAG, "can't write %s", tmp);
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    // FatFs rename() refuses to replace an existing file.
    unlink(path);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

bool os_data_append(const char *rel, const char *line)
{
    char path[160];
    if (!os_data_ready() || !os_data_mkdirs(rel)) {
        return false;
    }
    os_data_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "ab");
    if (!f) {
        return false;
    }
    bool ok = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
    return fclose(f) == 0 && ok;
}

static bool remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return true;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char child[256];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
                continue;
            }
            if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < (int)sizeof(child)) {
                remove_tree(child);
            }
        }
        closedir(d);
    }
    return rmdir(path) == 0;
}

bool os_data_remove(const char *rel)
{
    char path[160];
    os_data_path(path, sizeof(path), rel);
    return remove_tree(path);
}

void os_data_wipe(void)
{
    bsp_sd_info_t sd;
    bsp_sd_get_info(&sd);
    if (sd.state != BSP_SD_MOUNTED) {
        return;
    }
    remove_tree(OS_DATA_DIR);
    ESP_LOGI(TAG, "wiped %s", OS_DATA_DIR);
}
