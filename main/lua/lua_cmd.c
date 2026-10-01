// "lua" console command (os_cmd, so the badge's UART console and the
// emulator's emu_console both serve it).
//
//   lua list | run <id> | rm <id> | mem
//   lua put <id> <main.lua|app.json> begin|<base64>|end
//
// put copies an app over the serial console: begin starts the file, each
// base64 line appends to it, end checks it and moves it into place.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mbedtls/base64.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "lua_priv.h"

#define LIST_MAX    40

static int cmd_list(void)
{
    lua_pkg_t *pkgs = malloc(LIST_MAX * sizeof(*pkgs));
    int n = pkgs ? lua_pkg_scan(pkgs, LIST_MAX) : 0;
    if (n < 0) {
        printf("no microSD card\n");
    } else if (n == 0) {
        printf("no apps installed\n");
    }
    for (int i = 0; i < n; i++) {
        const lua_pkg_t *p = &pkgs[i];
        char perms[64] = "";
        for (int b = 0; b < LUA_PERM_COUNT; b++) {
            if (p->perms & (1u << b)) {
                strlcat(perms, perms[0] ? "," : "", sizeof(perms));
                strlcat(perms, lua_perm_name(b), sizeof(perms));
            }
        }
        printf("%-24s %-10s %-20s %s%s%s\n", p->id, p->version, p->name, perms,
               p->temp ? "  temp" : "", lua_pkg_expired(p) ? " (expired)" : "");
    }
    free(pkgs);
    return n < 0;
}

static int cmd_rm(const char *id)
{
    const char *running = lua_host_running();
    if (running && !strcmp(running, id)) {
        printf("%s is running; close it first\n", id);
        return 1;
    }
    if (!lua_pkg_valid_id(id) || !lua_pkg_remove(id)) {
        printf("can't remove %s\n", id);
        return 1;
    }
    printf("removed %s\n", id);
    return 0;
}

static int cmd_mem(void)
{
    lh_t *s = lh_cur;
    if (s && s->L) {
        printf("running %s: %u bytes of Lua heap (peak %u, cap %d)\n", s->pkg.id, (unsigned)s->mem,
               (unsigned)s->mem_peak, LUA_HOST_HEAP_MAX);
    }
    if (!lh_stats.id[0]) {
        printf("no app has finished yet\n");
        return 0;
    }
    printf("last run %s: Lua heap peak %u bytes (cap %d), %u left after close\n"
           "  most instructions in one callback %u (cap %d)\n"
           "  lua task stack never used %u bytes\n"
           "  system heap free at launch %u\n"
           "  last error: %s\n",
           lh_stats.id, (unsigned)lh_stats.mem_peak, LUA_HOST_HEAP_MAX, (unsigned)lh_stats.mem_end,
           (unsigned)lh_stats.steps_peak, LUA_HOST_STEPS_MAX, (unsigned)lh_stats.stack_free,
           (unsigned)lh_stats.heap_before, lh_stats.err[0] ? lh_stats.err : "none");
    return 0;
}

static int cmd_put(const char *id, const char *file, const char *arg)
{
    bool src = !strcmp(file, "main.lua");
    if (!lua_pkg_valid_id(id) || (!src && strcmp(file, "app.json"))) {
        printf("usage: lua put <id> <main.lua|app.json> begin|<base64>|end\n");
        return 1;
    }
    if (!os_data_ready()) {
        printf("no microSD card\n");
        return 1;
    }
    char rel[64], part[168], path[160];
    lua_pkg_rel(rel, sizeof(rel), id, file);
    os_data_path(path, sizeof(path), rel);
    snprintf(part, sizeof(part), "%s.put", path);
    size_t max = src ? LUA_PKG_SRC_MAX : LUA_PKG_JSON_MAX;
    if (!strcmp(arg, "begin")) {
        os_data_mkdirs(rel);
        FILE *f = fopen(part, "wb");
        if (!f) {
            printf("can't create %s\n", part);
            return 1;
        }
        fclose(f);
        return 0;
    }
    struct stat st;
    if (stat(part, &st) != 0) {
        printf("run 'lua put %s %s begin' first\n", id, file);
        return 1;
    }
    if (!strcmp(arg, "end")) {
        if (!src) {
            lua_pkg_t pkg;
            FILE *f = fopen(part, "rb");
            char *buf = malloc(LUA_PKG_JSON_MAX + 1);     // the console task's stack is small
            size_t n = f && buf ? fread(buf, 1, LUA_PKG_JSON_MAX, f) : 0;
            if (f) {
                fclose(f);
            }
            bool ok = buf && (buf[n] = 0, lua_pkg_parse(buf, id, &pkg));
            free(buf);
            if (!ok) {
                printf("app.json is not a manifest for %s\n", id);
                unlink(part);
                return 1;
            }
        }
        unlink(path);
        if (rename(part, path) != 0) {
            printf("can't write %s\n", path);
            return 1;
        }
        printf("%s: %ld bytes\n", rel, (long)st.st_size);
        return 0;
    }
    unsigned char bin[96];
    size_t n = 0;
    if (mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)arg, strlen(arg)) != 0) {
        printf("bad base64 (at most 128 characters per line)\n");
        return 1;
    }
    if (st.st_size + n > max) {
        printf("%s is limited to %u bytes\n", file, (unsigned)max);
        return 1;
    }
    FILE *f = fopen(part, "ab");
    bool ok = f && fwrite(bin, 1, n, f) == n;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    if (!ok) {
        printf("write failed\n");
    }
    return !ok;
}

static int cmd_lua(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "list")) {
        return cmd_list();
    } else if (!strcmp(sub, "run") && argc == 3) {
        return !lua_host_launch(argv[2]);
    } else if (!strcmp(sub, "rm") && argc == 3) {
        return cmd_rm(argv[2]);
    } else if (!strcmp(sub, "mem")) {
        return cmd_mem();
    } else if (!strcmp(sub, "put") && argc == 5) {
        return cmd_put(argv[2], argv[3], argv[4]);
    }
    printf("usage: lua [list|run <id>|rm <id>|mem|put <id> <file> begin|<base64>|end]\n");
    return 1;
}

void lh_cmd_register(void)
{
    os_cmd_register("lua", "lua [list|run <id>|rm <id>|mem|put <id> <file> begin|<b64>|end]", cmd_lua);
}
