#ifndef SIMULATION_HH_
#define SIMULATION_HH_

#include <cstdint>

/**
 * Bench / demo simulation. When enabled (via AT+SIM on the RP2040, whose setting is synced to the
 * ESP32 over SPI), injects a set of synthetic moving aircraft into the ESP32 aircraft dictionary so
 * the radar display, web UI, and output feeds all show traffic without any live RF reception.
 *
 * The synthetic aircraft use obviously-fake callsigns ("SIMxxx") and a reserved ICAO block so they
 * can't be confused with -- or collide with -- real traffic. If no real receiver position is
 * available, a fixed demo center is used (see GetActiveCenter) so the radar has a point to center
 * on; if a real position exists the synthetic traffic is placed around it instead.
 *
 * Runs entirely in the ESP32 main loop (same task as ADSBeeServer::Update() and the display), so no
 * locking is needed around the dictionary or the center state.
 */
class Simulation {
   public:
    // Hard cap on synthetic aircraft, independent of the (clamped) settings value.
    static constexpr uint8_t kMaxAircraft = 12;

    // Fixed demo center used when no real receiver position is set (Seattle-Tacoma area).
    static constexpr float kDemoCenterLatDeg = 47.4489f;
    static constexpr float kDemoCenterLonDeg = -122.3094f;

    /**
     * Advance the simulation by one tick. Cheap no-op when disabled, apart from a one-time teardown
     * (removing the synthetic aircraft from the dictionary) on the enabled -> disabled transition.
     * Call once per ADSBeeServer::Update().
     * @param[in] now_ms Milliseconds since boot; used for motion integration and to keep the
     * synthetic aircraft fresh so the dictionary's pruner doesn't drop them.
     */
    void Update(uint32_t now_ms);

    /**
     * Center the display should use while the simulation owns the scene. Returns true (and writes
     * the active center: the real receiver position if one exists, otherwise the fixed demo center)
     * only while the simulation is enabled. Used by Display::ResolveCenter() as a fallback when no
     * real position fix is present.
     * @param[out] lat_deg Active center latitude, degrees.
     * @param[out] lon_deg Active center longitude, degrees.
     * @retval True if the simulation is enabled and a center was written, false otherwise.
     */
    bool GetActiveCenter(float& lat_deg, float& lon_deg) const;

   private:
    struct Track {
        uint32_t icao = 0;
        float lat_deg = 0.0f;
        float lon_deg = 0.0f;
        float heading_deg = 0.0f;  // 0 = north, clockwise.
        float speed_kts = 0.0f;
        float altitude_ft = 0.0f;
        int32_t vertical_rate_fpm = 0;
        uint8_t emitter_category = 0;
    };

    // (Re)place a single track at a random bearing/radius around the center with fresh kinematics.
    void RespawnTrack(Track& track, int index, float range_km);
    // Write a track into the aircraft dictionary as a fresh ModeSAircraft snapshot.
    void PublishTrack(const Track& track, uint32_t now_ms);
    // Remove all currently-published synthetic aircraft from the dictionary.
    void ClearTracks();

    // Small deterministic PRNG (xorshift32) so behavior is reproducible and self-contained.
    float RandUnit();                    // [0, 1)
    float RandRange(float lo, float hi);  // [lo, hi)

    bool active_ = false;      // True once tracks are populated and being published.
    uint8_t num_tracks_ = 0;   // Number of live tracks (<= kMaxAircraft).
    uint32_t last_update_ms_ = 0;
    float center_lat_deg_ = kDemoCenterLatDeg;  // Active center used for placement / GetActiveCenter.
    float center_lon_deg_ = kDemoCenterLonDeg;
    uint32_t rng_state_ = 0x1a2b3c4du;
    Track tracks_[kMaxAircraft];
};

extern Simulation simulation;

#endif  // SIMULATION_HH_
