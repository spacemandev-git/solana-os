// Shared SPI bus + microSD.
//
// Four problems stopped the stock firmware from reading the card. The first
// three affect any card; the fourth is specific to the fitted one.
//
// 1. Bus ordering. A freshly powered SD card is in SD (native) mode, where it
//    ignores CS and decodes MOSI as its CMD line. LCD traffic before the card
//    has been switched to SPI mode is taken as SD commands. Fix: mount the
//    card before the display is started.
//
// 2. Lagging error flags. The fitted card reports a command's error bits in
//    the R1 of the *next* command. The SDIO probes sdmmc_card_init() sends
//    (CMD52, CMD5) are illegal for a memory card, so the following CMD0 or
//    CMD59 comes back flagged and init aborts. sd_do_transaction() answers
//    those probes itself, as a non-SDIO card would, without sending them.
//
// 3. Filesystem. The card was formatted FAT32 with 30,624 clusters. FAT32
//    needs at least 65,525, so FatFs rejects it (FR_NO_FILESYSTEM) even though
//    PCs accept it. bsp_sd_format() rewrites it as a spec-valid volume.
//
// 4. CS ignored. The fitted no-name card ("asdfg", 120 MB) keeps executing
//    commands from bus traffic while CS is high (a spec violation; the board
//    wiring checks out). LCD traffic then acts as random card commands and
//    wedges it within seconds, even when parked in idle with CRC checking on.
//    Such a card cannot share this bus with the display, so probe_ignores_cs()
//    detects it and the card is rejected rather than risking its data.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"

static const char *TAG = "bsp_sd";

static SemaphoreHandle_t s_lock;        // recursive: format re-enters mount
static spi_device_handle_t s_raw;       // raw access with manual CS, for probing and parking
static sdmmc_card_t *s_card;
static bsp_sd_state_t s_state = BSP_SD_NOT_MOUNTED;
static esp_err_t s_last_err;
static uint64_t s_total_bytes, s_free_bytes;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

esp_err_t bsp_spi_bus_init(void)
{
    static bool s_done;
    if (s_done) {
        return ESP_OK;
    }
    // Deselect both devices before the first clock edge on the bus.
    gpio_config_t cs = {
        .pin_bit_mask = BIT64(BOARD_LCD_CS) | BIT64(BOARD_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs), TAG, "cs gpio");
    gpio_set_level(BOARD_LCD_CS, 1);
    gpio_set_level(BOARD_SD_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_SPI_SCK,
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);

    spi_device_interface_config_t raw_cfg = {
        .clock_speed_hz = 400 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BOARD_SPI_HOST, &raw_cfg, &s_raw), TAG, "raw sd device");
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_done = true;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Raw SPI-mode commands with manual CS (only used while sdspi is idle).

static uint8_t crc7(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = d[i];
        for (int j = 0; j < 8; j++) {
            crc <<= 1;
            if ((b ^ crc) & 0x80) {
                crc ^= 0x09;
            }
            b <<= 1;
        }
    }
    return (crc << 1) | 1;
}

static void raw_xfer(uint8_t *buf, int len)
{
    spi_transaction_t t = { .length = len * 8, .tx_buffer = buf, .rx_buffer = buf };
    spi_device_polling_transmit(s_raw, &t);
}

// Returns R1, or 0xFF if the card did not answer.
static uint8_t raw_cmd(uint8_t cmd, uint32_t arg, bool select)
{
    uint8_t b[18];
    memset(b, 0xFF, sizeof(b));
    b[1] = 0x40 | cmd;
    b[2] = arg >> 24;
    b[3] = arg >> 16;
    b[4] = arg >> 8;
    b[5] = arg;
    b[6] = crc7(b + 1, 5);
    if (select) {
        gpio_set_level(BOARD_SD_CS, 0);
    }
    raw_xfer(b, sizeof(b));
    gpio_set_level(BOARD_SD_CS, 1);
    uint8_t ff = 0xFF;
    raw_xfer(&ff, 1);
    for (int i = 7; i < sizeof(b); i++) {
        if (!(b[i] & 0x80)) {
            return b[i];
        }
    }
    return 0xFF;
}

// sdspi leaves CS as an undriven input when its device is removed.
static void drive_cs_high(void)
{
    gpio_config_t cs = { .pin_bit_mask = BIT64(BOARD_SD_CS), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cs);
    gpio_set_level(BOARD_SD_CS, 1);
}

// Leave CS driven high and the card in idle state after it is released.
static void park_card(void)
{
    drive_cs_high();
    raw_cmd(MMC_GO_IDLE_STATE, 0, true);
}

// Send CMD0 with CS *high*. A compliant card ignores it and answers the
// following CMD58 exactly as before (R1 = 0x00). Any change - idle, or with
// this card's lagging error flags, ILLEGAL_COMMAND - means it decodes traffic
// addressed to other devices on the bus.
static bool probe_ignores_cs(void)
{
    drive_cs_high();
    uint8_t r1_before = raw_cmd(SD_READ_OCR, 0, true);
    raw_cmd(MMC_GO_IDLE_STATE, 0, false);
    uint8_t r1_after = raw_cmd(SD_READ_OCR, 0, true);
    ESP_LOGI(TAG, "CS compliance probe: R1 %02x -> %02x", r1_before, r1_after);
    return r1_before == 0x00 && r1_after != 0x00;
}

// ---------------------------------------------------------------------------

static esp_err_t sd_do_transaction(int slot, sdmmc_command_t *cmd)
{
    if (cmd->opcode == SD_IO_RW_DIRECT || cmd->opcode == SD_IO_SEND_OP_COND) {
        return ESP_ERR_NOT_SUPPORTED;   // "illegal command": not an SDIO card
    }
    esp_err_t err = sdspi_host_do_transaction(slot, cmd);
    // A card left mid-transfer answers CMD0 with stream data. End the transfer
    // (CMD12) and clear its status (CMD13) before trying again.
    for (int i = 0; i < 3 && cmd->opcode == MMC_GO_IDLE_STATE && (cmd->flags & SCF_RSP_R1) && err != ESP_OK; i++) {
        sdmmc_command_t stop = { .opcode = MMC_STOP_TRANSMISSION, .flags = SCF_CMD_AC | SCF_RSP_R1B, .timeout_ms = 100 };
        sdmmc_command_t status = { .opcode = MMC_SEND_STATUS, .flags = SCF_CMD_AC | SCF_RSP_R1, .timeout_ms = 100 };
        sdspi_host_do_transaction(slot, &stop);
        sdspi_host_do_transaction(slot, &status);
        err = sdspi_host_do_transaction(slot, cmd);
    }
    // Last resort: CRC checking of data is optional in SPI mode.
    if (cmd->opcode == SD_CRC_ON_OFF && (err == ESP_ERR_NOT_SUPPORTED || cmd->error == ESP_ERR_NOT_SUPPORTED)) {
        ESP_LOGW(TAG, "card rejected CMD59 (CRC on); continuing without data CRC");
        cmd->error = ESP_OK;
        return ESP_OK;
    }
    return err;
}

// ---------------------------------------------------------------------------

static void update_usage_locked(void)
{
    s_total_bytes = s_free_bytes = 0;
    if (s_card && s_state == BSP_SD_MOUNTED) {
        esp_vfs_fat_info(BSP_SD_MOUNT, &s_total_bytes, &s_free_bytes);
    }
}

static esp_err_t mount_locked(bool format_if_unusable)
{
    if (s_card) {
        return ESP_OK;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.do_transaction = sd_do_transaction;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SPI_HOST;
    slot.gpio_cs = BOARD_SD_CS;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = format_if_unusable,
        .max_files = 5,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT, &host, &slot, &cfg, &s_card);
    // The card is initialised after both a clean mount and a FAT failure.
    if ((err == ESP_OK || err == ESP_FAIL) && probe_ignores_cs()) {
        ESP_LOGE(TAG, "card executes commands while deselected; it cannot share the bus with the LCD");
        if (err == ESP_OK) {
            esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT, s_card);
        }
        err = ESP_ERR_NOT_SUPPORTED;
        s_state = BSP_SD_INCOMPATIBLE;
    } else {
        switch (err) {
        case ESP_OK:
            s_state = BSP_SD_MOUNTED;
            break;
        case ESP_FAIL:          // card initialised, f_mount() failed
            s_state = BSP_SD_BAD_FS;
            break;
        case ESP_ERR_TIMEOUT:   // no answer to CMD0
            s_state = BSP_SD_NO_CARD;
            break;
        default:
            s_state = BSP_SD_ERROR;
            break;
        }
    }
    s_last_err = err;
    if (err != ESP_OK) {
        s_card = NULL;
        park_card();
        ESP_LOGW(TAG, "mount failed: %s -> %s", esp_err_to_name(err), bsp_sd_state_str(s_state));
    } else {
        update_usage_locked();
        ESP_LOGI(TAG, "mounted %s", s_card->cid.name);
    }
    return err;
}

esp_err_t bsp_sd_mount(void)
{
    ESP_RETURN_ON_ERROR(bsp_spi_bus_init(), TAG, "bus");
    // Keep sdspi's per-command chatter out of the console.
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);
    LOCK();
    esp_err_t err = mount_locked(false);
    UNLOCK();
    return err;
}

esp_err_t bsp_sd_unmount(void)
{
    LOCK();
    esp_err_t err = ESP_OK;
    if (s_card) {
        err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT, s_card);
        s_card = NULL;
        park_card();
    }
    s_state = BSP_SD_NOT_MOUNTED;
    UNLOCK();
    return err;
}

esp_err_t bsp_sd_format(void)
{
    LOCK();
    esp_err_t err;
    if (s_card) {
        ESP_LOGW(TAG, "formatting mounted card");
        err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT, s_card);
        update_usage_locked();
    } else {
        // Mounting with format_if_mount_failed repartitions and formats a card
        // whose volume FatFs cannot use.
        ESP_LOGW(TAG, "formatting unusable card");
        err = mount_locked(true);
    }
    UNLOCK();
    return err;
}

void bsp_sd_refresh_usage(void)
{
    LOCK();
    update_usage_locked();
    UNLOCK();
}

void bsp_sd_get_info(bsp_sd_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        out->state = BSP_SD_NOT_MOUNTED;
        return;
    }
    LOCK();
    out->state = s_state;
    out->last_err = s_last_err;
    if (s_card) {
        strlcpy(out->name, s_card->cid.name, sizeof(out->name));
        out->type = (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
        out->card_mb = (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
        out->total_bytes = s_total_bytes;
        out->free_bytes = s_free_bytes;
    }
    UNLOCK();
}

void bsp_sd_print_card(void)
{
    LOCK();
    if (s_card) {
        sdmmc_card_print_info(stdout, s_card);
    } else {
        printf("SD: %s (last error %s)\n", bsp_sd_state_str(s_state), esp_err_to_name(s_last_err));
    }
    UNLOCK();
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
