# Quartz roadmap (work packages)

Each WP section is self-contained: paste it into an implementation agent's prompt together with
"Read docs/ARCHITECTURE.md (sections cited), docs/COMPONENTS.md, and the headers named below."

## Contents
1. [Rules for every WP](#1-rules-for-every-wp)
2. [Milestones, waves, critical path](#2-milestones-waves-critical-path)
3. [Work packages](#3-work-packages)
4. [Integration order](#4-integration-order)

## 1. Rules for every WP

- Public headers in `components/*/include/**` are the contract. A WP may add private members and
  inline helpers; changing a public signature requires a note in the WP's final report (and a
  `docs/COMPONENTS.md` "Changes" entry if a dependency changes).
- Files: a WP edits only what it owns (listed). New sources go into the owner component's `src/`
  (globbed; no CMake edits). Tests: `components/qz_<c>/test/*_test.cpp` (GoogleTest, host).
- Done = host tests pass (`tools/host.sh test`), `tools/format.sh --check` and `tools/tidy.sh` clean,
  firmware still builds (`tools/fw.sh build`) when the WP touches code linked into firmware, no new
  warnings, every public function has at least one test, ASSUMED facts tagged in code comments.
- Conventions: ARCHITECTURE.md section 2 (C++23, `qz::Result`, no exceptions/RTTI/heap on wake path,
  integer-only rendering, `QZ_ASSERT` for programmer errors only).
- Model hints: **opus** = critical module (time/TZ, power policy, display driver, wake dispatcher);
  **sonnet** = everything else. Sizes: S < 400 LOC, M 400-1200, L 1200-2500 (incl. tests).

## 2. Milestones, waves, critical path

| Milestone | Proof | WPs needed |
|---|---|---|
| M1 host face | simulator writes `face.png` for a scripted time | 01, 02, 05, 06, 16, 23 (minimal) |
| M2 firmware boots on host CI | `tools/fw.sh build` (radio + offline variants) | 24, 25, 26, 28 + stubs |
| M3 face on the watch | owner bring-up B1-B5 | M2 + 07, 20, 21 (minimal wake loop) |
| M4 a week in 10 s | virtual-time suite green | 04, 09, 10, 11, 13, 20, 21, 22 |
| M5 full feature | console catalog, menus, selftest, radio | all |

Waves (run at most ~4 agents at once to respect the usage budget; within a wave order by value):

| Wave | WPs (parallel) |
|---|---|
| A | WP-01 (alone, short) |
| B | WP-02, WP-05, WP-07, WP-24, WP-25, WP-09, WP-11, WP-12, WP-08 |
| C | WP-03, WP-04, WP-06, WP-10, WP-13, WP-14, WP-17, WP-26, WP-27 |
| D | WP-15, WP-16, WP-18, WP-20, WP-28 |
| E | WP-19, WP-21, WP-23 |
| F | WP-22, WP-29, WP-30 |

Critical path: WP-01 -> WP-02 -> WP-04 -> WP-20 -> WP-21 -> WP-22 -> device bring-up.
Deep (opus) reviews before merge: WP-02, WP-04, WP-07, WP-11, WP-20, WP-21, WP-25.

## 3. Work packages

### WP-01 Core runtime + base fakes (sonnet, S, deps: none)
- Goal: implement `qz_core` and the simple `qz_testkit` fakes everything else tests with.
- Owns: `components/qz_core/src/**`, `components/qz_core/test/**`; in `qz_testkit/src/`:
  `virtual_clock.cpp`, `fake_board_io.cpp`, `fake_rtc_memory.cpp`, `fake_sleep_system.cpp`,
  `fake_console_port.cpp` (+ tests in `qz_testkit/test/`).
- Provides: `qz/core/*.hpp` (to_token, assert_fail, crc32, log, Secret wipe), testkit classes
  `VirtualClock`, `FakeBoardIo`, `FakeRtcMemory`, `FakeSleepSystem`, `FakeConsolePort`.
- Acceptance: crc32 known vectors ("123456789" -> 0xCBF43926) and incremental property; every Errc
  has a unique non-empty token; Result/Status/QZ_RETURN_IF_ERROR tests; death test for QZ_ASSERT;
  log truncation and level filtering; Secret wipes memory; VirtualClock drift math exact.

### WP-02 Civil time + POSIX-TZ engine (opus, M, deps: 01)
- Goal: `qz/time/civil.hpp` and `qz/time/tz.hpp` (except the generated table). ARCH section 9.
- Owns: `components/qz_time/src/{civil,format,tz}.cpp`, `test/{civil,tz,tz_oracle}_test.cpp`.
- Acceptance: days_from_civil/civil_from_days round trip for every day 1900-2200; TZ parser accepts
  all RFC 9636 3.3.1 forms (quoted names, negative/ >24 h times, Jn, n, permanent DST) and rejects
  malformed strings (table of >= 40 cases); oracle test vs glibc (`setenv("TZ")`, `localtime_r`,
  `mktime`) for >= 30 hand-picked strings incl. southern hemisphere, Lord Howe 30-min DST,
  `<+0545>-5:45`, transitions +-1 s, hourly samples 1970-2100; gap/overlap policies.

### WP-03 TZ table generator (sonnet, M, deps: 02)
- Goal: `tools/tzgen.py` (stdlib only) + committed `components/qz_time/src/tz_table.inc` +
  `tz_db.cpp` (`builtin_zones`, `find_zone`, `tzdata_version`). ARCH section 9 curation rules.
- Owns: `tools/tzgen.py`, `tools/tzgen/` cache README, `qz_time/src/tz_db.cpp`, `tz_table.inc`,
  `test/tz_db_test.cpp`.
- Acceptance: deterministic output (run twice -> identical); every entry parses with
  `TimeZone::parse`; every entry matches glibc for 2026-2030 hourly; names unique; contains UTC;
  sorted; <= 6 KiB; generator records tzdata version + input hashes.

### WP-04 TimeKeeper (opus, S, deps: 01, 02)
- Goal: `qz/time/timekeeper.hpp` incl. `parse_iso8601`. ARCH section 8.
- Owns: `qz_time/src/timekeeper.cpp`, `iso8601.cpp`, `test/timekeeper_test.cpp`.
- Acceptance (VirtualClock with crystal error): drift converges within 3 syncs to +-1 ppm for
  errors of +-50 ppm; residual > 500 ppm -> clock fault, no drift change; manual set restarts the
  window; rtc_at_utc(utc_at_rtc(x)) == x +-1 us over 30 days; int64 overflow impossible for 10 years.

### WP-05 Graphics core + PNG (sonnet, M, deps: 01)
- Goal: `Framebuffer`, `Canvas`, `encode_png` in `qz/gfx/framebuffer.hpp` (fonts are WP-06).
- Owns: `qz_gfx/src/{framebuffer,canvas,png}.cpp`, `test/{canvas,png}_test.cpp`.
- Acceptance: clipping on all primitives; text uses a test font fixture; PNG output decodes with
  Python `zlib`/a minimal reader in the test (CRC + Adler-32 checked) and is byte-identical across
  runs; integer-only (`grep -E "float|double"` finds nothing in src).

### WP-06 Fonts pipeline (sonnet, M, deps: 05)
- Goal: build-time BDF -> C++ font generation; `gfx::font(FontId)` for kSmall/kMedium/kLarge/kHuge.
- Owns: `tools/fontgen.py` (stdlib only), `components/qz_gfx/fonts/**` (BDF sources + LICENSE +
  PROVENANCE.md), `qz_gfx/src/fonts.cpp`; the lead wires the CMake hook (COMPONENTS.md section 6).
- Font choice: permissive bitmap fonts (candidate: Spleen, BSD-2-Clause; verify licence text and
  record URL + SHA-256). kHuge must render "88:88" within 200 px width at >= 48 px height.
- Acceptance: generator deterministic; glyph metrics tests; Latin-1 coverage for kSmall/kMedium;
  digits + ":" + "-" for kHuge; total font data <= 40 KiB.

### WP-07 SSD1681 driver + panel model (opus, M, deps: 01, 05; reads docs/research/ssd1681.md)
- Goal: `qz/ssd1681/panel.hpp` from the datasheet; `testkit::FakeEpdPanel`. ARCH section 14.
- Owns: `qz_ssd1681/src/**`, `qz_ssd1681/test/**`, `qz_testkit/src/fake_epd_panel.cpp`.
- Follow ssd1681.md s9 sequences: internal temperature sensor (`0x18`=`80`), full `0x22`=`F7`,
  partial `0x22`=`FF` with both RAM planes rewritten, `0x10`=`01` after every update, 10 MHz SPI,
  `probe_mode2_waveform()` for experiment E1; any timeout -> HW reset + caller marks frame invalid.
- Acceptance: command sequences for init/full/partial/sleep match a documented table (each command
  cites the datasheet section); FakeEpdPanel rejects out-of-order/unknown commands, models RAM
  0x24/0x26 + window/address counters, BUSY timing, deep sleep requiring HW reset; partial update
  shows exactly `next`; timeout path returns kTimeout; RAM polarity conversion tested.

### WP-08 BMA423 wrapper (sonnet, M, deps: 01; reads docs/research/bma423.md)
- Goal: `qz/bma423/accel.hpp` over vendored Bosch SensorAPI; `testkit::FakeBma423`.
- Owns: `qz_bma423/src/**`, `qz_bma423/third_party/bosch/**` (unmodified files at a pinned commit +
  LICENSE + PROVENANCE.md with URL, commit, SHA-256), `qz_bma423/test/**`,
  `qz_testkit/src/fake_bma423.cpp`.
- Acceptance: init against the fake (config blob upload observed, INTERNAL_STATUS ok), chip-id
  mismatch -> kNotFound, step count read, tap interrupt config, INT1 polarity per Config; attach()
  does no writes; Bosch sources compile on host and target with vendor warnings relaxed only there.

### WP-09 Settings + KV fake (sonnet, M, deps: 01, 02)
- Goal: `qz/settings/settings.hpp`; `testkit::FakeKvStore`. ARCH sections 7, 13.
- Owns: `qz_settings/src/**`, `qz_settings/test/**`, `qz_testkit/src/fake_kv_store.cpp`.
- Acceptance: schema names unique/<= 15 chars; every key: default valid, min/max/choices
  enforced, set_from_string/format_value round trip; save writes only changed keys (counter);
  migration framework with a v0 -> v1 test fixture; CredentialStore never exposes the password
  through any non-`reveal()` path; `FakeKvStore::contains_text` used to prove no password in
  other namespaces.

### WP-10 Steps (sonnet, M, deps: 01, 02, 09)
- Goal: `qz/steps/step_tracker.hpp`. ARCH section 10 (all rules in its table).
- Owns: `qz_steps/src/**`, `qz_steps/test/**`.
- Acceptance: one test per table row in ARCH section 10, plus DST spring/fall (Europe/Berlin,
  Australia/Sydney), midnight-transition zones, SNTP jumps +-1 min/+-1 day, multi-day gap, sensor
  reset, pending bucket, history order newest first, flush at most once per local day.

### WP-11 Power model + policy (opus, S-M, deps: 01)
- Goal: `qz/power/power.hpp`. ARCH section 11.
- Owns: `qz_power/src/**`, `qz_power/test/**`.
- Acceptance: curve interpolation exact at points, monotonic; robust mean; hysteresis tables for
  every transition (no flapping on +-20 mV noise); Critical exits on USB; decision() table per
  level; awake accounting resets on day change; estimate_hours matches hand-computed cases.

### WP-12 Weather (sonnet, S, deps: 01, 02)
- Goal: `qz/weather/provider.hpp`, Open-Meteo provider with cJSON. ARCH section 12.
- Owns: `qz_weather/src/**`, `qz_weather/test/**` (recorded JSON fixtures, synthetic only).
- Acceptance: URL exact for sample coordinates (5 decimals, no locale issues); parse fixtures
  (normal, missing daily, nulls, wrong types, truncated, 4 KiB limit) -> report or kCorrupt; all WMO
  codes mapped; freshness boundaries; F conversion rounding incl. negatives. Verify request fields
  against https://open-meteo.com/en/docs and note the date checked.

### WP-13 Connectivity (sonnet, M, deps: 01, 02, 12)
- Goal: `qz/conn/conn.hpp` (Scheduler, SyncSession, Provisioning); `testkit::FakeNetStack`.
- Owns: `qz_conn/src/**`, `qz_conn/test/**`, `qz_testkit/src/fake_net_stack.cpp`.
- Acceptance: Off/compiled-out/no-creds/low-power -> plan empty; backoff sequence and cap with
  jitter bounds; piggyback window; session order and shutdown on every path (incl. connect failure
  and total-budget expiry); provisioning form parser (urlencoded, `+`, `%XX`, token mismatch,
  oversize) and expiry; password alphabet excludes ambiguous characters.

### WP-14 UI framework (sonnet, L, deps: 01, 02, 05, 09)
- Goal: `qz/ui/ui.hpp`: GestureRecognizer, Ui navigation/stack, Menu, Choice list, TimeDate,
  StepGoal and Location editors, screen names; private `src/screen.hpp` base class for WP-15.
- Owns: `qz_ui/src/{ui,gesture,navigation,menu,choice,editors,screen}.*`, `qz_ui/test/**`.
- Acceptance: gesture timing table (click/hold/repeat/debounce/seeded wake press); navigation
  graph of ARCH section 15 (each row a test); editors emit exactly one Action on save, none on
  cancel; idle timeout; Hold MENU -> face; refresh hint on leaving menus.

### WP-15 System screens + icons (sonnet, L, deps: 14)
- Goal: render functions for StepsHistory, WeatherDetail, TimezonePicker, WeatherSettings,
  SyncNow, Provisioning, Diagnostics pages, About, FactoryReset, ChargeMe, StatusOverlay; icon set
  (battery levels, charging, weather conditions, sync states, saver).
- Owns: `qz_ui/src/screens/**`, `qz_ui/src/icons.*`, `qz_ui/test/screens_test.cpp`.
- Acceptance: every screen renders every WatchState variant without asserting (fuzz-ish matrix);
  long strings truncated, never clipped mid-glyph off-screen; icons drawn from in-repo bitmaps.

### WP-16 Faces (sonnet, M, deps: 05, 06; WatchState header only)
- Goal: default face (big time, date, steps+goal, battery, weather, sync indicator) + a second
  minimal face; registry. ARCH section 15.1.
- Owns: `qz_faces/src/**`, `qz_faces/test/**`.
- Acceptance: registry ids unique; each face renders all variants (time invalid, 12h/24h, weather
  fresh/stale/hidden, every SyncIndicator, saver/charging); readability rule: time digits >= 48 px.

### WP-17 Console protocol + dispatcher (sonnet, M, deps: 01)
- Goal: `qz/console/protocol.hpp`, `registry.hpp` (Registry, Dispatcher). ARCH section 16.
- Owns: `qz_console/src/{protocol,json_writer,registry,dispatcher}.cpp`, matching tests.
- Acceptance: tokenizer/quoting table; JSON escaping (control chars, quotes, UTF-8 passthrough);
  overflow -> `ERR no_space`; unknown command; id echo; sensitive flag suppresses logging (log sink
  capture test); radio-flagged commands -> unsupported when compiled out.

### WP-18 Console command catalog (sonnet, M, deps: 17, 09)
- Goal: `register_builtin_commands` implementing every row of ARCH section 16 against DeviceApi.
- Owns: `qz_console/src/commands_*.cpp`, `qz_console/test/commands_test.cpp`,
  `qz_console/test/fake_device_api.hpp`.
- Acceptance: >= 1 success and >= 1 error test per command; registry-introspection test fails if a
  command has no test; `wifi` commands never output the password; `display dump` base64 decodes to
  5000 bytes.

### WP-19 Self-test, scenes, goldens (sonnet, M, deps: 15, 16, 07, 08, 09)
- Goal: `qz/selftest/selftest.hpp`: runner, test list (ARCH section 18 table), scenes for every
  screen and face variant, host golden tool (`--update`, writes PNGs + `golden_crc.inc`).
- Owns: `qz_selftest/src/**`, `qz_selftest/golden/**`, `qz_selftest/test/**`, `host/golden/**`.
- Acceptance: all suites pass against testkit fakes on host; every ScreenId has >= 1 scene; golden
  compare exact; CRC table regenerated == committed (CI check).

### WP-20 App core: RTC state, tether, wake planner (opus, M, deps: 01, 04, 10, 11, 13)
- Goal: `qz/app/rtc_state.hpp` (RtcStore), `TetherPolicy`, next-wake planning incl. wake-ahead
  (ARCH 8.4), Saver/Critical plans, safe mode.
- Owns: `qz_app/src/{rtc_store,tether,wake_planner}.*`, matching tests.
- Acceptance: layout test pins sizeof/offsets; any corruption (each byte flipped) -> kCorrupt;
  tether invariant test (random event sequences on battery never allow console); planner: boundary
  alignment with latency EWMA, clamp, DST-independent (UTC minutes), Saver 5-min cadence, Critical
  has no timer.

### WP-21 App wiring + DeviceApi (opus, L, deps: 14, 16, 18, 20)
- Goal: `App::run_wake` implementing every flow in ARCH section 4 (cold boot, timer, button
  interactive session, accel, USB/tethered loop, reset/safe mode, time invalid), snapshot builder,
  action executor, `DeviceApi` implementation.
- Owns: `qz_app/src/{app,flows,snapshot,actions,device_api,interactive}.*`, `qz_app/test/**`.
- Acceptance: one integration test per flow with full testkit; zero heap allocations on the timer
  path (counting operator new); display before radio; radio failure never blanks the face.

### WP-22 Virtual-time simulation suite (sonnet, M, deps: 21)
- Goal: scenario tests running App for days/weeks in seconds (`qz_app/test/sim_*_test.cpp`).
- Owns: `qz_app/test/sim_*`, `qz_testkit/src/sim_harness.*`.
- Acceptance: 7 days per connectivity mode (Off: radio_init_count == 0; NVS writes <= 14);
  DST both directions with minute flips on boundary (displayed minute == true minute at every
  update); sync backoff under scripted failures; battery drain through Low/Saver/Critical and
  recovery on USB; drift +-40 ppm corrected to < 2 s/day; power-loss mid-week; awake-time totals.

### WP-23 Host simulator (sonnet, M, deps: 21; minimal face-only mode after 16)
- Goal: `host/sim/` executable: `qz_sim --time ... --steps ... --battery ... --weather ... --press
  menu,down --out face.png`, `--scene <name>`, `--console` (stdin/stdout ConsolePort, same protocol).
- Owns: `host/sim/**`.
- Acceptance: CLI tests via CTest; console mode passes the same command tests as the device.

### WP-24 Platform HAL basics (sonnet, M, deps: 01; IDF-only)
- Goal: `qz_platform` implementations of BoardIo, Adc (`esp_adc` oneshot + curve fitting
  calibration), Delay, Clock (`esp_rtc_get_time_us`, `settimeofday`), RtcMemory (RTC_NOINIT),
  System (reset reason, `esp_sleep_get_wakeup_causes`, EXT1 status, slow clock info, RNG, heap),
  KvStore over NVS (lazy init).
- Owns: `qz_platform/src/{board_io,adc,delay,clock,rtc_memory,system,kv_store}.cpp`,
  `test_apps/platform/` Unity tests.
- Acceptance: firmware builds; Unity tests for KvStore round trip and RtcMemory persistence across
  `esp_restart` (owner-run); verify the RTC_NOINIT placement + size budget from the map file.

### WP-25 Platform sleep/wake + buses (opus, M, deps: 01; IDF-only)
- Goal: SleepControl (EXT0/EXT1 from `board::wake_config`, timer, GPIO parking/hold per ARCH
  section 5, light sleep with GPIO wake), EpdBus (SPI2 master, BUSY wait via light sleep), I2cDevice
  (`i2c_master`).
- Owns: `qz_platform/src/{sleep,epd_bus,i2c}.cpp`, `test_apps/sleep/`.
- Acceptance: firmware builds; review checklist: every pin in ARCH section 5 table handled;
  holds released after wake; no pulls on GPIO0; owner test steps listed for HARDWARE_BRINGUP B8/B9.

### WP-26 Console port (sonnet, S, deps: 17, 24; IDF-only)
- Goal: `hal::ConsolePort` over USB-Serial-JTAG with `esp_console` (`func_w_context` trampolines,
  no history, no prompt), output mutex shared with an `esp_log_set_vprintf` hook.
- Owns: `qz_platform/src/console_port.cpp`.
- Acceptance: firmware builds; protocol lines never interleave with logs (owner pytest in WP-29).

### WP-27 Radio (sonnet, L, deps: 01; IDF-only)
- Goal: `qz_net`: NetStack (Wi-Fi STA connect with timeout, SNTP via `esp_netif_sntp`, HTTPS via
  `esp_http_client` + `esp_crt_bundle_attach`, full teardown), ProvisioningPortal (SoftAP WPA2,
  1 client, `esp_http_server`), stubs when `CONFIG_QZ_RADIO=n`.
- Owns: `qz_net/src/**`, `components/qz_net/Kconfig` (if needed).
- Acceptance: both firmware variants build; offline image has no `esp_wifi_init` symbol (CI nm
  check); teardown verified by heap returning to baseline in an owner-run test app.

### WP-28 Firmware entry + configuration (sonnet, S, deps: 24, 25, 27; lead-owned files)
- Goal: `main/` wiring per ARCH section 3, `main/Kconfig.projbuild` (SDKCONFIG.md symbols),
  `sdkconfig.defaults*`, `partitions.csv` (PARTITIONS.md), `main/idf_component.yml` (cJSON pinned).
- Owns: `main/**`, `sdkconfig.defaults*`, `partitions.csv` (lead may do this instead).
- Acceptance: both variants build; size report under budget (ARCH section 20).

### WP-29 Host tooling + on-target automation (sonnet, M, deps: 18, 26)
- Goal: `tools/qzctl` (protocol client, reconnect by VID/PID + serial, `screenshot` -> PNG,
  `wait-ready`, `run`), pytest-embedded suites in `test_apps/console/` (every command) and
  `test_apps/wake/` (`sleep 5` -> re-enumeration -> ready).
- Owns: `tools/qzctl/**`, `test_apps/console/**`, `test_apps/wake/**`.
- Acceptance: qzctl unit tests against the simulator's console (no device); device suites are
  owner-run (HARDWARE_BRINGUP B10).

### WP-30 Developer docs + release (sonnet, S, deps: most; lead-owned files)
- Goal: AGENTS.md skills (build/flash/monitor, add a face, add a console command, power
  measurement, release), `CHANGELOG.md`, `docs/POWER_BUDGET.md` template, release checklist.
- Owns: as assigned by the lead.

## 4. Integration order

1. WP-01 -> merge (everything links against it).
2. Pure libraries as they turn green: 02, 05, 09, 11, 12, then 03, 04, 06, 10, 13.
3. Drivers with fakes: 07, 08. UI: 14 -> 15, 16. Console: 17 -> 18.
4. App: 20 -> 21 -> 22 (virtual-time gate), 19 (goldens), 23 (simulator).
5. Platform: 24, 25, 26 -> 28 (first firmware image) -> 27 (radio variant).
6. 29 (qzctl + device suites), 30 (docs), then owner bring-up (HARDWARE_BRINGUP.md) and the
   power baseline. Hardware findings flow back as [TUNE]/[ASSUMED] updates in ARCHITECTURE.md.
