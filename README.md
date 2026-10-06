# Quartz

Production-grade custom firmware for the [SQFMI Watchy v3](https://watchy.sqfmi.com/) (ESP32-S3, 200x200
e-paper), written in C++23 on ESP-IDF v6.1. Goals: a watch you can wear every day - correct time
(own drift-compensated clock and POSIX time-zone engine, minute flips on the boundary), step
counting, weeks of battery, an optional Wi-Fi time/weather sync, a USB console for automation, and
everything above the hardware layer buildable and testable on a Linux host.

## Status (honest)
Version 0.1.0 (`version.txt`), `docs/STATUS.md` has the details.
- **Verified on the host:** all pure logic (time, zones, settings, steps, power policy, UI, faces,
  console protocol and 50 commands, app wiring, a virtual-time "week in seconds" suite) passes 1348
  tests under ASan+UBSan, clang-tidy, formatting and a component-dependency contract; 61 golden images
  match byte for byte; both firmware variants (radio, offline) build warning-free.
- **Unverified on hardware:** nothing has ever run on a real watch. Panel waveforms, deep-sleep current,
  button/USB wake, the BMA423 step counter, battery curve, USB console behaviour and the whole radio
  stack are `[ASSUMED]` until the owner completes `docs/HARDWARE_BRINGUP.md`. Battery-life figures in
  `docs/POWER_BUDGET.md` are a model, not a measurement.
- CI: GitHub Actions (`.github/workflows/ci.yml`) runs the same gates in the `espressif/idf:v6.1` container; it was green on 2026-10-06. `tools/check.sh --fw` is the local equivalent.

## Quick start (Linux)
```bash
tools/bootstrap.sh            # once: pinned ESP-IDF (.idf-version) into .toolchain/ (git-ignored)
tools/setup-dev.sh            # once: .venv with pinned clang-format / clang-tidy / pytest / pyserial
tools/check.sh --fast         # the gate without clang-tidy: contract, format, host tests (ASan+UBSan)
tools/check.sh --fw           # full gate + both firmware builds (what a release runs)
```
Render a watch face to a PNG with the host simulator (no device needed):
```bash
tools/host.sh build
build/host/sim/qz_sim --face minimal --time 2026-10-06T08:15:00 --tz America/Chicago \
  --steps 7421 --goal 10000 --battery 76 --weather 'temp_c=18,code=61,age_min=20' --scale 2 --out face.png
```
Firmware: `tools/fw.sh build` (radio) and `QZ_FW_VARIANT=offline tools/fw.sh build` (no Wi-Fi code).
**Flashing a real watch: follow `docs/FIRST_FLASH.md`** (download-mode chord, offline image first,
console commands to try, what to report). Never flash from an agent session.

## Repository map
| Path | What |
|---|---|
| `components/qz_*` | ESP-IDF components; pure ones (`core hal model time settings steps power weather conn gfx ui faces console selftest app ssd1681 bma423 board testkit`) build on the host; `qz_platform`, `qz_net` and `main/` are the only IDF-specific code |
| `host/` | host CMake project: unit tests (GoogleTest), `sim/` simulator `qz_sim`, `golden/` golden-image tool `qz_golden` |
| `tools/` | `check.sh` gate, `host.sh`, `fw.sh`, `format.sh`, `tidy.sh`, `check_deps.py`, `release.sh`, generators (`fontgen.py`, `tzgen.py`) |
| `docs/` | specification, architecture, decisions, research, bring-up and release procedures |
| `.claude/skills/` | agent skills: build/flash/monitor, add a face, add a console command, power measurement, release |
| `test_apps/` | on-device test firmware (`net`, `platform`, `sleep`) |
| `partitions.csv`, `sdkconfig.defaults*` | 8 MB flash layout, firmware configuration (radio and offline) |

## Documents
- Brief and requirements: `docs/SPEC.md`; architecture and coding conventions: `docs/ARCHITECTURE.md`
  (read section 2 first); component contract: `docs/COMPONENTS.md` (enforced by `tools/check_deps.py`).
- Plans and decisions: `docs/ROADMAP.md`, `docs/DECISIONS.md`, `docs/OPEN_QUESTIONS.md`, `docs/PUSHBACK.md`, `docs/adr/`.
- Hardware facts with sources: `docs/research/*.md`; partitions `docs/PARTITIONS.md`; sdkconfig `docs/SDKCONFIG.md`.
- Testing: `docs/TEST_PLAN.md`. Power: `docs/POWER_BUDGET.md`.
- Owner procedures needing the physical watch: `docs/FIRST_FLASH.md`, `docs/HARDWARE_BRINGUP.md`.
- Releases: `docs/RELEASE.md`, `CHANGELOG.md`. Current state and tech debt: `docs/STATUS.md`.
- Contributors and agents: `AGENTS.md` (ground rules, commands, skills).

## Recovery in one line
Hold BACK + UP for more than 4 s, release BACK first, then UP: the ROM bootloader appears on USB
(`/dev/ttyACM*`) and `tools/fw.sh -p $P erase-flash` plus a flash always recovers the watch.
