// Embedded "small_airport" airport + runway dataset for the radar runway overlay.
//
// Source: OurAirports (public domain, https://ourairports.com/data/), filtered to
// small_airport rows. Regenerate with:
//   python3 firmware/scripts/build_airports.py --category small_airport
// (filtering logic ported from ESP32-Plane-Radar's scripts/build_large_airports.py,
// MIT, (c) 2026 MatixYo). Do not edit by hand; regenerate from the script if the dataset
// changes.
//
// Struct definitions and the AirportDataset descriptor live in airport_data.hh so every
// category dataset shares one set of POD types.
#pragma once

#include "airport_data.hh"

namespace data::small_airports {

constexpr size_t kAirportCount = 24238;
constexpr size_t kRunwayCount = 6939;

extern const Airport kAirports[];
extern const Runway kRunways[];

constexpr AirportDataset kDataset{kAirports, kAirportCount, kRunways, kRunwayCount};

}  // namespace data::small_airports
