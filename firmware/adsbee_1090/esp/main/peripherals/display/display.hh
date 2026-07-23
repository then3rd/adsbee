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

    // Candidate heights, in pixels, for the reusable off-screen strip used for banded rendering.
    // There is no PSRAM and internal SRAM is fragmented (WiFi/lwIP/TLS): a full-frame buffer does
    // not fit at any color depth (240x240 is ~115 KB at 16-bit / ~57 KB at 8-bit, but the largest
    // free block is only tens of KB). Instead the whole scene is redrawn into a small 240xH strip
    // and pushed once per band, so each screen region is written exactly once per frame (no
    // flicker) while keeping full 16-bit color.
    //
    // The block available at Init() time varies with how fragmented the heap is when the display
    // comes up, so rather than commit to one height (and collapse to the flickering direct-draw
    // path when it does not fit), Init() tries these in order and keeps the first that allocates
    // AND still leaves kMinFreeHeapAfterStripBytes for the network stack (see Init()).
    //
    // Heights are deliberately capped small: the web UI (HTTPS + WebSocket) allocates a large
    // transient working set when a client connects -- a peak that happens long after Init(), when
    // this strip is already resident. A big strip (e.g. 40 rows = ~19 KB) survives boot but starves
    // that later peak and OOMs the web server. A short strip just means more bands per frame
    // (redraw cost, negligible at ~5 Hz) while keeping flicker-free 16-bit color. 10 rows = ~4.8 KB
    // (24 bands), 8 rows = ~3.8 KB (30 bands). The reserve check below cannot see the *later* web-UI
    // peak (the heap is near-empty of connections at Init), so this hard cap -- not the reserve --
    // is what bounds the strip. Every entry MUST evenly divide RadarView::kScreenHeight (240).
    static constexpr int16_t kBandHeightCandidates[] = {10, 8};

    // Preferred minimum internal heap (bytes) to leave free after allocating the strip, so WiFi/
    // lwIP/TLS and the web UI keep headroom. This is only a *preference*: Init() picks the largest
    // candidate that still leaves this much free, but if none does it falls back to the SMALLEST
    // candidate that allocates rather than to flickery direct-draw -- a ~3.8 KB strip is cheap and
    // flicker-free rendering matters more. Measured boot-time free heap is ~42 KB (see the "Heap
    // before strip alloc" log), so this must sit below ~38 KB or the smallest strip is never
    // "preferred". 30 KB keeps ~7-8 KB of slack above the smallest strip. Direct-draw happens only
    // if even the smallest strip fails to allocate at all.
    static constexpr uint32_t kMinFreeHeapAfterStripBytes = 30u * 1024u;

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
    // Reusable 240xband_height_ off-screen strip for banded, flicker-free rendering. nullptr only
    // if every candidate height in kBandHeightCandidates failed to allocate, in which case we fall
    // back to drawing directly to lcd_ (flickers).
    LGFX_Sprite* band_ = nullptr;
    // Row count of the successfully-allocated strip (one of kBandHeightCandidates). Set by Init();
    // drives the band loop in RenderFrame(). 0 while band_ is nullptr.
    int16_t band_height_ = 0;
    RadarView radar_;

    // Snapshot of this frame's plottable targets, gathered once per frame in Update() before the
    // banded render loop. The tag layout (RadarView::LayoutTags) needs every aircraft position up
    // front, and each band re-draws the same set, so we collect once here rather than re-iterating
    // the dictionary per band. Sized above RadarView::kMaxTags so that when more aircraft are in
    // range than get managed labels, the extras still draw as symbols (with a fallback tag angle)
    // rather than vanishing.
    static constexpr int kMaxFrameTargets = 24;
    RadarTarget frame_targets_[kMaxFrameTargets];
    int frame_target_count_ = 0;

    bool initialized_ = false;
    uint32_t last_frame_timestamp_ms_ = 0;
};

extern Display display;
