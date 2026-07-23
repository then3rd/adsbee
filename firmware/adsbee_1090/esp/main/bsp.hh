#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"

class BSP {
   public:
    static const spi_host_device_t copro_spi_handle = SPI2_HOST;
    static const gpio_num_t copro_spi_mosi_pin = GPIO_NUM_41;
    static const gpio_num_t copro_spi_miso_pin = GPIO_NUM_42;
    static const gpio_num_t copro_spi_clk_pin = GPIO_NUM_40;
    static const gpio_num_t copro_spi_cs_pin = GPIO_NUM_39;
    static const gpio_num_t copro_spi_handshake_pin = GPIO_NUM_0;

    static const gpio_num_t network_led_pin = GPIO_NUM_5;

    // GC9A01 round display (optional, WITH_DISPLAY). Reuses the aux expansion header that the
    // W5500 Ethernet add-on otherwise uses (SPI3_HOST + GPIO47/48). The display and W5500 are
    // mutually exclusive; the authoritative aux-bus pin definitions live on CommsManagerConfig
    // (comms.hh). GPIO47/48 are not ESP32-S3 strapping pins (those are 0/3/45/46), so they are
    // safe to drive at boot. The GC9A01 has no MISO line.
    static const spi_host_device_t display_spi_handle = SPI3_HOST;
    static const gpio_num_t display_spi_clk_pin = GPIO_NUM_17;   // AUX_SPI_CLK
    static const gpio_num_t display_spi_mosi_pin = GPIO_NUM_14;  // AUX_SPI_MOSI
    static const gpio_num_t display_spi_cs_pin = GPIO_NUM_18;    // AUX_SPI_CS
    static const gpio_num_t display_dc_pin = GPIO_NUM_47;        // AUX_GPIOC (was W5500 INT)
    static const gpio_num_t display_rst_pin = GPIO_NUM_48;       // AUX_GPIOB (was W5500 RST)
    // Backlight on AUX_GPIOA (3rd header GPIO). Set to GPIO_NUM_NC if BL is tied to 3V3 instead.
    static const gpio_num_t display_backlight_pin = GPIO_NUM_NC;
    static const int display_spi_clk_hz = 40 * 1000 * 1000;  // 40 MHz (aux bus can go to 80 MHz).
};

extern BSP bsp;