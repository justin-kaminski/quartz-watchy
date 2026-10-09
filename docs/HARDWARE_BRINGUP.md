# Hardware bring-up (owner procedure)

Nothing hardware-facing is considered working until the step below passes on a real Watchy v3.
Do the steps in order; each lists what to connect, what to run, and what to expect.

## Contents
- [B0 Equipment](#b0-equipment) - [B1 First flash](#b1-first-flash-and-recovery) -
  [B2 Console](#b2-console-sanity) - [B3 Sensors and pins](#b3-sensors-and-pins) -
  [B4 Buttons, vibration, wake](#b4-buttons-vibration-wake) - [B5 Display](#b5-display) -
  [B6 Battery](#b6-battery-calibration) - [B7 Steps](#b7-step-counter-and-axis-remap) -
  [B8 Crystal and drift](#b8-rtc-crystal-and-drift) - [B9 Power](#b9-power-measurement) -
  [B10 Automated suites](#b10-automated-suites) - [B11 Wi-Fi](#b11-wi-fi-sync-and-weather) -
  [B12 Provisioning](#b12-provisioning-page) - [Reporting](#reporting)

Conventions: `$P` = the watch's serial port (usually `/dev/ttyACM0`; `ls /dev/ttyACM*` after plugging
in). `qzctl` = `tools/qzctl/qzctl` (WP-29). All commands run from the repo root.

## B0 Equipment
- Watchy v3, a micro-USB **data** cable (charge-only cables power the watch but never enumerate).
- Linux PC with the repo bootstrapped (`tools/bootstrap.sh`).
- Multimeter. For B6: adjustable bench supply 3.0-4.3 V (current limit 300 mA) and leads to the
  battery connector (J1). For B9: a power profiler able to source 3.8 V and measure 1 uA..200 mA
  (Nordic PPK2 recommended; Joulescope JS220 or uCurrent + scope also fine).
- 2.4 GHz Wi-Fi network and a phone (B11-B12).
- Warning: the board has **no battery protection IC** [hardware.md s3]: never leave the cell
  below 3.0 V; disconnect it if the watch will sit unused for weeks.

## B1 First flash and recovery
1. Optional backup of the stock firmware: enter download mode (step 3), then
   `tools/idf.sh esptool.py --chip esp32s3 -p $P read_flash 0 0x800000 stock_backup.bin`.
2. Build: `tools/fw.sh build` (radio variant) — expect "Project build complete" and the size report.
3. Download mode (needed when other firmware runs or the watch sleeps): hold **BACK + UP** for more
   than 4 s, release **BACK first** while still holding UP, then release UP. The display does not
   change; `ls /dev/ttyACM*` shows the port.
4. `tools/fw.sh -p $P erase-flash` (first time only), then `tools/fw.sh -p $P flash monitor`.
5. Expect: a full display refresh showing the face with `--:--` and a "set time" hint, and in the
   monitor a line starting `@QZ1 ! EVT {"evt":"ready"`. Exit the monitor with Ctrl+].
6. Recovery check: repeat step 3 while Quartz is running -> port reappears in download mode;
   flash again. A plain reset is BACK + UP for 4 s, releasing UP first (or together).
Report: monitor output from boot to `ready` (copy text), photo of the display.

## B2 Console sanity
With USB connected (the watch stays awake while tethered):
`qzctl status`, `qzctl run version`, `qzctl run "diag info"`, `qzctl run "diag clock"`.
Expect: `version` shows fw + git hash; `diag clock` reports `external_crystal: true` and
`measured_hz` within 32768 +- 16; `diag info` reset reason `power_on` (first boot).
Report: the four JSON responses.

## B3 Sensors and pins
`qzctl run "selftest run drivers"` and `qzctl run "diag sensors"`.
Expect: BMA423 chip id 0x13 and feature engine ok; ADC battery 3000-4400 mV; buttons idle =
none pressed; USB detect = 1; STAT (GPIO10) = 1 while USB is present (R1 predicts HIGH in both
charging and full states — record what you see); panel BUSY toggles and temperature plausible.
Report: full selftest JSON; STAT value with battery charging and (if you can wait) when full.

## B4 Buttons, vibration, wake
1. `qzctl run "selftest run interactive"`: press each button when prompted; confirm the buzz.
2. Unplug USB. Within 1 minute the face updates (deep-sleep cycle). Press each button once:
   MENU opens the menu immediately, BACK returns, UP/DOWN move. Hold MENU 1 s -> back to face.
3. While the watch sleeps, press and hold **UP alone** for 2 s: it must wake normally (menu or
   steps screen), **not** enter download mode.
4. Listen/feel for any motor tick at boot or during sleep (there must be none).
Report: pass/fail per button, wake latency impression, any motor noise.

## B5 Display
0. Run the panel experiments first: `qzctl run "selftest run drivers/epd_probe"` (E1: OTP partial
   waveform present?) and the visual checks E2-E5 listed in `docs/research/ssd1681.md` section 11.
   Report the JSON and photos; partial-update design depends on E1.
1. Set the time from the PC: `qzctl run "time set $(date -u +%Y-%m-%dT%H:%M:%SZ)"`.
2. `qzctl screenshot face.png` and compare with the panel (photo). They must match pixel-for-pixel
   in layout (the screenshot is the framebuffer).
3. Unplug USB. Watch a minute boundary next to an NTP-synced phone clock (seconds visible):
   estimate when the digits change relative to :00 (target -0.25..+0.5 s). Repeat 3 times.
4. Leave on battery 2 h; check ghosting before and after the hourly full refresh.
5. Menu > Time & date: change the minute, save; the face shows it.
Report: offsets measured in step 3, ghosting photos, any flicker on partial updates.

## B6 Battery calibration
1. Disconnect the LiPo; connect the bench supply to J1 (+ to pin 1 = +BATT, check polarity with
   the multimeter first). No USB.
2. For V in 3.30, 3.40, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20: set the supply, wait 10 s,
   press MENU then BACK (a button wake samples the battery), wait for the display update.
3. Connect USB first, then disconnect the bench supply (USB keeps the watch powered, so the RTC
   wake log survives), and run `qzctl run "log wakes 32"`: each record has `battery_mv`.
4. Expect readings within +-50 mV up to 3.7 V; above 3.7 V the ADC is outside its calibrated range
   [hardware.md s4] — record the actual values; they become the percentage curve.
5. Below 3.40 V the watch must show "Charge me" and stop minute updates.
Report: table of supply V vs `battery_mv`, the Critical behaviour seen.

## B7 Step counter and axis remap
1. `qzctl run "diag sensors"` with the watch (a) lying face up, (b) standing with 12 o'clock up,
   (c) standing with 3 o'clock up. Expect about +1000 mg on one axis each time; report x/y/z.
2. Wear the watch, `qzctl run "steps get"`, unplug, walk 200 counted steps, reconnect,
   `steps get` again. Expect the difference within 5 %.
Report: the three raw readings and the step counts.

## B8 RTC crystal and drift
1. `qzctl run "diag clock"`: external crystal, measured frequency.
2. Set time from the PC (B5 step 1), unplug, leave 24 h (48 h better) on battery, reconnect and
   compare: `qzctl run "time get"` vs `date -u` (qzctl prints both and the ppm figure).
3. Expect |drift| < 20 ppm (< 1.7 s/day) before compensation.
Report: elapsed hours and offset in seconds; room temperature range.

## B9 Power measurement
Setup: profiler in source mode at 3.80 V into J1 (LiPo disconnected), USB unplugged, 1 kHz+ sampling.
Flash each variant first (USB), then unplug and measure:

| Run | Firmware / mode | Measure | Record |
|---|---|---|---|
| P1 | radio build, `conn off` | 10 min average; sleep floor between wakes; charge per minute wake | uA, uA, uC |
| P2 | offline build (`sdkconfig.defaults.offline`) | same as P1 | must equal P1 within noise (Off means off) |
| P3 | radio build, `conn time`, `sync_h 6` | one sync session (charge, duration) + 1 h average | uC, s, uA |
| P4 | radio build, `conn time+weather`, `wx_min 30` | one weather session + 1 h average | uC, s, uA |
| P5 | Critical (supply 3.35 V) | floor | uA |
| P6 | radio build with `CONFIG_QZ_USB_WAKE=n` | floor vs P1 | uA delta |
| P7 | button-driven menu use, 30 s | average during session | mA |

Then compute days of life per mode with 170 mAh (the profiler software or
`qzctl run "diag power"` estimate). Report: screenshots/CSV exports + the table; these numbers become
`docs/POWER_BUDGET.md` (the CI budget source).

## B10 Automated suites
With USB connected:
`pytest test_apps/console --target esp32s3 --port $P`,
`pytest test_apps/wake --target esp32s3 --port $P`,
`pytest test_apps/drivers --target esp32s3 --port $P`,
`qzctl run "selftest run all"`.
Expect all green; `test_apps/wake` deep-sleeps the watch and checks that qzctl reconnects.
Report: pytest summaries (and the JUnit XML files they write).

## B11 Wi-Fi sync and weather
1. `qzctl wifi-set` (prompts for SSID and password; the password is never echoed or logged).
2. `qzctl run "settings set lat <your lat>"`, `... lon <your lon>`, `... conn time+weather`.
3. `qzctl run "sync now"`: expect `time: ok`, `weather: ok` within 30 s; the face shows temperature.
4. Negative test: `qzctl wifi-set` with a wrong password, `sync now` -> `ERR` with reason, face keeps
   working and shows "last sync failed"; restore the right password.
Report: the JSON results, session durations, a photo of the face with weather.

## B12 Provisioning page
Menu > Connectivity > Setup Wi-Fi. Join the SSID shown with the password shown, open
`http://192.168.4.1`, fill the form, submit. Expect success page, AP shuts down, watch syncs.
Wait 5 min without submitting: the AP must stop by itself.
Report: pass/fail, phone model/browser, anything confusing on the page.

## B13 Phone sync (radio image)
Needs Chrome on Android (or desktop Chrome) and the page from `web/phone/` served over https (see
`web/phone/README.md`). On the watch: Menu > Phone > Sync with phone. On the phone: open the page,
Connect, pick `Quartz-XXXX`, type the six digits the watch shows when the phone asks. Expect: the
sync list shows Time, Time zone and Weather done (Weather needs a location: "Use this phone's
location"); the watch face shows the phone's time; changing a setting on the page changes the watch.
Then: tap Done -> the watch shows "Sync complete" and its Bluetooth is off. Start again: no code is
asked this time (bonded). Start once more and do nothing: after 2 min the watch says it stopped.
Run three sessions in a row (stack re-initialization).
Report: pass/fail per item, phone model, anything confusing, and (with a profiler, B9 setup) the
current while waiting for the phone and while connected.

## Reporting
Create `docs/bringup/YYYY-MM-DD.md` with one section per step: pass/fail, pasted outputs, measured
numbers, photos (in `docs/bringup/img/`). Agents read that file to update [TUNE]/[ASSUMED] items.
