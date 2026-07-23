#include "radar_view.hh"

// Body compiled out unless the display is enabled; the TU still compiles (empty) so SRC_DIRS
// globbing does not break non-display builds.
#ifdef WITH_DISPLAY

#include <cmath>

#include "lgfx_config.hpp"  // Pulls in LovyanGFX (lgfx::LGFXBase and drawing primitives).

namespace {
// ---- Theme (ported from ESP32-Plane-Radar include/ui/radar_theme.h) ----
constexpr uint16_t kColorBackground = 0x0000;  // Black.
constexpr uint16_t kColorGrid = 0x0320;        // Dim green rings/crosshairs.
constexpr uint16_t kColorGridBright = 0x05E0;  // Brighter green for the outer ring.
constexpr uint16_t kColorLabel = 0x07E0;       // Green cardinal labels.
constexpr uint16_t kColorTarget = 0xFFE0;      // Yellow aircraft.
constexpr uint16_t kColorTargetTag = 0xC618;   // Light grey tags.
constexpr uint16_t kColorVector = 0xFD20;      // Orange speed vector.
constexpr uint16_t kColorRimDot = 0xF800;      // Red off-range bearing dots.
constexpr uint16_t kColorAcquiring = 0x8410;   // Grey "acquiring" text.

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kKmPerDegLat = 111.0f;

// Length of the drawn heading triangle, in pixels.
constexpr float kTriangleLenPx = 9.0f;
constexpr float kTriangleHalfWidthPx = 4.0f;
// Speed vector: pixels drawn per knot, capped so fast aircraft don't draw off-screen.
constexpr float kVectorPxPerKt = 0.06f;
constexpr float kVectorMaxPx = 28.0f;
}  // namespace

void RadarView::LatLonToScreen(float latitude_deg, float longitude_deg, float& out_x, float& out_y,
                               float& out_range_km) const {
    // Equirectangular (flat-earth) projection, north-up. The cos(center_lat) term corrects the
    // longitude scale so east-west distances are not overstated away from the equator -- this
    // is the fix for the known Plane-Radar bug (it omits the cosine and is only correct near
    // the receiver's meridian).
    float dy_km = (latitude_deg - center_lat_deg_) * kKmPerDegLat;
    float dx_km = (longitude_deg - center_lon_deg_) * kKmPerDegLat * cosf(center_lat_deg_ * kDegToRad);

    out_range_km = sqrtf(dx_km * dx_km + dy_km * dy_km);

    float px_per_km = static_cast<float>(kRadiusPx) / range_km_;
    out_x = static_cast<float>(kCenterX) + dx_km * px_per_km;
    out_y = static_cast<float>(kCenterY) - dy_km * px_per_km;  // Screen y grows downward; north is up.
}

void RadarView::DrawBackground(lgfx::LGFXBase* gfx, bool position_valid) {
    gfx->fillScreen(kColorBackground);

    // Range rings at 1/3, 2/3 and full range.
    gfx->drawCircle(kCenterX, kCenterY, kRadiusPx / 3, kColorGrid);
    gfx->drawCircle(kCenterX, kCenterY, (kRadiusPx * 2) / 3, kColorGrid);
    gfx->drawCircle(kCenterX, kCenterY, kRadiusPx, kColorGridBright);

    // Crosshairs.
    gfx->drawFastVLine(kCenterX, kCenterY - kRadiusPx, kRadiusPx * 2, kColorGrid);
    gfx->drawFastHLine(kCenterX - kRadiusPx, kCenterY, kRadiusPx * 2, kColorGrid);

    // Cardinal labels.
    gfx->setTextColor(kColorLabel);
    gfx->setTextDatum(lgfx::textdatum_t::middle_center);
    gfx->setTextSize(1);
    gfx->drawString("N", kCenterX, kCenterY - kRadiusPx + 8);
    gfx->drawString("S", kCenterX, kCenterY + kRadiusPx - 8);
    gfx->drawString("E", kCenterX + kRadiusPx - 8, kCenterY);
    gfx->drawString("W", kCenterX - kRadiusPx + 8, kCenterY);

    // Range scale label (outer ring range in km) near the bottom of the screen.
    char scale_buf[16];
    snprintf(scale_buf, sizeof(scale_buf), "%dkm", static_cast<int>(range_km_ + 0.5f));
    gfx->setTextColor(kColorGridBright);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_center);
    gfx->drawString(scale_buf, kCenterX, kScreenHeight - 2);

    if (!position_valid) {
        gfx->setTextColor(kColorAcquiring);
        gfx->setTextDatum(lgfx::textdatum_t::middle_center);
        gfx->drawString("acquiring", kCenterX, kCenterY - 6);
        gfx->drawString("position", kCenterX, kCenterY + 6);
    }
}

void RadarView::DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target) {
    float sx, sy, range_km;
    LatLonToScreen(target.latitude_deg, target.longitude_deg, sx, sy, range_km);

    if (range_km > range_km_) {
        // Off-range: draw a bearing dot on the rim in the aircraft's direction.
        float bearing_rad = atan2f(sx - kCenterX, kCenterY - sy);  // 0 = north, clockwise.
        int16_t dot_x = kCenterX + static_cast<int16_t>(sinf(bearing_rad) * kRadiusPx);
        int16_t dot_y = kCenterY - static_cast<int16_t>(cosf(bearing_rad) * kRadiusPx);
        gfx->fillSmoothCircle(dot_x, dot_y, 2, kColorRimDot);
        return;
    }

    int16_t ix = static_cast<int16_t>(sx + 0.5f);
    int16_t iy = static_cast<int16_t>(sy + 0.5f);

    if (target.direction_valid) {
        // Heading triangle pointing along direction_deg (0 = north, clockwise).
        float hr = target.direction_deg * kDegToRad;
        float fx = sinf(hr);   // Forward unit vector (screen space, north-up).
        float fy = -cosf(hr);
        // Perpendicular (right) unit vector.
        float rx = -fy;
        float ry = fx;

        float tipx = sx + fx * kTriangleLenPx;
        float tipy = sy + fy * kTriangleLenPx;
        float baseLx = sx - fx * (kTriangleLenPx * 0.5f) + rx * kTriangleHalfWidthPx;
        float baseLy = sy - fy * (kTriangleLenPx * 0.5f) + ry * kTriangleHalfWidthPx;
        float baseRx = sx - fx * (kTriangleLenPx * 0.5f) - rx * kTriangleHalfWidthPx;
        float baseRy = sy - fy * (kTriangleLenPx * 0.5f) - ry * kTriangleHalfWidthPx;

        gfx->fillTriangle(static_cast<int16_t>(tipx), static_cast<int16_t>(tipy),
                          static_cast<int16_t>(baseLx), static_cast<int16_t>(baseLy),
                          static_cast<int16_t>(baseRx), static_cast<int16_t>(baseRy), kColorTarget);

        // Speed vector.
        if (target.speed_kts > 0) {
            float len = target.speed_kts * kVectorPxPerKt;
            if (len > kVectorMaxPx) len = kVectorMaxPx;
            gfx->drawWideLine(sx, sy, sx + fx * len, sy + fy * len, 1.5f, kColorVector);
        }
    } else {
        // Unknown heading: plain dot.
        gfx->fillSmoothCircle(ix, iy, 3, kColorTarget);
    }

    // Callsign / altitude tag, offset up-right from the target. Prefer the callsign; else fall
    // back to the flight level, but only when the baro altitude is actually valid (otherwise a
    // 0 ft placeholder would render a false "000"); else show "?".
    char tag[24];
    const char* cs = (target.callsign[0] != '\0' && target.callsign[0] != '?') ? target.callsign : "";
    if (cs[0] != '\0') {
        snprintf(tag, sizeof(tag), "%s", cs);
    } else if (target.baro_altitude_valid) {
        snprintf(tag, sizeof(tag), "%03d", static_cast<int>(target.baro_altitude_ft / 100));
    } else {
        snprintf(tag, sizeof(tag), "?");
    }
    gfx->setTextColor(kColorTargetTag);
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_left);
    gfx->drawString(tag, ix + 5, iy - 3);
}

#endif  // WITH_DISPLAY
