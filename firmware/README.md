# ads-bee Firmware

## Prerequisites

### Docker

Install Docker via the official apt repository — **do not use the snap package**, as it has socket permission issues.

### Git Submodules

```bash
git submodule update --init --recursive
```

> **Windows note:** Cloning on Windows may produce files with CR+LF line endings, which break Docker builds. Run `git config --global core.autocrlf false` before cloning.

### Developer Setup

Bootstrap a fresh clone (submodules + git hooks) with:

```bash
bash firmware/scripts/setup_dev.sh
```

This initializes submodules and installs a native git pre-commit hook (no extra tooling) that enforces the [version-management rule](AGENTS.md) on every commit. It does not install the build toolchain (Docker/ESP-IDF/TI SDK).

---

## Build System Overview

The build system uses Docker Compose with three pre-built containers — no local image build is required. Images are pulled automatically on first run.

| Service | Image | Builds |
|---------|-------|--------|
| `pico-docker` | `coolnamesalltaken/pico-docker:latest` | RP2040 firmware, host tests |
| `esp-idf` | `espressif/idf:v5.5.2` | ESP32-S3 firmware |
| `ti-lpf2` | `coolnamesalltaken/ti-lpf2:latest` | TI CC1312 firmware |

The compose file lives at `firmware/adsbee_1090/compose.yml`. The build script handles the required build order (ESP32 → CC1312 → RP2040) automatically.

### Generated boot-splash image

The GC9A01 round display shows the ADSBee logo (`images/adsbee_logo.png`) for a few seconds on boot. The image is compiled into the ESP32 firmware as an RGB565 C array in `esp/main/peripherals/display/splash_data.{hh,cpp}`.

These two files are **generated build artifacts** — they are gitignored and regenerated automatically on every ESP32 build by `firmware/scripts/gen_splash_image.py` (invoked from `build.sh`). Generation runs on the **host** (the ESP-IDF container has no Pillow), so it requires Pillow:

```bash
just flash-deps        # installs Pillow (and other Python deps) into .venv
# or: pip install Pillow
```

To change how the logo is framed, edit the `ZOOM` constant near the top of `gen_splash_image.py` (higher = bee larger / more cropped, lower = smaller / more of the logo visible) and rebuild. No manual generation step is needed — the build picks up PNG or `ZOOM` changes automatically.

### Airport / runway overlay datasets

The radar view can draw runway lines and airport idents underneath the aircraft symbols. That overlay is fed by embedded datasets generated from [OurAirports](https://ourairports.com/data/) (public-domain) data. Coordinates are stored as `int32` degrees × 1e7 in `.rodata` (memory-mapped flash), so a dataset costs flash, not RAM.

Unlike the boot splash, these datasets are **committed to the repo** (not gitignored, not regenerated on every build) — regenerate them only when you want fresh data or a new category. The generator is `firmware/scripts/build_airports.py` (stdlib only, no extra deps):

```bash
python3 firmware/scripts/build_airports.py                              # large_airport (default)
python3 firmware/scripts/build_airports.py --category medium_airport
python3 firmware/scripts/build_airports.py --category large_airport heliport
```

`--category` accepts one or more OurAirports `type` values: `large_airport`, `medium_airport`, `small_airport`, `heliport`, `seaplane_base`, `balloonport`, `closed`. Each category produces its own pair of files in `esp/main/peripherals/display/` — `<slug>.hh` and `<slug>_data.cpp` (e.g. `medium_airports.{hh,cpp}`) — in namespace `data::<slug>`, so multiple categories can coexist. Shared POD types (`Airport`, `Runway`, `AirportDataset`) live in `airport_data.hh`.

**Which categories actually render** is controlled by the `kEnabledDatasets[]` table near the top of `esp/main/peripherals/display/radar_view.cpp`. By default only `large_airports` is enabled. To add a category: generate it with the script, then add its header include plus a `&data::<slug>::kDataset` entry to that table. Datasets not listed in the table are dropped from the image by `--gc-sections`, so generating extra files costs nothing until you enable them.

> **Regenerate CMake after adding a new dataset file.** ESP-IDF globs `SRC_DIRS` at CMake *configure* time, so a plain incremental build won't compile a newly generated `<slug>_data.cpp` — the link fails with `undefined reference to data::<slug>::kAirports`. Force a reconfigure first: `touch esp/main/CMakeLists.txt` (or `build.sh clean esp`), then `build.sh esp`. Editing an already-tracked dataset in place does not need this.

> **Flash footprint:** `large_airport` is ~55 KB. `medium_airport` is roughly 3× the airports and `small_airport` far more — check the ESP32 partition headroom (`build.sh esp` prints free space) before enabling the larger tiers.

---

## Building Firmware

Run from the repo root:

```bash
bash firmware/adsbee_1090/build.sh [options] [target]
```

### Targets

| Target | Description |
|--------|-------------|
| `all` (default) | Build all three firmware targets in the correct order |
| `esp` | ESP32-S3 only |
| `ti` | TI CC1312 only |
| `pico` | RP2040 only (requires ESP32 and CC1312 builds to exist first) |
| `test [filter]` | Build and run host unit tests; `filter` is an optional ctest `-R` regex (e.g. `AircraftJSON`) |
| `clean` | Delete all build output directories |

### Options

| Flag | Description |
|------|-------------|
| `-d` | Build in Debug mode (default is Release) |

### Examples

```bash
bash firmware/adsbee_1090/build.sh            # full release build
bash firmware/adsbee_1090/build.sh -d         # full debug build
bash firmware/adsbee_1090/build.sh esp        # ESP32 only
bash firmware/adsbee_1090/build.sh test       # run all host tests
bash firmware/adsbee_1090/build.sh test AircraftJSON  # run filtered tests
```

### Build Output

| Target | Output |
|--------|--------|
| RP2040 (Release) | `firmware/adsbee_1090/pico/build/Release/application/combined.uf2` |
| RP2040 (Debug) | `firmware/adsbee_1090/pico/build/Debug/application/combined.uf2` |
| ESP32 (Release) | `firmware/adsbee_1090/esp/build/Release/adsbee_esp.bin` |
| CC1312 | `firmware/adsbee_1090/ti/sub_ghz_radio/build/sub_ghz_radio.bin` |

The `combined.uf2` embeds all three binaries. See [Developers_Guide.md](adsbee_1090/Developers_Guide.md) for flashing instructions.

---

## Interactive Shell

To open a shell inside a container for debugging or manual builds:

```bash
cd firmware/adsbee_1090
docker compose run --rm pico-docker bash   # RP2040 / test container
docker compose run --rm esp-idf bash       # ESP32 container
docker compose run --rm ti-lpf2 bash       # CC1312 container
```

---

## VS Code Integration

1. Install the **Dev Containers** VS Code extension.
2. Start the target container: `cd firmware/adsbee_1090 && docker compose run --rm pico-docker bash`
3. In VS Code, open the Remote Explorer panel, right-click the running container, and select **Attach to Container**.
4. Inside the attached VS Code, install the **Cortex-Debug** and **C/C++** extensions.
5. Use **Open Folder** to navigate to `/firmware/adsbee_1090` to pick up the `.vscode/launch.json` debug configuration.

---

## Removing Docker Images

```bash
docker image rm coolnamesalltaken/pico-docker:latest
docker image rm espressif/idf:v5.5.2
docker image rm coolnamesalltaken/ti-lpf2:latest
```
