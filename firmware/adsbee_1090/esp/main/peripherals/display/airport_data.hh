// Shared POD types for the embedded airport/runway datasets used by the radar overlay.
//
// Every category dataset (large_airports.hh, medium_airports.hh, ...) shares these types so
// radar_view.cpp can treat any mix of categories uniformly via a table of AirportDataset
// descriptors. The category headers only declare their arrays + counts and expose a kDataset.
//
// Coordinates are stored as int32 degrees x 1e7 (e7). The arrays are POD placed in .rodata
// (memory-mapped flash on the ESP32-S3), so a dataset costs flash, not DRAM. Runway.airport_idx
// indexes the kAirports[] of the same dataset.
#pragma once

#include <cstddef>
#include <cstdint>

namespace data {

struct Airport {
  char ident[5];
  int32_t lat_e7;
  int32_t lon_e7;
};

struct Runway {
  uint16_t airport_idx;
  int32_t le_lat_e7;
  int32_t le_lon_e7;
  int32_t he_lat_e7;
  int32_t he_lon_e7;
  uint16_t length_m;
};

// Descriptor pointing at one category's arrays, so the renderer can iterate a heterogeneous set
// of datasets. Runway.airport_idx is relative to this descriptor's airports[].
struct AirportDataset {
  const Airport* airports;
  size_t airport_count;
  const Runway* runways;
  size_t runway_count;
};

}  // namespace data
