# Quartz architecture

Firmware for the SQFMI Watchy v3 (ESP32-S3FN8, 200x200 SSD1681 e-paper). Source of truth for
requirements: `docs/SPEC.md`. Component list: `docs/COMPONENTS.md`. Decisions: `docs/DECISIONS.md`.

Fact tags: **[IDF:path]** verified in the pinned tree `.toolchain/esp-idf/`; **[R1]** see
`docs/research/*.md` (hardware research); **[ASSUMED]** must be confirmed (bring-up or a WP);
**[TUNE]** a constant to calibrate on hardware. Nothing hardware-facing is tested until the
owner runs `docs/HARDWARE_BRINGUP.md`.

## Contents
1. [Layers and rules](#1-layers-and-rules)
2. [Coding conventions](#2-coding-conventions)
3. [Runtime model](#3-runtime-model)
4. [Wake flows and time budgets](#4-wake-flows-and-time-budgets)
5. [Deep sleep, wake sources, GPIO](#5-deep-sleep-wake-sources-gpio)
6. [RTC memory state](#6-rtc-memory-state)
7. [NVS layout and flash wear](#7-nvs-layout-and-flash-wear)
8. [Time model](#8-time-model)
9. [Time zones](#9-time-zones)
10. [Step tracking](#10-step-tracking)
11. [Battery and power policy](#11-battery-and-power-policy)
12. [Connectivity and weather](#12-connectivity-and-weather)
13. [Provisioning, credentials, security](#13-provisioning-credentials-security)
    13a. [Phone sync (Bluetooth LE)](#13a-phone-sync-bluetooth-le)
14. [Display refresh policy](#14-display-refresh-policy)
15. [UI model](#15-ui-model)
16. [Console protocol v1](#16-console-protocol-v1)
17. [Tether policy](#17-tether-policy)
18. [Self-test, goldens, diagnostics](#18-self-test-goldens-diagnostics)
19. [Logging](#19-logging)
20. [Memory and power budgets](#20-memory-and-power-budgets)

---

## 1. Layers and rules

```
app (qz_app: wake dispatcher, tether, wiring)           <- main/ (IDF entry) constructs it
UI / console / self-test (qz_ui, qz_faces, qz_console, qz_selftest)
services (qz_time*, qz_settings, qz_steps, qz_power, qz_conn, qz_weather)   * TimeKeeper
drivers + pure libs (qz_ssd1681, qz_bma423, qz_gfx, qz_time, qz_model)
HAL interfaces + board (qz_hal, qz_board)  <-- implemented by qz_platform / qz_net (IDF-only)
foundation (qz_core)
```

- Everything above the HAL is **pure** and runs on the host: unit tests, golden images,
  virtual-time simulation and the simulator all use the real code with `qz_testkit` fakes.
- UI and faces never call services. They render a `ui::WatchState` snapshot and emit
  `ui::Action`s; the app executes actions against services (one place to test effects).
- The console never calls services directly either: commands use `console::DeviceApi`,
  implemented by `qz_app` (real) and by test fakes.
- Build options reach pure code only as `app::BuildFeatures` (radio compiled in, board rev).

## 2. Coding conventions

| Topic | Rule |
|---|---|
| Language | Code must be valid **C++23** (DECISIONS.md D-03). IDF compiles chip targets with `-std=gnu++26` [IDF:tools/cmake/build.cmake]; host with `-std=c++23` (`cmake/qz_flags.cmake`). No C++26-only features. C only in vendored Bosch code. |
| Exceptions/RTTI | Off everywhere (`-fno-exceptions -fno-rtti`). No `dynamic_cast`, `typeid`, `throw`. Never call `std::expected::value()` / `std::optional::value()` (abort paths); use `*`/`->` after checking. |
| Errors | `qz::Status` / `qz::Result<T>` (`qz/core/result.hpp`), both `[[nodiscard]]`. `qz::Error{Errc code; uint16_t detail}`. `Errc` is a closed enum mapped 1:1 to console error tokens. Ignoring a `Status` requires `(void)` + comment. |
| Assertions | `QZ_ASSERT(cond)` for programmer errors only (never for I/O or user input). Firmware: logs file:line then `abort()` (panic -> reboot -> wake log records it). Host: `std::abort` (GoogleTest death tests). `QZ_ASSERT` stays enabled in release. |
| Heap | No allocation on the steady-state wake path (timer/button/USB wakes): fixed-capacity containers (`qz::StaticVector`, `qz::RingBuffer`), static or member storage. Allowed: radio sessions (Wi-Fi/TLS/cJSON), provisioning, console. Host tests assert zero allocations in the minute-tick path via a counting `operator new` in the sim test. |
| Naming | Types `PascalCase`; functions/variables `snake_case`; members `snake_case_` (trailing underscore); constants `kPascalCase`; macros `QZ_UPPER`; namespaces `qz::<component>`; files `snake_case.hpp/.cpp`. Interfaces are abstract classes named for the role (`EpdBus`, not `IEpdBus`). |
| Units | Encode in names or strong types: `_ms`, `_us`, `_mv`, `_ppb`, `_e5deg`; time values are `qz::time::UnixMicros` (int64 UTC microseconds) / `UnixSeconds`; durations `int64_t` microseconds unless the name says otherwise. Temperatures `int16_t` deci-degrees C (`temp_dc`). Coordinates `int32_t` 1e-5 degrees. |
| Integers | Fixed-width types; `int64_t` for all time arithmetic (no 2038 problem); no `float`/`double` in rendering or anything feeding golden images (bit-exact host vs target); floats allowed only in weather parsing (immediately converted). |
| Constants | Tunables live in one `constexpr` table per component (`src/tuning.hpp` or a public `*_config.hpp` when the app overrides them) with source/[TUNE] comments. No magic numbers in logic. |
| Threads | Pure classes are **not thread-safe** unless documented; the app calls them from one task. Exceptions documented per header (console output mutex, button ISR -> queue). |
| Ownership | Interfaces passed by reference, lifetime owned by `main`/`App`; no global mutable singletons in pure code. Platform singletons are created once in `main`. |
| Style | `.clang-format` / `.clang-tidy` committed by the lead; warnings are errors (`cmake/qz_flags.cmake`). Doxygen-lite `///` comments on public declarations: ownership, units, errors, thread-safety. |
| Secrets | Wi-Fi password only in `qz::Secret` (no formatting operator, zeroed on destruction); never passed to logging or console output. |

## 3. Runtime model

- One application task (the IDF main task) runs each wake **to completion**, then deep sleep.
  No other app tasks on battery. IDF's own tasks (IPC, timer, Wi-Fi during radio sessions)
  exist as IDF needs.
- Interactive session: after a button wake the app stays awake in **light sleep** between
  inputs (GPIO wake on buttons + timer for minute ticks) until the UI is idle, then deep sleep.
- Tethered (USB present): app stays awake, a console task reads lines; the app task keeps
  processing minute ticks and inputs from a queue (section 17). CPU 80 MHz, no light sleep.
- `App::run_wake(WakeInfo) -> SleepPlan` is the pure entry point; `main` loops
  `run_wake` -> `platform.sleep(plan)`. The simulator drives the same function with a
  virtual clock.

## 4. Wake flows and time budgets

Budget = active CPU time target on battery (excludes panel waveform time spent in light sleep).
All budgets [ASSUMED] until measured in bring-up step B9.

| Wake | Detection | Sequence (in order) | Budget |
|---|---|---|---|
| **Minute tick** | timer cause | 1 validate RTC state 2 read time (TimeKeeper) 3 tether check (USB pin) 4 steps: read HW counter (I2C) + rollover 5 battery sample if due (every 10 min) 6 power policy 7 build snapshot for the **target minute** 8 render 9 EPD: reset, init, write old+new RAM, trigger partial/full 10 light sleep until BUSY low 11 panel deep sleep 12 conn due? (section 12) 13 append wake record 14 plan next wake (section 8.4), store state + CRC, deep sleep | <= 60 ms CPU + waveform |
| **Button** | EXT1 status mask | 1-6 as above 7 seed gesture recognizer with pressed pin(s) 8 interactive session: render on each UI change, light sleep between inputs, minute ticks still on time 9 idle (face: 2 s after release; menus: 30 s) -> back to face, full refresh if leaving menus, plan, deep sleep | first frame <= 150 ms after wake |
| **Accelerometer** (only if `tap_wake` on) | EXT1 status = INT1 | read/clear BMA423 int status; action per section 15 (status overlay 5 s); plan; sleep | <= 80 ms + waveform |
| **USB attach** | EXT0 (GPIO21 high) or USB pin high at any wake | enter Tethered (section 17): console up, `ready` event, stay awake | n/a (USB powered) |
| **Cold boot** (power-on, brownout) | reset reason + invalid RTC state | init RTC state defaults, time invalid; load settings (migrate) + step history from NVS; BMA423 full init (config blob); panel full init + full refresh; if mode != Off and creds and battery Normal: immediate sync session | <= 2 s incl. BMA init |
| **Software reset / panic / WDT** | reset reason, RTC state valid | resume with RTC state; record reset in wake log; crash counter (>= 3 within 10 min -> safe mode: radio off, no tap wake, face only, until next USB attach or 24 h) | as minute tick |
| **Time invalid** (any wake while `time_valid=false`) | TimeKeeper flag | face shows `--:--` + "set time" hint; ticks every 10 min (housekeeping only); steps accumulate into the pending bucket (section 10) | as minute tick |

Ordering rules: battery is sampled **before** display/radio load; the display is updated
**before** any radio activity (a radio failure can never delay or blank the face); radio
sessions run last and are bounded by a hard deadline (section 12).

## 5. Deep sleep, wake sources, GPIO

Pin map from SPEC (v3), verified against the v3.0 schematic in `docs/research/hardware.md` (R1). ESP32-S3 RTC GPIOs are
GPIO0-21 (`SOC_RTCIO_PIN_COUNT 22`) [IDF:components/soc/esp32s3/include/soc/soc_caps.h].

| Source | Mechanism | Polarity | Notes |
|---|---|---|---|
| Minute tick | `esp_sleep_enable_timer_wakeup` | — | RTC timer, 32.768 kHz crystal |
| Buttons MENU 7, BACK 6, UP 0, DOWN 8 | EXT1 `esp_sleep_enable_ext1_wakeup_io(mask, mode)` | active-low, external 100 k pull-ups [R1 s6] -> `ESP_EXT1_WAKEUP_ANY_LOW` | S3 EXT1 has one mode for all pins (no per-pin mode cap in soc_caps) [IDF:esp_sleep.h] |
| BMA423 INT1 (GPIO14) | same EXT1 mask | INT lines have no board pulls [R1 s7]: configure INT1 push-pull active-low; clear a latched INT before sleeping | only armed when `tap_wake` on |
| USB detect (GPIO21) | EXT0 `esp_sleep_enable_ext0_wakeup(21, 1)` | active-high (VBUS divider) [R1 s5] | EXT0 keeps RTC_PERIPH powered [IDF:docs/en/api-reference/system/sleep_modes.rst]; Kconfig `QZ_USB_WAKE` (default y) — disable if B9 shows a measurable sleep-current cost; then USB is noticed at the next tick |

`qz_board` computes the EXT1 mask and mode with `constexpr` helpers and `static_assert`s:
all EXT1 pins RTC-capable and same polarity; at most one opposite-polarity pin left for EXT0.
A held button re-wakes immediately (level-sensed): the app waits for release (or excludes held
pins from the mask) before sleeping [R1 s6]. **BACK+UP held ~4 s is a hardware reset chord**
(SR2 smart-reset IC); releasing BACK first while holding UP enters ROM download mode [R1 s6].
The UI must never assign a BACK+UP chord.

Pin states during deep sleep (applied by `hal::SleepControl::enter_deep_sleep`):

| Pin(s) | State in deep sleep | Why |
|---|---|---|
| Vibration GPIO17 | output low + hold (RTC GPIO) [IDF:esp_driver_gpio gpio.h]; also driven low first thing at boot | active-high NPN drive without base pull-down [R1 s9]: floating = motor may run |
| EPD CS 33 / RST 35 | output high + digital pad hold (`gpio_hold_en` + `gpio_deep_sleep_hold_en`; not RTC GPIOs) | deselected; RES# also has a 100 k pull-up [R1 s8] |
| EPD DC 34, MOSI 48, SCK 47 | output low + hold | no floating inputs on the panel |
| EPD BUSY 36 | input, no pull | driven by panel |
| GPIO46 (listed as MISO) | never configured | strapping pin; SSD1681 is write-only here |
| GPIO0 (UP) | input; never pull-down, never output | strapping pin: UP held during **reset** = ROM download mode (recovery path). Deep-sleep wake while UP held must boot normally [ASSUMED, bring-up B4] |
| Buttons | RTC input, **no internal pulls** (external 100 k pull-ups) | [R1 s6] |
| INT1/INT2 | RTC input, no pulls (BMA423 drives push-pull) | [R1 s7] |
| I2C 11/12 | input, no internal pull | external 10 k pull-ups [R1 s7] |
| Battery ADC 9 | analog (digital input disabled) | leakage |
| Charge status 10, USB detect 21 | input, no pulls (resistor networks from VBUS) | GPIO10 reads HIGH whenever USB is present (charging *and* full) [R1 s5]: it is not a charge-state signal |

## 6. RTC memory state

Placed with `RTC_NOINIT_ATTR` [IDF:components/esp_common/include/esp_attr.h] so it also
survives software resets/panics; validated on every boot, never trusted blindly.

```
struct RtcState (qz/app/rtc_state.hpp)               ~1.7 KiB, layout pinned by test
  RtcHeader      magic 'QZRS' u32 | version u16 | size u16 | crc32 u32 (payload) | boot_count u32
  time::TimeKeeperState   anchors, drift_ppb, source, valid, last_sync_utc, last_set_utc
  steps::StepState        last_hw_count, today, today_day, pending, history[7], last_flush_day
  power::PowerState       filtered_mv, level, policy, last_sample_us, awake_ms_today, counters
  conn::ConnState         next_time_sync, next_weather, fail_streak, last_result, last_ok_utc
  model::WeatherReport    cached report + fetched_utc
  DisplayState            partials_since_full, last_full_utc, frame_valid
  WakeTiming              ewma_wake_latency_us, scheduled_wake_us, crash window
  settings::Settings      cache of NVS settings (NVS is never initialized on the minute path)
  WakeLog                 ring of 32 x 16 B WakeRecord (section 18)
struct FrameShadow   magic u32 | crc32 u32 | 5000 B last displayed frame (1 bpp)
```

- Validation: magic, version, size == sizeof, CRC32 (qz_core) over the payload. Any failure ->
  cold-boot defaults (+ NVS restore of settings/history). CRC is recomputed once, right before
  sleep (`RtcStore::commit`).
- Any layout change bumps `kRtcStateVersion`; a host test pins `sizeof` and field offsets.
- Total RTC use <= 7.5 KiB of the 8 KiB RTC slow memory; ULP unused [ASSUMED: IDF's own RTC
  usage measured from the map file in WP-P1].

What survives which event:

| Event | RTC timer/time | RtcState | FrameShadow | NVS | BMA423 counter |
|---|---|---|---|---|---|
| Deep-sleep wake | yes | yes | yes | yes | yes |
| `esp_restart`, panic, task WDT | yes [IDF:docs/en/api-reference/system/system_time.rst] | yes (NOINIT) | yes | yes | yes |
| USB reflash (USB-JTAG reset) | yes [ASSUMED] | yes if `version` unchanged | yes | yes (unless `erase-flash`) | yes |
| Power-on, battery swap, brownout | **no** -> time invalid | no (CRC fails) | no | yes | no (sensor reset) |
| Factory reset | yes (time kept) | reset to defaults except TimeKeeper | invalidated | all `qz_*` namespaces erased | software baseline reset |

## 7. NVS layout and flash wear

Partition `nvs` (see PARTITIONS.md). Keys <= 15 chars. Every namespace has key `ver` (u16).
Migrations are pure functions `migrate(from, to, KvStore&)` with host tests per step.

| Namespace | Keys (type) | Written when |
|---|---|---|
| `qz_set` | `ver` u16, `tfmt` u8, `tz` str (IANA name), `tzposix` str (fallback), `units` u8, `conn` u8, `wx_hilo` u8, `lat` i32, `lon` i32, `sync_h` u16, `wx_min` u16, `goal` u32, `vib` u8, `face` u8, `tapwake` u8 | user saves a setting (UI or console) |
| `qz_cred` | `ver`, `ssid` str, `pass` str | provisioning / `wifi set` / `wifi clear` |
| `qz_steps` | `ver`, `hist` blob (7 x {day i32, steps u32}) + `today` blob {day, steps} | local-midnight rollover (<= 1/day); before controlled reboot/factory reset; on entering Critical |
| `qz_time` | `ver`, `drift` i32 (ppb), `drift_t` i64 | after a sync changes drift by > 1 ppm (<= 1/day) |
| `qz_diag` | `ver`, `crashes` u32, `lastpanic` blob (reason, utc) | after a panic/WDT reset (rare) |

Wear policy: **no NVS access on the minute path** (settings are cached in `RtcState`; `KvStore` initializes NVS lazily on first use). Settings writes only on change (compare
first). Virtual-time test: 7 simulated days with no user action => <= 14 NVS writes total.
Wi-Fi driver NVS (PHY calibration, `CONFIG_ESP_WIFI_NVS_ENABLED`) disabled for Wi-Fi config
storage (we pass credentials explicitly) — see SDKCONFIG.md.

## 8. Time model

### 8.1 Time base
- Slow clock = external 32.768 kHz crystal (`CONFIG_RTC_CLK_SRC_EXT_CRYS`
  [IDF:components/esp_hw_support/port/esp32s3/Kconfig.rtc]). Raw time source:
  `esp_rtc_get_time_us()` [IDF:components/esp_hw_support/include/esp_rtc_time.h] behind
  `hal::RtcClock::now_us()` (monotonic since power-on, survives deep sleep and resets).
- If the crystal fails to start, IDF falls back to the internal RC [ASSUMED: verify in
  `components/esp_system/port/soc/esp32s3/clk.c`]; `hal::System::slow_clock()` reports it,
  TimeKeeper marks `clock_degraded` (diag + face sync indicator).

### 8.2 TimeKeeper (qz_time, pure)
`utc_us = anchor_utc_us + d + d * drift_ppb / 1e9` where `d = rtc_us - anchor_rtc_us`
(int64; `d` up to months fits). Re-anchored on every set/sync. Also pushes UTC to the libc
clock (`settimeofday` via `hal::RtcClock::set_system_utc`) so TLS certificate checks see it.

| Event | Effect |
|---|---|
| Cold boot | `valid=false`, `source=None` |
| Manual set (UI/console) | anchor; `valid=true`, `source=Manual`; drift estimation window restarts |
| SNTP success | if previous anchor came from SNTP, no manual set since, elapsed >= 6 h, residual within ±500 ppm: `drift_ppb += residual_ppb / 2` (clamp ±200 ppm); anchor; `source=Sntp`; `last_sync_utc` |
| Residual > 500 ppm | treat as clock fault: anchor only, no drift update, log |
| First eligible sync while `drift_ppb == 0` | (WP-04, [TUNE]) apply the whole residual (bootstrap gain 1) so +-50 ppm converges within 3 syncs; later syncs use gain 1/2. A residual is judged a clock fault only when a drift estimate is eligible. |
| Time jump (any set) | services get `on_time_jump(old, new)` (steps rollover, schedules recomputed) |

### 8.3 Validity and indicators
`valid` drives face rendering (`--:--` when false). Sync indicator states (model::SyncIndicator):
`None` (Off mode, valid manual time), `NeverSynced`, `LastFailed`, `Stale` (no success for
> 3 x sync interval), `Ok`.

### 8.4 Wake-ahead (minute flips on the boundary)
- Target: the partial-update waveform midpoint lands on `hh:mm:00.000` (visible change within
  -250..+500 ms).
- `wake_at = boundary - lead`, `lead = ewma_wake_latency + render_and_spi + waveform/2`
  (initial 350 ms [TUNE], EWMA alpha 1/4, clamped 100..1500 ms). The frame rendered is for the
  **upcoming** minute. If ready more than 50 ms early, the app light-sleeps until
  `boundary - waveform/2`, then triggers the update.
- Measured each wake: `latency = first_app_instruction_rtc_us - scheduled_wake_us`
  (`hal::System::boot_rtc_us()` captured as early as possible in `app_main`).
- Timer wake duration converts UTC deltas back to raw RTC time with the drift correction.

## 9. Time zones

- Engine: in-house POSIX-TZ parser + evaluator on `int64` seconds (`qz/time/tz.hpp`): `std`/`dst`
  names (alpha and `<+03>` forms), offsets with minutes/seconds, rules `Mm.w.d`, `Jn`, `n`, transition
  times with negative and > 24 h hours (RFC 9636 section 3.3.1 extensions), southern hemisphere
  (DST across new year), permanent DST. Gap/overlap resolution for local->UTC: `Earlier`/`Later`/
  `Reject` (manual time entry uses `Earlier`, editors show a hint on gaps).
- Oracle: host tests compare against glibc `localtime_r`/`mktime` with `TZ` set, for every
  built-in entry, every hour across 1970-2100 sampled + every transition +- 1 s.
- Built-in list: `tools/tzgen.py` (stdlib only) reads TZif footers from a pinned IANA release
  (`tzdata` tarball compiled with the pinned `zic`, or the PyPI `tzdata` package of the same
  version — WP picks one and records it) and emits `components/qz_time/src/tz_table.inc`
  (committed, header records tzdata version + generator hash; CI re-generates offline from a
  cached copy and diffs).
- Curation: zones from `zone1970.tab`; drop entries whose footer duplicates another zone of the
  same country unless population/recognisability rule keeps it (keep capital + largest city);
  always include `UTC` and `Etc/GMT±N` fixed offsets; label = city name + footer offset; sort by
  standard offset then label. Target ~120-160 entries (<= 6 KiB flash). Stored setting is the
  IANA name plus the POSIX string fallback (a removed name keeps working).

## 10. Step tracking

Hardware counter only (BMA423 step counter feature; read via Bosch API, `uint32_t` output
[ASSUMED until docs/research/bma423.md lands]). The counter is **never reset** in normal operation; software keeps a baseline.

| Situation | Rule |
|---|---|
| Normal read `c >= last` | `delta = c - last` |
| `c < last` | sensor reset (power loss or re-init): `delta = c` (32-bit wrap is physically impossible) |
| Local day computed from TimeKeeper + TZ | `day = days_from_civil(local date)` |
| `day > today_day` | push `{today_day, today}` into 7-day history (fill skipped days with 0), `today = 0`, flush history to NVS, then add `delta` to the **previous** day if the last read was before the boundary and the gap is <= 2 min, else to the new day |
| `day < today_day` (clock moved back across midnight, or fall-back at 00:00) | no rollback: keep counting into `today_day`; log `time_back` |
| Time invalid | deltas accumulate in `pending`; when time first becomes valid, `pending` goes to today |
| Daily goal | `goal > 0`: face shows progress; optional single vibration when crossed (if `vib`) |

Edge cases are covered by virtual-time tests: DST spring/fall in both hemispheres, zones with
midnight transitions (e.g. `America/Santiago`, `America/Havana`), SNTP jumps of ±1 min/±1 day
across midnight, manual date set forward/back, multi-day sleep in Critical, sensor reset.
Axis remap is a board constant applied at BMA423 init (`bma423::AxisRemap`, from R1 / bring-up B7).

## 11. Battery and power policy

- Measurement: `hal::Adc::read_pin_mv()` returns calibrated pin millivolts (`esp_adc` oneshot,
  ADC1_CH8, 12 dB attenuation, curve-fitting calibration [IDF:components/esp_adc]); battery mV =
  pin mV x 460/360 (R8 100 k / R9 360 k [R1 s4]). **The calibrated ADC range ends at 2900 mV pin =
  3706 mV battery** [R1 s4]: all policy thresholds sit below it; percentages above ~3.7 V are
  uncharacterised until bring-up B6. 16 samples, drop min/max, mean; sampled before display/radio load,
  every 10 min and on button wakes; EWMA alpha 1/4 across samples. No percentage while USB is
  present (charging icon; GPIO10 cannot tell charging from full [R1 s5]).
- Percentage: piecewise-linear LiPo OCV table (11 points, 3300..4200 mV) [TUNE: replace with
  the curve measured in bring-up B6]. Display rounds to 5 % up to 3.7 V and to 10 % above
  (uncalibrated ADC range).

| State | Enter (filtered mV) | Exit | Behaviour |
|---|---|---|---|
| Normal | default | — | everything per settings |
| Low | <= 3600 [TUNE] | >= 3700 | radio sessions forbidden (incl. "Sync now": shows "battery low"); weather ages out normally; tap wake off; vibration off |
| Saver | <= 3500 [TUNE] | >= 3600 | Low + display updates every 5 min (face shows a saver mark, minutes still correct at each update), full refresh only every 4 h |
| Critical | <= 3400 [TUNE] | USB present, or >= 3600 | flush steps to NVS, show "Charge me" screen once (full refresh), panel deep sleep, deep sleep with **button + USB wake only** (no timer). A button shows the face with the current time + banner for 10 s, then back to Charge me |

Hysteresis prevents flapping; transitions are evaluated once per battery sample. Brownout
detector stays enabled (SDKCONFIG.md); a brownout is a cold boot. **The board has no battery
protection IC** [R1 s3]: Critical additionally suspends the BMA423 so the floor drops to
~20 uA; an uncharged watch still reaches the 2.75 V cell cutoff after weeks (PUSHBACK P-06).
Capacity for estimates: 170 mAh (cell minimum; marketing says 200) [R1 s3].

Power estimate (diagnostics + console `diag power`): per-wake awake time and cause are summed
per day in `PowerState`; `estimate_days = capacity_mah / (sleep_floor_ma*24 + sum(awake_ms x
active_ma)/3.6e6)` with constants from bring-up B9 [TUNE].

## 12. Connectivity and weather

Modes: `Off`, `TimeOnly`, `TimeWeather` (setting `conn`). The pure `conn::Scheduler` decides;
`conn::SyncSession` executes through `hal::NetStack` (implemented by `qz_net`).

| Rule | Value |
|---|---|
| Off means off | In `Off`, `NetStack::start()` is never called (no `esp_wifi_init`). Proven by: virtual-time test (fake asserts 0 calls over 7 days), on-device counter `radio_inits` in `diag radio` (must stay 0), power measurement B9 |
| Compile-out | `CONFIG_QZ_RADIO=n`: `qz_net` builds stubs only, no `esp_wifi`/lwIP/mbedTLS symbols linked (CI: `nm` check on the offline image); `BuildFeatures.radio=false` hides connectivity/weather UI, forces `Off`, console net commands return `unsupported` |
| Preconditions | mode != Off, credentials present, power state Normal, not tethered-only-debug, time valid (weather only) |
| Time sync interval | `sync_h` in {6, 12, 24, 48, 168} h, default 24 |
| Weather interval | `wx_min` in {30, 60, 120, 180, 360} min, default 60 |
| Piggyback | if one job is due, the other runs too when due within the next 2 h |
| Hard budgets | Wi-Fi associate+DHCP 10 s; SNTP 8 s; HTTPS weather 12 s; whole session 30 s (deadline checked between steps and passed as timeouts; teardown always runs) |
| Backoff | failure streak n: next attempt = now + min(15 min x 2^(n-1), min(interval, 12 h)) x (1 ± 10 % jitter, seeded per device). Success resets. Never on every wake |
| Teardown | `esp_wifi_stop` + `esp_wifi_deinit` + netif destroy after every session (radio fully down before sleep) |
| Manual "Sync now" | runs immediately (ignores backoff) if preconditions hold; result shown on screen |

Session order: connect -> SNTP (if due or time invalid) -> weather (if due and time valid) ->
teardown. Results are applied after teardown (TimeKeeper anchor, weather cache, backoff).

Weather (`qz_weather`): `weather::Provider` interface { `build_request`, `parse` }. v1 provider
Open-Meteo (`/v1/forecast?latitude&longitude&current=temperature_2m,weather_code&daily=
temperature_2m_max,temperature_2m_min&timezone=GMT&forecast_days=1`; fields [ASSUMED: WP
verifies against https://open-meteo.com/en/docs]). Body capped at 4 KiB; parsed with cJSON;
temperatures stored as deci-°C; WMO code -> `model::WeatherCondition` (clear, partly, cloudy,
fog, drizzle, rain, snow, showers, thunder, unknown). Freshness: **fresh** age <= 2 x interval;
**stale** (rendered with a stale mark) <= 6 h; **hidden** beyond 6 h, or time invalid, or mode
!= TimeWeather. A failed fetch keeps the previous report and only changes the indicator.
Licence note: Open-Meteo free tier is non-commercial — swap the provider before any sale.

## 13. Provisioning, credentials, security

**Recommendation: custom time-limited SoftAP page** (DECISIONS D-10), plus the USB console.

| | IDF `network_provisioning` (Security 2, PoP) | Custom SoftAP page (chosen) |
|---|---|---|
| Availability in v6.1 | removed from IDF, now a registry component [IDF:docs/en/migration-guides/release-6.x/6.0/provisioning.rst] | `esp_wifi` SoftAP + `esp_http_server` (in tree) |
| Client | Espressif phone app; only Wi-Fi creds without a custom app | any browser; one form: SSID, password, TZ, lat/lon, units, mode |
| Security | SRP6a + AES-GCM; strong | WPA2-PSK with a fresh random 12-char password shown only on the watch (~70 bits) = proof of possession; 1 client max; 5 min timeout; one-time form token; no HTTPS (link is WPA2-encrypted) |

Flow: Menu > Connectivity > Setup -> watch shows SSID `Quartz-XXXX`, password, `http://192.168.4.1`
and a countdown; POST is validated by the same pure validators as the console (`qz_settings`);
on success the AP shuts down and a sync session starts. Blocked in Low/Saver/Critical.

Credentials: NVS `qz_cred` only; never logged, printed, rendered, or returned by any console
command (`wifi status` shows SSID + "password set"). Console `wifi set` is marked sensitive: the
binding disables linenoise history and the dispatcher never logs its arguments.

At-rest encryption trade-off (owner decision, OPEN_QUESTIONS Q-04): v1 default **off**.
- NVS encryption with HMAC key protection (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`
  [IDF:components/nvs_sec_provider/Kconfig]) needs a one-time, irreversible eFuse key burn; USB
  reflashing stays possible; protects against a passive flash dump only (without secure boot,
  attacker firmware can still use the HMAC peripheral).
- Flash encryption (release mode) blocks plaintext reflashing over USB permanently and
  complicates recovery; development mode is not a security feature. Not recommended for a
  hobby-serviceable watch.

## 13a. Phone sync (Bluetooth LE)

On-demand link to a companion web page (`web/phone/index.html`, Web Bluetooth in Chrome on Android
or desktop). DECISIONS D-29. The page sets the time and zone from the phone, edits settings and the
face, pushes weather it fetched from Open-Meteo itself (so the watch needs no Wi-Fi), and can store
Wi-Fi credentials.

| Aspect | Rule |
|---|---|
| Start | Only from the watch: Menu > Phone > Sync with phone. Never scheduled, never on a timer. |
| Radio | NimBLE peripheral (`hal::PhoneLink`, qz_net). Initialized at session start, fully deinitialized at the end. While up, IdfSleep polls instead of light-sleeping (the link needs the radio clock); buttons are sampled every 25 ms. |
| Session end (`conn::PhoneSession`) | no secure connection within 120 s; 120 s without a command; the phone disconnects; 15 min hard cap; BACK; the setting turned off; the end of the wake. Each one powers the stack down. |
| Security | DisplayOnly IO capability, LE Secure Connections, MITM, bonding (NVS, `nimble_bond`, max 4). The watch requests security on connect; the six-digit passkey (esp_random, radio on) is shown on the e-paper only. Both characteristics require an encrypted, authenticated link; writes before that are refused. Failed pairing disconnects. |
| Transport | Nordic-UART-style service `6E400001-...`, RX write `...0002`, TX notify `...0003`. Lines of section 16 unchanged; responses are notified in MTU-sized chunks. |
| Permissions | `console::Origin::kPhone`: commands flagged U (USB only: `reboot`, `sleep`, `factory-reset`, `selftest run`, `sync now`, `weather fetch`, `provision start`) answer `unsupported` without running. Everything else, `wifi set` included, is allowed over the secure link. |
| Off means off | Setting `phone` (default on: costs nothing until started). Off: the Phone list only shows the switch and the stack is never initialized. Compile-out: `CONFIG_QZ_PHONE` (depends on NimBLE; the offline image has no Bluetooth, checked by `tools/check_offline.sh`). |
| Concurrency | One radio session at a time: phone sync, provisioning and Wi-Fi syncs refuse each other with `busy`. |
| Forget | Menu > Phone > Forget phones, console `phone forget`, and factory reset erase all bonds. |

Weather pushed by the page (`weather push <temp_dc> <code> <hi> <lo> [observed_unix]`) is stored as
a real report (not `faked`); the observation time must be at most 6 h old and at most 5 min ahead
of the watch clock.

## 14. Display refresh policy

| Rule | Value |
|---|---|
| Minute update | display mode 2 (`0x22`=`FF`, typ 0.26 s at 25 C): old frame from `FrameShadow` to RAM 0x26, new frame to 0x24 every time, because SSD1681 and vendor docs disagree on deep-sleep RAM retention [R1/ssd1681.md s1, s9.3]. **Whether the panel OTP holds a mode-2 waveform is undocumented** (ssd1681.md risk #1): bring-up E1 decides; fallback needs a vendor LUT (owner approval, Q-12) |
| Full refresh | display mode 1 (`0x22`=`F7`, ~2 s, flashes): every N partial updates (N default 30 [TUNE in B5]; vendor guidance is 5 and >= 180 s between updates, a deliberate deviation logged as tech debt [ssd1681.md s1]), on leaving menus, after Critical, after any abnormal reset or aborted update, when panel temperature moved > 10 C, on cold boot, on `display refresh full`, and BACK on the face |
| Temperature | internal sensor must be selected (`0x18`=`80`; TSCL/TSDA unconnected, POR would pick the 127.9 C waveform) [ssd1681.md s6]. Panel rated 0-50 C; outside, the controller may refuse to update: never hang on BUSY |
| Waiting | `EpdBus::wait_idle(timeout)`: light sleep with GPIO wake on BUSY (battery) or polling (tethered); timeouts partial 5 s, full 10 s [TUNE]; timeout -> `Errc::kTimeout`, HW reset, `frame_valid=0` (next update is full), error in wake log |
| Panel sleep | after BUSY low: `0x10`=`01` (deep sleep, ~1 uA); without it the controller idles at ~20 uA [ssd1681.md s7]; HW reset at next wake. SPI write clock 10 MHz (max 20) |
| Lifetime | ~525 k partial updates/year; the vendor publishes no refresh-cycle rating (ssd1681.md s8): PUSHBACK P-05 |
| Flicker | partial updates have no black/white flash; full refreshes flash and are therefore batched into the rules above |

## 15. UI model

Input: `model::Button {Menu, Back, Up, Down}`; recognizer (`ui::GestureRecognizer`) emits
`InputEvent{button, kind, t_ms}` with kinds `Click` (released < 700 ms), `Hold` (700 ms), `Repeat`
(every 150 ms after Hold, Up/Down only); debounce 25 ms [TUNE]. A deep-sleep button wake seeds
the recognizer with "pressed at wake time".

Global: Hold MENU -> face (home). Idle 30 s in any menu -> face. Optional click vibration 15 ms.
BACK+UP together is never interpreted (hardware reset chord, section 5).

| Screen (`ui::ScreenId`) | Content | UP / DOWN | MENU | BACK |
|---|---|---|---|---|
| Face | active face (section 15.1) | Up: steps 7-day; Down: weather detail | open Menu | full refresh |
| StepsHistory / WeatherDetail | 7-day bars + goal / temp, hi/lo, condition, age | — | — | face |
| Menu | Time & date, Time zone, 12/24h, Units, Connectivity, Weather, Sync now, Phone, Step goal, Vibration, Watch face, Diagnostics, About, Factory reset | move (wrap) | enter | face |
| TimeDateEditor | fields Y-M-D h:m (seconds zeroed on save) | +/- (Repeat) | next field / save on last | previous field / cancel on first |
| TimezonePicker | list grouped by offset | move | select + save | cancel |
| Choice (12/24h, units, connectivity, vibration, face, intervals) | radio list | move | save | cancel |
| WeatherSettings | on/off, hi/lo on/off, interval, location editor (lat/lon digit editor), Setup Wi-Fi | move | enter/toggle | back |
| StepGoalEditor | 0 (off)..50000 step 500 | +/- | save | cancel |
| SyncNow | progress, then result + last sync time | — | retry | back |
| Provisioning | SSID, password, URL, countdown | — | — | stop + back |
| Diagnostics | pages: battery, time/drift, sync, wakes/awake time, sensors, self-test run | page | run self-test (last page) | back |
| About | version, git hash, IDF version, build features | — | — | back |
| FactoryReset | warning | — | Hold MENU 3 s to confirm | cancel |
| ChargeMe | "Charge me" + last time | — | — | — |
| StatusOverlay | battery %, steps vs goal, last sync (tap wake, 5 s) | — | — | face |

Rules: screens are pure (`ui::Screen::render(const WatchState&, gfx::Canvas&)`,
`handle(InputEvent) -> ActionList`). Editors work on a copy and emit one `ui::Action` on save
(`SetSetting`, `SetTime`, `SyncNow`, `FactoryReset`, `StartProvisioning`, ...), executed by the app,
which then refreshes the snapshot. Manual time entry works with the radio off.

### 15.1 Faces
`faces::Face` interface (`id`, `name`, `render(const WatchState&, Canvas&)`). Registry = constexpr
table in `qz_faces` (`faces::registry()`); setting `face` stores the stable numeric id; unknown id
-> id 0. Every face must render: time (12/24h), date (weekday, day, month), steps (+goal), battery,
weather when fresh/stale, sync indicator, time-invalid state, saver/charging marks. Adding a face
= one file + one registry line + scenes + goldens (AGENTS skill).

Registered faces (ids are persisted, never reused): 0 default, 1 minimal, 2 analog (dial and hands,
info spread around the dial), 3 stacked (hours over minutes in the Giant digits), 4 words (time in
words to the nearest five minutes), 5 dashboard (time plus a seven-day step chart), 6 progress (a
ring along the edge fills over the day; sunrise/sunset from the saved location, integer solar
approximation tested against published tables to within 5 min).

## 16. Console protocol v1

Transport: USB-Serial-JTAG CDC (primary IDF console). Lines UTF-8, `\n`-terminated.

| Element | Format |
|---|---|
| Request | `[#<id> ]<command> [args...]` — id 1-8 chars `[A-Za-z0-9]`; args space-separated, `"..."` quoting with `\"` `\\` escapes; max 256 bytes |
| Response | exactly one line: `@QZ1 <id|-> OK <json-object>` or `@QZ1 <id|-> ERR <code> <json-object>` (`{"msg":"..."}`) |
| Events | `@QZ1 ! EVT <json-object>`, e.g. `{"evt":"ready","proto":1,"fw":"1.0.0","git":"abc1234","reset":"deepsleep"}`, `{"evt":"wake",...}`, `{"evt":"detach"}` |
| Everything else | logs; clients ignore lines not starting with `@QZ1 ` |
| Error codes | `bad_args`, `unknown_cmd`, `unsupported`, `invalid_state`, `busy`, `io`, `timeout`, `not_found`, `no_time`, `no_creds`, `battery_low`, `corrupt`, `no_space`, `internal` (= `qz::Errc` tokens, `qz/core/result.hpp`) |
| Limits | response line <= 16 KiB; output serialized by one mutex (logs routed through it) |

Command catalog (all exercised by host tests via the in-process dispatcher and by
`test_apps/console` pytest on device). Flags: S sensitive, D destructive, R needs radio,
U USB console only (refused over the phone link, section 13a).

| Command | Args | Result JSON (main fields) |
|---|---|---|
| `help` | `[cmd]` | `cmds[]` / usage |
| `version` | — | `fw`, `git`, `idf`, `build`{radio,...}, `proto` |
| `status` | — | time, valid, tz, steps, battery{mv,pct,state,usb,charging}, conn{mode,last_sync,indicator}, weather{age_s,state}, screen, power_state |
| `time get` / `time set <ISO-8601 local or Z>` / `time drift` | | `utc`, `local`, `source`, `drift_ppb` |
| `tz list [filter]` / `tz get` / `tz set <IANA name>` | | entries / `name`, `posix` |
| `settings list` / `get <key>` / `set <key> <value>` / `reset` (D) | | key, value, range |
| `btn <menu|back|up|down> [click|hold|repeat]` | | `screen` after handling |
| `steps get` / `steps history` / `steps inject <delta>` / `steps reset-today` (D) | | `today`, `goal`, `history[]` |
| `battery get` / `battery fake <mv>` / `battery fake off` | | `mv`, `pct`, `state` |
| `weather get` / `weather fake <temp_dc> <code> [hi lo]` / `weather push <temp_dc> <code> <hi> <lo> [observed_unix]` / `weather clear` / `weather fetch` (R, U) | | report, `age_s`, `freshness` |
| `phone forget` (D) | — | `forgotten` |
| `wifi status` / `wifi set <ssid> <password>` (S) / `wifi clear` (D) | | `ssid`, `has_password` (never the password) |
| `sync now` (R) / `sync status` | | per-job results, `next_*` |
| `provision start` (R) / `provision stop` | | `ssid`, `expires_s` (password shown on the watch only) |
| `display refresh [full|partial]` / `display dump` / `display crc` | | `crc32`; dump: `w`,`h`,`fmt":"1bpp-msb"`,`b64` |
| `screen list` / `screen get` / `screen show <id>` | | ids / current |
| `face list` / `face set <id>` | | faces |
| `log wakes [n]` / `log clear` | | wake records |
| `diag info|power|radio|rtc|nvs|clock|sensors` | | per page (`sensors`: raw accel x/y/z mg, step counter, panel temperature, pin levels) |
| `selftest list` / `selftest run [suite|test]` | | `results[]` {name, status, ms, detail} |
| `vibrate [ms]` | 1..1000 | — |
| `sleep <seconds>` | 1..3600 | deep sleep even while tethered (one shot; for wake-path tests); response sent before sleeping |
| `reboot` | — | response then `esp_restart` |
| `factory-reset confirm` (D) | literal `confirm` | — |

`tools/qzctl` (lead/WP-owned) reconnects across re-enumeration: waits for the CDC device to
reappear (by USB VID/PID 303a:1001 + serial number), then for the `ready` event, retries the
pending request id once (commands are idempotent or carry ids so duplicates are detected).

## 17. Tether policy

Pure state machine `app::TetherPolicy`.

| State | Entered when | Behaviour | Leaves when |
|---|---|---|---|
| Untethered | USB absent at wake | console never initialized; deep sleep after each wake | USB present at a wake (EXT0 or tick) -> Tethered |
| Tethered | USB present (2 reads 10 ms apart) | console task up, `ready` event, awake; minute ticks via timer, inputs via queue, refresh policy unchanged; radio rules unchanged | USB absent for 2 consecutive 1 s polls -> Detaching; `sleep <s>` -> SleepOnce |
| SleepOnce | `sleep` command | console stops, deep sleep for `s` (timer + normal sources) | next wake re-evaluates USB |
| Detaching | USB lost | stop console, flush logs, plan next tick | -> Untethered (deep sleep) |

Invariant (host-tested): no transition sequence on battery creates the console or delays sleep.
Rationale vs "sleep even when plugged": staying awake while USB-powered costs no battery and
keeps the CDC device stable for agents; deep-sleep behaviour is still testable via `sleep`.

## 18. Self-test, goldens, diagnostics

| Suite (`selftest run <suite>`) | Device checks | Host (same code, fakes) |
|---|---|---|
| `drivers` | SSD1681 reset/BUSY cycle + temperature read; BMA423 chip id, feature config loaded, step counter enabled; ADC battery in 3000..4400 mV; buttons idle level; USB detect + charge pin readable; RTC slow clock = crystal and calibration within ±500 ppm of 32768 Hz; NVS write/read/erase in `qz_test` | fake panel/BMA/ADC/KV |
| `screens` | render every canonical scene (`selftest::scenes()`), CRC32 vs `golden_crc.inc` | PNG goldens compare (exact) |
| `settings` | every key: defaults valid, out-of-range rejected, persist round-trip in `qz_test`, restore | same |
| `time` | TZ table spot checks, TimeKeeper math | full oracle suite in unit tests |
| `interactive` (manual) | each button prompts "press X", vibration pulse "did it buzz? press UP" | — |
| console coverage | — | test fails if a registered command lacks a test case (registry introspection) |

Goldens: `components/qz_selftest/golden/<scene>.png` + `src/golden_crc.inc`, written by the host
golden tool (`--update`), compared exactly in CI. Scenes pin all inputs (time, steps, battery,
weather, version string `1.2.3 (abc1234)` — never the real git hash).

Wake record (16 B, ring of 32 in RTC): `start_utc_s u32`, `awake_ms u16`, `cause u8`, `flags u8`
(partial/full refresh, radio, input, error), `battery_mv u16`, `power_state u8`, `error u8`
(first `Errc`), `steps_delta u16`, `reserved u16`. Daily totals (awake ms per cause) in PowerState.

## 19. Logging

- Pure code logs through `qz/core/log.hpp` (`QZ_LOGE/W/I/D(tag, fmt, ...)` -> installable sink;
  compile-time max level). Platform sink = `esp_log_write`; host sink = capture buffer/stderr.
- Battery builds: `CONFIG_LOG_DEFAULT_LEVEL_WARN`, bootloader log off (boot time); no log output is
  required for correct operation. Tethered: console `log level <lvl>` may raise it at runtime.
- Never log secrets, coordinates with more than 2 decimals, or full HTTP bodies.

## 20. Memory and power budgets

| Resource | Budget | Check |
|---|---|---|
| App image (radio build) | <= 1.6 MiB (slot 3 MiB) | CI size report, fail > budget |
| App image (offline build) | <= 600 KiB | CI size report |
| Static DRAM (.data+.bss) | <= 96 KiB | `idf.py size` |
| Steady-state heap use | 0 B allocated on minute path | sim test + `diag` heap low-water |
| Radio session heap peak | <= 120 KiB free-heap drop | on-device `diag radio` |
| RTC slow memory | <= 7.5 KiB (state ~1.7 KiB + frame 5 KiB) | map file |
| Main task stack | 8 KiB; console task 6 KiB | high-water marks in `diag info` |

Power model (to be replaced by B9 measurements, recorded in `docs/POWER_BUDGET.md`):

| Item | Estimate [ASSUMED] |
|---|---|
| Deep-sleep floor (SoC 8 + divider 8 + BMA423 14 + LDO 2 + reset IC 1.5 + charger 2 + panel) | 25-40 uA [R1 s13] |
| Minute wake: boot + work at 80 MHz | ~70 ms x ~25 mA |
| Panel partial waveform (MCU light sleep) | ~350 ms x ~3-5 mA |
| Time sync session | ~3-6 s x ~100 mA |
| Targets | Off >= 14 d, TimeOnly >= 12 d, TimeWeather (hourly) >= 7 d on 170 mAh (stock Arduino firmware: 5-7 d [R1 s13]) |
