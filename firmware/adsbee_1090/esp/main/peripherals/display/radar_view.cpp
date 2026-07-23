#include "radar_view.hh"

// Body compiled out unless the display is enabled; the TU still compiles (empty) so SRC_DIRS
// globbing does not break non-display builds.
#ifdef WITH_DISPLAY

#include <cmath>

#include "large_airports.hh"  // Embedded airport/runway dataset for the overlay.
#include "lgfx_config.hpp"     // Pulls in LovyanGFX (lgfx::LGFXBase and drawing primitives).

namespace {
namespace airports = data::large_airports;

// Pack 8-bit RGB into RGB565. LovyanGFX applies the panel's BGR order internally (the panel is
// configured cfg.rgb_order = false), so standard RGB565 values render with correct hue -- no
// R/B swap is needed here (unlike Plane-Radar, which swaps manually for its RGB-ordered panel).
constexpr uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---- Theme (RGB triples ported from ESP32-Plane-Radar include/ui/radar_theme.h) ----
constexpr uint16_t kColorBackground = Rgb565(4, 10, 28);     // Deep navy.
constexpr uint16_t kColorGrid = Rgb565(16, 100, 32);         // Dim green rings/crosshairs.
constexpr uint16_t kColorLabel = Rgb565(255, 255, 255);      // White cardinal labels.
constexpr uint16_t kColorCenter = Rgb565(255, 255, 255);     // White receiver dot.
constexpr uint16_t kColorTarget = Rgb565(255, 0, 0);         // Red aircraft.
constexpr uint16_t kColorTargetTag = Rgb565(255, 255, 255);  // White callsign tag.
constexpr uint16_t kColorTargetAlt = Rgb565(90, 200, 255);   // Cyan altitude tag.
constexpr uint16_t kColorVector = Rgb565(255, 0, 255);       // Magenta track vector.
constexpr uint16_t kColorRimDot = Rgb565(255, 0, 0);         // Red off-range bearing dots.
constexpr uint16_t kColorRunway = Rgb565(56, 150, 170);      // Teal runway lines.
constexpr uint16_t kColorRunwayLabel = Rgb565(110, 210, 230);  // Lighter teal airport idents.
constexpr uint16_t kColorAcquiring = 0x8410;                 // Grey "acquiring" text.

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kKmPerDegLat = 111.0f;

// Number of concentric range rings (matches Plane-Radar's 4-ring grid).
constexpr int kRingCount = 4;
// White receiver dot at the center.
constexpr int kCenterDotRadiusPx = 2;

// Length of the drawn heading triangle, in pixels.
constexpr float kTriangleLenPx = 9.0f;
constexpr float kTriangleHalfWidthPx = 4.0f;
// Speed vector: pixels drawn per knot, capped so fast aircraft don't draw off-screen.
constexpr float kVectorPxPerKt = 0.06f;
constexpr float kVectorMaxPx = 28.0f;

// ---- Runway overlay ----
constexpr float kRunwayLineHalfWidth = 1.0f;  // drawWideLine half-width (~2 px stroke).
constexpr int kRunwayLabelGapPx = 3;          // Gap from ring to ident label.
// Convert an e7 (degrees x 1e7) coordinate to degrees.
constexpr float E7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

// In-range airport table, indexed by airport index. File-static (there is only ever one
// RadarView) and recomputed by RefreshVisibleAirports() when the center/range changes.
bool s_airport_in_range[airports::kAirportCount];

int DistSqFromCenter(int x, int y) {
    const int dx = x - RadarView::kCenterX;
    const int dy = y - RadarView::kCenterY;
    return dx * dx + dy * dy;
}

// True if segment (x0,y0)->(x1,y1) intersects the outer-ring disc. Absolute scene coords.
bool SegmentIntersectsDisc(int x0, int y0, int x1, int y1) {
    const int r_sq = RadarView::kRadiusPx * RadarView::kRadiusPx;
    if (DistSqFromCenter(x0, y0) <= r_sq || DistSqFromCenter(x1, y1) <= r_sq) return true;

    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int fx = x0 - RadarView::kCenterX;
    const int fy = y0 - RadarView::kCenterY;
    const int a = dx * dx + dy * dy;
    if (a == 0) return false;
    const int b = 2 * (fx * dx + fy * dy);
    const int c = fx * fx + fy * fy - r_sq;
    float disc = static_cast<float>(b) * b - 4.0f * a * c;
    if (disc < 0.0f) return false;
    disc = sqrtf(disc);
    const float inv2a = 1.0f / (2.0f * a);
    const float t0 = (-b - disc) * inv2a;
    const float t1 = (-b + disc) * inv2a;
    return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

// Pull the free endpoint (x1,y1) back along the segment toward (x0,y0) until it lands inside
// the outer ring, so runway lines never spill past the bezel. Absolute scene coords.
void ClipEndpointToRing(int x0, int y0, int& x1, int& y1) {
    const int r_sq = RadarView::kRadiusPx * RadarView::kRadiusPx;
    if (DistSqFromCenter(x1, y1) <= r_sq) return;
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    for (float t = 1.0f; t > 0.0f; t -= 0.05f) {
        const int px = x0 + static_cast<int>(lroundf(dx * t));
        const int py = y0 + static_cast<int>(lroundf(dy * t));
        if (DistSqFromCenter(px, py) <= r_sq) {
            x1 = px;
            y1 = py;
            return;
        }
    }
    x1 = x0;
    y1 = y0;
}

// Project a point that may be outside the ring onto the ring edge (used to anchor an off-screen
// airport's ident label at the rim). Absolute scene coords.
void ClipPointOntoRing(int& x, int& y) {
    const int dx = x - RadarView::kCenterX;
    const int dy = y - RadarView::kCenterY;
    const int d_sq = dx * dx + dy * dy;
    const int r_sq = RadarView::kRadiusPx * RadarView::kRadiusPx;
    if (d_sq <= r_sq || d_sq == 0) return;
    const float scale = RadarView::kRadiusPx / sqrtf(static_cast<float>(d_sq));
    x = RadarView::kCenterX + static_cast<int>(lroundf(dx * scale));
    y = RadarView::kCenterY + static_cast<int>(lroundf(dy * scale));
}
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
    // origin_y_ shifts every y so a single horizontal strip of the scene can be drawn into a
    // small sprite (banded rendering); it is 0 for full-frame / direct draw. fillScreen clears
    // only the target (the strip), so the union of strips reproduces the whole cleared frame.
    const int16_t oy = origin_y_;
    gfx->fillScreen(kColorBackground);

    // Concentric range rings, evenly spaced out to the outer ring.
    for (int i = 1; i <= kRingCount; i++) {
        gfx->drawCircle(kCenterX, kCenterY - oy, (kRadiusPx * i) / kRingCount, kColorGrid);
    }

    // Crosshairs.
    gfx->drawFastVLine(kCenterX, kCenterY - kRadiusPx - oy, kRadiusPx * 2, kColorGrid);
    gfx->drawFastHLine(kCenterX - kRadiusPx, kCenterY - oy, kRadiusPx * 2, kColorGrid);

    // White receiver dot at the center.
    if (position_valid) {
        gfx->fillSmoothCircle(kCenterX, kCenterY - oy, kCenterDotRadiusPx, kColorCenter);
    }

    // Cardinal labels.
    gfx->setTextColor(kColorLabel);
    gfx->setTextDatum(lgfx::textdatum_t::middle_center);
    gfx->setTextSize(1);
    gfx->drawString("N", kCenterX, kCenterY - kRadiusPx + 8 - oy);
    gfx->drawString("S", kCenterX, kCenterY + kRadiusPx - 8 - oy);
    gfx->drawString("E", kCenterX + kRadiusPx - 8, kCenterY - oy);
    gfx->drawString("W", kCenterX - kRadiusPx + 8, kCenterY - oy);

    // Range scale label (outer ring range in km) near the bottom of the screen.
    char scale_buf[16];
    snprintf(scale_buf, sizeof(scale_buf), "%dkm", static_cast<int>(range_km_ + 0.5f));
    gfx->setTextColor(kColorGrid);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_center);
    gfx->drawString(scale_buf, kCenterX, kScreenHeight - 2 - oy);

    if (!position_valid) {
        gfx->setTextColor(kColorAcquiring);
        gfx->setTextDatum(lgfx::textdatum_t::middle_center);
        gfx->drawString("acquiring", kCenterX, kCenterY - 6 - oy);
        gfx->drawString("position", kCenterX, kCenterY + 6 - oy);
    }
}

void RadarView::DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target) {
    // Geometry below is computed in absolute scene coordinates; oy is applied only at the final
    // draw calls so banded rendering (see SetOriginY) lands each strip correctly.
    const int16_t oy = origin_y_;
    float sx, sy, range_km;
    LatLonToScreen(target.latitude_deg, target.longitude_deg, sx, sy, range_km);

    if (range_km > range_km_) {
        // Off-range: draw a bearing dot on the rim in the aircraft's direction.
        float bearing_rad = atan2f(sx - kCenterX, kCenterY - sy);  // 0 = north, clockwise.
        int16_t dot_x = kCenterX + static_cast<int16_t>(sinf(bearing_rad) * kRadiusPx);
        int16_t dot_y = kCenterY - static_cast<int16_t>(cosf(bearing_rad) * kRadiusPx);
        gfx->fillSmoothCircle(dot_x, dot_y - oy, 2, kColorRimDot);
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

        gfx->fillTriangle(static_cast<int16_t>(tipx), static_cast<int16_t>(tipy - oy),
                          static_cast<int16_t>(baseLx), static_cast<int16_t>(baseLy - oy),
                          static_cast<int16_t>(baseRx), static_cast<int16_t>(baseRy - oy), kColorTarget);

        // Speed vector.
        if (target.speed_kts > 0) {
            float len = target.speed_kts * kVectorPxPerKt;
            if (len > kVectorMaxPx) len = kVectorMaxPx;
            gfx->drawWideLine(sx, sy - oy, sx + fx * len, sy + fy * len - oy, 1.5f, kColorVector);
        }
    } else {
        // Unknown heading: plain dot.
        gfx->fillSmoothCircle(ix, iy - oy, 3, kColorTarget);
    }

    // Callsign / altitude tag, offset up-right from the target. Prefer the callsign; else fall
    // back to the flight level, but only when the baro altitude is actually valid (otherwise a
    // 0 ft placeholder would render a false "000"); else show "?".
    char tag[24];
    uint16_t tag_color;
    const char* cs = (target.callsign[0] != '\0' && target.callsign[0] != '?') ? target.callsign : "";
    if (cs[0] != '\0') {
        snprintf(tag, sizeof(tag), "%s", cs);
        tag_color = kColorTargetTag;  // White callsign.
    } else if (target.baro_altitude_valid) {
        snprintf(tag, sizeof(tag), "%03d", static_cast<int>(target.baro_altitude_ft / 100));
        tag_color = kColorTargetAlt;  // Cyan flight level.
    } else {
        snprintf(tag, sizeof(tag), "?");
        tag_color = kColorTargetTag;
    }
    gfx->setTextColor(tag_color);
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_left);
    gfx->drawString(tag, ix + 5, iy - 3 - oy);
}

void RadarView::RefreshVisibleAirports() {
    // Mark every airport whose center lies within the current display range. Runs once per
    // center/range change (gated by airports_dirty_), not once per band.
    for (size_t i = 0; i < airports::kAirportCount; i++) {
        const airports::Airport& ap = airports::kAirports[i];
        float sx, sy, range_km;
        LatLonToScreen(E7ToDeg(ap.lat_e7), E7ToDeg(ap.lon_e7), sx, sy, range_km);
        s_airport_in_range[i] = (range_km <= range_km_);
    }
    airports_dirty_ = false;
}

void RadarView::DrawAirports(lgfx::LGFXBase* gfx) {
    if (!show_runways_) return;
    if (airports_dirty_) RefreshVisibleAirports();

    const int16_t oy = origin_y_;

    // Runway lines. Runways are grouped by airport; skip any whose airport is out of range, then
    // clip the segment to the outer ring so nothing spills past the bezel.
    for (size_t i = 0; i < airports::kRunwayCount; i++) {
        const airports::Runway& rw = airports::kRunways[i];
        if (!s_airport_in_range[rw.airport_idx]) continue;

        float lx, ly, hx, hy, r;
        LatLonToScreen(E7ToDeg(rw.le_lat_e7), E7ToDeg(rw.le_lon_e7), lx, ly, r);
        LatLonToScreen(E7ToDeg(rw.he_lat_e7), E7ToDeg(rw.he_lon_e7), hx, hy, r);

        int x0 = static_cast<int>(lroundf(lx)), y0 = static_cast<int>(lroundf(ly));
        int x1 = static_cast<int>(lroundf(hx)), y1 = static_cast<int>(lroundf(hy));
        if (!SegmentIntersectsDisc(x0, y0, x1, y1)) continue;
        ClipEndpointToRing(x0, y0, x1, y1);
        ClipEndpointToRing(x1, y1, x0, y0);

        gfx->drawWideLine(x0, y0 - oy, x1, y1 - oy, kRunwayLineHalfWidth, kColorRunway);
    }

    // Airport idents, one per in-range airport, anchored on (or clipped onto) the outer ring and
    // nudged outward by a small gap so the text clears the runway lines.
    gfx->setTextColor(kColorRunwayLabel);
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_center);
    for (size_t i = 0; i < airports::kAirportCount; i++) {
        if (!s_airport_in_range[i]) continue;
        const airports::Airport& ap = airports::kAirports[i];

        float sx, sy, range_km;
        LatLonToScreen(E7ToDeg(ap.lat_e7), E7ToDeg(ap.lon_e7), sx, sy, range_km);
        int ax = static_cast<int>(lroundf(sx)), ay = static_cast<int>(lroundf(sy));
        ClipPointOntoRing(ax, ay);

        // Offset the label radially outward from center by the gap.
        const int dx = ax - kCenterX;
        const int dy = ay - kCenterY;
        const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
        int lxp = ax, lyp = ay - kRunwayLabelGapPx;
        if (len >= 1.0f) {
            lxp = ax + static_cast<int>(lroundf(dx / len * kRunwayLabelGapPx));
            lyp = ay + static_cast<int>(lroundf(dy / len * kRunwayLabelGapPx));
        }
        gfx->drawString(ap.ident, lxp, lyp - oy);
    }
}

#endif  // WITH_DISPLAY
