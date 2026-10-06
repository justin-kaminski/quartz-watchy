# Watchy v3 Custom Firmware

We're building production-grade, simple and straight-forward custom firmware for the SQFMI Watchy v3 (ESP32-S3) e-paper watch. This is professional-grade software that needs to be stable, power-efficient, fully-tested, and real-world deployable on a device worn every day. No hacks, no shortcuts - solid as a rock.

## Dev Workflow

- Work within /mnt/d256a64c-4014-42d9-8f0f-68a0bbb3d11d/builds/newWatchyOS/
- Ask questions only when absolutely necessary - work as close to fully autonomously as possible. I may be away. If possible, push notifs to my mobile app, if not work with best judgement and log tech debt
- IMPORTANT! DO NOT look outside of working directory or use other code on my computer as reference
- Start by identifying a good, simple name. I named this folder newWatchyOS
- Testing is of the utmost importance - use lots of unit tests, run automated testing. All hardware-independent logic must build and pass tests on the host (Linux) with no device attached
- Set up a self-test/diagnostic suite which tests every single feature in the firmware - every driver, every screen, every setting, and the entire device console API. Pair it with golden-image tests for every screen
- Do things properly, this is production-grade and not a hack project
- Create AGENTS.md and relevant skills to support development, and contain concrete development guidelines (build/flash/monitor, adding a watch face, adding a console command, power measurement, release)
- Code style: modern C++ with committed `.clang-format` and `.clang-tidy`, enforced in CI. Follow ESP-IDF component layout conventions
- Create git repository, commit and push to GitHub repo. IMPORTANT: do a code review before committing, and make sure all changes comply with code style, production-grade quality standards, and have been properly tested/have unit tests that have passed where necessary
- Steps that need the physical watch (flashing, power measurement, on-target tests) - tell me exactly what to connect/press, then continue autonomously

## Tech Stack

- **ESP-IDF** (latest stable release, version pinned in repo) with its native CMake build. No Arduino layer
- **C++17** minimum for firmware; C only where vendor code requires it
- FreeRTOS (bundled with ESP-IDF), kept minimal - most wakes run to completion, then deep sleep
- Drivers written in-repo against datasheets: SSD1681 e-paper, buttons, vibration motor, battery ADC (using `esp_adc` calibration), charge/USB detect
- BMA423 via Bosch's official BMA423 SensorAPI (BSD-3) - needed for the step-counter config blob
- Graphics: in-house 1-bpp framebuffer + bitmap fonts generated at build time from permissively-licensed fonts. No LVGL (overkill for a 200x200 mono e-paper, costs RAM/flash)
- Networking: `esp_wifi`, `esp_netif_sntp`, `esp_http_client` over HTTPS with the ESP-IDF certificate bundle, cJSON
- Weather: Open-Meteo (no API key). Free tier is non-commercial only - weather provider must sit behind an interface so it can be swapped if this firmware is ever sold
- Storage: NVS for settings/credentials, RTC slow memory for per-wake state
- Tests: Unity (bundled with ESP-IDF) for on-target, Unity or GoogleTest for host; pytest-embedded for on-target automation
- CI: GitHub Actions using the official `espressif/idf` container - build, host tests, format/lint, binary size report

## Language Research (decision record - keep in repo)

- **ESP-IDF C++ (chosen)** - Espressif's first-party SDK. Full control of deep sleep, RTC clock source, wake sources, and power domains, which matters most on a 200mAh battery. Mature WiFi/SNTP/TLS/NVS/OTA, host-test support, long-term maintenance
- **Arduino + Watchy library / GxEPD2 (rejected)** - community default and fastest to prototype, but the Arduino layer hides power and clock configuration, mixes hardware access into application logic, and makes host testing hard. Not the right base for a production product
- **Rust - esp-hal / esp-idf-hal (runner-up)** - memory safety is a real win, but the S3 is Xtensa and needs Espressif's forked toolchain via `espup`, and there is no mature BMA423 step-counter or SSD1681 driver meeting this bar. Same drivers would be written anyway, plus porting the Bosch blob
- **MicroPython / CircuitPython (rejected)** - interpreter start-up on every deep-sleep wake costs time and battery; with a wake every minute this dominates the power budget
- **Zephyr (rejected)** - ESP32-S3 support exists, but deep-sleep/power management and peripheral coverage lag ESP-IDF on this chip. More porting for no gain

## Hardware - Watchy v3 (verify against the v3.0 schematic before writing any driver)

- SoC: ESP32-S3FN8 - 8MB flash, no PSRAM, native USB CDC/JTAG
- RTC: **no external RTC chip** - timekeeping uses the ESP32-S3 RTC timer clocked from an external 32.768kHz crystal. Slow clock must be configured to the external crystal. Time survives deep sleep but not power loss/brownout - track a "time valid" flag
- Display: GDEY0154D67, 1.54" 200x200 B/W, SSD1681 controller, SPI
- Accelerometer: BMA423, I2C
- 4 buttons, vibration motor, 200mAh LiPo, battery ADC, charge status, USB detect
- Pin map (per upstream Watchy `config.h`, v3):

| Function    | GPIO | Function        | GPIO |
| ----------- | ---- | --------------- | ---- |
| I2C SDA     | 12   | Display CS      | 33   |
| I2C SCL     | 11   | Display DC      | 34   |
| MENU button | 7    | Display RESET   | 35   |
| BACK button | 6    | Display BUSY    | 36   |
| UP button   | 0    | SPI MOSI        | 48   |
| DOWN button | 8    | SPI SCK         | 47   |
| Accel INT1  | 14   | SPI MISO        | 46   |
| Accel INT2  | 13   | Vibration motor | 17   |
| Battery ADC | 9    | Charge status   | 10   |
| USB detect  | 21   |                 |      |

- GPIO0 (UP) is a boot strapping pin - holding UP during reset enters ROM download mode. Document as the recovery path; don't fight it

## Basic Architecture

- Single firmware image, layered: board (pins/config) → drivers → services (time, steps, weather, power, settings, connectivity) → UI (screens, watch faces) → app (wake dispatcher)
- Hardware-independent core (time/date math, timezone/DST handling, step day rollover, weather parsing, settings validation, UI state machine, rendering) is plain C++ with no ESP-IDF dependency, so it compiles and is tested on host
- Deep-sleep driven: wake on RTC timer (minute tick), buttons (EXT1), or accelerometer interrupt → do work → sleep. Measure and track time-awake per wake
- Partition table with two app slots and rollback-capable bootloader, so OTA can be added later without repartitioning. v1 updates via USB only
- Host simulator: builds core + UI for Linux, renders any screen to PNG, accepts scripted inputs (time, buttons, steps, weather, battery). Lets humans and agents iterate on watch faces without hardware
- Firmware must be fully controllable by AI agents - I should be able to ask you to build me a new watch face and you should have all the tools to build, flash, drive, screenshot and verify it without my intervention:
  - USB serial console (`esp_console`) to set time, inject button presses, fake steps/weather/battery, change settings, trigger sync, dump the framebuffer, read logs and diagnostics
  - Console only runs while USB is connected; it must never keep the watch awake on battery
  - Host tooling handles USB CDC disappearing/reappearing across deep sleep

## Watch Face

- Time (12/24h), date (weekday, day, month), today's step count, battery indicator
- Weather/temp when enabled and data is fresh; stale data clearly marked or hidden
- Sync status indicator (time never synced / last sync failed)
- Partial refresh each minute, periodic full refresh to clear ghosting, no visible flicker at minute boundary
- Designed for 200x200 1-bit - readable at a glance

## Step Tracking

- BMA423 hardware step counter, not software step detection
- Axis remap for the watch's mounting orientation
- Daily reset at local midnight - timezone-aware, correct across DST changes and time jumps from sync
- 7-day history in RTC memory, flushed to NVS at most once per day to limit flash wear
- Optional daily step goal

## Time & Date

- Timezone via POSIX TZ string (handles DST), selected from a built-in list
- Manual time/date setting via buttons - fully functional with radio off
- SNTP sync when WiFi enabled, configurable interval
- Measure RTC drift between syncs and compensate

## Weather (optional, opt-in)

- Current temperature + simple condition icon, optional daily high/low, °C/°F
- Location set manually (lat/lon) during setup - no IP geolocation by default
- Configurable fetch interval; skipped when battery is low
- HTTPS with certificate validation, hard timeouts. A failed fetch never blocks or blanks the watch face

## Connectivity

- Three modes: **Off**, **Time only**, **Time + Weather**
- **Off means off** - WiFi driver is never initialized, not merely disconnected. Verify by power measurement
- Kconfig build option to compile the radio stack out entirely for an offline-only build
- Setup via ESP-IDF provisioning with proof-of-possession, or a time-limited, password-protected SoftAP page - SSID/password, timezone, location, units. USB console as alternative
- Hard timeout on every connection attempt, exponential backoff on failure - never retry on every wake
- Credentials never logged or printed. Stored in NVS; evaluate NVS + flash encryption for release builds (affects reflashing - document the trade-off)
- Factory reset (wipe NVS) via menu and console

## Power

- Battery life is a first-class requirement, not an afterthought
- Measure with a power profiler; establish a baseline per connectivity mode and record the power budget in repo. Regressions block merge
- Low battery: disable radio first, then reduce refresh. Critical: show a "charge me" screen and sleep with button-only wake
- Battery % from a LiPo voltage curve, using calibrated ADC readings

## UI / Navigation

- MENU / BACK / UP / DOWN driven menu: time/date, timezone, 12/24h, units, connectivity mode, weather on/off, sync now, step goal, diagnostics, about (version + git hash)
- Optional vibration feedback

## Release

- Semantic versioning, git hash embedded, pinned ESP-IDF version for reproducible builds
- Flash via USB (`idf.py` / esptool); document recovery via UP-button download mode
- Changelog per release

## Out of Scope (v1)

- BLE / phone notifications, third-party apps, WiFi OTA delivery, heart rate (no sensor on board)
