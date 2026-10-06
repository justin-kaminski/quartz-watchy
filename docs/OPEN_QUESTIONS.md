# Open questions and assumptions

Defaults chosen are implemented unless the owner overrides them. "Owner" = needs a decision;
"Assumed" = engineering assumption to confirm (bring-up step in brackets).

## Contents
1. [Owner decisions](#1-owner-decisions)
2. [Assumptions to confirm](#2-assumptions-to-confirm)
3. [Risks](#3-risks)

## 1. Owner decisions

| Id | Question | Default in the plan |
|---|---|---|
| Q-01 | What should an accelerometer wake do? | `tapwake` off; when on, double-tap -> 5 s status overlay |
| Q-02 | When may full refreshes (which flash) happen? | every N = 30 partial updates [TUNE in B5; vendor says 5] + on leaving menus; alternative: only at night/after inactivity |
| Q-03 | First-boot defaults | connectivity Off, 24 h, Celsius, face 0, vibration on, goal off, UTC |
| Q-04 | NVS encryption (HMAC eFuse key, irreversible burn) / flash encryption for release builds | both off (ARCH s13 trade-off) |
| Q-05 | SNTP servers | `pool.ntp.org`, `time.google.com` (Kconfig strings) |
| Q-06 | Date format / language | English names, "MON 05 OCT" style; no locale setting in v1 |
| Q-07 | Saver level: display updates every 5 min (minutes still exact at each update) | yes |
| Q-08 | Built-in time zone list size (~120-160 curated entries) | as ARCH s9 |
| Q-09 | Version numbering | 0.x until the release gate passes on hardware, then 1.0.0 |
| Q-10 | Weather provider if the firmware is ever sold (Open-Meteo free tier is non-commercial) | interface ready; no second provider in v1 |
| Q-11 | Goal reached vibration | single short buzz once per day if vibration is on |
| Q-12 | If E1 shows no OTP partial waveform: may we use Good Display's published reference LUT bytes (vendor data, not driver-library code)? | blocked until answered; full refresh fallback only |
| Q-13 | Authenticity of the BMA423 config blob (official Bosch repo gone; vendored from SQFMI's fork, see P-12): ask Bosch Sensortec support for the official driver to compare SHA-256 | vendor `sqfmi/BMA423-Sensor-API@bma423_v2.14.13`, unmodified |

## 2. Assumptions to confirm

| Id | Assumption | Confirm in |
|---|---|---|
| A-01 | Deep-sleep wake with UP held boots normally (strapping latched only at chip reset) | B4 |
| A-02 | RTC timer and RTC_NOINIT memory survive the USB-JTAG reset used by flashing | B1/B10 |
| A-03 | IDF falls back to an internal oscillator if the 32 kHz crystal fails, and reports it | WP-24 code reading + B2 |
| A-04 | RtcState (~1.7 KiB) + FrameShadow (5 KiB) fit RTC slow memory next to IDF's own use | WP-24 map file |
| A-05 | Wake latency (timer -> app) ~100-300 ms with validation skipped and boot logs off | B5/B9 |
| A-06 | SSD1681 partial waveform ~300-500 ms; full ~2-3 s; timings in ssd1681.md | WP-07, B5 |
| A-07 | Open-Meteo request fields and response shape as in ARCH s12 | WP-12 (docs check) |
| A-08 | EXT0 on USB detect costs < 2 uA extra in deep sleep | B9 P6 |
| A-09 | BMA423 step counter needs no periodic host service in deep sleep | WP-08, B7 |
| A-10 | Battery thresholds 3.4/3.5/3.6 V map to sensible remaining capacity | B6 |
| A-11 | QEMU (`idf.py qemu`, esp32s3 listed in tools/idf_py_actions/qemu_ext.py) can run pure Unity tests and the golden-CRC check in CI; no RTC/sleep fidelity | WP-29 |
| A-12 | `espressif/cjson` ^1.7.19 from the component registry (IDF 6 removed `json`) | WP-28 |

## 3. Risks

| Id | Risk | Mitigation |
|---|---|---|
| R-01 | 32 kHz crystal load-cap conflict (10 vs 18 pF, hardware.md s10) -> start failure or ppm error | runtime clock check, `clock_degraded` flag, drift compensation, B8 |
| R-02 | ADC above 3.7 V uncharacterised | thresholds below 3.7 V; per-unit curve from B6 |
| R-03 | Partial waveform undocumented (P-11); panel endurance unpublished (P-05) | E1 experiment in B5; Q-12; tunable N; night 5-min mode as fallback |
| R-04 | Over-discharge (no protection IC) | Critical suspends sensors; user guidance |
| R-05 | IDF 6.1 is new: API churn vs examples found online | everything verified against the pinned tree; WPs cite paths |
| R-06 | Bit-exact goldens across host/Xtensa compilers | integer-only rendering rule; QEMU CRC check (A-11) |
| R-07 | Usage budget: 30 WPs | waves ordered by value; M1-M3 reachable with ~12 WPs |
