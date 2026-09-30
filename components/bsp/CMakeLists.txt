idf_component_register(
    SRCS "bsp_sd.c" "bsp_display.c" "bsp_buttons.c" "bsp_rgb.c" "bsp_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_i2c esp_lcd
    PRIV_REQUIRES esp_driver_gpio esp_driver_sdspi esp_adc fatfs sdmmc vfs)
