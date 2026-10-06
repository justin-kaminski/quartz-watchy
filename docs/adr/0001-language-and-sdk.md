# ADR-0001: Language and SDK - ESP-IDF with C++

Status: accepted (2026-10-05). Source: docs/SPEC.md "Language Research".

## Context
Watch firmware that wakes every minute from deep sleep on a ~170-200 mAh battery, needs exact
control of the RTC clock source, wake sources and power domains, reliable Wi-Fi/SNTP/TLS/NVS, and
hardware-independent logic testable on a Linux host. Hardware: ESP32-S3 (Xtensa), SSD1681 e-paper,
BMA423 accelerometer.

## Options considered
- **ESP-IDF in C++ (chosen).** Espressif's first-party SDK exposes deep sleep, RTC clock selection,
  wake sources and power domains directly; mature Wi-Fi, SNTP, TLS, NVS and OTA; host-test support;
  long-term maintenance by the chip vendor.
- **Arduino + Watchy library / GxEPD2 (rejected).** Fastest to prototype and the community default,
  but the Arduino layer hides power and clock configuration, mixes hardware access into application
  logic and makes host testing hard; not a production base.
- **Rust (esp-hal / esp-idf-hal) (runner-up).** Memory safety is a real gain, but the S3 is Xtensa
  and needs Espressif's forked toolchain (`espup`), and no mature BMA423 step-counter or SSD1681 driver
  meets this bar: the same drivers would be written anyway, plus a port of the Bosch configuration blob.
- **MicroPython / CircuitPython (rejected).** Interpreter start-up on every deep-sleep wake costs time
  and energy; with a wake per minute it dominates the power budget.
- **Zephyr (rejected).** ESP32-S3 support exists, but deep-sleep/power management and peripheral
  coverage lag ESP-IDF on this chip: more porting for no gain.

## Decision
ESP-IDF (pinned v6.1) with its native CMake build, modern C++ (C++23 code, see DECISIONS.md D-03),
FreeRTOS kept minimal; C only for vendor code (Bosch BMA423 SensorAPI). No Arduino layer.

## Consequences
- Drivers (SSD1681, buttons, vibration, battery ADC, USB detect) are written in-repo from datasheets.
- Hardware-independent code is plain C++ behind HAL interfaces, built and tested on the host.
- Toolchain is Espressif's GCC for Xtensa (15.2 in v6.1); host builds use system GCC/Clang.
