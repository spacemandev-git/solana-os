// UART0 command console. Besides the hardware commands from the reference
// firmware it can drive the UI (open, key, shot), which is how the OS is
// tested without touching the buttons: see tools/badge.py.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "bsp.h"
#include "console.h"
#include "os.h"
#include "os_wifi.h"
#include "os_cloud.h"
#include "os_cmd.h"

static const char *TAG = "console";

static int cmd_heap(int argc, char **argv);

static int cmd_info(int argc, char **argv)
{
    printf("Solana OS %s\nBoard: ESP32-C5 badge\n"
           "  SPI2 SCK=%d MOSI=%d MISO=%d | LCD CS=%d DC=%d RST=%d BL=%d | SD CS=%d\n"
           "  I2C SCL=%d SDA=%d TCA9534@0x%02X INT=%d | BOOT=%d | WS2812 x%d on %d\n"
           "  Battery ADC GPIO%d (x%d) | UART0 TX=%d RX=%d\n",
           esp_app_get_description()->version, BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO, BOARD_LCD_CS, BOARD_LCD_DC, BOARD_LCD_RST, BOARD_LCD_BL,
           BOARD_SD_CS, BOARD_I2C_SCL, BOARD_I2C_SDA, BOARD_TCA9534_ADDR, BOARD_TCA9534_INT, BOARD_BOOT_BTN,
           BOARD_RGB_COUNT, BOARD_RGB_GPIO, BOARD_BAT_ADC_GPIO, BOARD_BAT_DIVIDER, BOARD_UART_TX, BOARD_UART_RX);
    int mv = bsp_battery_mv();
    printf("Battery: %d mV (%d%%)\nButtons: 0x%02lX\nHeap: %u bytes free\n", mv, bsp_battery_percent(mv),
           (unsigned long)bsp_buttons_get(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    bsp_sd_print_card();
    return 0;
}

static int sd_speed_test(void)
{
    const char *path = BSP_SD_MOUNT "/speed.bin";
    const int chunk = 4096, total = 256 * 1024;
    uint8_t *buf = malloc(chunk);
    if (!buf) {
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("open for write failed\n");
        free(buf);
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    for (int off = 0; off < total; off += chunk) {
        for (int i = 0; i < chunk; i++) {
            buf[i] = (uint8_t)((off + i) * 7);
        }
        fwrite(buf, 1, chunk, f);
    }
    fclose(f);
    int64_t t1 = esp_timer_get_time();
    f = fopen(path, "rb");
    int bad = f ? 0 : 1;
    for (int off = 0; f && off < total; off += chunk) {
        if (fread(buf, 1, chunk, f) != chunk) {
            bad++;
            break;
        }
        for (int i = 0; i < chunk; i++) {
            if (buf[i] != (uint8_t)((off + i) * 7)) {
                bad++;
                break;
            }
        }
    }
    if (f) {
        fclose(f);
    }
    int64_t t2 = esp_timer_get_time();
    unlink(path);
    free(buf);
    printf("SD test: wrote %d KB at %lld KB/s, read+verify at %lld KB/s -> %s\n", total / 1024,
           (int64_t)total * 1000 / (t1 - t0), (int64_t)total * 1000 / (t2 - t1), bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

static int cmd_sd(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "info";
    esp_err_t err = ESP_OK;
    if (!strcmp(sub, "info")) {
        bsp_sd_print_card();
        return 0;
    } else if (!strcmp(sub, "mount")) {
        err = bsp_sd_mount();
    } else if (!strcmp(sub, "unmount")) {
        err = bsp_sd_unmount();
    } else if (!strcmp(sub, "format")) {
        if (argc < 3 || strcmp(argv[2], "yes")) {
            printf("This erases the card. Run: sd format yes\n");
            return 1;
        }
        err = bsp_sd_format();
    } else if (!strcmp(sub, "test")) {
        int rc = sd_speed_test();
        bsp_sd_refresh_usage();
        return rc;
    } else {
        printf("usage: sd [info|mount|unmount|format yes|test]\n");
        return 1;
    }
    printf("%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_ls(int argc, char **argv)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s%s", BSP_SD_MOUNT, argc > 1 && argv[1][0] != '/' ? "/" : "", argc > 1 ? argv[1] : "");
    DIR *d = opendir(path);
    if (!d) {
        printf("cannot open %s\n", path);
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        char full[400];
        struct stat st = {0};
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        stat(full, &st);
        printf("%s %10ld  %s\n", de->d_type == DT_DIR ? "d" : "-", (long)st.st_size, de->d_name);
    }
    closedir(d);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return 1;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT, argv[1][0] == '/' ? argv[1] + 1 : argv[1]);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("\n");
    return 0;
}

static int cmd_rgb(int argc, char **argv)
{
    if (argc != 5) {
        printf("usage: rgb <0-%d|all> <r> <g> <b>   (0-255)\n", BOARD_RGB_COUNT - 1);
        return 1;
    }
    int r = atoi(argv[2]), g = atoi(argv[3]), b = atoi(argv[4]);
    if (!strcmp(argv[1], "all")) {
        bsp_rgb_fill(r, g, b);
    } else {
        bsp_rgb_set(atoi(argv[1]), r, g, b);
    }
    bsp_rgb_show();
    return 0;
}

static int cmd_apps(int argc, char **argv)
{
    const os_app_t *cur = os_current();
    for (int i = 0; i < os_app_count; i++) {
        printf("%c %-8s %s\n", os_apps[i] == cur ? '*' : ' ', os_apps[i]->id, os_apps[i]->name);
    }
    return 0;
}

static int cmd_open(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: open <app id|home>\n");
        return 1;
    }
    os_launch_id(argv[1]);
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: key <up|down|left|right|select|back|home>... \n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        os_key_t k = os_key_parse(argv[i]);
        if (k == OS_KEY_NONE) {
            printf("unknown key '%s'\n", argv[i]);
            return 1;
        }
        os_inject_key(k);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "connect") && argc >= 3) {
        // Optional channel: join the SSID's AP there (like picking a band row in the app).
        return os_wifi_connect_ch(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? atoi(argv[4]) : 0) == ESP_OK ? 0 : 1;
    } else if (!strcmp(sub, "forget")) {
        os_wifi_forget();
        return 0;
    } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        os_wifi_set_enabled(!strcmp(sub, "on"));
        return 0;
    } else if (strcmp(sub, "status")) {
        printf("usage: wifi [status|on|off|connect <ssid> [pass [channel]]|forget]\n");
        return 1;
    }
    static const char *names[] = {"idle", "connecting", "connected", "failed", "suspended"};
    os_wifi_info_t in;
    os_wifi_get_info(&in);
    printf("wifi: %s (%s)  ssid='%s' ip=%s rssi=%d ch=%d %s\n", names[in.state], in.enabled ? "on" : "off", in.ssid,
           in.ip, in.rssi, in.channel, in.fail_reason ? in.fail_reason : "");
    return 0;
}

static int cmd_server(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: server [url|clear]\n");
        return 1;
    }
    if (argc == 2) {
        const char *url = argv[1];
        if (!strcmp(url, "clear")) {
            url = "";
        } else if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
            printf("the server must start with http:// or https://\n");
            return 1;
        }
        os_cloud_set_server(url);
    }
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    printf("server: %s\n", in.server[0] ? in.server : "(not set)");
    return 0;
}

static int cmd_cloud(int argc, char **argv)
{
    static const char *names[] = {"offline", "connecting", "unclaimed", "claimed", "error", "linked elsewhere"};
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    printf("cloud: %s%s  server=%s\n  id=%s owner=%s\n  claim=%s\n", names[in.state], in.busy ? " (busy)" : "",
           in.server[0] ? in.server : "(not set)", in.id[0] ? in.id : "-", in.owner[0] ? in.owner : "-",
           in.claim_url[0] ? in.claim_url : "-");
    printf("  last http=%d next hello in %d s%s%s\n", in.http_status, in.retry_s, in.error ? "  error: " : "",
           in.error ? in.error : "");
    if (in.message[0]) {
        printf("  server: %s\n", in.message);
    }
    return cmd_heap(0, NULL);
}

static int cmd_shot(int argc, char **argv)
{
    os_screenshot();
    vTaskDelay(pdMS_TO_TICKS(100));
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    printf("heap: %u free, %u min, %u largest block\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "sol>";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "info", .help = "Board pinout, battery, buttons, SD card", .func = cmd_info },
        { .command = "sd", .help = "sd [info|mount|unmount|format yes|test]", .func = cmd_sd },
        { .command = "ls", .help = "ls [dir] - list files on the SD card", .func = cmd_ls },
        { .command = "cat", .help = "cat <file> - print a text file from the SD card", .func = cmd_cat },
        { .command = "rgb", .help = "rgb <index|all> <r> <g> <b>", .func = cmd_rgb },
        { .command = "apps", .help = "List installed apps", .func = cmd_apps },
        { .command = "open", .help = "open <app id|home> - launch an app", .func = cmd_open },
        { .command = "key", .help = "key <up|down|left|right|select|back|home>... - press keys", .func = cmd_key },
        { .command = "wifi", .help = "wifi [status|on|off|connect <ssid> [pass [channel]]|forget]", .func = cmd_wifi },
        { .command = "server", .help = "server [url|clear] - show or set the badge server", .func = cmd_server },
        { .command = "cloud", .help = "Badge server link: state, badge ID, owner, claim URL", .func = cmd_cloud },
        { .command = "shot", .help = "Dump a screenshot (decode with tools/badge.py)", .func = cmd_shot },
        { .command = "heap", .help = "Heap usage", .func = cmd_heap },
        { .command = "reboot", .help = "Restart the badge", .func = cmd_reboot },
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    // Commands from portable code (os_cmd.h): same signature as esp_console's.
    for (int i = 0; i < os_cmd_count(); i++) {
        const os_cmd_t *c = os_cmd_get(i);
        const esp_console_cmd_t ec = { .command = c->name, .help = c->help, .func = c->fn };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ec));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready, type 'help'");
}
