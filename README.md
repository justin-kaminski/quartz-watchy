# Quartz

Production-grade custom firmware for the [SQFMI Watchy v3](https://watchy.sqfmi.com/) (ESP32-S3, 200x200
e-paper), written in C++23 on ESP-IDF v6.1. Goals: a watch you can wear every day - correct time
(own drift-compensated clock and POSIX time-zone engine, minute flips on the boundary), step
counting, weeks of battery, optional Wi-Fi and Bluetooth phone sync that cost nothing when unused, a
USB console for automation, and everything above the hardware layer buildable and testable on a
Linux host.

## Features
- **Deep sleep between minute ticks.** The watch wakes once a minute for a partial panel update and
  sleeps the rest of the time; buttons, USB attach and (optionally) a wrist tap wake it.
- **Seven watch faces:** Default, Minimal, Analog, Stacked, Words (time in words), Dashboard
  (seven-day step chart) and Day progress (a ring that fills over the day, with sunrise and sunset
  from your location). Choose one in Menu > Watch face, over the console or from the phone page.
- **Time:** own time keeper with drift compensation, a built-in table of 139 time zones with DST,
  12/24 h. Set it in the menu, over USB, over Wi-Fi (SNTP) or from your phone.
- **Steps:** BMA423 step counter, daily goal, seven-day history.
- **Battery policy:** Normal / Low / Saver / Critical levels that switch radios, tap wake and
  vibration off as the cell drains, plus a "charge me" screen.
- **Weather:** Open-Meteo forecast fetched over Wi-Fi, or pushed by the phone page (no Wi-Fi needed).
- **Phone sync over Bluetooth LE** (below), Wi-Fi setup through a temporary access point and a web
  form, and a USB console with 51 commands for scripting and agents (`tools/qzctl`).
- **Self-test and diagnostics** on the watch (Menu > Diagnostics) and over the console.

## Phone sync (Bluetooth LE)
Open **https://justin-kaminski.github.io/quartz-watchy/** in Chrome on Android (or desktop Chrome)
and connect to the watch. No app to install: the page uses Web Bluetooth.

1. On the watch: **Menu > Phone > Sync with phone**. It shows `Quartz-XXXX` and waits.
2. On the page: **Connect**, pick `Quartz-XXXX`. The first time, the phone asks for a code: type the
   six digits the watch shows. The phone is remembered after that.
3. The page sets the watch's time and time zone from the phone, and pushes the current weather it
   fetched from Open-Meteo (tap "Use this phone's location" once). You can also change settings and
   the watch face, and store Wi-Fi credentials.
4. Tap **Done**. The watch shows "Sync complete" and turns Bluetooth off.

What to expect:
- **It only runs when you start it.** There is no background syncing and nothing advertises on a
  schedule, so leaving the feature enabled costs no battery. A session ends on its own when the
  phone leaves, after 2 minutes without a phone, after 2 minutes without activity, or after
  15 minutes at most; the Bluetooth stack is fully powered down afterwards.
- **Weather is a snapshot.** A pushed report shows as fresh for two weather intervals (2 h by
  default), is marked stale until 6 h, then disappears.
- **Security:** pairing uses LE Secure Connections with a random passkey shown only on the e-paper;
  every command needs the encrypted, authenticated link. Commands that restart, erase or block the
  watch (`reboot`, `sleep`, `factory-reset`, `selftest run`, Wi-Fi sync and provisioning) are
  refused over Bluetooth and stay on the USB console.
- **Off means off:** Menu > Phone > Phone sync: off (Bluetooth is then never powered), or build
  without it (`CONFIG_QZ_PHONE=n`; the offline image has no Bluetooth code at all). Menu > Phone >
  Forget phones, and factory reset, erase all pairings.
- Linux without a phone: `tools/phone_link_test.py` pairs and exercises the link over BlueZ.

Details: `docs/ARCHITECTURE.md` section 13a, `web/phone/README.md` (hosting and local testing).

## Status (honest)
Version 0.1.0 (`version.txt`); `docs/STATUS.md` has the details.
- **Verified on the host:** all pure logic (time, zones, settings, steps, power policy, UI, faces,
  console protocol and commands, phone-sync sessions, app wiring, a virtual-time "week in seconds"
  suite) passes 1659 tests under ASan+UBSan, clang-tidy, formatting and a component-dependency
  contract; 110 golden images match byte for byte; both firmware variants build warning-free.
- **Verified on the owner's watch (2026-10):** boot, panel full and partial refresh, the driver
  self-test, BMA423 step counting, the 32 kHz crystal, battery reading, deep sleep with per-minute
  updates on battery, button wake and taps during refreshes, USB console, and Bluetooth phone sync
  (passkey pairing, commands, bonded reconnect, repeated sessions) from a Linux PC.
- **Still unverified:** phone sync from an Android phone with the page, the Wi-Fi stack (SNTP,
  weather fetch, provisioning page), deep-sleep and radio current (`docs/POWER_BUDGET.md` is a
  model, not a measurement). `docs/HARDWARE_BRINGUP.md` lists the remaining steps.
- CI: GitHub Actions (`.github/workflows/ci.yml`) runs the same gates in the `espressif/idf:v6.1`
  container; `tools/check.sh --fw` is the local equivalent. `.github/workflows/pages.yml` publishes
  the phone page.

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
build/host/sim/qz_sim --face analog --time 2026-10-06T08:15:00 --tz America/Chicago \
  --steps 7421 --goal 10000 --battery 76 --weather 'temp_c=18,code=61,age_min=20' --scale 2 --out face.png
```
Faces: `default`, `minimal`, `analog`, `stacked`, `words`, `dashboard`, `progress`.

Firmware images:
- `tools/fw.sh build`: the **radio image** (Wi-Fi + Bluetooth phone sync).
- `QZ_FW_VARIANT=offline tools/fw.sh build`: the **offline image** (no Wi-Fi or Bluetooth code;
  `tools/check_offline.sh` proves it).

**Flashing a real watch: follow `docs/FIRST_FLASH.md`** (download-mode chord, which image to start
with, console commands to try, what to report).

## Repository map
| Path | What |
|---|---|
| `components/qz_*` | ESP-IDF components; pure ones (`core hal model time settings steps power weather conn gfx ui faces console selftest app ssd1681 bma423 board testkit`) build on the host; `qz_platform`, `qz_net` (Wi-Fi, provisioning, NimBLE phone link) and `main/` are the only IDF-specific code |
| `host/` | host CMake project: unit tests (GoogleTest), `sim/` simulator `qz_sim`, `golden/` golden-image tool `qz_golden` |
| `web/phone/` | phone-sync companion page (one self-contained HTML file, Web Bluetooth) |
| `tools/` | `check.sh` gate, `host.sh`, `fw.sh`, `format.sh`, `tidy.sh`, `check_deps.py`, `check_offline.sh`, `release.sh`, `qzctl` (USB console client), `phone_link_test.py`, generators (`fontgen.py`, `tzgen.py`) |
| `docs/` | specification, architecture, decisions, research, bring-up and release procedures |
| `.claude/skills/` | agent skills: build/flash/monitor, add a face, add a console command, power measurement, release |
| `test_apps/` | on-device test firmware (`console`, `net`, `platform`, `sleep`, `wake`) |
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
