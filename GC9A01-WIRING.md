# GC9A01 Display — Wiring Hookup

How to physically connect the 1.28" GC9A01 240×240 round SPI LCD to the ADSBee
`adsbee_1090u` board. The display plugs into the **aux expansion header** (the same
2.54 mm header the optional W5500 Ethernet add-on uses) on the ESP32-S3 network
coprocessor. **The display and the W5500 are mutually exclusive** — they share the
SPI3 bus and GPIO47/48, so only one can be installed at a time.

> Firmware note: display support is compiled in only with the `WITH_DISPLAY` build
> flag, which also compiles out the Ethernet SPI bring-up (`EthernetInit()`) so the
> two SPI3 owners can never coexist. See `GC9A01.md` for the full software plan.

## Pin map (as implemented)

Pins are defined in `firmware/adsbee_1090/esp/main/bsp.hh` and consumed by
`peripherals/display/lgfx_config.hpp`. They mirror the authoritative aux-bus pins on
`CommsManagerConfig` (`comms/comms.hh:43-52`).

| GC9A01 pin | Function            | Board net / header pin      | ESP32-S3 GPIO |
|------------|---------------------|-----------------------------|---------------|
| VCC        | 3.3 V power         | 3V3                         | —             |
| GND        | Ground              | GND                         | —             |
| SCL / SCLK | SPI clock           | AUX_SPI_CLK                 | **GPIO17**    |
| SDA / MOSI | SPI data in         | AUX_SPI_MOSI                | **GPIO14**    |
| CS         | Chip select         | AUX_SPI_CS                  | **GPIO18**    |
| DC         | Data/command select | AUX_GPIOC (was W5500 INT)   | **GPIO47**    |
| RST        | Reset               | AUX_GPIOB (was W5500 RST)   | **GPIO48**    |
| BL         | Backlight           | AUX_GPIOA, **or** tie to 3V3| GPIO_NUM_NC*  |

\* `display_backlight_pin` defaults to `GPIO_NUM_NC` in `bsp.hh` (backlight assumed
tied to 3V3). If you route BL to the 3rd aux GPIO (AUX_GPIOA), set
`display_backlight_pin` to that pin and LovyanGFX drives it as a PWM light.

- **No MISO.** The GC9A01 is a 3-wire SPI panel (`spi_3wire = true`, `pin_miso = -1`);
  reads happen over MOSI. Leave the panel's SDA as the only data line — do not wire
  the header's AUX_SPI_MISO (GPIO13).
- **Bus:** SPI3_HOST, mode 0, 40 MHz write clock (`display_spi_clk_hz`, aux bus rated
  to 80 MHz). SPI3 is dedicated to the display in a `WITH_DISPLAY` build.

## Header wiring diagram

The aux expansion header is a 13-pin header. The silkscreen pin order differs between
the top and bottom of the board (they mirror each other). Only 9 of the 13 pins are
used by the display; the UART and 5 V pins are left unconnected.

**Top-board pin order** (as silkscreened on the top side):

```
  Pin   Header label   Net             ──►  GC9A01
  ───   ────────────   ─────────────        ────────────
   1    5vin           —                    (unused)
   2    GND            —                    (unused)
   3    TX             COMMS_UART_TX        (unused)
   4    RX             COMMS_UART_RX        (unused)
   5    3.3v           —               ──►  VCC
   6    IOA            AUX_GPIOA  (BL)  ──►  BL   (optional; or tie BL to 3V3)
   7    IOB            AUX_GPIOB  (48)  ──►  RST
   8    IOC            AUX_GPIOC  (47)  ──►  DC
   9    CS             AUX_SPI_CS (18)  ──►  CS
  10    CLK            AUX_SPI_CLK(17)  ──►  SCL / SCLK
  11    MOSI           AUX_SPI_MOSI(14) ──►  SDA / MOSI
  12    MISO           AUX_SPI_MISO(13)     (unused — GC9A01 has no MISO)
  13    GND            —               ──►  GND
```

**Bottom-board pin order** (reverse of the top, no silkscreen):

```
  Pin   Header label   Net
  ───   ────────────   ─────────────
   1    GND            —               ──►  GND
   2    MISO           AUX_SPI_MISO(13)     (unused)
   3    MOSI           AUX_SPI_MOSI(14) ──►  SDA / MOSI
   4    CLK            AUX_SPI_CLK(17)  ──►  SCL / SCLK
   5    CS             AUX_SPI_CS (18)  ──►  CS
   6    IOC            AUX_GPIOC  (47)  ──►  DC
   7    IOB            AUX_GPIOB  (48)  ──►  RST
   8    IOA            AUX_GPIOA  (BL)  ──►  BL   (optional; or tie BL to 3V3)
   9    3.3v           —               ──►  VCC
  10    RX             COMMS_UART_RX        (unused)
  11    TX             COMMS_UART_TX        (unused)
  12    GND            —                    (unused)
  13    5vin           —                    (unused)
```

Note the GND at both ends and that 3.3v (not 5vin) powers the panel.

## Safety / hardware notes
- **Power the panel from 3.3 V**, not 5 V. VCC and the logic lines are 3.3 V.
- **Do not install the W5500 and the display simultaneously** — same bus, same pins.
