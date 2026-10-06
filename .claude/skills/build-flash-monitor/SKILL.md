---
name: build-flash-monitor
description: Build the Quartz firmware (radio and offline variants), find the watch's serial port, enter download mode, flash, monitor, and talk to the console. Use for any "build / flash / run on the watch" task. Flashing needs the owner at the physical watch.
---

# Build, flash, monitor

Flashing is owner-only: an agent never runs `flash`, `erase-flash` or `monitor` unattended. Agents
build, and tell the owner exactly what to press (steps below, full text in `docs/FIRST_FLASH.md`).

## 1. Build (safe, no device)
```bash
tools/bootstrap.sh                          # once: pinned ESP-IDF (.idf-version) into .toolchain/
tools/fw.sh build                           # radio image   -> build/fw/quartz.bin
QZ_FW_VARIANT=offline tools/fw.sh build     # offline image -> build/fw-offline/quartz.bin (no Wi-Fi code)
tools/fw.sh size                            # size report (budgets: radio <= 1.6 MiB, offline <= 600 KiB)
tools/check_offline.sh && tools/check_radio.sh   # the offline image has no radio symbols; the radio one has
```
Parallel agents use private dirs: `QZ_FW_BUILD_DIR=build/<name>-fw tools/fw.sh build`. After changing
the variant of an existing build dir, delete it (sdkconfig is generated once). The git hash in the
About screen is taken when CMake configures: run `tools/fw.sh reconfigure` before flashing a build
that must be labelled exactly (`-dirty` = tracked files differ from HEAD).

## 2. Find the port
```bash
ls /dev/ttyACM*                  # usually /dev/ttyACM0; P=/dev/ttyACM0
ls /dev/serial/by-id/            # stable name usb-Espressif_USB_JTAG_serial_debug_unit_*
lsusb | grep 303a:1001           # Espressif USB-Serial/JTAG
```
No port: charge-only cable, or the watch is deep-sleeping (the port vanishes while it sleeps).
Arch/CachyOS needs group `uucp`; ModemManager may hold the port for a few seconds.

## 3. Download mode (first flash and every recovery)
Plug USB, hold **BACK + UP** for more than 4 s, release **BACK first** while still holding UP, then
release UP. Releasing UP first (or together) is a plain reset instead. The display does not change;
the port appears.

## 4. Flash and monitor (owner)
```bash
P=/dev/ttyACM0
tools/fw.sh -p $P erase-flash            # first time / recovery only: wipes NVS (= factory reset)
tools/fw.sh -p $P flash monitor          # leave the monitor with Ctrl+]
QZ_FW_VARIANT=offline tools/fw.sh -p $P flash monitor   # offline variant
```
First bring-up: flash the **offline** image, then the radio image once offline behaves. If the watch
stays in download mode afterwards, hold BACK + UP 4 s and release UP first. Expect a full refresh,
the face with `--:--`, and `@QZ1 ! EVT {"evt":"ready",...}` in the monitor.

## 5. Console (USB attached = watch stays awake)
Protocol: `docs/ARCHITECTURE.md` section 16. Lines starting `@QZ1` are replies; everything else is log.
```bash
tools/idf.sh python -m serial.tools.miniterm $P 115200 --eol LF --echo   # type: version, status, help
tools/qzctl.sh run status               # client (WP-29; wrapper for `python -m qzctl`, other verbs: wait-ready screenshot shell selftest); optional
```
The port disappears on every deep sleep (`sleep 5`, unplug) and reappears on wake: reopen it. After a
wake the first message is a new `ready` event.

## 6. Recovery
Download mode (step 3), `tools/fw.sh -p $P erase-flash`, flash again. Stock firmware can be restored
from a backup taken with `tools/idf.sh python -m esptool --chip esp32s3 -p $P read-flash 0 0x800000 stock_backup.bin`.
The board has no battery protection: never leave the cell below 3.0 V.

## Report
Nothing hardware-facing counts as tested until the owner has run `docs/HARDWARE_BRINGUP.md`.
