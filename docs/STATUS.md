# Status

Maintained by the lead. Read this first when resuming. Last updated: 2026-10-06 13:05 CDT.

## Blockers needing the owner
| # | Item | Detail |
|---|---|---|
| B1 | GitHub push | This machine has no `gh`, no GitHub credentials and no SSH keys. The repo is complete locally (`git log`); to publish: `gh auth login && gh repo create quartz-watchy --private --source=. --push` (or create an empty private repo in the web UI, then `git remote add origin <url> && git push -u origin main`). CI cannot have run on GitHub yet; `tools/check.sh --fw` is the local equivalent. |
| B2 | Physical-watch steps | Everything hardware-facing is UNVERIFIED until `docs/HARDWARE_BRINGUP.md` has been run. |
| B3 | Owner decisions | `docs/OPEN_QUESTIONS.md` (Q-01..Q-13; defaults implemented). Most important: Q-12 (panel partial-refresh waveform, P-11) and Q-13 (BMA423 blob provenance, P-12). |

## Milestones (docs/ROADMAP.md section 2)
| Milestone | State |
|---|---|
| Skeleton (build system, tooling, CI, stubs) | done |
| Planning (architecture, roadmap, research, headers) | done (docs/*.md, 37 headers compile standalone under strict C++23) |
| M1 host face (simulator renders a face PNG) | not started |
| M2 firmware builds (radio + offline variants) | skeleton builds; no real code yet |
| M3 face on the watch | needs owner bring-up |
| M4 a week in 10 s (virtual-time suite) | not started |
| M5 full feature | not started |

## Work packages
| WP | State |
|---|---|
| WP-01 core + fakes, 02 civil time + POSIX-TZ, 03 TZ table, 04 TimeKeeper | committed |
| WP-05 gfx + PNG, 06 fonts, 14 UI framework, 15 system screens + icons, 16 faces | committed |
| WP-07 SSD1681 driver + panel model, 08 BMA423 wrapper + fake | committed |
| WP-09 settings, 10 steps, 11 power, 12 weather, 13 connectivity logic | committed |
| WP-17 console protocol, 18 console catalog (50 commands), 20 app core (RTC store, tether, wake planner), 21 app wiring + DeviceApi | committed |
| WP-24 platform HAL basics, 25 platform sleep/EPD bus/I2C (IDF-only, build-verified only), 28 firmware config | committed |
| WP-23 face-only simulator (milestone M1) | committed (`--press/--scene/--console` still open: the app now exists, so they can be added) |
| Not started | WP-19 self-test + goldens (needs the host golden tool; `selftest` currently answers `unsupported`), WP-22 virtual-time simulation suite, WP-26 console port (esp_console over USB-Serial-JTAG), WP-27 radio (Wi-Fi/SNTP/HTTPS/provisioning portal, IDF-only), WP-28 `main/` wiring (construct platform + App, `BuildFeatures`), WP-29 qzctl + device tests, WP-30 skills/docs/release/power budget, WP-23 rest |

Full gate at 12:05 CDT: `tools/check.sh --fw` = 1348 host tests (ASan+UBSan), tidy, generated-file checks, both firmware variants, offline-symbol check: all green.
FIRST FLASH IS NOW POSSIBLE: `main/` wiring, the USB console port and the platform init exist (image 0.45 MiB, radio off at run time until WP-27, no self-test until WP-19). Follow docs/FIRST_FLASH.md. Nothing has run on hardware.

## Resume playbook (do this after every usage-window reset)
1. `get_usage`; proceed only if the 5-hour window is < 50% used.
2. (No agents are in flight.)
3. For every finished WP the lead reviews and commits alone: `QZ_BUILD_DIR=build/lead tools/host.sh configure`,
   build + run only that component's `qz_<c>_test`, `clang-format --dry-run --Werror` and `tools/tidy.sh <files>` on its files,
   `python3 tools/check_deps.py`, one `tools/fw.sh build` if firmware-linked, read the non-test source, then
   `git add components/qz_<c>` and commit (message: what, tests, "Reviewed: ..."). Other agents' half-written files make
   the repo-wide gate unreliable until they finish.
4. Next, after the owner's first-flash report (docs/FIRST_FLASH.md section 6): fix whatever bring-up finds, then WP-22 (virtual-time
   week suite, the M4 gate) -> WP-19 (selftest + goldens + host golden tool) -> WP-27 (radio; add the positive `nm` check) ->
   WP-23 rest -> WP-29 -> WP-30; an Opus review pass over wake_planner/sleep/app wiring if budget allows.
5. Pacing (measured): 4 parallel Sonnet agents burn ~2.7 % of the 5-hour window per minute (the first burst used 96 % in
   35 min and 13 % of the week). Run at most 3 agents, stop launching at ~85 %, and when the window is nearly spent let the
   running agents finish, then END the turn after scheduling the next wake-up with `CronCreate` (one-shot, local time,
   reset time + 6 min). Never idle in repeated waiting turns (overage cache TTL = 5 min -> ~$1 per turn).
6. Opus is only reachable via `Agent(model="opus")`; reserve it for reviews of the TZ engine, SSD1681 driver and wake planner.

## Budget and pacing (read before spawning agents)
- Plan: Pro (5-hour window, resets 03:40 CDT then every 5 h) + extra usage capped at $40/month. **$36.46 of the $40 is
  spent (91 %)**: ~$20 by agents with 150+ tool calls over big contexts while the window was exhausted, and ~$13.5 by my own
  idle waiting turns (overage drops the prompt-cache TTL to 5 min, so every 10-minute wait re-billed a ~450k-token context).
  Treat the remaining $3.54 as untouchable.
- Weekly usage was 41 % at 23:20 CDT on 2026-10-05 (resets 07:00 CDT on 2026-10-07).
- Agents must use <= 60 tool calls and read only their sections (AGENTS.md "Working efficiently"); WP-11 (42 calls) shows it works.

## Tech debt / follow-ups
- [TECH-DEBT] `FixedString(const char*)` documents truncation as a programmer error but clears silently (WP-01).
- [TECH-DEBT] `QZ_LOGW` ignores `QZ_LOG_MAX_LEVEL`; there is no `QZ_LOGV` (WP-01).
- [TECH-DEBT] Header contract vs panel vendor guidance (P-05) and the partial-refresh waveform (P-11) are open until bring-up experiment E1.
- [TECH-DEBT] BMA423 blob authenticity (P-12/Q-13).
- [TECH-DEBT] main/console (WP-26/28): qz_net is a stub (both factories return nullptr) until WP-27; `IdfI2cDevice` init failure is non-fatal (logged); the git hash is stale until CMake reconfigures; the console log hook truncates at 256 B per line; the init-failure retry sleeps before any state check. [ASSUMED] esptool hard-reset over USB-Serial/JTAG, `idf.py monitor` forwarding typed lines, uninstalling the console driver leaves USB sane, 8 KiB main stack is enough.
- [TECH-DEBT] App (WP-21): selftest hooks are weak declarations until WP-19; Critical does not suspend the BMA423; `diag sensors` has no raw accel; the provisioning form path (`Core::apply`) is untested (private); BMA423 axis remap is still the default.
- [TECH-DEBT] Platform (WP-25): EpdBus 3-wire read is unimplemented (`read()` -> kUnsupported; needed for bring-up experiment E1); `CONFIG_QZ_USB_WAKE` must be mapped into `plan.wake_on_usb` by main; `IdfSleep::release_holds()` or `IdfEpdBus::init()` must run at every boot (WP-28).
- [TECH-DEBT] Platform (WP-25) [ASSUMED] until bring-up: B3 I2C; B4 GPIO0 held on wake boots normally, vibration hold works on the RTC pad, EXT1 holds are released after wake; B5 SPI idle levels in light sleep, reset timing, BUSY wake; B8 multi-stage Unity resume; B9 sleep floor incl. IDF isolating unheld pads.
- [TECH-DEBT] System screens (WP-15): Diagnostics "Sensors" page shows only what `WatchState` carries (no raw accel/temperature); `recent_wakes` order assumed oldest-first and the provisioning URL assumed `http://192.168.4.1` [ASSUMED]; screen sources live in `qz_ui/src/` (CMake glob is non-recursive).
- [TECH-DEBT] Platform (WP-24): RTC region sizes (2048 B state, 5024 B frame) are hard-coded in `qz_platform/src/platform_impl.hpp` but WP-20 pinned `RtcState` = 1024 B and `FrameShadow` = 5008 B (6032 B total, budget 7680). Align them and add a `static_assert` against `sizeof(RtcState)`/`sizeof(FrameShadow)` in `main/` (WP-28/21). With the current numbers `.rtc_noinit` is 7072 B of the 8 KiB RTC slow memory.
- [TECH-DEBT] Platform (WP-24): `QZ_GIT_HASH` is empty until WP-28 injects it; vibration polarity (active-high), "wake pins carry no pad hold" and RTC-time continuity across `esp_restart` are [ASSUMED] until bring-up.
- [TECH-DEBT] UI (WP-14): optional click vibration (15 ms) not implemented (needs a decision on the `vibration` setting; an extra action would break the one-action-per-save rule); add a UI-level zero-allocation test in the sim.
- [TECH-DEBT] UI (WP-14): idle timeouts for Provisioning (5 min), SyncNow (60 s), ChargeMe (2 s) and the 3 s MENU hold for FactoryReset are [ASSUMED]; no menu row opens the sync-interval choice yet.
- [TECH-DEBT] Console (WP-18): WP-21 must override DeviceApi::screen_count/screen_name_at/wifi_has_password.
- [TECH-DEBT] TZ table (WP-03): 139 entries = ~9 KB (the 6 KiB goal is unreachable with `TzEntry`; ceiling is now 10 KiB). POSIX footers only apply after a zone's last tabulated transition, so Casablanca (until 2026-09-20) and Edmonton/Vancouver/Winnipeg (until 2026-11-01) are WRONG until then, and Gaza's Ramadan shifts are ignored; Chicago/Berlin/Sydney-style zones are unaffected. Consider flagging these in the zone picker.
- [TECH-DEBT] SSD1681: outside 0-50 C the controller refuses an update but BUSY still falls; the app must gate on `temperature_dc()` (WP-07).
- [TECH-DEBT] SSD1681: 0x1B/0x2D read framing (no dummy byte) is [ASSUMED]; platform must set 10 MHz SPI and the reset timing (WP-25) (WP-07).
- [TECH-DEBT] BMA423: step-counter byte order and low-power counter read are [ASSUMED] until bring-up B7; I2C HAL must accept 64-byte writes (WP-25) (WP-08).
- [TECH-DEBT] WP-26: the console dispatcher needs a line buffer of >= 257 bytes (WP-17).
- [TECH-DEBT] qz_power: curve/thresholds unmeasured [TUNE B6]; EWMA not re-seeded on USB plug/unplug (WP-11).

## Environment notes
- ESP-IDF v6.1 in `.toolchain/` (bootstrap: `tools/bootstrap.sh`); dev venv in `.venv/` (`tools/setup-dev.sh`).
- The machine hard-rebooted once (nvidia-drm pageflip timeout, unrelated to builds); a btrfs crash artifact truncated one FetchContent checkout - `rm -rf build/` fixes any such corruption.
- Parallel agents need private build dirs: `QZ_BUILD_DIR=build/<name>`, `QZ_FW_BUILD_DIR=build/<name>-fw`.
