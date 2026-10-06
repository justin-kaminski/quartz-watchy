---
name: power-measurement
description: Measure Quartz's real current draw per connectivity mode (bring-up B9) with a power profiler and record it in docs/POWER_BUDGET.md. Owner-run, needs the physical watch; also covers the host-side energy model used by the merge gate.
---

# Power measurement

Two layers (docs/PUSHBACK.md P-04): the **merge gate** is the virtual-time model (host, no watch);
the **release gate** is real current measured per release. Only the owner can do the latter; an agent
prepares the firmware, tells the owner exactly what to connect, then enters the numbers.

## Host model (agents can run this)
```bash
QZ_BUILD_DIR=build/pwr tools/host.sh build
build/pwr/qz_app_test --gtest_filter='SimBudget*:*Connectivity*' 2>&1 | grep '\[sim\]'
```
Prints awake time per wake type and the WP-11 energy estimate (`qz::power::estimate_hours`, constants
`sleep_floor_ua` / `active_ma` are `[TUNE]` until B9). Merge gate = the budgets asserted in
`components/qz_app/test/sim_scenarios_test.cpp` (`SimBudget`: partial minute tick, cold boot, awake
share < 3 %/day) and `sim_connectivity_test.cpp`. Model numbers are never presented as measured.

## Equipment (docs/HARDWARE_BRINGUP.md B0)
Power profiler that sources 3.8 V and measures 1 uA..200 mA at >= 1 kHz (Nordic PPK2 recommended;
Joulescope JS220, or uCurrent + scope). The board has **no battery protection**: never below 3.0 V.

## Wiring
Disconnect the LiPo. Profiler in **source mode, 3.80 V**, output to the battery connector J1
(+ to pin 1 = +BATT: verify polarity with a multimeter first), **USB unplugged**. A fully-sleeping
watch can show a few uA of profiler offset: zero the profiler before connecting.
Flash each variant over USB first (skill `build-flash-monitor`), then unplug USB and measure.
Console commands need USB: set the mode first, then unplug (`qzctl run "settings set conn off"`
style; `tools/qzctl.sh run "..."` (WP-29, `python -m qzctl`) or `miniterm`).

## Runs (B9 table)
| Run | Firmware / mode | Measure |
|---|---|---|
| P1 | radio build, `conn off` | 10 min average; sleep floor between wakes; charge per minute wake (uA, uA, uC) |
| P2 | offline build (`QZ_FW_VARIANT=offline`) | as P1; must equal P1 within noise (Off means off) |
| P3 | radio, `conn time`, `sync_h 6` | one sync session (uC, s) + 1 h average (uA) |
| P4 | radio, `conn time+weather`, `wx_min 30` | one weather session (uC, s) + 1 h average |
| P5 | Critical (supply 3.35 V) | floor (uA) |
| P6 | radio with `CONFIG_QZ_USB_WAKE=n` | floor vs P1 (uA delta) |
| P7 | button-driven menu use, 30 s | average during the session (mA) |

Days of life = 170 mAh / (average current). Also compare with `diag power` on the watch (needs USB).

## Record
1. Raw exports (CSV/screenshots) and the run table go in `docs/bringup/YYYY-MM-DD.md` (`docs/bringup/img/`).
2. In `docs/POWER_BUDGET.md` fill the matching row, change its tag from `MODEL` to `MEASURED`, set
   the date, git hash (`git rev-parse --short HEAD`), profiler and variant. Fill the "Baseline" table.
3. Update the `[TUNE]` constants (`EstimateInputs::sleep_floor_ua`, `active_ma` in
   `components/qz_power/include/qz/power/power.hpp`) and the section 20 table in
   `docs/ARCHITECTURE.md`; run `tools/check.sh --fast`.
4. Regression rule (release gate): a later measurement more than **10 %** away from the recorded
   baseline in any row fails the release; either fix the regression or justify and re-baseline in the
   same change with the numbers and cause in `CHANGELOG.md`.
