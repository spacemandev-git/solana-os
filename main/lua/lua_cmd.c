// "lua" console command (os_cmd, so the badge's UART console and the
// emulator's emu_console both serve it).
//
//   lua list | run <id> | rm <id> [file] | mem | sums <id> | install <id>
//   lua put <id> <file> begin|<base64>|end
//   lua recv <id> <file> <bytes> <sha256>
//
// <file> is main.lua, app.json or a bundled path (docs/LUA_API.md
// "Console"). Writing or removing any of them deletes the store's compiled
// code (code.bin), which would no longer match: the app then runs from its
// source. Nothing here can write code.bin. put copies a file a line at a time: begin starts it, each
// base64 line appends to it, end checks it and moves it into place. recv
// takes a whole file in one stream after its line, a block at a time when
// the badge asks for it, so the card is never written while the UART
// receives (its buffer holds ~30 ms at 115200 baud). Tools pair it with
// sums to send only what changed. The console task's stack is small: big
// buffers are allocated.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "core/console.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "lua_install.h"
#include "lua_priv.h"

#define LIST_MAX        40
#define RECV_BLOCK      2048            // base64 characters per block, a multiple of 4
#define RECV_WAIT_MS    5000

static const char *refusal(const char *id)
{
    if (!os_data_ready()) {
        return "no microSD card";
    }
    if (lua_host_active(id)) {
        return "the app is running; close it first (open home)";
    }
    if (lua_install_busy(id)) {
        return "the App Store is installing it";
    }
    return NULL;
}

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

// The store's compiled code no longer matches the app's files.
static void drop_code(const char *id)
{
    char rel[64], path[160];
    lua_pkg_rel(rel, sizeof(rel), id, "code.bin");
    os_data_path(path, sizeof(path), rel);
    unlink(path);
}

// Removes the file, then the folders inside files/ it leaves empty.
static bool remove_file(const char *id, const char *rel)
{
    char path[160], top[160], files[48];
    os_data_path(path, sizeof(path), rel);
    if (unlink(path) != 0) {
        return false;
    }
    snprintf(files, sizeof(files), "apps/%s/files", id);
    os_data_path(top, sizeof(top), files);
    size_t keep = strlen(top);
    for (char *slash; (slash = strrchr(path, '/')) && (size_t)(slash - path) > keep;) {
        *slash = 0;
        if (rmdir(path) != 0) {
            break;                          // not empty
        }
    }
    return true;
}

static int cmd_rm(const char *id, const char *file)
{
    char rel[128];
    const char *why = lua_pkg_valid_id(id) ? refusal(id) : "no such app";
    if (why) {
        printf("can't remove %s: %s\n", id, why);
        return 1;
    }
    if (file) {
        if (!lua_pkg_file_rel(rel, sizeof(rel), id, file, NULL) || !remove_file(id, rel)) {
            printf("can't remove %s/%s\n", id, file);
            return 1;
        }
        drop_code(id);
        printf("removed %s/%s\n", id, file);
        return 0;
    }
    if (!lua_pkg_remove(id)) {
        printf("can't remove %s\n", id);
        return 1;
    }
    printf("removed %s\n", id);
    return 0;
}

static void print_code(const lh_code_stat_t *c)
{
    if (c->size) {
        printf("  code: the store's compiled code, %u bytes run in place (flash slot %d%s)\n", (unsigned)c->size,
               c->slot, c->copied ? ", copied in at launch" : "");
    } else {
        printf("  code: main.lua compiled on the badge\n");
    }
}

static int cmd_mem(void)
{
    lh_t *s = lh_cur;
    if (s && s->L) {
        printf("running %s: %u bytes of Lua heap (peak %u, cap %d)\n", s->pkg.id, (unsigned)s->mem,
               (unsigned)s->mem_peak, LUA_HOST_HEAP_MAX);
        print_code(&s->code_stat);
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
    print_code(&(lh_code_stat_t){ .size = lh_stats.code_size, .slot = lh_stats.code_slot,
                                  .copied = lh_stats.code_copied });
    return 0;
}

// The .put file in place of the real one, once app.json is checked.
static const char *finish_file(const char *id, const char *file, const char *part, const char *path)
{
    if (!strcmp(file, "app.json")) {
        lua_pkg_t pkg;
        FILE *f = fopen(part, "rb");
        char *buf = malloc(LUA_PKG_JSON_MAX + 1);
        size_t n = f && buf ? fread(buf, 1, LUA_PKG_JSON_MAX, f) : 0;
        if (f) {
            fclose(f);
        }
        bool ok = buf && (buf[n] = 0, lua_pkg_parse(buf, id, &pkg));
        free(buf);
        if (!ok) {
            return "app.json is not a manifest for this app";
        }
    }
    unlink(path);                           // FatFs rename() won't replace a file
    if (rename(part, path) != 0) {
        return "can't write it to the card";
    }
    drop_code(id);
    return NULL;
}

static int cmd_put(const char *id, const char *file, const char *arg)
{
    char rel[128], part[176], path[160];
    size_t max;
    if (!lua_pkg_file_rel(rel, sizeof(rel), id, file, &max)) {
        printf("usage: lua put <id> <main.lua|app.json|bundled path> begin|<base64>|end\n");
        return 1;
    }
    const char *why = refusal(id);
    if (why) {
        printf("%s\n", why);
        return 1;
    }
    os_data_path(path, sizeof(path), rel);
    snprintf(part, sizeof(part), "%s.put", path);
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
        why = finish_file(id, file, part, path);
        if (why) {
            printf("%s\n", why);
            unlink(part);
            return 1;
        }
        printf("%s/%s: %ld bytes\n", id, file, (long)st.st_size);
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

// ---------------------------------------------------------------------------
// recv

static bool parse_sha(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        char c = hex[i] | 0x20;             // either case
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = i % 2 ? (out[i / 2] | v) : v << 4;
    }
    return true;
}

// Fills buf with exactly n base64 characters from the stream (line breaks
// and spaces skipped). NULL, or why it couldn't.
static const char *read_block(char *buf, size_t n)
{
    size_t have = 0;
    while (have < n) {
        int got = console_stream_read(buf + have, n - have, RECV_WAIT_MS);
        if (got < 0) {
            return "this console takes no stream";
        }
        if (got == 0) {
            return "timeout";
        }
        char *p = buf + have;
        int kept = 0;
        for (int i = 0; i < got; i++) {
            if (p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && p[i] != '\t') {
                p[kept++] = p[i];
            }
        }
        have += kept;
    }
    return NULL;
}

static const char *receive(const char *id, const char *file, long bytes, const uint8_t want[32])
{
    char rel[128], path[160], part[176];
    size_t max;
    lua_pkg_file_rel(rel, sizeof(rel), id, file, &max);
    if (bytes > (long)max) {
        return "too big";
    }
    const char *why = refusal(id);
    if (why) {
        return why;
    }
    if (console_stream_read(part, 0, 0) < 0) {
        return "this console takes no stream";
    }
    os_data_mkdirs(rel);
    os_data_path(path, sizeof(path), rel);
    snprintf(part, sizeof(part), "%s.put", path);
    char *b64 = malloc(RECV_BLOCK);
    unsigned char *bin = malloc(RECV_BLOCK / 4 * 3);
    FILE *f = b64 && bin ? fopen(part, "wb") : NULL;
    if (!f) {
        free(b64);
        free(bin);
        return b64 && bin ? "can't write to the card" : "not enough memory";
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    size_t total = (bytes + 2) / 3 * 4, done = 0;
    long written = 0;
    printf("recv: ready %d\n", RECV_BLOCK);
    fflush(stdout);
    while (!why && done < total) {
        size_t n = total - done < RECV_BLOCK ? total - done : RECV_BLOCK, out = 0;
        why = read_block(b64, n);
        if (!why && mbedtls_base64_decode(bin, RECV_BLOCK / 4 * 3, &out, (unsigned char *)b64, n) != 0) {
            why = "bad base64";
        }
        if (!why && fwrite(bin, 1, out, f) != out) {
            why = "can't write to the card";
        }
        if (!why) {
            mbedtls_sha256_update(&sha, bin, out);
            written += out;
            done += n;
            if (done < total) {
                printf("recv: more %d\n", RECV_BLOCK);
                fflush(stdout);
            }
        }
    }
    uint8_t got[32];
    mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    free(b64);
    free(bin);
    if (fclose(f) != 0 && !why) {
        why = "can't write to the card";
    }
    if (!why && written != bytes) {
        why = "wrong size";
    }
    if (!why && memcmp(got, want, 32)) {
        why = "the SHA-256 doesn't match";
    }
    if (!why) {
        why = finish_file(id, file, part, path);
    }
    if (why) {
        unlink(part);
    }
    return why;
}

static int cmd_recv(const char *id, const char *file, const char *bytes, const char *sha)
{
    char rel[128];
    uint8_t want[32];
    char *end;
    long n = strtol(bytes, &end, 10);
    if (!lua_pkg_file_rel(rel, sizeof(rel), id, file, NULL) || end == bytes || *end || n < 0 || !parse_sha(sha, want)) {
        printf("usage: lua recv <id> <main.lua|app.json|bundled path> <bytes> <sha256>\n");
        return 1;
    }
    const char *why = receive(id, file, n, want);
    if (why) {
        printf("recv: error %s\n", why);
        return 1;
    }
    printf("recv: ok %ld\n", n);
    return 0;
}

// ---------------------------------------------------------------------------
// sums: one command at a time on the console, so the paths can be static.

static char s_rel[128], s_path[160], s_sub[LUA_PKG_PATH_MAX + 1];

static int sum_file(const char *id, const char *file)
{
    uint8_t sha[32];
    char hex[65];
    long size;
    if (!lua_pkg_file_rel(s_rel, sizeof(s_rel), id, file, NULL)) {
        return 0;                           // not a name the app could have shipped
    }
    os_data_path(s_path, sizeof(s_path), s_rel);
    if (!lua_pkg_sha256_file(s_path, sha, &size)) {
        return 0;
    }
    lua_pkg_hex(hex, sha, sizeof(sha));
    printf("%s %ld %s\n", hex, size, file);
    return 1;
}

// The files under files/<s_sub>, depth first.
static int sum_dir(const char *id, int depth)
{
    snprintf(s_rel, sizeof(s_rel), "apps/%s/files%s%s", id, s_sub[0] ? "/" : "", s_sub);
    os_data_path(s_path, sizeof(s_path), s_rel);
    DIR *d = opendir(s_path);
    if (!d) {
        return 0;
    }
    int n = 0;
    size_t base = strlen(s_sub);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || base + 1 + len > LUA_PKG_PATH_MAX) {
            continue;
        }
        bool dir = e->d_type == DT_DIR;
        char *at = s_sub + base;
        if (base) {
            *at++ = '/';
        }
        memcpy(at, e->d_name, len + 1);
        if (!dir) {
            n += sum_file(id, s_sub);
        } else if (depth < LUA_PKG_PATH_DEPTH) {
            n += sum_dir(id, depth + 1);
        }
        s_sub[base] = 0;
    }
    closedir(d);
    return n;
}

static int cmd_sums(const char *id)
{
    if (!lua_pkg_valid_id(id)) {
        printf("usage: lua sums <id>\n");
        return 1;
    }
    if (!os_data_ready()) {
        printf("sums: no microSD card\n");
        return 1;
    }
    int n = sum_file(id, "main.lua") + sum_file(id, "app.json");
    s_sub[0] = 0;
    n += sum_dir(id, 1);
    printf("sums: %d files\n", n);
    return 0;
}

static int cmd_install(const char *id)
{
    esp_err_t err = lua_install_start(id, NULL);
    if (err != ESP_OK) {
        printf("can't install %s: %s\n", id, err == ESP_ERR_INVALID_STATE ? "another install is running"
               : err == ESP_ERR_NOT_FOUND ? "no microSD card" : esp_err_to_name(err));
        return 1;
    }
    printf("installing %s from the App Store (the log and a toast tell how it went)\n", id);
    return 0;
}

// ---------------------------------------------------------------------------

static int cmd_lua(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "list")) {
        return cmd_list();
    } else if (!strcmp(sub, "run") && argc == 3) {
        return !lua_host_launch(argv[2]);
    } else if (!strcmp(sub, "rm") && (argc == 3 || argc == 4)) {
        return cmd_rm(argv[2], argc == 4 ? argv[3] : NULL);
    } else if (!strcmp(sub, "mem")) {
        return cmd_mem();
    } else if (!strcmp(sub, "put") && argc == 5) {
        return cmd_put(argv[2], argv[3], argv[4]);
    } else if (!strcmp(sub, "recv") && argc == 6) {
        return cmd_recv(argv[2], argv[3], argv[4], argv[5]);
    } else if (!strcmp(sub, "sums") && argc == 3) {
        return cmd_sums(argv[2]);
    } else if (!strcmp(sub, "install") && argc == 3) {
        return cmd_install(argv[2]);
    }
    printf("usage: lua [list|run <id>|rm <id> [file]|mem|sums <id>|install <id>|put <id> <file> begin|<base64>|end"
           "|recv <id> <file> <bytes> <sha256>]\n");
    return 1;
}

void lh_cmd_register(void)
{
    os_cmd_register("lua", "lua [list|run|rm <id> [file]|mem|sums|install <id>|put <id> <file> ...|recv <id> <file> <bytes> <sha256>]",
                    cmd_lua);
}
