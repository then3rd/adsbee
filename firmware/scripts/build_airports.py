#!/usr/bin/env python3
"""Generate embedded airport/runway datasets for the GC9A01 radar overlay.

Downloads OurAirports' public-domain airports.csv + runways.csv and emits, for each
requested `type` category, a pair of C++ files consumed by radar_view.cpp:

    <slug>.hh          declarations (namespace data::<slug>, counts, extern arrays, kDataset)
    <slug>_data.cpp    the brace-initialized arrays (guarded on WITH_DISPLAY)

where slug = "<category>s" (e.g. large_airport -> large_airports). Coordinates are stored
as int32 degrees x 1e7 (e7); runway lengths are converted to metres. The generated arrays
share the POD types declared in peripherals/display/airport_data.hh.

Usage:
    python3 firmware/scripts/build_airports.py                       # default: large_airport
    python3 firmware/scripts/build_airports.py --category medium_airport
    python3 firmware/scripts/build_airports.py --category large_airport medium_airport
    python3 firmware/scripts/build_airports.py --category heliport --out-dir /tmp/out

Filtering logic (large-airport + runway rules) ported from ESP32-Plane-Radar's
scripts/build_large_airports.py (MIT License, (c) 2026 MatixYo). No third-party deps.
"""

import argparse
import csv
import io
import pathlib
import sys
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_OUT_DIR = REPO_ROOT / "firmware/adsbee_1090/esp/main/peripherals/display"

BASE_URL = "https://davidmegginson.github.io/ourairports-data"
AIRPORTS_URL = f"{BASE_URL}/airports.csv"
RUNWAYS_URL = f"{BASE_URL}/runways.csv"

# All airport `type` values OurAirports uses.
VALID_CATEGORIES = [
    "large_airport",
    "medium_airport",
    "small_airport",
    "heliport",
    "seaplane_base",
    "balloonport",
    "closed",
]

FT_TO_M = 0.3048
# Runways shorter than this whose designators look like helipads (leading "H") are dropped
# for airport categories; skipped for the heliport category (where short pads are the point).
HELIPAD_MAX_FT = 2500


def to_e7(value: str) -> int:
    """Degrees string -> int32 degrees x 1e7."""
    return round(float(value) * 1e7)


def fetch_csv(url: str) -> list[dict]:
    print(f"Downloading {url} ...")
    with urllib.request.urlopen(url) as resp:
        text = resp.read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def is_helipad(row: dict) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    return le.startswith("H") or he.startswith("H")


def select_airports(rows: list[dict], category: str) -> list[dict]:
    """Airports of the given type with a 4-char ident and valid coordinates."""
    out = []
    for row in rows:
        if row.get("type") != category:
            continue
        ident = (row.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        try:
            lat = to_e7(row["latitude_deg"])
            lon = to_e7(row["longitude_deg"])
        except (KeyError, ValueError):
            continue
        out.append({"ident": ident, "lat_e7": lat, "lon_e7": lon})
    out.sort(key=lambda a: a["ident"])
    return out


def select_runways(rows: list[dict], airport_idx: dict[str, int], category: str) -> list[dict]:
    """Runways belonging to the selected airports, excluding closed and (for airport
    categories) helipads, with valid endpoint coordinates and a positive length."""
    keep_helipads = category in ("heliport", "seaplane_base")
    out = []
    for row in rows:
        ap_ident = (row.get("airport_ident") or "").strip()
        idx = airport_idx.get(ap_ident)
        if idx is None:
            continue
        if (row.get("closed") or "0").strip() in ("1", "true", "True"):
            continue
        try:
            length_ft = float(row["length_ft"])
            le_lat = to_e7(row["le_latitude_deg"])
            le_lon = to_e7(row["le_longitude_deg"])
            he_lat = to_e7(row["he_latitude_deg"])
            he_lon = to_e7(row["he_longitude_deg"])
        except (KeyError, ValueError):
            continue
        if length_ft <= 0:
            continue
        if not keep_helipads and is_helipad(row) and length_ft < HELIPAD_MAX_FT:
            continue
        length_m = round(length_ft * FT_TO_M)
        if length_m <= 0 or length_m > 0xFFFF:
            continue
        out.append(
            {
                "airport_idx": idx,
                "le_lat_e7": le_lat,
                "le_lon_e7": le_lon,
                "he_lat_e7": he_lat,
                "he_lon_e7": he_lon,
                "length_m": length_m,
            }
        )
    # Group by airport so runways for one airport are contiguous (matches existing dataset).
    out.sort(key=lambda r: r["airport_idx"])
    return out


HEADER_TEMPLATE = """\
// Embedded "{category}" airport + runway dataset for the radar runway overlay.
//
// Source: OurAirports (public domain, https://ourairports.com/data/), filtered to
// {category} rows. Regenerate with:
//   python3 firmware/scripts/build_airports.py --category {category}
// (filtering logic ported from ESP32-Plane-Radar's scripts/build_large_airports.py,
// MIT, (c) 2026 MatixYo). Do not edit by hand; regenerate from the script if the dataset
// changes.
//
// Struct definitions and the AirportDataset descriptor live in airport_data.hh so every
// category dataset shares one set of POD types.
#pragma once

#include "airport_data.hh"

namespace data::{slug} {{

constexpr size_t kAirportCount = {airport_count};
constexpr size_t kRunwayCount = {runway_count};

extern const Airport kAirports[];
extern const Runway kRunways[];

constexpr AirportDataset kDataset{{kAirports, kAirportCount, kRunways, kRunwayCount}};

}}  // namespace data::{slug}
"""

CPP_PREAMBLE = """\
// Generated "{category}" airport/runway dataset for the radar runway overlay. Do not edit by hand.
// See {slug}.hh for provenance and licensing.
//
// Guarded on WITH_DISPLAY so non-display builds do not carry the .rodata; the TU still
// compiles (empty) so SRC_DIRS globbing does not break those builds. --gc-sections drops the
// arrays from the image unless radar_view.cpp references this category's kDataset.
#ifdef WITH_DISPLAY

#include "{slug}.hh"

namespace data::{slug} {{

"""


def write_header(out_dir: pathlib.Path, slug: str, category: str, airports: list, runways: list):
    path = out_dir / f"{slug}.hh"
    path.write_text(
        HEADER_TEMPLATE.format(
            category=category,
            slug=slug,
            airport_count=len(airports),
            runway_count=len(runways),
        )
    )
    return path


def write_data(out_dir: pathlib.Path, slug: str, category: str, airports: list, runways: list):
    path = out_dir / f"{slug}_data.cpp"
    with path.open("w") as f:
        f.write(CPP_PREAMBLE.format(category=category, slug=slug))
        f.write("const Airport kAirports[] = {\n")
        for a in airports:
            f.write(f'  {{"{a["ident"]}", {a["lat_e7"]}, {a["lon_e7"]}}},\n')
        f.write("};\n\n")
        f.write("const Runway kRunways[] = {\n")
        for r in runways:
            f.write(
                f'  {{{r["airport_idx"]}, {r["le_lat_e7"]}, {r["le_lon_e7"]}, '
                f'{r["he_lat_e7"]}, {r["he_lon_e7"]}, {r["length_m"]}}},\n'
            )
        f.write("};\n\n")
        f.write(f"}}  // namespace data::{slug}\n\n#endif  // WITH_DISPLAY\n")
    return path


def build_category(category: str, airport_rows: list, runway_rows: list, out_dir: pathlib.Path):
    slug = f"{category}s"
    airports = select_airports(airport_rows, category)
    airport_idx = {a["ident"]: i for i, a in enumerate(airports)}
    runways = select_runways(runway_rows, airport_idx, category)

    hh = write_header(out_dir, slug, category, airports, runways)
    cpp = write_data(out_dir, slug, category, airports, runways)
    print(f"  {category}: {len(airports)} airports, {len(runways)} runways -> {hh.name}, {cpp.name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--category",
        nargs="+",
        default=["large_airport"],
        choices=VALID_CATEGORIES,
        metavar="TYPE",
        help="OurAirports type(s) to build. One or more of: " + ", ".join(VALID_CATEGORIES) + " (default: large_airport)",
    )
    parser.add_argument(
        "--out-dir",
        type=pathlib.Path,
        default=DEFAULT_OUT_DIR,
        help=f"Output directory for generated files (default: {DEFAULT_OUT_DIR}).",
    )
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    airport_rows = fetch_csv(AIRPORTS_URL)
    runway_rows = fetch_csv(RUNWAYS_URL)

    print(f"Generating datasets in {args.out_dir}:")
    for category in args.category:
        build_category(category, airport_rows, runway_rows, args.out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
