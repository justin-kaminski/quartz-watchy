# Power budget

Battery life is a first-class requirement (docs/SPEC.md "Power"). This file is the single record of
the power model and, once the owner runs `docs/HARDWARE_BRINGUP.md` B9, of the measured baseline.
**Every number carries a tag.** `MODEL` = computed from assumptions, never measured. `MEASURED` =
read from a power profiler on a real watch (date, git hash, profiler and firmware variant recorded).
Nothing here is `MEASURED` yet: do not judge battery life from this file until it is.

Capacity: 170 mAh (cell minimum; marketing says 200) [docs/PUSHBACK.md P-08]. Supply for measurement: 3.80 V.

## 1. Model

### 1a. ARCHITECTURE section 20 itemised model - `MODEL` [ASSUMED]
| Item | Value | Source |
|---|---|---|
| Deep-sleep floor | 25-40 uA (midpoint 32.5 uA = 0.78 mAh/day) | ARCHITECTURE s20, hardware research s13 |
| Minute wake (boot + work at 80 MHz) | 70 ms x 25 mA = 1.75 mAs | ARCHITECTURE s20 |
| Panel partial waveform (MCU light sleep) | 350 ms x 4 mA (3-5 mA) = 1.4 mAs | ARCHITECTURE s20 |
| Per minute tick | 3.15 mAs; x 1440 = 4536 mAs = 1.26 mAh/day | arithmetic |
| Time sync session | 4.5 s x 100 mA = 450 mAs (3-6 s) | ARCHITECTURE s20 |
| Weather session | 5 s x 100 mA = 500 mAs | `[ASSUMED]` same as time sync |
Excluded: periodic full refreshes (2 s waveform), button sessions, step flushes, USB time.

### 1b. Virtual-time estimate - `MODEL` [TUNE]
`qz::power::estimate_hours` (components/qz_power) with `sleep_floor_ua = 50`, `active_ma = 25`
applied to the *whole* simulated awake time (waveform included), printed by
`build/host/qz_app_test --gtest_filter='SimBudget*:*Connectivity*'` (`[sim]` lines;
components/qz_app/test/sim_scenarios_test.cpp, sim_connectivity_test.cpp). Run on 2026-10-06
(git 6afe6f2, one simulated day / week): timer wake mean 442-451 ms, awake 643-650 s/day,
estimate 713-720 h (29-30 d) for Off, TimeOnly and TimeWeather. It is pessimistic for the minute
path (charges 25 mA across the panel wait) and ignores radio current (100 mA), so it understates
radio modes. It exists to catch regressions in awake time, not to predict battery life.

## 2. Per-connectivity-mode budget
Days = 170 mAh / total mAh/day. Model rows use 1a: floor 0.78 + minute ticks 1.26 mAh/day, plus
radio sessions (`sync_h 6` = 4 time syncs/day; weather hourly = 24 sessions/day).

| Mode | Target | Model mAh/day | Model days | Measured avg current | Measured days | Tag |
|---|---|---|---|---|---|---|
| Off (`conn off`, radio build) | >= 14 d | 2.04 | 83 | NOT MEASURED | NOT MEASURED | MODEL |
| Off (offline build) | >= 14 d, == radio Off | 2.04 | 83 | NOT MEASURED | NOT MEASURED | MODEL |
| TimeOnly (`conn time`, `sync_h 6`) | >= 12 d | 2.54 (+0.50) | 67 | NOT MEASURED | NOT MEASURED | MODEL |
| TimeWeather (`conn time+weather`, hourly) | >= 7 d | 5.87 (+0.50 +3.33) | 29 | NOT MEASURED | NOT MEASURED | MODEL |
| TimeWeather (`wx_min 30`) | informational | 9.21 (+0.50 +6.67) | 18 | NOT MEASURED | NOT MEASURED | MODEL |
| Critical (button/USB wake only) | informational | floor ~20 uA = 0.48 | 354 | NOT MEASURED | NOT MEASURED | MODEL |
| Menu session, 30 s | informational | ~25 mA x 30 s = 750 mAs | n/a | NOT MEASURED | n/a | MODEL |

The model days exceed the targets by 3x or more because the targets assume the stock-firmware
class of numbers (5-7 d). The first measurement decides which is right; expect the floor (P1) to
dominate the difference.

## 3. Measured baseline (fill from B9; this table is the release-gate reference)
| Run | What | Value | Tag | Date | Git | Profiler | Variant |
|---|---|---|---|---|---|---|---|
| P1 | sleep floor, `conn off` | NOT MEASURED uA | MODEL | | | | radio |
| P1 | charge per minute wake | NOT MEASURED uC | MODEL | | | | radio |
| P1 | 10 min average | NOT MEASURED uA | MODEL | | | | radio |
| P2 | sleep floor, offline build | NOT MEASURED uA | MODEL | | | | offline |
| P3 | time sync session | NOT MEASURED uC / s | MODEL | | | | radio |
| P3 | 1 h average, `conn time`, `sync_h 6` | NOT MEASURED uA | MODEL | | | | radio |
| P4 | weather session | NOT MEASURED uC / s | MODEL | | | | radio |
| P4 | 1 h average, `time+weather`, `wx_min 30` | NOT MEASURED uA | MODEL | | | | radio |
| P5 | Critical floor | NOT MEASURED uA | MODEL | | | | radio |
| P6 | `CONFIG_QZ_USB_WAKE=n` floor delta vs P1 | NOT MEASURED uA | MODEL | | | | radio |
| P7 | menu session, 30 s | NOT MEASURED mA | MODEL | | | | radio |

Procedure and wiring: `docs/HARDWARE_BRINGUP.md` B9, skill `.claude/skills/power-measurement`.
When a row is measured, replace `NOT MEASURED` with the value and `MODEL` with `MEASURED`, and update
the `[TUNE]` constants in `qz_power` and the ARCHITECTURE section 20 table.

## 4. Regression rule (docs/PUSHBACK.md P-04)
- **Merge gate (CI, host):** the virtual-time suite budgets in `components/qz_app/test/sim_*`
  (partial tick, cold boot, awake share of the day, 7-day scenarios, NVS write counts). They run in
  `tools/check.sh`; a change that raises awake time past a budget fails the merge. Do not loosen a
  budget to pass: fix the code or justify and re-baseline here.
- **Release gate (owner, real watch):** re-run B9 for the release candidate. The release **fails** if
  any measured value in section 3 differs from the recorded baseline by more than **10 %** (up for
  current/charge, down for days of life). Fix the regression, or re-baseline in the same change with
  the cause in `CHANGELOG.md`.
- A release at 1.0.0 or above also requires that no `NOT MEASURED` cell remains (`tools/release.sh`
  refuses otherwise).
