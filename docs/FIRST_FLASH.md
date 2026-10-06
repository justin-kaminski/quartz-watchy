# First flash (owner guide)

Goal: put the first Quartz image on a real Watchy v3, see the face, and report what happened.
**Nothing in this image has ever run on hardware.** It builds warning-free in both Kconfig variants
and every IDF call was checked against the IDF source, but all behaviour is `[ASSUMED]` until you
report back. Read "Honest warnings" before you flash. This is the short path through
`docs/HARDWARE_BRINGUP.md` B0-B2; the full list of experiments stays there.

What this image does: boots, draws the face (`--:--` and a "set time" hint until you set the time),
sleeps, wakes every minute, handles the four buttons, and runs the USB console while USB is
attached. The radio stack (Wi-Fi, SNTP time sync, weather, provisioning page) is now implemented in the default
(radio) image but has never run either. **For the very first bring-up flash the OFFLINE image**, which has no
Wi-Fi code at all and isolates display/sleep/button/USB problems from radio problems:
`QZ_FW_VARIANT=offline tools/fw.sh build` (output in `build/fw-offline`) and flash it with
`QZ_FW_VARIANT=offline tools/fw.sh -p $P flash monitor`. Move to the radio image (default `tools/fw.sh build`)
once the offline one behaves; set Wi-Fi up through the USB console (`wifi set ...`) or the SoftAP page.

## 0. You need
- Watchy v3 with its battery connected, and a micro-USB **data** cable (a charge-only cable powers
  the watch but never enumerates). Do not leave the cell below 3.0 V: the board has no protection
  circuit.
- This repo bootstrapped (`tools/bootstrap.sh`, once).
- Linux serial access. On CachyOS/Arch the group is `uucp`: `sudo usermod -aG uucp $USER`, then log
  out and in. A quick stopgap is `sudo chmod 666 /dev/ttyACM0` after each re-enumeration. If
  `ModemManager` is running it may grab the port for a few seconds after it appears.

## 1. Build
```bash
QZ_FW_VARIANT=offline tools/fw.sh build   # first bring-up image: no radio code (recommended first)
tools/fw.sh build                         # radio image (Wi-Fi/SNTP/weather/portal, never run on hardware)
```
Expect "Project build complete" and `quartz.bin binary size 0x74ed0` (about 0.45 MiB of the 3 MiB
slot). The git hash shown on the About screen and in the console `ready` event is taken when CMake
configures; `-dirty` means tracked files differ from HEAD. `tools/fw.sh reconfigure` refreshes it.

## 2. Enter download mode (do this for the first flash and for every recovery)
The watch sleeps between ticks and the USB device disappears while it sleeps, so do not rely on
automatic reset into the bootloader.

1. Plug in the USB cable.
2. Hold **BACK** and **UP** (the two top buttons; UP is GPIO0) together. Keep holding for **more than 4 s**
   (the smart-reset chip U1 resets the ESP32-S3 after about 4.0 s [R1 hardware.md s6]; the display
   does not change).
3. Release **BACK first, while still holding UP**. Then release UP.
   Releasing UP first (or both together) gives a plain reset instead: try again.
4. The ROM bootloader is now running with GPIO0 low and the USB-Serial/JTAG device enumerates:
   ```bash
   ls /dev/ttyACM*                       # usually /dev/ttyACM0
   lsusb | grep 303a:1001                # Espressif USB JTAG/serial debug unit
   ls /dev/serial/by-id/                 # stable name: usb-Espressif_USB_JTAG_serial_debug_unit_*
   dmesg | tail -5                       # shows "cdc_acm ... ttyACM0" on attach
   ```
   Placement notes: the watch is 33.8 x 46.0 mm; BACK and UP are the top pair on the same side
   (names inferred by elimination for MENU/DOWN, see hardware.md s6). If nothing enumerates, check
   the cable first, then `dmesg`.

Optional but recommended once: back up the stock firmware (8 MB, a minute or two):
```bash
P=/dev/ttyACM0
tools/idf.sh python -m esptool --chip esp32s3 -p $P read-flash 0 0x800000 stock_backup.bin
```

## 3. Flash
```bash
P=/dev/ttyACM0
tools/fw.sh -p $P erase-flash            # first time only (wipes NVS and otadata)
tools/fw.sh -p $P flash monitor          # flash, hard-reset, then show the console; leave with Ctrl+]
```
The monitor needs a real terminal. Equivalent esptool command (printed by the build; offsets from
`sdkconfig.defaults`: table at 0x10000, app at 0x40000):
```bash
cd build/fw && ../../tools/idf.sh python -m esptool --chip esp32s3 -p $P -b 460800 \
  --before default-reset --after hard-reset write-flash "@flash_args"
```
If the watch stays in download mode after flashing (no face), do a plain reset: hold BACK+UP for
4 s and release **UP first** (or both at once). [ASSUMED] esptool's `hard-reset` works over
USB-Serial/JTAG on this board.

## 4. What the watch should show and do
1. Within a few seconds of reset: a full panel refresh (visible flashing) and the face with `--:--`
   plus a "set time" hint. Cold boot budget is about 2 s plus the panel waveform.
2. USB attached: the watch **stays awake** (tethered, by design: USB power costs no battery). The
   monitor shows a line like
   `@QZ1 ! EVT {"evt":"ready","proto":1,"fw":"0.1.0","git":"<hash>","reset":"..."}`.
   Boot-time ESP_LOG lines are at WARN level, so a quiet monitor is normal.
3. Set the time (no radio): either the console (below) or on the watch: MENU opens the menu, UP/DOWN
   move, MENU enters ("Time & date": UP/DOWN change a field, MENU next field / save, BACK
   previous / cancel). BACK on the face forces a full refresh. Hold MENU returns to the face.
4. After the time is set the face shows the time; the minute changes arrive as partial updates
   (see risk R1 below).
5. **Unplug USB:** within about 2 s (two 1 s polls) the console stops and the watch deep-sleeps; the
   image stays on the panel (e-paper). It wakes each minute on the RTC timer, redraws, sleeps. Any
   button wakes it. Plugging USB in wakes it (EXT0) and re-enumerates the port about a second later.

## 5. USB console
Protocol: `docs/ARCHITECTURE.md` section 16. Lines end with `\n` (CR or CRLF also accepted). Every
request gets one reply `@QZ1 <id|-> OK {json}` or `@QZ1 <id|-> ERR <code> {json}`; log lines never
start with `@QZ1`. Optional request id: `#7 status`.
```bash
tools/idf.sh python -m serial.tools.miniterm /dev/ttyACM0 115200 --eol LF --echo
# or:  screen /dev/ttyACM0 115200       (leave: Ctrl-A then K)
# or:  tools/fw.sh -p /dev/ttyACM0 monitor   (typed lines go to the watch; untested)
```
The baud rate is ignored by USB CDC. The port **disappears whenever the watch deep sleeps** and
miniterm exits; reopen it after the next wake.

Commands to try, in this order:
```
version                              fw, git, idf, build features
status                               time, steps, battery{mv,pct,usb,charging}, power state
tz set America/Chicago               then:
time set 2026-10-06T14:30:00         local time in the zone just set (or ...Z for UTC)
time get                             utc/local/source/drift
display dump                         JSON with b64 1-bpp 200x200 of the frame buffer
display refresh full                 force a full panel refresh
diag clock                           expect external_crystal true, measured_hz 32768 +-16
diag sensors                         step counter, panel temperature, pin levels (no raw accel yet)
battery get
vibrate 200                          motor for 200 ms
sleep 5                              deep sleep 5 s even with USB; port vanishes and returns,
                                     then a new `ready` event appears
reboot
```
`selftest` answers `unsupported` (WP-19 not done). `help` lists all commands.

## 6. Observe and report back
Please send: the monitor text from boot to `ready`, the JSON of `version`, `status`, `diag clock`,
`diag sensors`, `battery get`, and a photo of the display. Specifically:
- **Panel:** orientation (is the text upright, is anything mirrored or rotated), pixel order and
  inversion (white background with black text expected), ghosting, flicker on the minute change
  (partial vs full-looking refresh), anything stuck after several minutes.
- **Buttons:** do all four work, before and after the first deep sleep (unplugged)? Wrong names
  (MENU/DOWN are inferred) are a one-line fix.
- **Battery:** `battery get` mV against a multimeter on the cell; USB present/charging flags.
- **Steps:** walk 30 steps, `steps get` / `status`. 0 forever means the BMA423 init failed (see R3).
- **Vibration:** `vibrate 200` actually buzzes briefly, and does NOT run while the watch sleeps.
- **USB:** does the port come back after `sleep 5`, after unplug/replug, after a button wake?
- **Time keeping:** set the time, unplug for 30 minutes, compare to a reference (`time drift`).
- **Crashes:** a reset reason of `panic`/`watchdog` in `ready` or `diag info`, or the face blanking.

## 7. Recovery
- Anything odd: download mode (section 2), then `tools/fw.sh -p $P erase-flash` and flash again.
- A crash loop makes USB flap; the BACK+UP chord is a hardware reset and always works, and
  releasing BACK first while UP is held always reaches ROM download mode.
- Return to the stock firmware (if you took the backup):
  `tools/idf.sh python -m esptool --chip esp32s3 -p $P write-flash 0 stock_backup.bin`.
- If the panel shows a ghost image you cannot clear, `display refresh full` (or BACK on the face)
  runs the full waveform; it is also what the firmware does on every cold boot.
- Unused for weeks: disconnect the battery connector (no protection IC on the board).

## 8. Honest warnings (unverified, ordered by how likely they are to bite)
- **R1 (P-11):** the flicker-free minute update relies on the SSD1681 partial-refresh waveform from
  the panel OTP, which is undocumented. It may flash visibly or ghost. The fallback (full refresh
  each minute) violates the brief; Q-12 in `docs/OPEN_QUESTIONS.md` needs your decision.
- **R2 (STATUS tech debt, B4/B5/B9):** deep-sleep GPIO parking, pad holds and releasing them at boot,
  EXT1 button wake, EXT0 USB wake, the EPD pin idle levels in light sleep and the BUSY wake are all
  `[ASSUMED]`. Symptoms: buttons dead after the first deep sleep, vibration motor running while
  asleep, a blank or garbled panel after a wake, high sleep current (battery flat in days).
- **R3 (P-12/Q-13):** the BMA423 config blob is vendored from SQFMI's fork and its authenticity is
  unconfirmed; the step-counter byte order is `[ASSUMED]`. A failed init leaves the face working
  but with zero steps.
- **R4:** vibration polarity (active high), RTC time continuity across `esp_restart`, the 32 kHz
  crystal start-up, battery calibration thresholds (`[TUNE]`) and the USB-Serial/JTAG console path
  (driver install/uninstall, log routing, no-host behaviour) were written from the IDF source and
  datasheets, not run.
- **R5:** the radio image's Wi-Fi/SNTP/HTTPS/portal code and its heap teardown have never run (use
  `test_apps/net` after the offline image works); the `selftest` command exists but nothing was run on
  hardware. The idle timeouts, the tether timing and the wake budgets are unmeasured.
- Power: the deep-sleep floor is a model (25-40 uA), not a measurement. Do not judge battery life
  from this image; `docs/HARDWARE_BRINGUP.md` B9 does that properly.
