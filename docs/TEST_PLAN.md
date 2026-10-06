# Test plan

## Contents
1. [Layers and commands](#1-layers-and-commands)
2. [Requirement matrix](#2-requirement-matrix)
3. [Gates](#3-gates)

## 1. Layers and commands

| Id | Layer | Runs where | Command |
|---|---|---|---|
| U | Host unit tests (GoogleTest, ASan+UBSan) | Linux, CI | `tools/host.sh all` (preset `default`); coverage: `tools/host.sh all coverage` |
| G | Golden images (exact PNG compare, CRC table check) | Linux, CI | part of `tools/host.sh test` (`qz_selftest_test`); update: `host/build/.../qz_golden --update` then review PNG diffs |
| V | Virtual-time simulation (days/weeks of App wakes) | Linux, CI | part of `tools/host.sh test` (`qz_app_test --gtest_filter='Sim*'`) |
| O | glibc TZ oracle | Linux, CI | part of `tools/host.sh test` (`qz_time_test --gtest_filter='*Oracle*'`) |
| C | Static checks: format, tidy, include-layering, generated files current, offline `nm` check, size budget | CI | `tools/check.sh` (lead), `tools/format.sh --check`, `tools/tidy.sh`, `tools/fw.sh size` |
| T | On-target Unity apps + pytest-embedded | owner's watch over USB | `pytest test_apps/<app> --target esp32s3 --port /dev/ttyACM0` (after `tools/fw.sh` builds them) |
| S | Device self-test | watch, via console or menu | `tools/qzctl run "selftest run all"` / Menu > Diagnostics > Self-test |
| M | Manual hardware procedure | owner | `docs/HARDWARE_BRINGUP.md` step Bn |

No hardware result counts until the owner reports it (HARDWARE_BRINGUP.md "Reporting").

## 2. Requirement matrix

| # | Requirement (SPEC) | U | G | V | T | S | M | Notes / main test |
|---|---|---|---|---|---|---|---|---|
| 1 | Hardware-independent logic builds and passes on host | x | | | | | | CI job `host` |
| 2 | Time 12/24h, date on face | x | x | | | x | B5 | face scenes `face_24h`, `face_12h_pm` |
| 3 | Today's steps + optional goal on face | x | x | x | | x | B7 | scenes with/without goal |
| 4 | Battery indicator, % from curve | x | x | x | | x | B6 | `power` curve tests |
| 5 | Weather when enabled + fresh; stale marked/hidden | x | x | x | | x | B11 | freshness boundaries, scenes fresh/stale/hidden |
| 6 | Sync status indicator | x | x | x | | x | | every SyncIndicator scene |
| 7 | Partial refresh each minute, periodic full, no flicker at boundary | x | | x | | x | B5 | FakeEpdPanel update log; sim asserts minute alignment |
| 8 | BMA423 hardware step counter | x | | | x | x | B7 | FakeBma423 + Unity `test_apps/drivers` |
| 9 | Axis remap | x | | | | | B7 | remap config test; on-wrist check |
| 10 | Daily reset at local midnight, DST + time jumps | x | | x | | | | ARCH section 10 table rows |
| 11 | 7-day history in RTC, NVS flush <= 1/day | x | | x | x | | | sim counts NVS writes; RTC persistence Unity test |
| 12 | POSIX TZ from built-in list | x | | | | x | | O + table tests |
| 13 | Manual time/date via buttons, radio off | x | x | x | | | B5 | editor tests + sim with `Off` |
| 14 | SNTP sync, configurable interval | x | | x | x | | B11 | scheduler tests; device sync |
| 15 | RTC drift measured + compensated | x | | x | | | B8 | VirtualClock crystal error; 24 h drift check |
| 16 | Weather: temp, icon, hi/lo, C/F | x | x | | | | B11 | parser fixtures, unit conversions |
| 17 | Manual lat/lon, no geolocation | x | x | | | | | location editor; URL builder |
| 18 | Fetch interval, skipped on low battery | x | | x | | | | scheduler preconditions |
| 19 | HTTPS + cert validation, hard timeouts, failure never blanks face | x | | x | x | | B11 | session tests; pytest with bad DNS |
| 20 | Modes Off / Time only / Time+Weather | x | | x | | | B9 | |
| 21 | Off means off (driver never initialized) | x | | x | | x | B9 | `radio_init_count()==0` over 7 sim days; `diag radio`; current |
| 22 | Kconfig compile-out of radio | | | | | | | C: offline build + `nm` has no `esp_wifi_init` |
| 23 | Provisioning (SoftAP page, time-limited, password) + USB console alternative | x | x | | x | | B12 | form parser, expiry; pytest provisioning flow |
| 24 | Exponential backoff, never retry every wake | x | | x | | | | backoff sequence test; sim with failures |
| 25 | Credentials never logged/printed | x | | | x | | | log-capture tests; `FakeKvStore::contains_text`; pytest greps output |
| 26 | Factory reset via menu and console | x | | x | x | | | sim + pytest `factory-reset confirm` |
| 27 | Power measured per mode; budget in repo; regressions block merge | | | x | | | B9 | sim awake-time budget (CI); B9 current numbers -> `docs/POWER_BUDGET.md` |
| 28 | Low battery: radio off, then reduced refresh; critical screen + button-only wake | x | x | x | | | B6 | power policy tables; sim drain scenario |
| 29 | Menu items (time/date ... about) | x | x | | | x | B5 | navigation graph tests; scenes per screen |
| 30 | Optional vibration feedback | x | | | | x | B4 | `vibrate` command, interactive self-test |
| 31 | Console: set time, inject buttons, fake steps/weather/battery, settings, sync, framebuffer dump, logs, diagnostics | x | | | x | | B10 | catalog tests + `test_apps/console` pytest (every command) |
| 32 | Console only with USB; never keeps watch awake on battery | x | | x | x | | B10 | TetherPolicy invariant test; current check on battery |
| 33 | Host tooling survives CDC disappearing | x | | | x | | B10 | qzctl reconnect tests vs simulator; `test_apps/wake` `sleep 5` |
| 34 | Self-test covers every driver, screen, setting, console command | x | x | | | x | B10 | registry-introspection coverage tests |
| 35 | Golden image for every screen | | x | | | x | | `ScreenId` coverage test; on-device CRC compare |
| 36 | Deep-sleep wake on timer, buttons, accelerometer | x | | x | x | | B4, B9 | planner tests; `test_apps/wake` |
| 37 | Time-awake tracked per wake | x | | x | | | | wake-record tests |
| 38 | Partition table: 2 app slots + rollback bootloader | | | | | | B1 | C: partition table check in CI |
| 39 | Version + git hash embedded, About screen | x | x | | x | | | `version` command; About scene uses fixture strings |
| 40 | Recovery via UP download mode | | | | | | B1 | documented procedure exercised once |
| 41 | Simulator renders any screen to PNG with scripted inputs | x | | | | | | `host/sim` CTest cases |
| 42 | Code style enforced in CI | | | | | | | C |

## 3. Gates

- Merge gate (CI): U, G, V, O, C all green; size budget not exceeded; sim awake-time budget for a
  week not exceeded (regression guard until B9 numbers exist).
- Release gate: merge gate + owner-run T and S suites on a real watch + B9 power baseline within
  10 % of the recorded budget (`docs/POWER_BUDGET.md`).
