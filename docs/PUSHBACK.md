# Pushback on the brief

Each item: objection first, then reasoning, then what the plan does. The brief is still planned as
written unless noted; the owner decides.

## Contents
P-01 Weather on/off duplicates the connectivity mode - P-02 "Charge status" cannot be shown -
P-03 Battery % is not calibrated above 3.7 V - P-04 Power regressions cannot block merges
automatically - P-05 Minute partial refresh vs panel lifetime - P-06 No battery protection on the
board - P-07 On-device self-test cannot run every console command - P-08 Battery is 170-180 mAh,
not 200 - P-09 Accelerometer wake has no defined purpose - P-10 Deep sleep while plugged in -
P-11 Flicker-free minute updates rest on an undocumented waveform

### P-01 "Weather on/off" duplicates connectivity mode "Time + Weather"
Two controls for one state invite contradictions (weather on while mode is Time only). The plan keeps a
single source of truth: the menu's Weather on/off toggles the mode between Time only and Time +
Weather; the Weather menu holds only weather options (hi/lo, location, interval).

### P-02 The board cannot report charge status
GPIO10 (STAT) reads HIGH whenever USB is present, both while charging and when full, and in the full
state the network pushes the pin above its 3.6 V rating through the ESD diode
(docs/research/hardware.md s5). Firmware shows "USB powered/charging" only. Hardware erratum worth
reporting to SQFMI; nothing firmware can fix.

### P-03 "Battery % from calibrated ADC readings" holds only below 3.7 V
The divider (100 k/360 k) puts the pin above the ADC's calibrated 2.9 V range once the cell exceeds
3.706 V, roughly the top 60 % of capacity (hardware.md s4). The plan keeps every power-policy threshold
below 3.7 V (accurate where it matters) and builds the upper curve from bring-up B6 measurements;
upper percentages are coarse (rounded to 10 %) until characterised per unit.

### P-04 "Power regressions block merge" cannot be automated literally
CI has no watch. The merge gate uses the simulated awake-time/energy model (virtual-time suite with
budgets per mode); real current is measured per release (B9) and the release gate fails if it drifts
> 10 % from docs/POWER_BUDGET.md.

### P-05 Minute partial updates exceed the panel vendor's usage guidance
Good Display asks for >= 180 s between updates and a full refresh after every 5 partials, and
publishes no lifetime figure (ssd1681.md s1, s8). The brief's 60 s partials (525 k/year) and "rare"
full refreshes deviate on both counts. The plan follows the brief, makes N (partials per full
refresh, default 30) a tunable chosen from B5 ghosting observations, and logs the deviation as tech
debt. Alternative if panels degrade: 5-minute updates at night (owner choice).

### P-06 No battery protection IC: Critical mode cannot fully protect the cell
The cell connects straight to +BATT (hardware.md s3); the watch draws ~20-40 uA even in Critical
(divider alone ~7 uA). An uncharged watch reaches the 2.75 V cutoff in weeks. The plan suspends the
BMA423 in Critical and documents "charge within ~2 weeks of Charge me". Keeping button wake (as the
brief asks) costs nothing extra; true protection needs hardware.

### P-07 "Self-test tests every console command" cannot mean on-device self-execution
`reboot`, `sleep`, `factory-reset` and radio commands are destructive or slow. Coverage is split:
every command has host tests (in-process dispatcher, fakes) and a host-driven pytest on the device
(`test_apps/console`, owner-run); the on-device self-test covers drivers, screens, settings, time.

### P-08 Battery capacity is 170-180 mAh, not 200 mAh
The cell datasheet rates 180 mAh typical / 170 mAh minimum (hardware.md s3). Estimates use 170 mAh.

### P-09 "Wake on accelerometer interrupt" has no user-facing purpose in the brief
On an always-on e-paper face, tilt-to-wake adds wakes (power) without showing anything new. Plan:
plumbing complete, setting `tapwake` default off, double-tap shows a 5 s status overlay. See Q-01.

### P-10 Deep sleep while USB is connected would make the console unusable
The brief asks host tooling to survive CDC re-enumeration across deep sleep. Doing that every
minute while plugged in would make agent workflows flaky. The tether policy keeps the watch awake
on USB power (no battery cost) and still exercises re-enumeration via `sleep <s>` and resets.

### P-11 "Partial refresh each minute, no visible flicker" rests on an undocumented waveform
Neither the SSD1681 datasheet nor the panel spec publishes the partial (display mode 2) waveform or
sequence (ssd1681.md risk #1). If the panel OTP lacks one (bring-up experiment E1), a flicker-free
update needs the panel vendor's reference LUT, which the owner's no-third-party-code rule may cover:
owner decision Q-12. Without it the only fallback is a full refresh every minute (visible flash,
~2 s, more energy), which violates the brief. This is the project's largest technical risk.

### P-12 "Bosch's official BMA423 SensorAPI" no longer exists upstream (added by the lead after R1)
`github.com/boschsensortec/BMA423_SensorAPI` returns 404 and Bosch's site no longer lists the BMA423; the
active official repo (`BMA456_SensorAPI`) has no BMA423 variant or config blob, and the BMA456 blob must not
be used on this chip (docs/research/bma423.md s12). The only surviving copy is SQFMI's fork
`sqfmi/BMA423-Sensor-API` (tag `bma423_v2.14.13`, commit `df5c8ee95f7544451090f70eec4911fb6c9a7c72`,
Bosch's original commit history, BSD-3-Clause, 6144-byte blob, SHA-256 recorded). The plan vendors exactly
that tag, unmodified, with LICENSE + PROVENANCE.md, and treats blob authenticity as unconfirmed until the
owner gets the official driver from Bosch Sensortec support (Q-13). Bring-up B7 validates the blob
functionally (INTERNAL_STATUS ok, step counter counts).
