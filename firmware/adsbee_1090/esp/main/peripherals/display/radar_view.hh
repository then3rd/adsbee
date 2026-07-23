// Sonar-style radar rendering for the GC9A01 round display.
//
// Attribution: the projection, palette and drawing approach are ported from ESP32-Plane-Radar
// (https://github.com/MatixYo/ESP32-Plane-Radar, MIT License, (c) 2026 MatixYo), files
// src/ui/radar_display.cpp, include/ui/radar_theme.h, src/ui/radar_range.cpp and
// src/ui/runway_overlay.cpp. The equirectangular latLonToScreen projection is reused with a
// bug fix: Plane-Radar omits the cos(latitude) correction on the longitude term (fine within
// ~25 km, wrong beyond); we apply it here. The runway/airport overlay and its embedded dataset
// (see large_airports.hh) are ported likewise. MIT->GPL-3.0 reuse is permitted with attribution.
#pragma once

#include <cmath>
#include <cstdint>

// Forward-declare LovyanGFX's base canvas type so this header stays light (no LovyanGFX
// include). v1 MUST be declared `inline` to match LovyanGFX — a non-inline forward declaration
// here would make LovyanGFX's own `inline namespace v1` ill-formed ("inline namespace must be
// specified at initial definition") in any TU that includes this header before LovyanGFX.hpp.
namespace lgfx {
inline namespace v1 {
class LGFXBase;
}  // namespace v1
}  // namespace lgfx

// Minimal per-aircraft snapshot handed to the renderer. Populated by Display from the
// AircraftDictionary; decoupling the renderer from the dictionary keeps this file portable and
// makes a future dedicated render task (fed from a snapshot queue) a drop-in change.
struct RadarTarget {
    float latitude_deg = 0.0f;
    float longitude_deg = 0.0f;
    int32_t geom_altitude_ft = 0;
    float direction_deg = 0.0f;
    int32_t speed_kts = 0;
    bool direction_valid = false;
    bool geom_altitude_valid = false;
    char callsign[16] = {0};
    char category[4] = {0};       // 3-letter emitter-category abbreviation, "" if unknown.
    uint32_t icao_address = 0;    // Identity used to key the position trail.
};

class RadarView {
   public:
    static constexpr int16_t kScreenWidth = 240;
    static constexpr int16_t kScreenHeight = 240;
    static constexpr int16_t kCenterX = kScreenWidth / 2;
    static constexpr int16_t kCenterY = kScreenHeight / 2;
    // Leave a small margin inside the round bezel for rim dots and cardinal labels.
    static constexpr int16_t kRadiusPx = 112;

    // Default display range (edge of the outermost ring), in kilometers.
    static constexpr float kDefaultRangeKm = 50.0f;

    /**
     * Set the receiver position that the radar is centered on.
     * @param[in] latitude_deg Center latitude in degrees.
     * @param[in] longitude_deg Center longitude in degrees.
     */
    void SetCenter(float latitude_deg, float longitude_deg) {
        // Only invalidate the in-range airport cache when the center actually moves, so the
        // per-frame SetCenter() call (with an unchanged fix) does not force a recompute.
        if (latitude_deg != center_lat_deg_ || longitude_deg != center_lon_deg_) {
            center_lat_deg_ = latitude_deg;
            center_lon_deg_ = longitude_deg;
            // Cache the longitude-scale cosine so LatLonToScreen (called per point, per band) does
            // not recompute the same cosf for every projection. cosf(deg * pi/180).
            cos_center_lat_ = cosf(latitude_deg * 0.017453292519943295f);
            airports_dirty_ = true;
        }
    }

    /**
     * Set the range represented by the outermost ring, in kilometers.
     */
    void SetRangeKm(float range_km) {
        if (range_km != range_km_) {
            range_km_ = range_km;
            airports_dirty_ = true;
        }
    }

    /**
     * Enable/disable the runway + airport-ident overlay. On by default.
     */
    void SetShowRunways(bool show) { show_runways_ = show; }

    /**
     * Set a vertical render offset (in pixels) subtracted from every drawn y coordinate.
     * Used for banded rendering: to draw the horizontal strip [y0, y0+H) of the full 240x240
     * scene into a small H-tall sprite, set origin_y = y0 so absolute scene coordinates land in
     * the sprite's local space (off-strip pixels are clipped by the sprite bounds). Leave at 0
     * to draw at absolute coordinates (full-frame / direct draw). Projection geometry is always
     * computed in absolute coordinates; the offset is applied only at the final draw call.
     */
    void SetOriginY(int16_t origin_y) { origin_y_ = origin_y; }

    /**
     * Draw the static radar background (rings, crosshairs, cardinal labels) onto the canvas.
     * Call once per frame before drawing targets. Clears the canvas.
     * @param[in] gfx Canvas to draw onto (an LGFX device or an off-screen sprite).
     * @param[in] position_valid True if the receiver center position is known. If false, an
     *            "acquiring position" state is drawn and no targets should be plotted.
     */
    void DrawBackground(lgfx::LGFXBase* gfx, bool position_valid);

    /**
     * Draw the runway lines and airport idents for every airport within range of the center,
     * across all enabled embedded datasets (see the kEnabledDatasets table in radar_view.cpp).
     * No-op if the overlay is disabled. Call after DrawBackground and before DrawTarget so
     * aircraft draw on top.
     * @param[in] gfx Canvas to draw onto.
     */
    void DrawAirports(lgfx::LGFXBase* gfx);

    /**
     * Project and draw one aircraft. If the aircraft is within range it is drawn as a heading
     * triangle (with a speed vector and callsign/altitude tag); if beyond range it is drawn as
     * a bearing dot on the rim.
     * @param[in] gfx Canvas to draw onto.
     * @param[in] target Aircraft snapshot with a valid position.
     */
    void DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target);

    /**
     * Compute the angular placement of every target's callsign/altitude tag for this frame. Must be
     * called once per frame after BeginFrame() and before the banded DrawTarget() loop, because the
     * labels must be positioned (using all aircraft positions at once) before any band draws them.
     * Each label orbits its symbol at a continuous angle relaxed by a force-directed pass so tags
     * repel each other and fan out; the angle is carried across frames so labels swivel smoothly.
     * @param[in] targets Array of the frame's plottable targets (in- and off-range both allowed;
     *            off-range targets are skipped since they render as rim dots without a tag).
     * @param[in] count Number of entries in targets.
     */
    void LayoutTags(const RadarTarget* targets, int count);

    /**
     * Mark the start of a new frame. Stores the current timestamp used for trail sampling and
     * ages out trails for aircraft not seen within kTrailExpiryMs. Must be called exactly once
     * per frame (not once per band) before the banded render loop.
     * @param[in] now_ms Milliseconds since boot.
     */
    void BeginFrame(uint32_t now_ms);

   private:
    // Project lat/lon to screen coordinates (north-up equirectangular). Returns the distance
    // from center in km via out_range_km. cos(center_lat) longitude correction applied.
    void LatLonToScreen(float latitude_deg, float longitude_deg, float& out_x, float& out_y,
                        float& out_range_km) const;

    // Recompute which embedded airports fall within range_km_ of the center into the file-static
    // in-range table (see radar_view.cpp). Cheap-gated by airports_dirty_ so it runs once per
    // center/range change, not once per band.
    void RefreshVisibleAirports();

    // ---- Aircraft position trails ----
    // The retention window is expressed in minutes (kTrailRetentionMin). Points are appended at
    // roughly kTrailSampleMs spacing, so the ring-buffer depth (kTrailLen) sized from those two
    // constants IS the time window: the oldest breadcrumb rolls out after ~kTrailRetentionMin.
    //
    // Memory note: this table lives in static SRAM (no PSRAM on this board) and competes directly
    // with the banded-render strip buffer AND the WiFi/lwIP/TLS heap the web UI needs when a
    // client connects. Every byte here is permanent pressure on that peak. Footprint is
    // kMaxTrails * kTrailLen * 8 bytes; keep it small (~5 KB). To lengthen the window, prefer
    // raising kTrailSampleMs (coarser breadcrumbs) over growing the point count or trail count.
    static constexpr int kMaxTrails = 16;              // Aircraft tracked simultaneously.
    static constexpr uint32_t kTrailRetentionMin = 5;  // Minutes of history to retain per trail.
    static constexpr uint32_t kTrailSampleMs = 8000;   // Min spacing between samples.
    // If an aircraft goes unseen longer than this (several missed samples), its buffered points are
    // stale: connecting them to the position after the gap would draw a false straight chord across
    // airspace it never crossed. Past this gap the trail is reset and restarts from the new point.
    static constexpr uint32_t kTrailGapResetMs = kTrailSampleMs * 3;
    static constexpr uint32_t kTrailRetentionMs = kTrailRetentionMin * 60u * 1000u;
    // Ring-buffer depth that covers the retention window at the sample rate; also bounds memory.
    static constexpr int kTrailLen = static_cast<int>(kTrailRetentionMs / kTrailSampleMs);
    static constexpr uint32_t kTrailExpiryMs = kTrailRetentionMs;  // Drop a trail unseen this long.

    // Per-aircraft breadcrumb ring buffer. Stores only lat/lon (not screen coords, so trails
    // re-project when range/center change; no per-point timestamp -- the fixed-depth ring bounds
    // history to the window, keeping the table small).
    struct Trail {
        uint32_t icao = 0;             // 0 = empty slot.
        uint32_t last_seen_ms = 0;     // Frame time this aircraft was last recorded (for expiry).
        uint32_t last_sample_ms = 0;   // Time the most recent point was appended (for sampling).
        uint8_t count = 0;             // Valid points in the ring buffer.
        uint8_t head = 0;              // Next-write index into lat[]/lon[].
        float lat[kTrailLen];
        float lon[kTrailLen];
    };

    // Append the aircraft's current position to its trail, throttled to kTrailSampleMs. Allocates
    // or evicts a slot as needed. Called once per aircraft per frame (first band only).
    void RecordTrail(uint32_t icao, float lat, float lon);
    // Look up an aircraft's trail slot, or nullptr if none.
    const Trail* FindTrail(uint32_t icao) const;

    // ---- Aircraft tag (callsign/category/altitude) label placement ----
    // Each tag orbits its heading triangle at a continuous angle rather than sitting at a fixed
    // offset. LayoutTags() relaxes every label's angle once per frame with a force-directed pass
    // (labels repel each other and the other symbols, and are pushed in off the rim) so clustered
    // aircraft fan out and stay legible -- circle-packing by repulsion. The chosen angle is kept
    // across frames and stepped in small increments, so labels swivel smoothly instead of snapping.
    // Placement is angle-only in absolute scene coords, so it is identical in every band; DrawTarget
    // just reads the angle back by ICAO. Footprint: kMaxTags * sizeof(TagLabel).
    static constexpr int kMaxTags = kMaxTrails;  // Simultaneous on-screen labels laid out.
    struct TagLabel {
        uint32_t icao = 0;          // 0 = empty slot.
        uint32_t last_seen_ms = 0;  // Frame time last laid out (for expiry, mirrors trails).
        float angle_rad = 0.0f;     // Orbit angle of the label around its symbol (screen space).
        bool valid = false;         // Angle has been initialized at least once.
    };
    TagLabel labels_[kMaxTags];

    // Find an aircraft's persistent label slot (nullptr if none); or acquire/evict one for it.
    const TagLabel* FindLabel(uint32_t icao) const;
    TagLabel* AcquireLabel(uint32_t icao);

    float center_lat_deg_ = 0.0f;
    float cos_center_lat_ = 1.0f;  // cos(center_lat); cached by SetCenter for LatLonToScreen.
    float center_lon_deg_ = 0.0f;
    float range_km_ = kDefaultRangeKm;
    int16_t origin_y_ = 0;  // Subtracted from every drawn y (banded rendering); see SetOriginY.
    bool show_runways_ = true;
    bool airports_dirty_ = true;  // In-range airport table needs recompute (center/range moved).
    uint32_t now_ms_ = 0;         // Current frame timestamp (set by BeginFrame); trail sampling.
    Trail trails_[kMaxTrails];
};
