// GC9A01 round-display driver for ADSBee (ESP32-S3, ESP-IDF).
//
// Owns the LovyanGFX device, an optional off-screen double-buffer sprite, and the RadarView
// renderer. Update() is called from the ESP32 main loop (single-task, cooperative) and is
// internally rate-limited; it reads the ESP32's in-memory AircraftDictionary and the RP2040
// receiver position directly (no network involved).
//
// Attribution: display bring-up structure adapted from ESP32-Plane-Radar (MIT, (c) 2026
// MatixYo), src/hardware/display.cpp. See lgfx_config.hpp / radar_view.hh for details.
#pragma once

#include "lgfx_config.hpp"
#include "radar_view.hh"

class Display {
   public:
    // Minimum interval between rendered frames. 200 ms => ~5 Hz, keeping render cost bounded and
    // staying clear of the SPI2 coprocessor link and the web server.
    static constexpr uint32_t kMinFrameIntervalMs = 200;

    // Height, in pixels, of the reusable off-screen strip used for banded rendering. There is no
    // PSRAM and internal SRAM is fragmented (WiFi/lwIP/TLS): a full-frame buffer does not fit at
    // any color depth (240x240 is ~115 KB at 16-bit / ~57 KB at 8-bit, but the largest free block
    // is only ~31 KB). Instead the whole scene is redrawn into a small 240xkBandHeight strip and
    // pushed once per band, so each screen region is written exactly once per frame (no flicker)
    // while keeping full 16-bit color. 40 divides 240 evenly (6 bands); 240*40*2 = ~19 KB fits the
    // free block with margin. Must evenly divide RadarView::kScreenHeight.
    static constexpr int16_t kBandHeight = 40;

    // How long the boot splash (ADSBee logo) stays on screen before the radar view takes over.
    static constexpr uint32_t kSplashDurationMs = 3000;

    /**
     * Initialize the display: bring up the panel over SPI3, allocate the off-screen sprite (or
     * fall back to direct-to-panel drawing if the ~115 KB allocation fails), show the boot splash
     * for kSplashDurationMs, and draw the initial radar background. Must be called once at
     * startup. Blocks for kSplashDurationMs; safe to call from app_main() before the main loop.
     * @retval True if the panel initialized, false otherwise. A sprite allocation failure is
     *         not a failure (direct draw is used instead).
     */
    bool Init();

    /**
     * Render one frame if the rate-limit interval has elapsed. Reads the aircraft dictionary and
     * receiver position, projects and draws all valid targets. Cheap no-op between frames.
     */
    void Update();

   private:
    // Draw the embedded ADSBee logo centered on the panel and hold it for kSplashDurationMs.
    // Blocking (vTaskDelay); called once from Init() before the radar view starts rendering.
    void ShowSplash();

    // Whether a valid receiver center position is currently available.
    bool ResolveCenter();

    // Render one full frame: draw the scene strip-by-strip into band_ and push each strip (banded,
    // flicker-free), or straight to lcd_ if band_ is unavailable (direct fallback, flickers).
    void RenderFrame(bool position_valid);

    // Draw the full radar scene (background + all valid targets) onto gfx at the renderer's
    // current origin. Shared by the banded and direct-draw paths.
    void DrawScene(lgfx::LGFXBase* gfx, bool position_valid);

    LGFX lcd_;
    // Reusable 240xkBandHeight off-screen strip for banded, flicker-free rendering. nullptr if the
    // (small) allocation failed, in which case we fall back to drawing directly to lcd_ (flickers).
    LGFX_Sprite* band_ = nullptr;
    RadarView radar_;

    bool initialized_ = false;
    uint32_t last_frame_timestamp_ms_ = 0;
};

extern Display display;
