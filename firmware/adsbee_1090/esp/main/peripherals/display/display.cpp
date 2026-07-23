// Entire body compiled out unless the display is enabled; the TU still compiles (empty) so
// SRC_DIRS globbing does not break non-display builds. The display.hh include lives inside the
// guard so a WITH_DISPLAY-off build never pulls in LovyanGFX (via lgfx_config.hpp).
#ifdef WITH_DISPLAY

#include "display.hh"

#include <cstring>
#include <variant>

#include "adsbee_server.hh"        // adsbee_server, AircraftDictionary
#include "aircraft_dictionary.hh"  // ModeSAircraft, UATAircraft, get_if, HasBitFlag
#include "comms.hh"                // CONSOLE_* logging (tunnels to the RP2040 console for visibility).
#include "esp_heap_caps.h"         // heap_caps_get_free_size / _largest_free_block (strip sizing)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"       // vTaskDelay, pdMS_TO_TICKS
#include "hal.hh"                // get_time_since_boot_ms()
#include "object_dictionary.hh"  // object_dictionary, RxPosition
#include "settings.hh"
#include "splash_data.hh"  // data::splash::kImage / kWidth / kHeight

static const char* kTag = "Display";

Display display = Display();

namespace {
// Map an ADS-B emitter category to a compact 3-letter abbreviation for the radar tag. Returns
// "" for categories with no useful label (unknown/reserved), which hides the tag line.
const char* EmitterCategoryAbbrev(ADSBTypes::EmitterCategory cat) {
    switch (cat) {
        case ADSBTypes::kEmitterCategoryLight:
            return "LGT";
        case ADSBTypes::kEmitterCategoryMedium1:
        case ADSBTypes::kEmitterCategoryMedium2:
            return "MED";
        case ADSBTypes::kEmitterCategoryHighVortexAircraft:
        case ADSBTypes::kEmitterCategoryHeavy:
            return "HVY";
        case ADSBTypes::kEmitterCategoryHighPerformance:
            return "HPF";
        case ADSBTypes::kEmitterCategoryRotorcraft:
            return "ROT";
        case ADSBTypes::kEmitterCategoryGliderSailplane:
            return "GLD";
        case ADSBTypes::kEmitterCategoryLighterThanAir:
            return "LTA";
        case ADSBTypes::kEmitterCategoryParachutistSkydiver:
            return "PAR";
        case ADSBTypes::kEmitterCategoryUltralightHangGliderParaglider:
            return "ULT";
        case ADSBTypes::kEmitterCategoryUnmannedAerialVehicle:
            return "UAV";
        case ADSBTypes::kEmitterCategorySpaceTransatmosphericVehicle:
            return "SPC";
        case ADSBTypes::kEmitterCategorySurfaceEmergencyVehicle:
        case ADSBTypes::kEmitterCategorySurfaceServiceVehicle:
            return "SRF";
        case ADSBTypes::kEmitterCategoryPointObstacle:
        case ADSBTypes::kEmitterCategoryClusterObstacle:
        case ADSBTypes::kEmitterCategoryLineObstacle:
            return "OBS";
        default:
            return "";
    }
}

// Populate a RadarTarget from either aircraft type. The shared fields live on the Aircraft base
// class, and callsign/HasBitFlag/kBitFlag* names are identical on ModeSAircraft and UATAircraft,
// so one template covers both. Returns false (skip plotting) if the position is not valid.
template <typename AC>
bool FillTarget(const AC* ac, RadarTarget& t) {
    if (!ac->HasBitFlag(AC::kBitFlagPositionValid)) {
        return false;
    }
    t.latitude_deg = ac->latitude_deg;
    t.longitude_deg = ac->longitude_deg;
    t.geom_altitude_ft = ac->gnss_altitude_ft;
    t.geom_altitude_valid = ac->HasBitFlag(AC::kBitFlagGNSSAltitudeValid);
    t.direction_deg = ac->direction_deg;
    t.speed_kts = ac->speed_kts;
    t.direction_valid = ac->HasBitFlag(AC::kBitFlagDirectionValid);
    t.icao_address = ac->icao_address;
    strncpy(t.callsign, ac->callsign, sizeof(t.callsign) - 1);
    strncpy(t.category, EmitterCategoryAbbrev(ac->emitter_category), sizeof(t.category) - 1);
    return true;
}
}  // namespace

bool Display::Init() {
    if (!lcd_.init()) {
        CONSOLE_ERROR(kTag, "GC9A01 panel init failed; display disabled.");
        return false;  // Leave initialized_ false so Update() is a no-op.
    }
    lcd_.setRotation(0);
    lcd_.fillScreen(0x0000);

    // Reusable off-screen strip for banded, flicker-free rendering. There is no PSRAM, so buffers
    // compete with WiFi/lwIP/TLS and the AircraftDictionary in fragmented internal SRAM: a
    // full-frame buffer does not fit at any depth. Whatever contiguous block is free when the
    // display comes up depends on heap fragmentation, so try progressively smaller 16-bit strips
    // (kBandHeightCandidates) and keep the first that allocates -- a shorter strip just means more
    // bands per frame (negligible at ~5 Hz) and preserves flicker-free rendering. Only if even the
    // smallest fails do we fall back to drawing directly to the panel (functional, but flickers).
    static_assert(
        [] {
            for (int16_t h : kBandHeightCandidates)
                if (h <= 0 || RadarView::kScreenHeight % h != 0) return false;
            return true;
        }(),
        "every band height candidate must evenly divide screen height");

    // PSRAM probe: MALLOC_CAP_SPIRAM heap is only registered if CONFIG_SPIRAM is enabled AND the
    // module actually has a PSRAM die that initialized at boot. total>0 => PSRAM present and usable
    // (in which case trails / a full framebuffer / WiFi buffers could move off internal SRAM);
    // total==0 => no PSRAM detected in the current mode (absent, or an octal module probed in quad
    // mode). See the boot log for the ESP-IDF "SPIRAM" init line as well.
    CONSOLE_INFO(kTag, "PSRAM probe: total=%u bytes, free=%u bytes.",
                 static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // Log the heap budget up front so the strip size / kMinFreeHeapAfterStripBytes can be tuned
    // against real numbers if the web UI still runs out of memory on connect.
    CONSOLE_INFO(kTag, "Heap before strip alloc: free=%u, largest block=%u.",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    // Pick a strip height, largest first. Prefer the largest that still leaves the preferred
    // reserve free; but smaller strips leave MORE free, so if none meets the reserve we keep the
    // SMALLEST candidate that allocates -- a tiny strip is only a few KB and stays flicker-free,
    // which beats dropping to direct-to-panel draw. Direct-draw is used only if even the smallest
    // strip can't allocate at all.
    constexpr size_t kNumCandidates = sizeof(kBandHeightCandidates) / sizeof(kBandHeightCandidates[0]);
    band_ = new LGFX_Sprite(&lcd_);
    band_->setColorDepth(16);
    // Track the smallest height that actually allocated but fell under the reserve, so if no height
    // meets the reserve we can fall back to it (flicker-free beats direct-draw) WITHOUT having bet
    // on a smaller allocation that might not succeed. Freeing an under-reserve strip before trying
    // smaller is safe to re-create later: the heap is at least as free as when it first allocated.
    int16_t fallback_height = 0;
    for (size_t i = 0; i < kNumCandidates; i++) {
        const int16_t candidate = kBandHeightCandidates[i];
        if (band_->createSprite(RadarView::kScreenWidth, candidate) == nullptr) {
            continue;  // Too large to allocate at all right now; try a shorter strip.
        }
        const uint32_t free_after = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (free_after >= kMinFreeHeapAfterStripBytes) {
            band_height_ = candidate;  // Meets the reserve: keep this sprite.
            CONSOLE_INFO(kTag, "Strip buffer: %d rows (%d bands), heap free after=%u.", candidate,
                         RadarView::kScreenHeight / candidate, static_cast<unsigned>(free_after));
            break;
        }
        // Allocated but under the reserve. A smaller candidate leaves more free, so prefer it --
        // remember this height as a fallback and give the sprite back before trying smaller.
        fallback_height = candidate;
        CONSOLE_WARNING(kTag, "Strip %d rows leaves only %u free (< %u reserve); trying smaller.",
                        candidate, static_cast<unsigned>(free_after),
                        static_cast<unsigned>(kMinFreeHeapAfterStripBytes));
        band_->deleteSprite();
    }
    // No height met the reserve, but at least one allocated: re-create the smallest such height
    // (guaranteed to fit now -- the heap is at least as free as when it allocated during the loop)
    // rather than dropping to flickery direct-draw.
    if (band_height_ == 0 && fallback_height != 0 &&
        band_->createSprite(RadarView::kScreenWidth, fallback_height) != nullptr) {
        band_height_ = fallback_height;
        CONSOLE_WARNING(kTag, "Strip buffer: %d rows below reserve -- using it (flicker-free beats direct draw).",
                        fallback_height);
    }
    if (band_height_ == 0) {
        CONSOLE_WARNING(kTag, "No strip height could allocate; direct panel draw (may flicker).");
        delete band_;
        band_ = nullptr;
    }

    ShowSplash();

    // Draw the initial background so the panel shows something before the first aircraft arrive.
    RenderFrame(false);

    initialized_ = true;
    CONSOLE_INFO(kTag, "GC9A01 display initialized (%s rendering).", band_ ? "banded" : "direct");
    return true;
}

void Display::ShowSplash() {
    lcd_.fillScreen(0x0000);
    // The embedded array is native-endian RGB565; the panel's SPI bus wants the bytes swapped, so
    // without this the high/low bytes invert (e.g. yellow reads as blue/purple). Only pushImage()
    // honors this flag -- the radar view draws with color primitives, which are unaffected -- so
    // it is safe to leave set. Drawing straight to the panel is fine here: this is a one-shot draw
    // (not a per-frame redraw), so there is no flicker to avoid.
    lcd_.setSwapBytes(true);
    lcd_.pushImage((RadarView::kScreenWidth - data::splash::kWidth) / 2,
                   (RadarView::kScreenHeight - data::splash::kHeight) / 2, data::splash::kWidth, data::splash::kHeight,
                   data::splash::kImage);
    vTaskDelay(pdMS_TO_TICKS(kSplashDurationMs));
}

bool Display::ResolveCenter() {
    // rx_position is written by the SPI receive task on another core; snapshot the fields we need
    // into locals in one read pass to narrow (not eliminate) the torn-read window. A stale/torn
    // read here is cosmetic for the display -- at worst a brief center jump -- so no lock is taken.
    const SettingsManager::RxPosition& rx = object_dictionary.composite_device_status.rp2040.rx_position;
    SettingsManager::RxPosition::PositionSource source = rx.source;
    float lat = rx.latitude_deg;
    float lon = rx.longitude_deg;

    if (source == SettingsManager::RxPosition::kPositionSourceNone) {
        return false;
    }
    // Treat a still-unpopulated (0,0) fix as unavailable to avoid centering on the Gulf of Guinea.
    if (lat == 0.0f && lon == 0.0f) {
        return false;
    }
    radar_.SetCenter(lat, lon);
    return true;
}

void Display::Update() {
    if (!initialized_) return;

    uint32_t now_ms = get_time_since_boot_ms();
    if (now_ms - last_frame_timestamp_ms_ < kMinFrameIntervalMs) {
        return;  // Rate limit.
    }
    last_frame_timestamp_ms_ = now_ms;

    // Pick up the latest range/zoom setting (synced from the RP2040 over SPI). SetRangeKm() only
    // marks the airport cache dirty when the value actually changes, so applying it every frame is
    // cheap.
    radar_.SetRangeKm(settings_manager.settings.display_range_km);

    // Advance the trail clock and expire stale trails once per frame (not per band).
    radar_.BeginFrame(now_ms);

    RenderFrame(ResolveCenter());
}

void Display::DrawScene(lgfx::LGFXBase* gfx, bool position_valid) {
    radar_.DrawBackground(gfx, position_valid);

    if (position_valid) {
        // Runway/airport overlay sits under the aircraft symbols.
        radar_.DrawAirports(gfx);

        // The ESP32 main loop owns the aircraft dictionary, so it is safe to iterate it directly
        // here (same task as ADSBeeServer::Update()). NOTE: this single-task guarantee covers the
        // dictionary only -- the rx_position read in ResolveCenter() is cross-core (see there). If
        // a dedicated render task is ever introduced, this must switch to a snapshot queue instead.
        for (auto& itr : adsbee_server.aircraft_dictionary.dict) {
            RadarTarget target;
            bool plot = false;

            if (ModeSAircraft* ac = std::get_if<ModeSAircraft>(&itr.second)) {
                plot = FillTarget(ac, target);
            } else if (UATAircraft* ac = std::get_if<UATAircraft>(&itr.second)) {
                plot = FillTarget(ac, target);
            }

            if (plot) {
                radar_.DrawTarget(gfx, target);
            }
        }
    }
}

void Display::RenderFrame(bool position_valid) {
    if (band_ != nullptr) {
        // Banded path: redraw the whole scene into the strip once per band (RadarView clips the
        // out-of-strip pixels) and push each strip to its screen row. Every screen region is
        // written exactly once per frame, so there is no clear-then-redraw flicker. The scene is
        // redrawn per band, but at ~5 Hz over a handful of aircraft the cost is negligible.
        for (int16_t y0 = 0; y0 < RadarView::kScreenHeight; y0 += band_height_) {
            radar_.SetOriginY(y0);
            DrawScene(band_, position_valid);
            band_->pushSprite(0, y0);
        }
        radar_.SetOriginY(0);
        return;
    }

    // Direct fallback (strip alloc failed): draw straight to the panel. DrawBackground()
    // fillScreen()s the live panel each frame, so this flickers -- acceptable degraded mode.
    radar_.SetOriginY(0);
    DrawScene(&lcd_, position_valid);
}

#endif  // WITH_DISPLAY
