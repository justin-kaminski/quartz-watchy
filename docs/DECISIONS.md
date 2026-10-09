# Decision log

ADR-0001 (`docs/adr/0001-language-and-sdk.md`) records the language/SDK choice. Every other decision
is one row here; details live in the ARCHITECTURE.md section cited.

| Id | Decision | Why (short) | Where |
|---|---|---|---|
| D-01 | ESP-IDF + C++, no Arduino | power/clock control, host tests, vendor support | ADR-0001 |
| D-02 | ESP-IDF v6.1 pinned (`.idf-version`), in-tree toolchain, CI container `espressif/idf:v6.1` | reproducible builds | COMPONENTS s6 |
| D-03 | Code is valid C++23; target compiled with IDF's `-std=gnu++26` (`tools/cmake/build.cmake`), host with `-std=c++23` | `std::expected`, `<=>`, constexpr; meets "C++17 minimum"; GCC 15.2 (Xtensa), GCC 16 / Clang 23 (host), Ubuntu 24.04 GCC in the IDF image all support it | ARCH s2 |
| D-04 | Errors as values: `qz::Result<T>`/`Status`, `[[nodiscard]]` wrapper over `std::expected<T, qz::Error>`; closed `Errc` = console tokens | ignored errors fail the build; one error vocabulary end to end | `qz/core/result.hpp` |
| D-05 | Dual-mode pure components + HAL interfaces + `qz_testkit` fakes; IDF-only code limited to `qz_platform`/`qz_net`/`main` | everything above the HAL runs on the host | COMPONENTS |
| D-06 | UI renders a `WatchState` snapshot and emits `Action`s; console uses `DeviceApi`; neither calls services | golden tests and console tests need no services | ARCH s1, s15 |
| D-07 | Own `TimeKeeper` on raw RTC microseconds with ppb drift correction; libc time pushed for TLS | explicit drift compensation, no hidden IDF time adjustments | ARCH s8 |
| D-08 | Wake-ahead: render the upcoming minute and align the waveform midpoint to :00 (EWMA latency) | minute flips on the boundary | ARCH s8.4 |
| D-09 | In-house POSIX-TZ engine, glibc oracle on host, curated table generated from IANA TZif footers; settings store IANA name + POSIX fallback | DST correctness without a tz database on device | ARCH s9 |
| D-10 | Custom time-limited WPA2 SoftAP page (+ USB console), not `network_provisioning` | IDF 6 removed `wifi_provisioning`; one form for creds + TZ + location + units, no phone app | ARCH s13 |
| D-11 | cJSON from the `espressif/cjson` managed component (lock file committed), same version via FetchContent on host | IDF 6 removed the built-in `json` component | COMPONENTS s4 |
| D-12 | Tether policy: awake and console only while USB is present; `sleep <s>` for deep-sleep tests | stable CDC for agents, zero battery cost | ARCH s17 |
| D-13 | Console protocol v1: one-line `@QZ1` responses with request ids + JSON, events, logs ignored | robust to log interleaving and re-enumeration | ARCH s16 |
| D-14 | Previous frame kept in RTC memory (`FrameShadow`); MCU light-sleeps while the panel is BUSY | partial updates survive resets; ~20x less energy while waiting | ARCH s14 |
| D-15 | Settings cached in `RtcState`; NVS initialized lazily; no NVS access on the minute path | wake time and flash wear | ARCH s6, s7 |
| D-16 | `RtcState` in `RTC_NOINIT` memory, validated by magic/version/size/CRC32; layout pinned by test | survives panics and resets, never trusted blindly | ARCH s6 |
| D-17 | Buttons (+INT1) on EXT1 ANY_LOW, USB detect on EXT0 (Kconfig `QZ_USB_WAKE`) | single-polarity EXT1 on S3; instant tether | ARCH s5 |
| D-18 | Steps: hardware counter never reset; software baseline; forward-only local-day rollover | robust to resets, DST and time jumps | ARCH s10 |
| D-19 | Power levels Normal/Low/Saver/Critical with hysteresis, thresholds below the ADC's calibrated limit | brief's degrade order; measurable thresholds | ARCH s11 |
| D-20 | Connectivity: one session (connect -> SNTP -> weather -> full deinit) with hard budgets and capped exponential backoff with jitter | "never retry every wake", Off means off | ARCH s12 |
| D-21 | Radio compile-out via `CONFIG_QZ_RADIO` + stubs + `BuildFeatures`; CI checks the offline image with `nm` | "Kconfig compile-out" verifiable | SDKCONFIG s3 |
| D-22 | Goldens: deterministic in-repo PNG encoder; device compares framebuffer CRC32 against a generated table; scenes use fixed version strings | same render code on both sides | ARCH s18 |
| D-23 | Fonts: permissive BDF sources converted by a stdlib-only Python generator at build time | deterministic, no FreeType dependency | ROADMAP WP-06 |
| D-24 | No heap on steady-state wakes; heap allowed in radio, provisioning, console | deterministic wakes | ARCH s2 |
| D-25 | Host tests GoogleTest; on-target Unity + pytest-embedded; QEMU (esp32s3 supported by `idf.py qemu` in v6.1) for pure Unity tests and the golden-CRC check in CI | coverage without hardware where possible | TEST_PLAN |
| D-26 | Partitions: table at 0x10000, NVS 64 KiB, otadata, coredump, 2 x 3 MiB OTA slots, spare 1.75 MiB; rollback enabled | OTA later without repartitioning | PARTITIONS |
| D-27 | NVS/flash encryption off by default; HMAC-based NVS encryption documented as the opt-in | irreversible eFuse burns need the owner (Q-04) | ARCH s13 |
| D-28 | 80 MHz CPU, QIO 80 MHz flash, bootloader logs off, image validation skipped on deep-sleep wake | wake energy | SDKCONFIG |
| D-29 | On-demand Bluetooth LE phone link + a Web Bluetooth page (no native app); NimBLE, passkey pairing shown on the watch, bonding; started only from the watch menu; `phone` setting + `CONFIG_QZ_PHONE` compile-out. Refines D-10: still no app to install. | the phone can push time and weather (Wi-Fi is the costliest thing the watch does); zero idle cost; one page serves Android and desktop | ARCH s13a |
