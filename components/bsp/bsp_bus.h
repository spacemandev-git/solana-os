#pragma once

// The LCD and the microSD card share SPI2. esp_lcd queues each flush as DMA
// transfers that finish after it has released the bus, and the SPI bus lock
// only makes a device wait for its *own* queued transfers: an SD command that
// starts mid-flush trips spi_hal_setup_trans's "running cmd" assert (long card
// writes during redraws, e.g. an OTA download). A flush holds the LCD from
// LV_EVENT_FLUSH_START until its last transfer is done; SD commands hold it too.
// No-ops until the display has started.
void bsp_lcd_hold(void);
void bsp_lcd_release(void);
