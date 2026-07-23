// Embedded "large" airport + runway dataset for the radar runway overlay.
//
// Source: OurAirports (public domain, https://ourairports.com/data/), filtered to
// large_airport rows. Regenerate with:
//   python3 firmware/scripts/build_airports.py --category large_airport
// (see that script for the full OurAirports pipeline; filtering logic ported from
// ESP32-Plane-Radar's scripts/build_large_airports.py, MIT, (c) 2026 MatixYo).
// Do not edit by hand; regenerate from the script if the dataset changes.
//
// Struct definitions and the AirportDataset descriptor live in airport_data.hh so every
// category dataset shares one set of POD types.
#pragma once

#include "airport_data.hh"

namespace data::large_airports {

constexpr size_t kAirportCount = 1166;
constexpr size_t kRunwayCount = 1706;

extern const Airport kAirports[];
extern const Runway kRunways[];

constexpr AirportDataset kDataset{kAirports, kAirportCount, kRunways, kRunwayCount};

}  // namespace data::large_airports
