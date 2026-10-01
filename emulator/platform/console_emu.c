// Emulator counterpart of core/console.c. There is no UART: the page (or the
// browser devtools) sends command lines to emu_console(), e.g.
//   emulator.command("open wifi")   /  Module.ccall('emu_console', null, ['string'], ['key down'])
// Output goes to the browser console. Same command names as the badge's
// console for the parts that make sense in a browser.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "bsp.h"
#include "core/console.h"
#include "core/os.h"
#include "core/os_wifi.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"

static const char *TAG = "console";

static void cmd_apps(void)
{
    const os_app_t *cur = os_current();
    for (int i = 0; i < os_app_count; i++) {
        printf("%c %-8s %s\n", os_apps[i] == cur ? '*' : ' ', os_apps[i]->id, os_apps[i]->name);
    }
}

static void cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "connect") && argc >= 3) {
        os_wifi_connect_ch(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? atoi(argv[4]) : 0);
        return;
    }
    if (!strcmp(sub, "forget")) {
        os_wifi_forget();
        return;
    }
    if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        os_wifi_set_enabled(!strcmp(sub, "on"));
        return;
    }
    static const char *names[] = {"idle", "connecting", "connected", "failed", "suspended"};
    os_wifi_info_t in;
    os_wifi_get_info(&in);
    printf("wifi: %s (%s)  ssid='%s' ip=%s rssi=%d ch=%d %s\n", names[in.state], in.enabled ? "on" : "off", in.ssid,
           in.ip, in.rssi, in.channel, in.fail_reason ? in.fail_reason : "");
}

EMSCRIPTEN_KEEPALIVE void emu_console(const char *line)
{
    // Split like the badge's esp_console: spaces separate words, "double
    // quotes" group them ("wifi connect \"Solana Demo\" pass").
    static char buf[512];
    char *argv[16];
    int argc = 0;
    strlcpy(buf, line ? line : "", sizeof(buf));
    for (char *p = buf, *out = buf; *p && argc < 16;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = out;
        bool quoted = false;
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') {
                quoted = !quoted;
            } else {
                *out++ = *p;
            }
        }
        if (*p) {
            p++;
        }
        *out++ = 0;
    }
    if (!argc) {
        return;
    }
    const char *cmd = argv[0];
    if (!strcmp(cmd, "help")) {
        printf("commands: info, apps, open <id|home|calib>, key <up|down|left|right|select|back|home>...,\n"
               "          wifi [status|on|off|connect <ssid> [pass [channel]]|forget], cloud, server <url|clear>,\n"
               "          rgb <i|all> <r> <g> <b>, shot, heap\n");
        for (int i = 0; i < os_cmd_count(); i++) {
            printf("          %s\n", os_cmd_get(i)->help);
        }
    } else if (!strcmp(cmd, "info")) {
        int mv = bsp_battery_mv();
        printf("Solana OS %s (browser emulator)\nBattery: %d mV (%d%%)\nButtons: 0x%02lX\n",
               esp_app_get_description()->version, mv, bsp_battery_percent(mv), (unsigned long)bsp_buttons_get());
        bsp_sd_print_card();
    } else if (!strcmp(cmd, "apps")) {
        cmd_apps();
    } else if (!strcmp(cmd, "open") && argc == 2) {
        os_launch_id(argv[1]);
    } else if (!strcmp(cmd, "key") && argc >= 2) {
        for (int i = 1; i < argc; i++) {
            os_key_t k = os_key_parse(argv[i]);
            if (k == OS_KEY_NONE) {
                printf("unknown key '%s'\n", argv[i]);
                return;
            }
            os_inject_key(k);
        }
    } else if (!strcmp(cmd, "wifi")) {
        cmd_wifi(argc, argv);
    } else if (!strcmp(cmd, "rgb") && argc == 5) {
        int r = atoi(argv[2]), g = atoi(argv[3]), b = atoi(argv[4]);
        if (!strcmp(argv[1], "all")) {
            bsp_rgb_fill(r, g, b);
        } else {
            bsp_rgb_set(atoi(argv[1]), r, g, b);
        }
        bsp_rgb_show();
    } else if (!strcmp(cmd, "cloud")) {
        static const char *names[] = {"offline", "connecting", "unclaimed", "claimed", "error", "linked elsewhere"};
        os_cloud_info_t in;
        os_cloud_get_info(&in);
        printf("cloud: %s%s  server=%s\n  id=%s owner=%s\n  claim=%s\n", names[in.state], in.busy ? " (busy)" : "",
               in.server[0] ? in.server : "(not set)", in.id[0] ? in.id : "-", in.owner[0] ? in.owner : "-",
               in.claim_url[0] ? in.claim_url : "-");
    } else if (!strcmp(cmd, "server") && argc == 2) {
        os_cloud_set_server(!strcmp(argv[1], "clear") ? "" : argv[1]);
    } else if (!strcmp(cmd, "shot")) {
        os_screenshot();
    } else if (!strcmp(cmd, "heap")) {
        printf("heap: %u free, %u min, %u largest block (wasm heap)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    } else if (os_cmd_run(argc, argv) < 0) {
        printf("unknown command '%s', try 'help'\n", cmd);
    }
}

void console_start(void)
{
    ESP_LOGI(TAG, "console ready, type 'help'");
}
