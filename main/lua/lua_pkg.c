// Installed Lua apps on the microSD card, see lua_pkg.h.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "lvgl.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "lua_pkg.h"

static const char *const s_perm_names[LUA_PERM_COUNT] = {
    "led", "wifi", "mesh", "lamps", "items", "profile", "quest",
};

static const char *const s_perm_texts[LUA_PERM_COUNT] = {
    "Light the LEDs",
    "Turn Wi-Fi on and off",
    "Talk to nearby badges",
    "Spend and earn your !LAMPs",
    "Create and read your items",
    "See your profile",
    "Report quest progress",
};

const char *lua_perm_name(int i)
{
    return i >= 0 && i < LUA_PERM_COUNT ? s_perm_names[i] : "";
}

const char *lua_perm_text(int i)
{
    return i >= 0 && i < LUA_PERM_COUNT ? s_perm_texts[i] : "";
}

uint32_t lua_perm_parse(const char *name)
{
    for (int i = 0; name && i < LUA_PERM_COUNT; i++) {
        if (!strcmp(name, s_perm_names[i])) {
            return 1u << i;
        }
    }
    return 0;
}

// LVGL's symbol font has no star, heart, gift or bulb: use the closest glyph.
const char *lua_pkg_symbol(const char *icon)
{
    static const struct { const char *name, *sym; } map[] = {
        {"play", LV_SYMBOL_PLAY}, {"star", LV_SYMBOL_OK}, {"heart", LV_SYMBOL_BELL},
        {"bell", LV_SYMBOL_BELL}, {"gift", LV_SYMBOL_DOWNLOAD}, {"home", LV_SYMBOL_HOME},
        {"image", LV_SYMBOL_IMAGE}, {"list", LV_SYMBOL_LIST}, {"wifi", LV_SYMBOL_WIFI},
        {"shuffle", LV_SYMBOL_SHUFFLE}, {"eye", LV_SYMBOL_EYE_OPEN}, {"edit", LV_SYMBOL_EDIT},
        {"charge", LV_SYMBOL_CHARGE}, {"gps", LV_SYMBOL_GPS}, {"settings", LV_SYMBOL_SETTINGS},
        {"audio", LV_SYMBOL_AUDIO}, {"bulb", LV_SYMBOL_CHARGE},
    };
    for (size_t i = 0; icon && i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(icon, map[i].name)) {
            return map[i].sym;
        }
    }
    return LV_SYMBOL_PLAY;
}

void lua_pkg_plain(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; in && *in && o + 1 < n; in++) {
        if (*in >= 0x20 && *in <= 0x7E) {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

bool lua_pkg_valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n < 3 || n > LUA_PKG_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

void lua_pkg_rel(char *out, size_t n, const char *id, const char *file)
{
    snprintf(out, n, "apps/%s/%s", id, file);
}

static void copy_str(const cJSON *obj, const char *key, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    strlcpy(out, cJSON_IsString(v) ? v->valuestring : "", n);
}

bool lua_pkg_parse(const char *json, const char *want_id, lua_pkg_t *out)
{
    cJSON *m = cJSON_Parse(json);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return false;
    }
    memset(out, 0, sizeof(*out));
    copy_str(m, "id", out->id, sizeof(out->id));
    bool ok = lua_pkg_valid_id(out->id) && (!want_id || !strcmp(want_id, out->id));
    copy_str(m, "name", out->name, sizeof(out->name));
    if (!out->name[0]) {
        strlcpy(out->name, out->id, sizeof(out->name));
    }
    copy_str(m, "version", out->version, sizeof(out->version));
    copy_str(m, "author", out->author, sizeof(out->author));
    copy_str(m, "summary", out->summary, sizeof(out->summary));
    copy_str(m, "category", out->category, sizeof(out->category));
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(m, "icon");
    out->symbol = lua_pkg_symbol(cJSON_IsString(icon) ? icon->valuestring : NULL);
    const cJSON *accent = cJSON_GetObjectItemCaseSensitive(m, "accent");
    out->accent = cJSON_IsString(accent) ? strtoul(accent->valuestring, NULL, 16) & 0xFFFFFF : 0xB6B0FF;
    const cJSON *perm;
    cJSON_ArrayForEach(perm, cJSON_GetObjectItemCaseSensitive(m, "permissions")) {
        out->perms |= cJSON_IsString(perm) ? lua_perm_parse(perm->valuestring) : 0;
    }
    out->temp = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "temp"));
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(m, "expires");
    out->expires = cJSON_IsNumber(exp) ? (int64_t)exp->valuedouble : 0;
    cJSON_Delete(m);
    return ok;
}

bool lua_pkg_load(const char *id, lua_pkg_t *out)
{
    char rel[64], path[160];
    struct stat st;
    if (!lua_pkg_valid_id(id) || !os_data_ready()) {
        return false;
    }
    lua_pkg_rel(rel, sizeof(rel), id, "main.lua");
    os_data_path(path, sizeof(path), rel);
    if (stat(path, &st) != 0) {
        return false;
    }
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    bool ok = json && lua_pkg_parse(json, id, out);
    free(json);
    return ok;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const lua_pkg_t *)a)->name, ((const lua_pkg_t *)b)->name);
}

int lua_pkg_scan(lua_pkg_t *out, int max)
{
    if (!os_data_ready()) {
        return -1;
    }
    char path[160];
    os_data_path(path, sizeof(path), "apps");
    DIR *d = opendir(path);
    if (!d) {
        return 0;       // nothing installed yet
    }
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (lua_pkg_valid_id(e->d_name) && lua_pkg_load(e->d_name, &out[n])) {
            n++;
        }
    }
    closedir(d);
    qsort(out, n, sizeof(*out), by_name);
    return n;
}

bool lua_pkg_remove(const char *id)
{
    char rel[40];
    if (!lua_pkg_valid_id(id)) {
        return false;
    }
    snprintf(rel, sizeof(rel), "apps/%s", id);
    return os_data_remove(rel);
}

bool lua_pkg_expired(const lua_pkg_t *p)
{
    int64_t now = os_cloud_time_ms();
    return p->temp && p->expires && now && now >= p->expires;
}

static bool set_expires(const char *id, int64_t expires)
{
    char rel[64];
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    cJSON *m = json ? cJSON_Parse(json) : NULL;
    free(json);
    char *out = NULL;
    if (cJSON_IsObject(m)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "expires");
        cJSON_AddNumberToObject(m, "expires", (double)expires);
        out = cJSON_PrintUnformatted(m);
    }
    cJSON_Delete(m);
    bool ok = out && os_data_write(rel, out, strlen(out));
    cJSON_free(out);
    return ok;
}

int lua_pkg_apply_library(const char *body)
{
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *it;
    int changed = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "items")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(it, "expires");
        lua_pkg_t pkg;
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "temp")) || !cJSON_IsString(id) || !cJSON_IsNumber(exp)
                || !lua_pkg_load(id->valuestring, &pkg) || !pkg.temp || pkg.expires == (int64_t)exp->valuedouble) {
            continue;
        }
        changed += set_expires(pkg.id, (int64_t)exp->valuedouble);
    }
    cJSON_Delete(j);
    return changed;
}
