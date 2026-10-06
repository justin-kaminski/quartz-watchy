# Changelog

All notable changes to Quartz are recorded here, per release. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning: [Semantic Versioning](https://semver.org/). Versions stay 0.x until the hardware release
gate in `docs/RELEASE.md` passes. The firmware version is `version.txt`.

## [Unreleased]

## [0.1.0] - 2026-10-06 (first complete build, unverified on hardware)

**Nothing in this release has run on a real Watchy v3.** It builds warning-free in both firmware
variants and is covered by host tests; every hardware-facing behaviour is `[ASSUMED]` until the
owner completes `docs/HARDWARE_BRINGUP.md`. Build: ESP-IDF v6.1 (`.idf-version`).

### Added
- ESP-IDF C++23 firmware for the SQFMI Watchy v3 (ESP32-S3, 200x200 e-paper): minute-tick deep-sleep
  wake planner with wake-ahead, RTC-memory state and frame shadow, SSD1681 panel driver, BMA423 step
  counter, battery policy (Normal/Low/Saver/Critical), settings in NVS.
- Time keeping with drift compensation, an in-house POSIX-TZ engine and a curated zone table
  (139 zones), manual time set via menu and console.
- Two watch faces (`default`, `minimal`) behind an explicit registry; menu, system screens and
  diagnostics; 61 golden images.
- Radio image: Wi-Fi STA, SNTP, HTTPS client, weather (Open-Meteo) and a SoftAP provisioning page.
  Offline image: the same firmware with no Wi-Fi/TLS code (checked by `tools/check_offline.sh`).
- USB console protocol v1 (`@QZ1`, 50 commands) and the host simulator `qz_sim`.
- Self-test suite, virtual-time "a week in seconds" simulation suite, 1348 host tests under ASan+UBSan.
- Developer tooling: `tools/check.sh` gate, CI workflow, agent skills in `.claude/skills/`,
  `docs/POWER_BUDGET.md` model, `tools/release.sh`.

### Known limitations
- Unverified on hardware (see `docs/STATUS.md` tech debt and `docs/FIRST_FLASH.md` "Honest warnings"):
  flicker-free partial refresh (P-11), BMA423 blob authenticity (P-12), deep-sleep floor and GPIO
  holds, USB console behaviour, battery curve, vibration polarity, Wi-Fi/SNTP/portal runtime.
- Battery life numbers are a `MODEL`; no current has been measured (`docs/POWER_BUDGET.md`).
- `tools/qzctl` and on-device pytest suites (WP-29) are not part of this release.
