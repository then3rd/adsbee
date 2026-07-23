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

    /**
     * Initialize the display: bring up the panel over SPI3, allocate the off-screen sprite (or
     * fall back to direct-to-panel drawing if the ~115 KB allocation fails), and draw the
     * initial background. Must be called once at startup.
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
    // Whether a valid receiver center position is currently available.
    bool ResolveCenter();

    LGFX lcd_;
    LGFX_Sprite* canvas_ = nullptr;  // Off-screen buffer; nullptr => draw directly to lcd_.
    RadarView radar_;

    bool initialized_ = false;
    uint32_t last_frame_timestamp_ms_ = 0;
};

extern Display display;
