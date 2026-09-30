// PB1..PB6 on the TCA9534 expander (INT on GPIO25) plus the BOOT button.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"

static const char *TAG = "bsp_buttons";

#define TCA9534_REG_INPUT   0
#define TCA9534_REG_CONFIG  3
#define POLL_MS             50

static i2c_master_dev_handle_t s_tca;
static SemaphoreHandle_t s_irq;
static bsp_btn_cb_t s_cb;
static volatile uint32_t s_pressed;

static void IRAM_ATTR tca_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    portYIELD_FROM_ISR(woken);
}

static uint32_t read_buttons(void)
{
    uint8_t reg = TCA9534_REG_INPUT, port = 0xFF;
    if (i2c_master_transmit_receive(s_tca, &reg, 1, &port, 1, 50) != ESP_OK) {
        return s_pressed;   // keep last state on a bus hiccup
    }
    uint32_t pressed = ~port & ((1u << BOARD_BTN_COUNT) - 1);
    if (!gpio_get_level(BOARD_BOOT_BTN)) {
        pressed |= BSP_BTN_BOOT;
    }
    return pressed;
}

static void button_task(void *arg)
{
    for (;;) {
        // The expander interrupt wakes us early; polling covers BOOT and any missed edge.
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(POLL_MS));
        uint32_t now = read_buttons();
        uint32_t changed = now ^ s_pressed;
        if (changed) {
            s_pressed = now;
            if (s_cb) {
                s_cb(now, changed);
            }
        }
    }
}

esp_err_t bsp_buttons_start(bsp_btn_cb_t cb)
{
    s_cb = cb;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TCA9534_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_tca), TAG, "tca9534");
    uint8_t all_inputs[2] = {TCA9534_REG_CONFIG, 0xFF};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_tca, all_inputs, sizeof(all_inputs), 50), TAG, "tca9534 not responding");

    gpio_config_t io = {
        .pin_bit_mask = BIT64(BOARD_TCA9534_INT) | BIT64(BOARD_BOOT_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    gpio_set_intr_type(BOARD_TCA9534_INT, GPIO_INTR_NEGEDGE);
    s_irq = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_TCA9534_INT, tca_isr, NULL), TAG, "isr");

    s_pressed = read_buttons();
    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
    return ESP_OK;
}

uint32_t bsp_buttons_get(void)
{
    return s_pressed;
}
