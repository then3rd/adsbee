// GC9A01 240x240 round SPI LCD panel configuration for LovyanGFX under ESP-IDF.
//
// Attribution: the panel/bus parameter layout is adapted from ESP32-Plane-Radar
// (https://github.com/MatixYo/ESP32-Plane-Radar, MIT License, (c) 2026 MatixYo),
// file include/hardware/lgfx_config.hpp. Only the LovyanGFX configuration pattern is
// reused; the Arduino-framework data layer of that project is not. Reuse of MIT-licensed
// code in this GPL-3.0 project is permitted with attribution.
//
// Pins come from BSP (bsp.hh) and map onto the aux expansion header (SPI3_HOST). This whole
// header is only referenced from display sources that are compiled out unless WITH_DISPLAY.
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "bsp.hh"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Light_PWM light_;

   public:
    LGFX() {
        {
            auto cfg = bus_.config();
            cfg.spi_host = BSP::display_spi_handle;
            cfg.spi_mode = 0;
            cfg.freq_write = BSP::display_spi_clk_hz;
            cfg.freq_read = 16 * 1000 * 1000;
            cfg.spi_3wire = true;      // GC9A01 has no MISO line; read over MOSI.
            cfg.use_lock = true;       // Share the SPI bus safely (single owner here, but cheap).
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = BSP::display_spi_clk_pin;
            cfg.pin_mosi = BSP::display_spi_mosi_pin;
            cfg.pin_miso = -1;         // Not connected.
            cfg.pin_dc = BSP::display_dc_pin;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }

        {
            auto cfg = panel_.config();
            cfg.pin_cs = BSP::display_spi_cs_pin;
            cfg.pin_rst = BSP::display_rst_pin;
            cfg.pin_busy = -1;

            cfg.panel_width = 240;
            cfg.panel_height = 240;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;

            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;         // GC9A01 panels are typically inverted.
            cfg.rgb_order = false;     // BGR order.
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;    // SPI3 is dedicated to the display when WITH_DISPLAY.

            panel_.config(cfg);
        }

        // Optional backlight. If BL is tied to 3V3 in hardware, display_backlight_pin is
        // GPIO_NUM_NC and no PWM light is attached.
        if (BSP::display_backlight_pin != GPIO_NUM_NC) {
            auto cfg = light_.config();
            cfg.pin_bl = BSP::display_backlight_pin;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            light_.config(cfg);
            panel_.setLight(&light_);
        }

        setPanel(&panel_);
    }
};
