# Status

Maintained by the lead. Read this first when resuming. Last updated: 2026-10-06 16:00 CDT.

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

## First hardware run (2026-10-06, offline image, owner's Watchy v3)
Flashed via USB-Serial/JTAG (download mode reached with the BACK+UP chord; esptool auto-reset caught the chip).
Quartz boots, enters tethered mode and answers the console. Driver self-test on the real watch: **11 pass, 0 fail,
1 skip** (panel temperature read, deliberate): display init / full refresh 2070 ms / partial refresh 483 ms (the OTP
mode-2 waveform exists, P-11 risk reduced; visual quality still to be confirmed), BMA423 chip id 0x13 + config blob
loaded (P-12 blob works functionally), 32 kHz crystal running (32773 Hz measured), battery ADC 3906-3913 mV on USB,
buttons idle, USB/charge pins, NVS round trip, heap 283 kB. Time set over the console; framebuffer shows the face.
Host-side bug found and fixed: opening the port with DTR/RTS forced low resets the ESP32-S3 (qzctl now leaves them alone).
On battery (same day): deep sleep was refused every ~2 s (restart + full refresh loop). Breadcrumbs (RTC_NOINIT ring at
0x50000000, read over USB JTAG with gdb `dump binary memory`) showed GPIO0 (UP) at its EXT1 wake level in the RTC
domain with the button released: the strapping pull-up does not hold once the pad is routed to RTC, and the external
pull-up is evidently not effective on this unit. Fix: RTC pull-up on GPIO0 before arming EXT1, its level judged by the
RTC read, RTC_PERIPH kept powered while it is armed (sleep-current cost to measure in B9); a refused sleep now retries
timer-only before restarting. Confirmed by the owner: per-minute partial updates on battery, all buttons wake, steps count.
Second finding: taps were lost while the firmware waited on the panel (0.5-2 s) or on a 250 ms tethered console slice.
Light sleep now always arms released buttons and latches them (`BoardIo::take_latched_buttons`, new HAL method;
`GestureRecognizer::sample` takes the latch and turns an unseen tap into a Click); the tethered slice is 25 ms.
Still to verify on hardware: the tap-latch fix, panel look (ghosting, flicker), sleep current (B9).

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
| WP-19 selftest + 61 goldens, WP-22 virtual-time week suite (18 scenarios), WP-27 radio (Wi-Fi/SNTP/HTTPS/provisioning portal) | committed |
| WP-29 qzctl + device test skeletons, WP-30 README/skills/release flow/power budget | committed |
| Remaining | WP-23 rest (`qz_sim --press/--scene/--console`), tech-debt items below, and everything the first hardware flash finds |

Full gate at 12:05 CDT: `tools/check.sh --fw` = 1348 host tests (ASan+UBSan), tidy, generated-file checks, both firmware variants, offline-symbol check: all green.
FIRST FLASH IS POSSIBLE (flash the OFFLINE image first, see docs/FIRST_FLASH.md; an Opus pre-flash review found no blockers and its fixes are in): `main/` wiring, the USB console port and the platform init exist (radio image 1.18 MiB with the radio now implemented but never run; the first-flash guide still describes the radio-off behaviour of the earlier image: re-read docs/FIRST_FLASH.md before flashing the radio variant). Follow docs/FIRST_FLASH.md. Nothing has run on hardware.

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
- [TECH-DEBT] Pre-flash review (Opus): no blockers; findings 1-5 fixed (RTC calibration cycles, wake-source guarantee, pad isolation, INT1 gating, temperature-read skip). Open: (6) after a failed update or BUSY timeout the panel can stay in standby (~20 uA) until the next good update: do a best-effort reset+init+0x10 on those paths; (7) a rejected `esp_deep_sleep_try_to_start` restarts the chip (forces a full refresh, counts as abnormal reset): rebuild the wake setup and retry a few times first. Test on hardware first: B9 sleep current (> 60 uA suggests pad isolation), awake time per tick, B4 buttons after the first deep sleep, 32 kHz crystal start (10 pF vs 18 pF caps), BUSY polarity/OTP mode-2 waveform, motor tick at power-on (GPIO17 floats in the bootloader).
- [TECH-DEBT] qzctl (WP-29): device suites (`test_apps/console`, `test_apps/wake`) argument/field checks and "DTR/RTS low on open" are [ASSUMED] until bring-up B10; add `qz_sim --console` and a simulator-backed qzctl test once it exists.
- [TECH-DEBT] Virtual-time suite (WP-22): a FRESH device writes 15 NVS entries in its first week (2 per local midnight x 7 + the one-time step-store schema key; the "Off" scenario pre-seeds that key and measures 14, the budget). Wake-ahead assumes the partial waveform, so the periodic FULL refresh (every 31st minute) completes up to ~1.9 s after the true minute (partials stay < 0.7 s): start full refreshes earlier or accept. Changing the time zone after the time is set does not re-derive the step day until the next rollover. The 7-day scenarios take ~7.3 s each at -O0+ASan (target 5 s): add -O1 for the sim test target.
- [TECH-DEBT] Radio (WP-27): worst-case HTTPS open can exceed `timeout_ms` (DNS unbounded); static DRAM headroom is only ~3.7 KiB of the 96 KiB budget (radio image 92.4 KiB); heap teardown is unproven until `test_apps/net` is run; stop-event order, RSSI threshold are [ASSUMED].
- [TECH-DEBT] Self-test (WP-19): battery divider constants mirrored in selftest tuning (use qz_board once allowed); step-counter test checks only that the engine is loaded (counting is bring-up B7); tz-picker goldens depend on the tz table (a tzdata bump needs `qz_golden --update`).
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
