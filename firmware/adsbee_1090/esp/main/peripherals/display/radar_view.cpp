#include "radar_view.hh"

// Body compiled out unless the display is enabled; the TU still compiles (empty) so SRC_DIRS
// globbing does not break non-display builds.
#ifdef WITH_DISPLAY

#include <cmath>
#include <cstring>

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

// ---- Color schemes ---------------------------------------------------------------------------
// Each RadarTheme (see radar_view.hh) is a complete RGB565 palette. The active one is chosen at
// runtime by RadarView::SetColorScheme(), fed from settings.display_color_scheme (synced from the
// RP2040). Field order matches the RadarTheme struct; index order matches the DisplayColorScheme
// enum in SettingsManager::Settings, so entry N here is the palette for scheme N there.
//   0 Default        -- the original ADSBee radar palette (navy background, primary-colored data).
//   1 High Contrast  -- pure black with full-brightness primaries; best in bright ambient light.
//   2 Night (Red)    -- monochrome red on black to preserve dark-adapted night vision.
//   3 Daylight       -- light background with dark elements for direct-sunlight legibility.
//   4 Amber Phosphor -- retro amber CRT radar-scope look.
//   5 Chromodynamics -- inspired by MagicStack's Chromodynamics editor theme
//                       (github.com/MagicStack/Chromodynamics): near-black background with crimson,
//                       cyan, lime, gold and purple syntax accents.
constexpr RadarTheme kRadarThemes[] = {
    // 0 -- Default: the original ADSBee radar palette.
    {
        Rgb565(4, 10, 28),      // background:   deep navy
        Rgb565(16, 100, 32),    // grid:         dim green rings / crosshairs
        Rgb565(255, 255, 255),  // label:        white cardinal (N/E/S/W) labels
        Rgb565(255, 255, 255),  // center:       white receiver dot
        Rgb565(255, 0, 0),      // target:       red aircraft
        Rgb565(255, 255, 255),  // target_tag:   white callsign tag
        Rgb565(90, 200, 255),   // target_cat:   cyan emitter-category tag
        Rgb565(255, 255, 0),    // target_alt:   yellow altitude tag
        Rgb565(0, 255, 0),      // vector:       green track vector
        Rgb565(255, 0, 0),      // rim_dot:      red off-range bearing dots
        Rgb565(255, 0, 255),    // runway:       magenta runway lines
        Rgb565(110, 210, 230),  // runway_label: lighter teal airport idents
        0x8410,                 // acquiring:    grey "acquiring" text
        Rgb565(60, 140, 255),   // range:        blue range / distance text
        Rgb565(70, 70, 80),     // trail:        dim, faint position trail
    },
    // 1 -- High Contrast: black background, full-brightness data for bright ambient light.
    {
        Rgb565(0, 0, 0),        // background:   black
        Rgb565(0, 160, 0),      // grid:         bright green rings / crosshairs
        Rgb565(255, 255, 255),  // label:        white cardinal labels
        Rgb565(255, 255, 255),  // center:       white receiver dot
        Rgb565(255, 0, 0),      // target:       pure red aircraft
        Rgb565(255, 255, 255),  // target_tag:   white callsign tag
        Rgb565(0, 255, 255),    // target_cat:   bright cyan category tag
        Rgb565(255, 255, 0),    // target_alt:   bright yellow altitude tag
        Rgb565(0, 255, 0),      // vector:       bright green track vector
        Rgb565(255, 0, 0),      // rim_dot:      red off-range bearing dots
        Rgb565(255, 0, 255),    // runway:       magenta runway lines
        Rgb565(0, 255, 255),    // runway_label: bright cyan airport idents
        Rgb565(200, 200, 200),  // acquiring:    light grey "acquiring" text
        Rgb565(0, 180, 255),    // range:        bright blue range / distance text
        Rgb565(120, 120, 130),  // trail:        light grey position trail
    },
    // 2 -- Night (Red): monochrome red on black to preserve night vision.
    {
        Rgb565(0, 0, 0),        // background:   black
        Rgb565(60, 0, 0),       // grid:         very dim red rings / crosshairs
        Rgb565(200, 40, 40),    // label:        red cardinal labels
        Rgb565(255, 80, 80),    // center:       bright red receiver dot
        Rgb565(255, 40, 40),    // target:       red aircraft
        Rgb565(200, 40, 40),    // target_tag:   red callsign tag
        Rgb565(150, 30, 30),    // target_cat:   dark red category tag
        Rgb565(255, 90, 90),    // target_alt:   light red altitude tag
        Rgb565(180, 0, 0),      // vector:       red track vector
        Rgb565(255, 40, 40),    // rim_dot:      red off-range bearing dots
        Rgb565(120, 0, 0),      // runway:       dark red runway lines
        Rgb565(160, 30, 30),    // runway_label: red airport idents
        Rgb565(120, 20, 20),    // acquiring:    dim red "acquiring" text
        Rgb565(200, 50, 50),    // range:        red range / distance text
        Rgb565(60, 10, 10),     // trail:        very dim red position trail
    },
    // 3 -- Daylight (Bright): light background, dark data for direct-sunlight legibility.
    {
        Rgb565(232, 236, 240),  // background:   light grey
        Rgb565(90, 120, 170),   // grid:         blue-grey rings / crosshairs
        Rgb565(16, 24, 32),     // label:        near-black cardinal labels
        Rgb565(16, 24, 32),     // center:       near-black receiver dot
        Rgb565(192, 0, 0),      // target:       dark red aircraft
        Rgb565(16, 24, 32),     // target_tag:   near-black callsign tag
        Rgb565(0, 90, 140),     // target_cat:   dark teal category tag
        Rgb565(128, 90, 0),     // target_alt:   dark amber altitude tag
        Rgb565(0, 100, 32),     // vector:       dark green track vector
        Rgb565(192, 0, 0),      // rim_dot:      dark red off-range bearing dots
        Rgb565(150, 0, 150),    // runway:       dark magenta runway lines
        Rgb565(0, 90, 120),     // runway_label: dark teal airport idents
        Rgb565(90, 100, 110),   // acquiring:    mid-grey "acquiring" text
        Rgb565(30, 70, 150),    // range:        dark blue range / distance text
        Rgb565(170, 175, 185),  // trail:        light grey trail (darker than background)
    },
    // 4 -- Amber Phosphor: retro amber CRT radar-scope look.
    {
        Rgb565(0, 0, 0),        // background:   black
        Rgb565(74, 48, 0),      // grid:         dim amber rings / crosshairs
        Rgb565(255, 176, 0),    // label:        amber cardinal labels
        Rgb565(255, 200, 80),   // center:       light amber receiver dot
        Rgb565(255, 176, 0),    // target:       amber aircraft
        Rgb565(255, 176, 0),    // target_tag:   amber callsign tag
        Rgb565(200, 130, 0),    // target_cat:   dim amber category tag
        Rgb565(255, 208, 96),   // target_alt:   light amber altitude tag
        Rgb565(255, 128, 0),    // vector:       orange track vector
        Rgb565(255, 176, 0),    // rim_dot:      amber off-range bearing dots
        Rgb565(160, 90, 0),     // runway:       dark amber runway lines
        Rgb565(220, 150, 40),   // runway_label: light amber airport idents
        Rgb565(150, 100, 0),    // acquiring:    dim amber "acquiring" text
        Rgb565(255, 150, 0),    // range:        orange range / distance text
        Rgb565(80, 50, 0),      // trail:        very dim amber position trail
    },
    // 5 -- Chromodynamics: MagicStack Chromodynamics editor theme mapped onto the radar. Comments
    //      note the source syntax scope / hex each color is drawn from.
    {
        Rgb565(6, 6, 6),        // background:   near-black (editor bg #060606)
        Rgb565(89, 89, 89),     // grid:         dim grey rings (gutter fg #595959)
        Rgb565(198, 198, 198),  // label:        light grey labels (foreground #c6c6c6)
        Rgb565(223, 223, 223),  // center:       near-white receiver dot (caret #dfdfdf)
        Rgb565(232, 54, 79),    // target:       crimson aircraft (keyword #e8364f)
        Rgb565(198, 198, 198),  // target_tag:   light grey callsign (foreground #c6c6c6)
        Rgb565(102, 217, 239),  // target_cat:   cyan category (storage type #66d9ef)
        Rgb565(211, 201, 112),  // target_alt:   gold altitude (string #d3c970)
        Rgb565(166, 226, 46),   // vector:       lime green vector (function/class #a6e22e)
        Rgb565(232, 54, 79),    // rim_dot:      crimson off-range dots (keyword #e8364f)
        Rgb565(211, 60, 120),   // runway:       pink runway lines (tag localname #d33c78)
        Rgb565(233, 156, 66),   // runway_label: orange airport idents (function arg #e99c42)
        Rgb565(116, 116, 117),  // acquiring:    grey "acquiring" text (comment #747475)
        Rgb565(154, 121, 215),  // range:        purple range / distance text (number #9a79d7)
        Rgb565(74, 74, 80),     // trail:        faint grey position trail
    },
};
static_assert(sizeof(kRadarThemes) / sizeof(kRadarThemes[0]) == RadarView::kNumColorSchemes,
              "kRadarThemes must have exactly RadarView::kNumColorSchemes entries.");

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
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

// ---- Aircraft tag label layout (force-directed, see RadarView::LayoutTags) ----
// LovyanGFX's default GLCD glyph is 6x8 px at text size 1; used to size label blocks in the layout
// pass (which has no canvas to measure against). DrawTarget measures the real widths when drawing.
constexpr float kFontCharWpx = 6.0f;
constexpr float kFontLineHpx = 8.0f;
// A label's center orbits its symbol at this radius plus half the block's diagonal, so the block
// clears the triangle at every angle.
constexpr float kTagOrbitClearPx = 9.0f;
constexpr float kTagLabelRepel = 900.0f;   // Label<->label soft (long-range) repulsion strength.
constexpr float kTagSymbolRepel = 500.0f;  // Label<->other-symbol soft (long-range) repulsion.
// Point repulsion alone can't guarantee two label boxes (or a label box and another symbol) don't
// overlap: inverse-square is measured center-to-center, so two boxes can overlap heavily while
// their centers are still "far enough". So on top of the soft terms we hard-eject any overlap of
// the padded axis-aligned boxes, pushing apart along the shortest exit with force proportional to
// penetration depth. These scales dominate the soft terms whenever boxes actually intersect.
constexpr float kTagSymbolPadPx = 3.0f;    // Keep-out margin around the label box, per side.
constexpr float kTagSymbolEject = 3.0f;    // Symbol-in-label-box penetration -> ejection force scale.
constexpr float kTagLabelEject = 4.0f;     // Label-box overlap penetration -> separation force scale.
constexpr float kTagRimRepel = 0.8f;       // Inward push once a label crosses the rim margin.
constexpr float kTagRimMarginPx = 6.0f;    // Start pushing labels inward this far inside the rim.
// Relaxation is angle-only (each label can only orbit its own symbol), which converges slowly, so
// run enough iterations with a large enough gain that clustered labels actually swing apart within
// a frame. The per-iteration clamp still bounds how fast any one label swivels (smooth motion).
constexpr int kTagRelaxIters = 24;         // Relaxation iterations per frame.
constexpr float kTagStepGain = 0.05f;      // Force -> angle-step scale.
constexpr float kTagMaxStepRad = 0.20f;    // Max angle change per iteration (smooth swivel cap).
// Spring pulling a label toward the angle directly behind the aircraft's heading, so tags prefer to
// trail the target (like a wake). Weak enough that label/symbol repulsion still wins in a cluster.
// Spring toward the preferred (beside-the-aircraft, perpendicular-to-heading) angle. Kept weak so
// that in a cluster the hard box separation wins (legibility first); an isolated label, with no one
// to repel it, still settles beside its aircraft.
constexpr float kTagSidePull = 0.04f;

// Accumulate an inverse-distance repulsion of (dx,dy) (from the source to the point) into (fx,fy).
// Falls off with distance and is clamped near zero separation so coincident points don't blow up.
inline void AddRepulsion(float& fx, float& fy, float dx, float dy, float k) {
    float d2 = dx * dx + dy * dy;
    if (d2 < 1.0f) d2 = 1.0f;  // Avoid the singularity when two points coincide.
    const float inv = k / d2;  // Magnitude ~ k / distance.
    fx += dx * inv;
    fy += dy * inv;
}

// Eject a symbol out of a label's bounding box. (dx,dy) is the vector from the symbol to the label
// center; (hw,hh) are the box half-extents already padded by the keep-out margin. If the symbol
// lies inside the padded box, push the label center out along the axis of least penetration (the
// shortest way to clear the overlap), with force proportional to how deep the symbol has intruded.
// No force if the symbol is outside the box, so labels are only shoved when they'd actually cover a
// symbol -- long-range spreading is left to AddRepulsion.
inline void AddBoxEject(float& fx, float& fy, float dx, float dy, float hw, float hh, float k) {
    const float ox = hw - fabsf(dx);  // Penetration on x (>0 => symbol is within the box in x).
    const float oy = hh - fabsf(dy);  // Penetration on y.
    if (ox <= 0.0f || oy <= 0.0f) return;  // Symbol clears the box on at least one axis: no overlap.
    if (ox < oy) {
        fx += (dx >= 0.0f ? ox : -ox) * k;  // Shortest exit is horizontal.
    } else {
        fy += (dy >= 0.0f ? oy : -oy) * k;  // Shortest exit is vertical.
    }
}

// Preferred orbit angle for a label whose aircraft has a known heading: beside the aircraft,
// perpendicular to its direction of travel (so it reads like a callout to the side, not a wake).
// Screen space: heading hr (0 = north, clockwise) gives forward = (sin hr, -cos hr); the two
// perpendicular screen directions are at angles hr (right of travel) and hr+pi (left). Choose the
// side pointing more outward from the radar center (sx,sy vs. center) so tags sit toward the rim
// and off the center crosshairs rather than being pinned to a fixed left/right side.
inline float TagSideAngle(float hr, float sx, float sy) {
    const float side_x = cosf(hr), side_y = sinf(hr);  // Right-of-travel screen direction.
    const float out_x = sx - RadarView::kCenterX, out_y = sy - RadarView::kCenterY;
    return (side_x * out_x + side_y * out_y >= 0.0f) ? hr : (hr + kPi);
}

// Orbit radius that places a label box (text half-extents thw,thh) centered along `angle` so the
// symbol sits `clear` px outside the box's nearest edge -- i.e. the tag hugs the triangle at every
// angle. For an axis-aligned box the center-to-edge distance along a unit direction is
// 1 / max(|cos|/thw, |sin|/thh); add the clearance margin. This replaces a fixed worst-case
// (half-diagonal) radius, which parked cardinal-ish placements needlessly far out.
inline float TagOrbitRadius(float angle, float thw, float thh, float clear) {
    const float ex = fabsf(cosf(angle)) / thw;
    const float ey = fabsf(sinf(angle)) / thh;
    const float e = ex > ey ? ex : ey;
    return clear + (e > 1e-4f ? 1.0f / e : 0.0f);
}

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

RadarView::RadarView() : theme_(&kRadarThemes[0]) {}

void RadarView::SetColorScheme(uint8_t scheme) {
    if (scheme >= kNumColorSchemes) {
        scheme = 0;
    }
    theme_ = &kRadarThemes[scheme];
}

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
    const RadarTheme& pal = *theme_;  // Active color palette (see SetColorScheme).
    // origin_y_ shifts every y so a single horizontal strip of the scene can be drawn into a
    // small sprite (banded rendering); it is 0 for full-frame / direct draw. fillScreen clears
    // only the target (the strip), so the union of strips reproduces the whole cleared frame.
    const int16_t oy = origin_y_;
    gfx->fillScreen(pal.background);

    // Concentric range rings, evenly spaced out to the outer ring.
    for (int i = 1; i <= kRingCount; i++) {
        gfx->drawCircle(kCenterX, kCenterY - oy, (kRadiusPx * i) / kRingCount, pal.grid);
    }

    // Crosshairs.
    gfx->drawFastVLine(kCenterX, kCenterY - kRadiusPx - oy, kRadiusPx * 2, pal.grid);
    gfx->drawFastHLine(kCenterX - kRadiusPx, kCenterY - oy, kRadiusPx * 2, pal.grid);

    // White receiver dot at the center.
    if (position_valid) {
        gfx->fillSmoothCircle(kCenterX, kCenterY - oy, kCenterDotRadiusPx, pal.center);
    }

    // Cardinal labels, enlarged and pushed all the way out onto the outer ring. Each is centered
    // on the ring line, with a background-filled pad breaking the ring behind it (like the range
    // label). Drawn after the rings so the pad erases the grid underneath.
    gfx->setTextColor(pal.label);
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
        gfx->fillRect(c.x - cw / 2 - 2, c.y - ch / 2 - 1 - oy, cw + 4, ch + 2, pal.background);
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
    gfx->setTextColor(pal.range);
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
        gfx->fillRect(lx - tw / 2 - 2, ly - th / 2 - 1 - oy, tw + 4, th + 2, pal.background);
        gfx->drawString(scale_buf, lx, ly - oy);
    }

    if (!position_valid) {
        gfx->setTextColor(pal.acquiring);
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

const RadarView::TagLabel* RadarView::FindLabel(uint32_t icao) const {
    if (icao == 0) return nullptr;
    for (const TagLabel& l : labels_) {
        if (l.icao == icao) return &l;
    }
    return nullptr;
}

RadarView::TagLabel* RadarView::AcquireLabel(uint32_t icao) {
    if (icao == 0) return nullptr;
    TagLabel* empty = nullptr;
    TagLabel* oldest = &labels_[0];
    for (TagLabel& l : labels_) {
        if (l.icao == icao) return &l;
        if (empty == nullptr && l.icao == 0) empty = &l;
        if (l.last_seen_ms < oldest->last_seen_ms) oldest = &l;
    }
    // No existing slot: reuse an empty one, else evict the least-recently-laid-out label.
    TagLabel* slot = (empty != nullptr) ? empty : oldest;
    slot->icao = icao;
    slot->valid = false;  // Force re-seed of the orbit angle for the new occupant.
    return slot;
}

void RadarView::LayoutTags(const RadarTarget* targets, int count) {
    // Expire labels for aircraft not seen recently (mirrors trail expiry) so slots can be reused.
    for (TagLabel& l : labels_) {
        if (l.icao != 0 && now_ms_ - l.last_seen_ms > kTrailExpiryMs) l = TagLabel{};
    }

    // Build the working set of in-range targets (off-range ones are rim dots with no tag). Each
    // gets a persistent angle slot; new labels seed at their preferred angle (behind the aircraft
    // when its heading is known, else radially outward from the busy center) so the first frame is
    // already sensible.
    struct Work {
        TagLabel* slot;
        float px, py;      // Symbol anchor, screen coords.
        float r;           // Orbit radius of the label center (recomputed per angle each iteration).
        float thw, thh;    // Text block half-extents (unpadded), for the per-angle orbit radius.
        float hw, hh;      // Label box half-extents (padded), for symbol/label box-ejection.
        float pref_angle;  // Preferred orbit angle: beside the heading, or radially outward.
    } work[kMaxTags];
    int n = 0;

    for (int i = 0; i < count && n < kMaxTags; i++) {
        const RadarTarget& t = targets[i];
        float sx, sy, range_km;
        LatLonToScreen(t.latitude_deg, t.longitude_deg, sx, sy, range_km);
        if (range_km > range_km_) continue;  // Off-range: no tag to place.

        TagLabel* slot = AcquireLabel(t.icao_address);
        if (slot == nullptr) continue;
        slot->last_seen_ms = now_ms_;

        // Preferred orbit angle: beside the aircraft, perpendicular to its heading (see
        // TagSideAngle). Without a heading, fall back to radially outward from the radar center.
        float pref_angle;
        if (t.direction_valid) {
            pref_angle = TagSideAngle(t.direction_deg * kDegToRad, sx, sy);
        } else {
            pref_angle = atan2f(sy - kCenterY, sx - kCenterX);
        }
        if (!slot->valid) {
            slot->angle_rad = pref_angle;
            slot->valid = true;
        }

        // Approximate the block size from character/line counts (no canvas here to measure). The
        // widest line is the callsign, category (<=3 ch) or altitude (up to "-NNNNNft").
        size_t max_chars = (t.callsign[0] != '\0' && t.callsign[0] != '?') ? strlen(t.callsign) : 1;
        int lines = 1;
        if (t.category[0] != '\0') {
            lines++;
            if (strlen(t.category) > max_chars) max_chars = strlen(t.category);
        }
        if (t.geom_altitude_valid) {
            lines++;
            if (max_chars < 7) max_chars = 7;
        }
        const float w = max_chars * kFontCharWpx;
        const float h = lines * kFontLineHpx;

        work[n].slot = slot;
        work[n].px = sx;
        work[n].py = sy;
        work[n].thw = 0.5f * w;
        work[n].thh = 0.5f * h;
        work[n].r = TagOrbitRadius(slot->angle_rad, work[n].thw, work[n].thh, kTagOrbitClearPx);
        work[n].hw = 0.5f * w + kTagSymbolPadPx;
        work[n].hh = 0.5f * h + kTagSymbolPadPx;
        work[n].pref_angle = pref_angle;
        n++;
    }
    if (n == 0) return;

    // Force-directed relaxation: each label may move only along its orbit circle (angle is the sole
    // degree of freedom). Take the tangential component of the net repulsion, add a spring toward
    // the preferred (beside) angle, and step the angle -- clamped per iteration so labels ease around
    // instead of snapping.
    float cx[kMaxTags], cy[kMaxTags];
    for (int iter = 0; iter < kTagRelaxIters; iter++) {
        // Current label-center positions for this iteration. The orbit radius is re-derived from the
        // current angle so the box keeps hugging the symbol as the label swivels around it.
        for (int i = 0; i < n; i++) {
            work[i].r =
                TagOrbitRadius(work[i].slot->angle_rad, work[i].thw, work[i].thh, kTagOrbitClearPx);
            cx[i] = work[i].px + cosf(work[i].slot->angle_rad) * work[i].r;
            cy[i] = work[i].py + sinf(work[i].slot->angle_rad) * work[i].r;
        }
        for (int i = 0; i < n; i++) {
            float fx = 0.0f, fy = 0.0f;
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                AddRepulsion(fx, fy, cx[i] - cx[j], cy[i] - cy[j], kTagLabelRepel);   // vs. labels
                AddRepulsion(fx, fy, cx[i] - work[j].px, cy[i] - work[j].py, kTagSymbolRepel);  // vs. symbols
                // Hard eject: separate overlapping label boxes (combined half-extents = AABB test).
                AddBoxEject(fx, fy, cx[i] - cx[j], cy[i] - cy[j], work[i].hw + work[j].hw,
                            work[i].hh + work[j].hh, kTagLabelEject);
                // Hard eject: if aircraft j's symbol falls inside this label's box, shove it clear.
                AddBoxEject(fx, fy, cx[i] - work[j].px, cy[i] - work[j].py, work[i].hw, work[i].hh,
                            kTagSymbolEject);
            }
            // Keep labels inside the round bezel: push inward once past the rim margin.
            const float dcx = cx[i] - kCenterX, dcy = cy[i] - kCenterY;
            const float rr = sqrtf(dcx * dcx + dcy * dcy);
            const float over = rr - (kRadiusPx - kTagRimMarginPx);
            if (over > 0.0f && rr > 0.01f) {
                fx -= kTagRimRepel * over * (dcx / rr);
                fy -= kTagRimRepel * over * (dcy / rr);
            }
            // Tangent to the orbit (perpendicular to the radius) at the current angle.
            const float angle = work[i].slot->angle_rad;
            const float tx = -sinf(angle);
            const float ty = cosf(angle);
            float dtheta = (fx * tx + fy * ty) / work[i].r * kTagStepGain;
            // Spring toward the beside-the-aircraft (preferred) angle, via the shortest signed delta.
            const float da = work[i].pref_angle - angle;
            dtheta += kTagSidePull * atan2f(sinf(da), cosf(da));
            if (dtheta > kTagMaxStepRad) dtheta = kTagMaxStepRad;
            if (dtheta < -kTagMaxStepRad) dtheta = -kTagMaxStepRad;
            work[i].slot->angle_rad += dtheta;
        }
    }
}

void RadarView::DrawTarget(lgfx::LGFXBase* gfx, const RadarTarget& target) {
    const RadarTheme& pal = *theme_;  // Active color palette (see SetColorScheme).
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
                gfx->drawLine(prev_x, prev_y - oy, cx, cy - oy, pal.trail);
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
        gfx->fillSmoothCircle(dot_x, dot_y - oy, 2, pal.rim_dot);
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
                          static_cast<int16_t>(baseRx), static_cast<int16_t>(baseRy - oy), pal.target);

        // Speed vector. A crisp 1px line (not a wide line) so it stays centered on the heading
        // axis the triangle is built on, rather than rounding off to one side.
        if (target.speed_kts > 0) {
            float len = target.speed_kts * kVectorPxPerKt;
            if (len > kVectorMaxPx) len = kVectorMaxPx;
            gfx->drawLine(static_cast<int16_t>(sx), static_cast<int16_t>(sy - oy),
                          static_cast<int16_t>(sx + fx * len), static_cast<int16_t>(sy + fy * len - oy),
                          pal.vector);
        }
    } else {
        // Unknown heading: plain dot.
        gfx->fillSmoothCircle(ix, iy - oy, 3, pal.target);
    }

    // Stacked tag orbiting the target: callsign (white), then emitter category (cyan) and altitude
    // in feet (yellow) below it. Lines with no data are skipped so they leave no gap. The block's
    // orbit angle around the symbol was chosen by LayoutTags (force-directed repulsion) so clustered
    // aircraft fan out; here we just read that angle back by ICAO and center the block on the orbit
    // point. All geometry is in absolute scene coords; oy is applied only at the draw calls.
    gfx->setTextSize(1);
    gfx->setTextDatum(lgfx::textdatum_t::top_left);
    const int16_t line_h = gfx->fontHeight();

    // Collect the lines to draw (up to 3) with their colors, then measure the block.
    struct TagLine {
        const char* s;
        uint16_t color;
    } lines[3];
    int n_lines = 0;
    char alt_buf[16];

    // Line 1: callsign, or "?" when none is known.
    const char* cs = (target.callsign[0] != '\0' && target.callsign[0] != '?') ? target.callsign : "?";
    lines[n_lines++] = {cs, pal.target_tag};  // White callsign.

    // Line 2: emitter category abbreviation (only when known).
    if (target.category[0] != '\0') {
        lines[n_lines++] = {target.category, pal.target_cat};  // Cyan category.
    }

    // Line 3: geometric (GNSS) altitude in feet (only when valid, to avoid a false "0ft").
    if (target.geom_altitude_valid) {
        snprintf(alt_buf, sizeof(alt_buf), "%dft", static_cast<int>(target.geom_altitude_ft));
        lines[n_lines++] = {alt_buf, pal.target_alt};  // Yellow altitude.
    }

    int16_t block_w = 0;
    for (int i = 0; i < n_lines; i++) {
        const int16_t w = gfx->textWidth(lines[i].s);
        if (w > block_w) block_w = w;
    }
    const int16_t block_h = static_cast<int16_t>(n_lines * line_h);

    // Orbit angle from the layout pass. If this target was not laid out (e.g. the label table was
    // full this frame), fall back to the same preferred angle LayoutTags would seed: beside the
    // heading when known, else radially outward. Orbit radius clears the triangle at every angle.
    const TagLabel* lbl = FindLabel(target.icao_address);
    float angle;
    if (lbl != nullptr && lbl->valid) {
        angle = lbl->angle_rad;
    } else if (target.direction_valid) {
        angle = TagSideAngle(target.direction_deg * kDegToRad, static_cast<float>(ix),
                             static_cast<float>(iy));
    } else {
        angle = atan2f(iy - kCenterY, ix - kCenterX);
    }
    const float orbit_r =
        TagOrbitRadius(angle, 0.5f * block_w, 0.5f * block_h, kTagOrbitClearPx);

    // Center the block on the orbit point, then clamp to the panel so it never clips off-screen.
    int16_t tag_x = static_cast<int16_t>(ix + cosf(angle) * orbit_r - block_w * 0.5f);
    int16_t tag_y = static_cast<int16_t>(iy + sinf(angle) * orbit_r - block_h * 0.5f);
    if (tag_x < 0) tag_x = 0;
    if (tag_y < 0) tag_y = 0;
    if (tag_x + block_w > kScreenWidth) tag_x = kScreenWidth - block_w;
    if (tag_y + block_h > kScreenHeight) tag_y = kScreenHeight - block_h;

    // Draw the lines from the block's top-left, applying the band offset only now.
    int16_t draw_y = tag_y - oy;
    for (int i = 0; i < n_lines; i++) {
        gfx->setTextColor(lines[i].color);
        gfx->drawString(lines[i].s, tag_x, draw_y);
        draw_y += line_h;
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
    const RadarTheme& pal = *theme_;  // Active color palette (see SetColorScheme).
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

            gfx->drawWideLine(x0, y0 - oy, x1, y1 - oy, kRunwayLineHalfWidth, pal.runway);
        }
        base += ds.airport_count;
    }

    // Airport idents, one per in-range airport, anchored on (or clipped onto) the outer ring and
    // nudged outward by a small gap so the text clears the runway lines.
    gfx->setTextColor(pal.runway_label);
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
