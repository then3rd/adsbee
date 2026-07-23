#include "radar_view.hh"

// Body compiled out unless the display is enabled; the TU still compiles (empty) so SRC_DIRS
// globbing does not break non-display builds.
#ifdef WITH_DISPLAY

#include <cmath>

// Embedded airport/runway datasets rendered by the overlay. To add a category: generate it with
//   python3 firmware/scripts/build_airports.py --category <type>
// then include its header here and add &data::<slug>::kDataset to kEnabledDatasets below. The
// default is large airports only, which preserves the original flash footprint; each added
// dataset grows .rodata by its size (medium/small airports are substantially larger).
#include "large_airports.hh"
#include "medium_airports.hh"
// #include "small_airports.hh"
#include "lgfx_config.hpp"  // Pulls in LovyanGFX (lgfx::LGFXBase and drawing primitives).

namespace {

// The set of datasets the overlay draws. This is the single knob for which categories render.
constexpr const data::AirportDataset* kEnabledDatasets[] = {
    &data::large_airports::kDataset,
    &data::medium_airports::kDataset,
    // &data::small_airports::kDataset,
};
constexpr size_t kEnabledDatasetCount = sizeof(kEnabledDatasets) / sizeof(kEnabledDatasets[0]);

// Total airports across all enabled datasets; sizes the visibility cache below.
constexpr size_t ComputeTotalAirportCount() {
    size_t total = 0;
    for (size_t d = 0; d < kEnabledDatasetCount; d++) total += kEnabledDatasets[d]->airport_count;
    return total;
}
constexpr size_t kTotalAirportCount = ComputeTotalAirportCount();

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
constexpr uint16_t kColorTargetCat = Rgb565(90, 200, 255);   // Cyan emitter-category tag.
constexpr uint16_t kColorTargetAlt = Rgb565(255, 255, 0);    // Yellow altitude tag.
constexpr uint16_t kColorVector = Rgb565(255, 0, 255);       // Magenta track vector.
constexpr uint16_t kColorRimDot = Rgb565(255, 0, 0);         // Red off-range bearing dots.
constexpr uint16_t kColorRunway = Rgb565(56, 150, 170);      // Teal runway lines.
constexpr uint16_t kColorRunwayLabel = Rgb565(110, 210, 230);  // Lighter teal airport idents.
constexpr uint16_t kColorAcquiring = 0x8410;                 // Grey "acquiring" text.
constexpr uint16_t kColorRange = Rgb565(60, 140, 255);       // Blue range/distance text.
constexpr uint16_t kColorTrail = Rgb565(70, 70, 80);        // Dim, faint position trail.

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

// In-range airport table, indexed by a global airport index (dataset base offset + per-dataset
// index; see RefreshVisibleAirports). File-static (there is only ever one RadarView) and
// recomputed by RefreshVisibleAirports() when the center/range changes.
bool s_airport_in_range[kTotalAirportCount];

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
    float dx_km = (longitude_deg - center_lon_deg_) * kKmPerDegLat * cos_center_lat_;

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

    // Cardinal labels, enlarged and pushed all the way out onto the outer ring. Each is centered
    // on the ring line, with a background-filled pad breaking the ring behind it (like the range
    // label). Drawn after the rings so the pad erases the grid underneath.
    gfx->setTextColor(kColorLabel);
    gfx->setTextDatum(lgfx::textdatum_t::middle_center);
    gfx->setTextSize(1.5f);
    struct Cardinal {
        const char* s;
        int16_t x, y;
    } const cardinals[] = {
        {"N", kCenterX, static_cast<int16_t>(kCenterY - kRadiusPx)},
        {"S", kCenterX, static_cast<int16_t>(kCenterY + kRadiusPx)},
        {"E", static_cast<int16_t>(kCenterX + kRadiusPx), kCenterY},
        {"W", static_cast<int16_t>(kCenterX - kRadiusPx), kCenterY},
    };
    for (const Cardinal& c : cardinals) {
        const int16_t cw = gfx->textWidth(c.s);
        const int16_t ch = gfx->fontHeight();
        gfx->fillRect(c.x - cw / 2 - 2, c.y - ch / 2 - 1 - oy, cw + 4, ch + 2, kColorBackground);
        gfx->drawString(c.s, c.x, c.y - oy);
    }
    gfx->setTextSize(1);

    // Range scale labels (km at each ring), placed along the 45-degree (NE) diagonal, centered
    // over each ring line. The outermost ring shows the full range_km_; inner rings show their
    // proportional fraction. Drawn after the rings/crosshairs so a background-filled pad can break
    // the green grid where each label sits (avoids overlap). The 45-degree radial keeps them off
    // the crosshairs and clear of the cardinal labels.
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::middle_center);
    gfx->setTextColor(kColorRange);
    // Unit vector along the NE diagonal (+x right, -y up): cos/sin of 45 degrees.
    constexpr float kDiag = 0.70710678f;
    for (int i = 1; i <= kRingCount; i++) {
        const int16_t ring_radius = (kRadiusPx * i) / kRingCount;
        char scale_buf[16];
        snprintf(scale_buf, sizeof(scale_buf), "%dkm",
                 static_cast<int>(range_km_ * i / kRingCount + 0.5f));
        const int16_t lx = kCenterX + static_cast<int16_t>(ring_radius * kDiag);
        const int16_t ly = kCenterY - static_cast<int16_t>(ring_radius * kDiag);
        const int16_t tw = gfx->textWidth(scale_buf);
        const int16_t th = gfx->fontHeight();
        gfx->fillRect(lx - tw / 2 - 2, ly - th / 2 - 1 - oy, tw + 4, th + 2, kColorBackground);
        gfx->drawString(scale_buf, lx, ly - oy);
    }

    if (!position_valid) {
        gfx->setTextColor(kColorAcquiring);
        gfx->setTextDatum(lgfx::textdatum_t::middle_center);
        gfx->drawString("acquiring", kCenterX, kCenterY - 6 - oy);
        gfx->drawString("position", kCenterX, kCenterY + 6 - oy);
    }
}

void RadarView::BeginFrame(uint32_t now_ms) {
    now_ms_ = now_ms;
    // Age out trails for aircraft not seen recently so their slots can be reused.
    for (Trail& t : trails_) {
        if (t.icao != 0 && now_ms_ - t.last_seen_ms > kTrailExpiryMs) {
            t.icao = 0;
            t.count = 0;
            t.head = 0;
        }
    }
}

const RadarView::Trail* RadarView::FindTrail(uint32_t icao) const {
    if (icao == 0) return nullptr;
    for (const Trail& t : trails_) {
        if (t.icao == icao) return &t;
    }
    return nullptr;
}

void RadarView::RecordTrail(uint32_t icao, float lat, float lon) {
    if (icao == 0) return;

    // Locate the aircraft's slot, or claim an empty one, or evict the least-recently-seen.
    Trail* slot = nullptr;
    Trail* empty = nullptr;
    Trail* oldest = &trails_[0];
    for (Trail& t : trails_) {
        if (t.icao == icao) {
            slot = &t;
            break;
        }
        if (empty == nullptr && t.icao == 0) empty = &t;
        if (t.last_seen_ms < oldest->last_seen_ms) oldest = &t;
    }
    if (slot == nullptr) {
        slot = (empty != nullptr) ? empty : oldest;
        slot->icao = icao;
        slot->count = 0;
        slot->head = 0;
        slot->last_sample_ms = 0;
    }

    slot->last_seen_ms = now_ms_;

    // Reception gap: if we missed several samples, the buffered points are stale and connecting
    // them to the new position would draw a false straight line across the gap. Drop the old trail
    // and restart from the current point (the fixed ring depth otherwise has no per-point age).
    if (slot->count != 0 && now_ms_ - slot->last_sample_ms > kTrailGapResetMs) {
        slot->count = 0;
        slot->head = 0;
    }

    // Throttle how often points are appended so the trail spans a useful time window.
    if (slot->count != 0 && now_ms_ - slot->last_sample_ms < kTrailSampleMs) {
        return;
    }
    slot->lat[slot->head] = lat;
    slot->lon[slot->head] = lon;
    slot->head = (slot->head + 1) % kTrailLen;
    if (slot->count < kTrailLen) slot->count++;
    slot->last_sample_ms = now_ms_;
}

void RadarView::DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target) {
    // Geometry below is computed in absolute scene coordinates; oy is applied only at the final
    // draw calls so banded rendering (see SetOriginY) lands each strip correctly.
    const int16_t oy = origin_y_;
    float sx, sy, range_km;
    LatLonToScreen(target.latitude_deg, target.longitude_deg, sx, sy, range_km);

    // Record this aircraft's position into its trail once per frame. DrawTarget is called for
    // every aircraft in every band; gate on the first band (origin_y_ == 0, always drawn first)
    // so exactly one sample is taken per frame. Recorded before the off-range return so trails
    // persist as aircraft cross the range edge.
    if (origin_y_ == 0) {
        RecordTrail(target.icao_address, target.latitude_deg, target.longitude_deg);
    }

    // Draw the position trail as the bottom layer: thin faint polyline through the retained
    // breadcrumbs (the ring depth bounds them to the retention window), clipped to the outer
    // ring. Re-projected each frame so it tracks range/center changes.
    if (const Trail* trail = FindTrail(target.icao_address)) {
        constexpr int32_t kRadiusSq = static_cast<int32_t>(kRadiusPx) * kRadiusPx;
        bool have_prev = false;
        int16_t prev_x = 0, prev_y = 0;
        for (int i = 0; i < trail->count; i++) {
            int idx = (trail->head - trail->count + i + kTrailLen) % kTrailLen;
            float px, py, unused_range;
            LatLonToScreen(trail->lat[idx], trail->lon[idx], px, py, unused_range);
            int16_t cx = static_cast<int16_t>(px + 0.5f);
            int16_t cy = static_cast<int16_t>(py + 0.5f);
            int32_t dx = cx - kCenterX, dy = cy - kCenterY;
            bool inside = (dx * dx + dy * dy) <= kRadiusSq;
            if (have_prev && inside) {
                gfx->drawLine(prev_x, prev_y - oy, cx, cy - oy, kColorTrail);
            }
            prev_x = cx;
            prev_y = cy;
            have_prev = inside;
        }
    }

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

        // Speed vector. A crisp 1px line (not a wide line) so it stays centered on the heading
        // axis the triangle is built on, rather than rounding off to one side.
        if (target.speed_kts > 0) {
            float len = target.speed_kts * kVectorPxPerKt;
            if (len > kVectorMaxPx) len = kVectorMaxPx;
            gfx->drawLine(static_cast<int16_t>(sx), static_cast<int16_t>(sy - oy),
                          static_cast<int16_t>(sx + fx * len), static_cast<int16_t>(sy + fy * len - oy),
                          kColorVector);
        }
    } else {
        // Unknown heading: plain dot.
        gfx->fillSmoothCircle(ix, iy - oy, 3, kColorTarget);
    }

    // Stacked tag, offset up-right from the target: callsign (white), then emitter category
    // (cyan) and altitude in feet (yellow) below it. Lines that have no data are skipped so
    // they leave no gap.
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::top_left);
    const int16_t tag_x = ix + 5;
    int16_t tag_y = iy - 3 - oy;
    const int16_t line_h = gfx->fontHeight();
    char tag[24];

    // Line 1: callsign, or "?" when none is known.
    const char* cs = (target.callsign[0] != '\0' && target.callsign[0] != '?') ? target.callsign : "";
    gfx->setTextColor(kColorTargetTag);  // White callsign.
    gfx->drawString(cs[0] != '\0' ? cs : "?", tag_x, tag_y);
    tag_y += line_h;

    // Line 2: emitter category abbreviation (only when known).
    if (target.category[0] != '\0') {
        gfx->setTextColor(kColorTargetCat);  // Cyan category.
        gfx->drawString(target.category, tag_x, tag_y);
        tag_y += line_h;
    }

    // Line 3: geometric (GNSS) altitude in feet (only when valid, to avoid a false "0ft").
    if (target.geom_altitude_valid) {
        snprintf(tag, sizeof(tag), "%dft", static_cast<int>(target.geom_altitude_ft));
        gfx->setTextColor(kColorTargetAlt);  // Yellow altitude.
        gfx->drawString(tag, tag_x, tag_y);
    }
}

void RadarView::RefreshVisibleAirports() {
    // Mark every airport (across all enabled datasets) whose center lies within the current
    // display range. Runs once per center/range change (gated by airports_dirty_), not once per
    // band. The global index is the dataset's base offset plus its per-dataset airport index.
    size_t base = 0;
    for (size_t d = 0; d < kEnabledDatasetCount; d++) {
        const data::AirportDataset& ds = *kEnabledDatasets[d];
        for (size_t i = 0; i < ds.airport_count; i++) {
            const data::Airport& ap = ds.airports[i];
            float sx, sy, range_km;
            LatLonToScreen(E7ToDeg(ap.lat_e7), E7ToDeg(ap.lon_e7), sx, sy, range_km);
            s_airport_in_range[base + i] = (range_km <= range_km_);
        }
        base += ds.airport_count;
    }
    airports_dirty_ = false;
}

void RadarView::DrawAirports(lgfx::LGFXBase* gfx) {
    if (!show_runways_) return;
    if (airports_dirty_) RefreshVisibleAirports();

    const int16_t oy = origin_y_;

    // Runway lines, per dataset. Runways are grouped by airport; skip any whose airport is out of
    // range, then clip the segment to the outer ring so nothing spills past the bezel. rw.airport_idx
    // is relative to its own dataset, so add the dataset's base offset for the visibility lookup.
    size_t base = 0;
    for (size_t d = 0; d < kEnabledDatasetCount; d++) {
        const data::AirportDataset& ds = *kEnabledDatasets[d];
        for (size_t i = 0; i < ds.runway_count; i++) {
            const data::Runway& rw = ds.runways[i];
            if (!s_airport_in_range[base + rw.airport_idx]) continue;

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
        base += ds.airport_count;
    }

    // Airport idents, one per in-range airport, anchored on (or clipped onto) the outer ring and
    // nudged outward by a small gap so the text clears the runway lines.
    gfx->setTextColor(kColorRunwayLabel);
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::bottom_center);
    base = 0;
    for (size_t d = 0; d < kEnabledDatasetCount; d++) {
        const data::AirportDataset& ds = *kEnabledDatasets[d];
        for (size_t i = 0; i < ds.airport_count; i++) {
            if (!s_airport_in_range[base + i]) continue;
            const data::Airport& ap = ds.airports[i];

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
        base += ds.airport_count;
    }
}

#endif  // WITH_DISPLAY
