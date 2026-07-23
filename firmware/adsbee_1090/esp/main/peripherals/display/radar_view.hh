// Sonar-style radar rendering for the GC9A01 round display.
//
// Attribution: the projection and drawing approach is ported from ESP32-Plane-Radar
// (https://github.com/MatixYo/ESP32-Plane-Radar, MIT License, (c) 2026 MatixYo), files
// src/ui/radar_display.cpp, include/ui/radar_theme.h and src/ui/radar_range.cpp. The
// equirectangular latLonToScreen projection is reused with a bug fix: Plane-Radar omits the
// cos(latitude) correction on the longitude term (fine within ~25 km, wrong beyond); we apply
// it here. MIT->GPL-3.0 reuse is permitted with attribution.
#pragma once

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
    int32_t baro_altitude_ft = 0;
    float direction_deg = 0.0f;
    int32_t speed_kts = 0;
    bool direction_valid = false;
    bool baro_altitude_valid = false;
    char callsign[16] = {0};
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
        center_lat_deg_ = latitude_deg;
        center_lon_deg_ = longitude_deg;
    }

    /**
     * Set the range represented by the outermost ring, in kilometers.
     */
    void SetRangeKm(float range_km) { range_km_ = range_km; }

    /**
     * Draw the static radar background (rings, crosshairs, cardinal labels) onto the canvas.
     * Call once per frame before drawing targets. Clears the canvas.
     * @param[in] gfx Canvas to draw onto (an LGFX device or an off-screen sprite).
     * @param[in] position_valid True if the receiver center position is known. If false, an
     *            "acquiring position" state is drawn and no targets should be plotted.
     */
    void DrawBackground(lgfx::LGFXBase* gfx, bool position_valid);

    /**
     * Project and draw one aircraft. If the aircraft is within range it is drawn as a heading
     * triangle (with a speed vector and callsign/altitude tag); if beyond range it is drawn as
     * a bearing dot on the rim.
     * @param[in] gfx Canvas to draw onto.
     * @param[in] target Aircraft snapshot with a valid position.
     */
    void DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target);

   private:
    // Project lat/lon to screen coordinates (north-up equirectangular). Returns the distance
    // from center in km via out_range_km. cos(center_lat) longitude correction applied.
    void LatLonToScreen(float latitude_deg, float longitude_deg, float& out_x, float& out_y,
                        float& out_range_km) const;

    float center_lat_deg_ = 0.0f;
    float center_lon_deg_ = 0.0f;
    float range_km_ = kDefaultRangeKm;
};
