# Fork Notes

This repository is a **maintained fork** of ADSBee 1090. Its reason to exist: provide an
easy-to-build firmware image **with optional GC9A01 round-LCD radar support**, while staying easy
to keep in sync with upstream.

| | |
|---|---|
| `origin` | `git@github.com:then3rd/adsbee.git` (this fork) |
| `upstream` | `https://github.com/CoolNamesAllTaken/adsbee.git` (the project this forks) |
| Feature branch | `GC9A01-radar` |
| License | GPL-3.0-only (see `LICENSE`) |

The LCD work lives entirely in this fork and is **not** proposed upstream at this time.

## What this fork adds on top of upstream

- **GC9A01 round-LCD radar display** (the headline feature) — `firmware/adsbee_1090/esp/main/peripherals/display/`.
- Simulation / synthetic-aircraft mode — `esp/main/simulation.{cpp,hh}`.
- A `justfile` and several `firmware/scripts/*` dev helpers.
- Assorted CI / packaging tweaks.

All display code is compiled out by default and gated behind `WITH_DISPLAY` (see below), so a
default build stays byte-comparable to upstream and the fork's delta is small and localized.

## Building with LCD support

Display support is **OFF by default**. Enable it at build time:

```bash
# via build.sh
bash firmware/adsbee_1090/build.sh --display        # one-off
export WITH_DISPLAY=1                                # or persistently
bash firmware/adsbee_1090/build.sh --no-display      # force off even if env is set

# via just
just display=true build
```

`WITH_DISPLAY` is threaded through as a CMake `ON/OFF` cache variable (`-D WITH_DISPLAY=...`) to
every target. When OFF, no display code and no LovyanGFX is linked.

> **Reflash caveat:** toggling `--display` changes the ESP32 binary but **not** the firmware
> version, so the RP2040 won't automatically reflash the ESP32 on boot. Bump the firmware version
> (see below) or force a reflash (`just force=true ...`) when switching the display on or off on
> real hardware. Symptom of forgetting: old ESP32 behavior persists after flashing.

## Syncing with upstream (rebase workflow)

This fork keeps a **linear history rebased on top of upstream**, so the fork's commits read as a
clean "here's what I added" stack.

```bash
git fetch upstream
git switch GC9A01-radar
git rebase upstream/main
# resolve any conflicts (see the expected surface below), then:
git push --force-with-lease origin GC9A01-radar
```

The force-push is expected and normal here: rebasing rewrites the branch tip, and this is a
solo-maintained branch. Always use `--force-with-lease` (never bare `--force`) so a surprise remote
change aborts the push instead of being clobbered.

A convenience wrapper is provided: `bash firmware/scripts/sync_upstream.sh` (fetches, shows the
incoming commit range, and runs the rebase; it leaves the force-push for you to run manually).

### Expected conflict surface

New files this fork adds — `esp/main/peripherals/display/*`, `esp/main/simulation.*`, `justfile`,
`firmware/scripts/*` — do **not** conflict on rebase (upstream has no such files, so a rebase just
re-adds them). Conflicts only arise in the *existing upstream files* this fork edits. Display-related
edits are all `#ifdef WITH_DISPLAY`-guarded, so conflicts are usually trivial context clashes:

- Build plumbing: `firmware/adsbee_1090/build.sh`,
  `esp/CMakeLists.txt`, `esp/main/CMakeLists.txt`, `pico/CMakeLists.txt`, `esp/main/idf_component.yml`
- ESP32 integration: `esp/main/app_main.cpp`, `esp/main/bsp.hh`, `esp/main/comms/comms_ethernet.cpp`
- RP2040 AT commands: `pico/application/comms/at/comms_at.cc`, `pico/application/comms/comms.hh`
- Versioning: `common/coprocessor/object_dictionary.cpp` (version constants — conflicts on nearly
  every upstream bump; resolve by re-applying your bump on top of theirs)
- Settings: `common/settings/settings.hh` (`kSettingsVersion` + display fields), `common/settings/settings.cpp`

## Intentional design decisions (do not "fix" these during a rebase)

- **`display_rotation_deg` / `display_range_km` are unguarded in the shared `Settings` struct on
  purpose.** They are *not* wrapped in `#ifdef WITH_DISPLAY`. The `Settings` struct is mirrored
  byte-for-byte over SPI between the ESP32 and RP2040; guarding the fields would change the struct
  layout when the two processors are built with mismatched `WITH_DISPLAY` flags and desync the link.
  Only the `AT+DISPLAY_*` command handlers and the settings-dump/print lines are guarded.
- **Display and the W5500 Ethernet add-on are mutually exclusive.** They share the aux SPI3 bus and
  GPIO47/48, so `esp/main/comms/comms_ethernet.cpp` disables Ethernet init when `WITH_DISPLAY` is
  built. Don't enable both.
- **Version-bump discipline.** Any change to ESP32/CC1312 code bumps the firmware version, and any
  change to the `Settings` struct bumps `kSettingsVersion` **and** the firmware version, committed
  together. This is enforced by `firmware/scripts/check_version_sync.sh` (CI + local git hook). See
  `firmware/CLAUDE.md` → "Critical: Version Management".

## Licensing

The fork is GPL-3.0-only. The radar rendering (`peripherals/display/radar_view.*` and the
`lgfx_config.hpp` panel setup) ports code from
[ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) (MIT, GPL-compatible), and
LovyanGFX (BSD-2-Clause) is pulled in as an ESP-IDF component. All third-party code is attributed in
`THIRD_PARTY_NOTICES.md` and in inline file headers. Keep those current if more third-party code is
added — this is exactly the licensing hygiene that keeps the work self-contained in the fork.
