#include "simulation.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>  // abs

#include "adsb_types.hh"          // ADSBTypes::EmitterCategory
#include "adsbee_server.hh"       // adsbee_server (aircraft_dictionary)
#include "aircraft_dictionary.hh"  // Aircraft, ModeSAircraft
#include "object_dictionary.hh"   // object_dictionary (rx_position)
#include "settings.hh"            // settings_manager

Simulation simulation;

namespace {
constexpr float kDegToRad = 0.0174532925f;
constexpr float kMetersPerDegLat = 111320.0f;
constexpr float kKtsToMetersPerSec = 0.514444f;

// Reserved synthetic-aircraft ICAO block. Each track index owns a 256-address sub-block so that a
// respawn (which teleports the aircraft to a new spot) gets a brand-new ICAO -- the old track then
// prunes out and the display treats the respawn as a new aircraft, avoiding a false trail chord
// stretched across the map. 0x0Axxxx is not a real allocated ICAO country block.
constexpr uint32_t kSimIcaoBase = 0x0AC000;

// A little variety in the symbol/tag; cycled by track index.
constexpr ADSBTypes::EmitterCategory kSimCategories[] = {
    ADSBTypes::kEmitterCategoryLight,      ADSBTypes::kEmitterCategoryMedium1,
    ADSBTypes::kEmitterCategoryMedium2,    ADSBTypes::kEmitterCategoryHeavy,
    ADSBTypes::kEmitterCategoryHighVortexAircraft, ADSBTypes::kEmitterCategoryRotorcraft,
};
constexpr int kNumSimCategories = sizeof(kSimCategories) / sizeof(kSimCategories[0]);
}  // namespace

float Simulation::RandUnit() {
    // xorshift32.
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    return static_cast<float>(rng_state_ & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

float Simulation::RandRange(float lo, float hi) { return lo + (hi - lo) * RandUnit(); }

void Simulation::RespawnTrack(Track& track, int index, float range_km) {
    // Fresh ICAO within this index's sub-block (see kSimIcaoBase note).
    track.icao = kSimIcaoBase + (static_cast<uint32_t>(index) << 8) +
                 (static_cast<uint32_t>(RandUnit() * 256.0f) & 0xFFu);
    track.emitter_category = static_cast<uint8_t>(kSimCategories[index % kNumSimCategories]);

    // Place at a random bearing and radius (well inside the outer ring so it's visible on spawn).
    const float radius_km = RandRange(0.10f, 0.85f) * range_km;
    const float bearing_rad = RandRange(0.0f, 360.0f) * kDegToRad;
    const float north_m = radius_km * 1000.0f * cosf(bearing_rad);
    const float east_m = radius_km * 1000.0f * sinf(bearing_rad);
    const float cos_lat = fmaxf(0.01f, cosf(center_lat_deg_ * kDegToRad));
    track.lat_deg = center_lat_deg_ + north_m / kMetersPerDegLat;
    track.lon_deg = center_lon_deg_ + east_m / (kMetersPerDegLat * cos_lat);

    track.heading_deg = RandRange(0.0f, 360.0f);
    track.speed_kts = RandRange(120.0f, 480.0f);
    track.altitude_ft = RandRange(3000.0f, 40000.0f);
    track.vertical_rate_fpm = static_cast<int32_t>(RandRange(-1500.0f, 1500.0f));
}

void Simulation::PublishTrack(const Track& track, uint32_t now_ms) {
    ModeSAircraft ac(track.icao);
    ac.latitude_deg = track.lat_deg;
    ac.longitude_deg = track.lon_deg;
    ac.gnss_altitude_ft = static_cast<int32_t>(track.altitude_ft);
    ac.baro_altitude_ft = static_cast<int32_t>(track.altitude_ft);
    ac.direction_deg = track.heading_deg;
    ac.speed_kts = static_cast<int32_t>(track.speed_kts);
    ac.gnss_vertical_rate_fpm = track.vertical_rate_fpm;
    ac.baro_vertical_rate_fpm = track.vertical_rate_fpm;
    ac.emitter_category = static_cast<ADSBTypes::EmitterCategory>(track.emitter_category);
    snprintf(ac.callsign, sizeof(ac.callsign), "SIM%03u", static_cast<unsigned>(track.icao % 1000u));

    ac.WriteBitFlag(ModeSAircraft::kBitFlagPositionValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagDirectionValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagGNSSAltitudeValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagBaroAltitudeValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagHorizontalSpeedValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagGNSSVerticalRateValid, true);
    ac.WriteBitFlag(ModeSAircraft::kBitFlagIsAirborne, true);

    ac.last_message_timestamp_ms = now_ms;  // Keep fresh so the dictionary pruner won't drop it.
    adsbee_server.aircraft_dictionary.InsertAircraft(ac);
}

void Simulation::ClearTracks() {
    for (uint8_t i = 0; i < num_tracks_; i++) {
        adsbee_server.aircraft_dictionary.RemoveAircraft(
            Aircraft::ICAOToUID(tracks_[i].icao, Aircraft::kAircraftTypeModeS));
    }
}

bool Simulation::GetActiveCenter(float& lat_deg, float& lon_deg) const {
    if (!settings_manager.settings.sim_mode_enabled) {
        return false;
    }
    lat_deg = center_lat_deg_;
    lon_deg = center_lon_deg_;
    return true;
}

void Simulation::Update(uint32_t now_ms) {
    const SettingsManager::Settings& settings = settings_manager.settings;

    uint8_t desired = settings.sim_num_aircraft;
    if (desired > kMaxAircraft) desired = kMaxAircraft;

    if (!settings.sim_mode_enabled || desired == 0) {
        // Disabled: tear down once, then stay idle.
        if (active_) {
            ClearTracks();
            active_ = false;
            num_tracks_ = 0;
        }
        last_update_ms_ = now_ms;
        return;
    }

    // Resolve the active center: real receiver position if one is set, else the fixed demo center.
    // Mirror Display::ResolveCenter()'s "unpopulated (0,0) fix means unavailable" convention.
    const SettingsManager::RxPosition& rx = object_dictionary.composite_device_status.rp2040.rx_position;
    if (rx.latitude_deg != 0.0f || rx.longitude_deg != 0.0f) {
        center_lat_deg_ = rx.latitude_deg;
        center_lon_deg_ = rx.longitude_deg;
    } else {
        center_lat_deg_ = kDemoCenterLatDeg;
        center_lon_deg_ = kDemoCenterLonDeg;
    }

    float range_km = fmaxf(settings.display_range_km, 2.0f);

    // (Re)seed all tracks on first enable or when the requested count changes.
    if (!active_ || num_tracks_ != desired) {
        ClearTracks();
        num_tracks_ = desired;
        for (uint8_t i = 0; i < num_tracks_; i++) {
            RespawnTrack(tracks_[i], i, range_km);
        }
        active_ = true;
        last_update_ms_ = now_ms;  // No motion on the seeding tick.
    }

    float dt_s = static_cast<float>(now_ms - last_update_ms_) / 1000.0f;
    if (dt_s < 0.0f) dt_s = 0.0f;
    if (dt_s > 5.0f) dt_s = 5.0f;  // Clamp so a long stall doesn't fling aircraft across the map.
    last_update_ms_ = now_ms;

    const float cos_lat = fmaxf(0.01f, cosf(center_lat_deg_ * kDegToRad));

    for (uint8_t i = 0; i < num_tracks_; i++) {
        Track& t = tracks_[i];

        // Integrate great-circle-ish motion (flat-earth approximation is fine at these ranges).
        const float dist_m = t.speed_kts * kKtsToMetersPerSec * dt_s;
        const float hdg_rad = t.heading_deg * kDegToRad;
        t.lat_deg += (dist_m * cosf(hdg_rad)) / kMetersPerDegLat;
        t.lon_deg += (dist_m * sinf(hdg_rad)) / (kMetersPerDegLat * cos_lat);

        // Gentle heading drift for a livelier picture.
        t.heading_deg += RandRange(-4.0f, 4.0f) * dt_s;
        if (t.heading_deg < 0.0f) t.heading_deg += 360.0f;
        if (t.heading_deg >= 360.0f) t.heading_deg -= 360.0f;

        // Altitude drift, bouncing off soft ceiling/floor.
        t.altitude_ft += t.vertical_rate_fpm * (dt_s / 60.0f);
        if (t.altitude_ft > 42000.0f) {
            t.altitude_ft = 42000.0f;
            t.vertical_rate_fpm = -abs(t.vertical_rate_fpm);
        } else if (t.altitude_ft < 2000.0f) {
            t.altitude_ft = 2000.0f;
            t.vertical_rate_fpm = abs(t.vertical_rate_fpm);
        }

        // Respawn once it drifts beyond the visible area so the scene stays populated.
        const float dlat = t.lat_deg - center_lat_deg_;
        const float dlon = (t.lon_deg - center_lon_deg_) * cos_lat;
        const float dist_km = sqrtf(dlat * dlat + dlon * dlon) * (kMetersPerDegLat / 1000.0f);
        if (dist_km > range_km * 1.2f) {
            // Remove the old ICAO before respawn assigns a new one, so it doesn't linger.
            adsbee_server.aircraft_dictionary.RemoveAircraft(
                Aircraft::ICAOToUID(t.icao, Aircraft::kAircraftTypeModeS));
            RespawnTrack(t, i, range_km);
        }

        PublishTrack(t, now_ms);
    }
}
