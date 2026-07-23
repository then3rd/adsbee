// Entire body compiled out unless the display is enabled; the TU still compiles (empty) so
// SRC_DIRS globbing does not break non-display builds. The display.hh include lives inside the
// guard so a WITH_DISPLAY-off build never pulls in LovyanGFX (via lgfx_config.hpp).
#ifdef WITH_DISPLAY

#include "display.hh"

#include <cstring>
#include <variant>

#include "adsbee_server.hh"       // adsbee_server, AircraftDictionary
#include "aircraft_dictionary.hh"  // ModeSAircraft, UATAircraft, get_if, HasBitFlag
#include "comms.hh"                // CONSOLE_* logging (tunnels to the RP2040 console for visibility).
#include "hal.hh"                  // get_time_since_boot_ms()
#include "object_dictionary.hh"    // object_dictionary, RxPosition
#include "settings.hh"

static const char* kTag = "Display";

Display display = Display();

namespace {
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
    t.baro_altitude_ft = ac->baro_altitude_ft;
    t.baro_altitude_valid = ac->HasBitFlag(AC::kBitFlagBaroAltitudeValid);
    t.direction_deg = ac->direction_deg;
    t.speed_kts = ac->speed_kts;
    t.direction_valid = ac->HasBitFlag(AC::kBitFlagDirectionValid);
    strncpy(t.callsign, ac->callsign, sizeof(t.callsign) - 1);
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

    // Off-screen double buffer for flicker-free rendering. There is no PSRAM, so this ~115 KB
    // (240*240*2) allocation competes with WiFi/lwIP/TLS and the AircraftDictionary in internal
    // SRAM. If it fails we fall back to drawing directly to the panel (acceptable at 5-10 Hz).
    canvas_ = new LGFX_Sprite(&lcd_);
    canvas_->setColorDepth(16);
    if (canvas_->createSprite(RadarView::kScreenWidth, RadarView::kScreenHeight) == nullptr) {
        CONSOLE_WARNING(kTag, "Off-screen sprite alloc failed; falling back to direct panel draw.");
        delete canvas_;
        canvas_ = nullptr;
    }

    // Draw the initial background so the panel shows something before the first aircraft arrive.
    lgfx::LGFXBase* gfx = canvas_ ? static_cast<lgfx::LGFXBase*>(canvas_) : static_cast<lgfx::LGFXBase*>(&lcd_);
    radar_.DrawBackground(gfx, false);
    if (canvas_) canvas_->pushSprite(0, 0);

    initialized_ = true;
    CONSOLE_INFO(kTag, "GC9A01 display initialized (%s buffering).", canvas_ ? "double" : "direct");
    return true;
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

    bool position_valid = ResolveCenter();

    lgfx::LGFXBase* gfx = canvas_ ? static_cast<lgfx::LGFXBase*>(canvas_) : static_cast<lgfx::LGFXBase*>(&lcd_);
    radar_.DrawBackground(gfx, position_valid);

    if (position_valid) {
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

    if (canvas_) {
        canvas_->pushSprite(0, 0);
    }
}

#endif  // WITH_DISPLAY
