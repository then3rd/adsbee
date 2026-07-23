# Third-Party Notices

ADSBee is licensed under GPL-3.0-only (see `LICENSE`). It incorporates portions of the
following third-party software, whose licenses are compatible with redistribution under
GPL-3.0.

## ESP32-Plane-Radar

- Project: ESP32-Plane-Radar — https://github.com/MatixYo/ESP32-Plane-Radar
- Copyright: (c) 2026 MatixYo
- License: MIT

The GC9A01 round-display support on the ESP32-S3
(`firmware/adsbee_1090/esp/main/peripherals/display/`) ports the LovyanGFX panel
configuration and the radar rendering/projection math from ESP32-Plane-Radar. The
equirectangular `latLonToScreen` projection is reused with a bug fix (application of the
`cos(latitude)` longitude correction, which the original omits). The Arduino-framework data
layer of the original project (its remote HTTPS/JSON poller) is not used.

Affected files carry an inline attribution comment:
- `firmware/adsbee_1090/esp/main/peripherals/display/lgfx_config.hpp`
- `firmware/adsbee_1090/esp/main/peripherals/display/radar_view.hh`
- `firmware/adsbee_1090/esp/main/peripherals/display/radar_view.cpp`
- `firmware/adsbee_1090/esp/main/peripherals/display/display.hh`
- `firmware/adsbee_1090/esp/main/peripherals/display/display.cpp`

### MIT License (ESP32-Plane-Radar)

```
MIT License

Copyright (c) 2026 MatixYo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## LovyanGFX

- Project: LovyanGFX — https://github.com/lovyan03/LovyanGFX
- License: FreeBSD (BSD-2-Clause) with portions under other permissive licenses

Pulled in as an ESP-IDF managed component (`lovyan03/LovyanGFX`, see
`firmware/adsbee_1090/esp/main/idf_component.yml`) and linked only when the display is
built (`WITH_DISPLAY`).
