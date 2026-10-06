# Quartz component list

Status: **stable contract** for the skeleton (v1, 2026-10-05). Changes are appended
in the "Changes" section at the bottom, never edited in place.

## Contents
1. [Rules every component follows](#1-rules-every-component-follows)
2. [Component table](#2-component-table)
3. [Dependency graph](#3-dependency-graph)
4. [Third-party code](#4-third-party-code)
5. [Directory layout inside a component](#5-directory-layout-inside-a-component)
6. [What the skeleton must provide](#6-what-the-skeleton-must-provide)
7. [Changes](#7-changes)

## 1. Rules every component follows

- Name `qz_<name>`; C++ namespace `qz::<name>` (e.g. `qz::time`), public headers at
  `components/qz_<name>/include/qz/<name>/*.hpp`, included as `#include "qz/<name>/x.hpp"`.
- **Pure** = dual-mode (IDF component *and* host static library through
  `cmake/qz_component.cmake`). Pure code never includes ESP-IDF, FreeRTOS, newlib-specific
  or `sdkconfig.h` headers and never reads `CONFIG_*` macros. Build-time options reach pure
  code as runtime values (`qz::app::BuildFeatures`, filled in `main/` from Kconfig).
- **IDF-only** components implement `qz_hal` interfaces (and the radio) on ESP-IDF.
  They are never built on the host.
- A component may only depend on components listed in its "Deps" column (enforced by
  REQUIRES/PRIV_REQUIRES on IDF and `target_link_libraries` on host; a host CI check greps
  includes against this table).
- Rendering and all code feeding golden images is integer-only (no `float`/`double`), so
  host and Xtensa framebuffers are bit-identical (CRC32 comparison on target).

## 2. Component table

Layer numbers: 0 foundation, 1 HAL/board, 2 drivers + pure libraries, 3 services,
4 UI/console/self-test, 5 app, P platform (IDF-only), T test support.

| Component | Kind | L | Purpose (one line) | Deps (qz) | Extra deps |
|---|---|---|---|---|---|
| `qz_core` | pure | 0 | Error/`Result<T>` value types, units, fixed-capacity containers, CRC32, assert policy, small utilities | — | — |
| `qz_hal` | pure | 1 | Abstract HAL interfaces: EPD bus, I2C, digital pins, ADC, KV store, RTC memory, clocks, sleep/wake control, system info, network stack | core | — |
| `qz_board` | pure (header-mostly) | 1 | Watchy v3 board description: pin map, polarities, RTC-GPIO/strapping facts, ADC divider; constexpr wake-mask helpers with static checks | core | — |
| `qz_time` | pure | 2 | 64-bit UTC/civil calendar math, POSIX-TZ engine, generated built-in TZ list, formatting, `TimeKeeper` (time-valid, drift estimate/compensation) | core, hal | — |
| `qz_model` | pure (header-mostly) | 2 | Shared domain vocabulary: settings enums, battery/power/sync/weather/step value types, wake causes, wake records, input events | core, time, hal | — |
| `qz_gfx` | pure | 2 | 1-bpp 200x200 framebuffer, primitives, bitmap-font format + build-time generated fonts, text layout, deterministic PNG encoder | core | python3 (font generator at build time) |
| `qz_ssd1681` | pure | 2 | SSD1681/GDEY0154D67 panel driver logic (init, RAM windows, full/partial update, deep sleep, BUSY timeouts) over `hal::EpdBus` | core, hal, gfx | — |
| `qz_bma423` | pure | 2 | BMA423 wrapper over vendored Bosch SensorAPI (C): init + config blob, axis remap, step counter, wake interrupt config | core, hal | Bosch BMA423 SensorAPI (vendored, BSD-3) |
| `qz_settings` | pure | 3 | Settings schema (keys, types, ranges, defaults), validation, NVS persistence + schema versioning, credential store with redaction | core, hal, time, model | — |
| `qz_steps` | pure | 3 | Step-day accounting: hardware counter deltas/wrap/reset, local-midnight rollover (DST/time jumps), 7-day history, once-per-day NVS flush | core, hal, time, model | — |
| `qz_power` | pure | 3 | Battery voltage filter + LiPo curve (%), power policy state machine (normal/low/critical, hysteresis), awake-time accounting, power estimate | core, model | — |
| `qz_weather` | pure | 3 | Weather provider interface, Open-Meteo request builder + response parser, WMO-code mapping, freshness/stale rules | core, hal, time, model | cJSON (`espressif/cjson` on IDF, pinned FetchContent on host) |
| `qz_conn` | pure | 3 | Connectivity state machine: mode policy, sync scheduling, exponential backoff, hard time budgets, sync-session orchestration (Wi-Fi -> SNTP -> weather -> teardown), provisioning session logic | core, hal, time, model, weather | — |
| `qz_ui` | pure | 4 | UI framework + system screens: input gesture recognizer, screen stack/navigation, menu model, settings editors, icons, `WatchState` snapshot and UI actions | core, gfx, time, model, settings | — |
| `qz_faces` | pure | 4 | Watch faces and the explicit face registry table | core, gfx, model, ui | — |
| `qz_console` | pure | 4 | Command registry, line protocol v1 (sentinel + JSON), arg parsing, JSON writer, full command catalog bound to the `console::DeviceApi` interface | core, model, settings, time | — |
| `qz_selftest` | pure | 4 | Self-test framework (registry/runner/report), canonical UI scenes (fixtures), golden CRC table, driver/settings/screen checks written against HAL + drivers | core, hal, time, model, gfx, ui, faces, settings, ssd1681, bma423, power | — |
| `qz_app` | pure | 5 | Wake dispatcher, RTC state block (magic/version/CRC), wake-record ring, tether policy, sleep planner, service wiring, `DeviceApi` implementation, snapshot builder, action executor | all pure except testkit | — |
| `qz_testkit` | pure, test-only | T | Fakes: every HAL interface, SSD1681 panel model (RAM + command protocol -> image), BMA423 register model, virtual clock, fake network; never linked into firmware | core, hal, gfx | — |
| `qz_platform` | IDF-only | P | ESP-IDF implementations of `qz_hal` (SPI EPD bus, I2C master, GPIO/RTC-GPIO, `esp_adc` oneshot + calibration, NVS, RTC memory, RTC clock, deep sleep/EXT0/EXT1/hold, system), console binding to `esp_console` over USB-Serial-JTAG | core, hal, board, console | esp_driver_spi, esp_driver_i2c, esp_driver_gpio, esp_adc, nvs_flash, esp_hw_support, esp_system, esp_timer, console, esp_driver_usb_serial_jtag, esp_app_format, log |
| `qz_net` | IDF-only | P | `hal::NetStack` on ESP-IDF: Wi-Fi STA with hard timeouts, SNTP (`esp_netif_sntp`), HTTPS GET (`esp_http_client` + certificate bundle), SoftAP provisioning page (`esp_http_server`); stubs when `CONFIG_QZ_RADIO=n` | core, hal | esp_wifi, esp_netif, esp_event, esp_http_client, esp-tls, mbedtls, esp_http_server, nvs_flash |
| `main` | IDF app | — | `app_main`: build `BuildFeatures` from Kconfig, construct platform + app, run one wake (or the tethered loop), sleep | app, platform, net | — |

Not components (lead-owned skeleton, implemented by work packages): `host/` (host CMake project:
all pure components + GoogleTest + simulator `host/sim/` + golden tool), `test_apps/` (IDF
Unity test apps), `tools/` (bootstrap, `qzctl`, font/TZ generators, size report).

## 3. Dependency graph

Arrows point from dependent to dependency (transitive edges omitted).

```
main ──> qz_app ──> qz_selftest ──> qz_faces ──> qz_ui ──> qz_settings ──> qz_model ──> qz_time ──> qz_hal ──> qz_core
  │         │            │                         │                                     ▲
  │         │            ├──> qz_ssd1681 ──> qz_hal │                                     │
  │         │            ├──> qz_bma423  ──> qz_hal └──> qz_gfx ──> qz_core               │
  │         ├──> qz_console ──> qz_settings                                                │
  │         ├──> qz_conn ──> qz_weather ──> qz_model                                       │
  │         ├──> qz_steps, qz_power ──> qz_model ───────────────────────────────────────────┘
  ├──> qz_platform ──> qz_hal, qz_board, qz_console
  └──> qz_net ──> qz_hal
qz_testkit ──> qz_hal, qz_gfx        (host tests, simulator, test_apps only)
```

Forbidden: anything below layer 5 depending on `qz_app`; `qz_ui`/`qz_faces` depending on
any service component (they see services only through `qz_model` values and UI actions);
pure components depending on `qz_platform`/`qz_net`; `qz_console` depending on `qz_app`
(the app implements `console::DeviceApi`, not the reverse).

## 4. Third-party code

| Code | Where | How pinned | License |
|---|---|---|---|
| Bosch BMA423 SensorAPI (`bma4.c/.h`, `bma423.c/.h`, `bma4_defs.h`) | `components/qz_bma423/third_party/bosch/` (unmodified, plus `LICENSE` and `PROVENANCE.md` with URL + commit + SHA-256 of each file) | exact upstream commit | BSD-3-Clause |
| cJSON | device: `espressif/cjson` via `main/idf_component.yml` (the built-in IDF `json` component was removed in v6.0, see `.toolchain/esp-idf/docs/en/migration-guides/release-6.x/6.0/protocols.rst`); host: FetchContent of the same upstream version, URL + hash pinned | `dependencies.lock` committed; same version both sides | MIT |
| GoogleTest | host only, FetchContent URL + SHA-256 | pinned release | BSD-3-Clause |
| Fonts (BDF sources) | `components/qz_gfx/fonts/<family>/` with license file | file SHA-256 in `PROVENANCE.md` | permissive only (see ARCHITECTURE.md) |
| IANA tzdata (generator input only) | not stored; generated table committed | tzdata release version recorded in the generated file header | public domain |

No code from GxEPD2, Adafruit, the Watchy Arduino library or any other driver library.

## 5. Directory layout inside a component

```
components/qz_<name>/
  CMakeLists.txt          # lead: qz_component(...) helper call; sources = src/*.c / src/*.cpp (globbed)
  include/qz/<name>/*.hpp # public API (planner-owned contracts)
  src/*.cpp, src/*.hpp    # implementation + private headers (WP-owned)
  test/*_test.cpp         # GoogleTest host tests (pure components only; built by host/ only)
  Kconfig                 # only qz_platform / qz_net / qz_app-facing options, if any
```

Golden images live in `components/qz_selftest/golden/*.png` with the generated CRC table
`components/qz_selftest/src/golden_crc.inc` (both committed, regenerated by the host golden
tool, verified in CI).

## 6. What the skeleton must provide

1. `cmake/qz_component.cmake`: one call declares a component once; under IDF it maps to
   `idf_component_register(SRC_DIRS src INCLUDE_DIRS include PRIV_INCLUDE_DIRS src REQUIRES ...)`,
   on host to `add_library(qz_<name> STATIC ...)` with the same sources/includes and
   `target_link_libraries` from the same dependency list. Work packages add `.cpp` files
   without touching CMake (`SRC_DIRS`/glob with `CONFIGURE_DEPENDS`).
2. Per-component host test executables `qz_<name>_test` from `test/*_test.cpp`, registered with
   CTest, linked with GoogleTest + `qz_testkit`.
3. Hooks for two build-time generators run with `python3` (stdlib only): font generation in
   `qz_gfx` (BDF -> `.cpp` in the build dir), and a "check generated file is current"
   test for the committed TZ table.
4. Third-party mapping in the helper: `qz_weather` links cJSON (`espressif__cjson` on IDF,
   FetchContent target on host); `qz_bma423` compiles `third_party/bosch/*.c` as C (warnings
   relaxed for vendor files only).
5. Firmware compile flags: C++ standard per ADR-0004 (`gnu++23` on IDF, `-std=c++23` on host;
   fallback C++20 if either compiler rejects a used feature), `-fno-exceptions -fno-rtti`
   (IDF Kconfig defaults; host tests compile pure code with the same flags; GoogleTest
   itself is built with its defaults).
6. `main/Kconfig.projbuild` "Quartz" menu (symbols listed in SDKCONFIG.md).

## 7. Changes

- 2026-10-05 (planner, while writing the interface headers): two direct-include edges that were
  only transitive in the table above. `qz_model` includes `qz/hal/board_io.hpp` (button bit
  constants) -> add **qz_hal** to `qz_model` deps. `qz_console` includes `qz/time/tz.hpp`
  (`DeviceApi::timezone()`) -> add **qz_time** to `qz_console` deps. No component added or removed.
- 2026-10-05 (lead): reconciled the table rows above with the two entries above, and added a third
  edge found by `tools/check_deps.py`: `qz_ssd1681` includes `qz/gfx/framebuffer.hpp` (the panel API
  takes a `gfx::Framebuffer`) -> add **qz_gfx** to `qz_ssd1681` deps. gfx depends only on core, so
  there is no cycle. Table rows now carry the dependencies; this log is history.
