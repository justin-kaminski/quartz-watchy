# qzctl - Quartz console client (WP-29)

Python (stdlib + pyserial 3.5) client for console protocol v1 (`docs/ARCHITECTURE.md` section 16).
Only lines starting with `@QZ1` are protocol; log lines in the stream are collected and ignored.

## Use
```bash
tools/qzctl.sh wait-ready                      # waits for the `ready` event (or a live reply)
tools/qzctl.sh run "tz set America/Chicago"    # one request; JSON on stdout; exit 0 OK / 1 ERR
tools/qzctl.sh screenshot shot.png             # `display dump` -> 1-bit 200x200 PNG
```
Equivalent: `.venv/bin/python tools/qzctl ...` or `PYTHONPATH=tools python -m qzctl ...`.

| Command | What it does |
|---|---|
| `ports` | list USB serial ports matching VID:PID |
| `wait-ready [--wait S] [--strict]` | wait for `ready`; without `--strict` a reply to a periodic `version` also counts (the event may predate the connect) |
| `run <cmd...>` | quote the whole command (`run "vibrate 200"`) or pass words (`run wifi set "My Net" pw`); `sleep`/`reboot` automatically ride out the port vanishing and print the new `ready` event on stderr |
| `screenshot out.png` | verifies size and CRC-32 of the dump, writes a deterministic 1-bit PNG (ink = black) |
| `selftest [target]` | runs `selftest run all` (or a suite/test), prints a table, exit 1 on any failure |
| `shell` | interactive line mode; the session survives sleep/reboot |

Connection options (before the subcommand): `--port /dev/ttyACM0` (path), else USB `--usb 303a:1001`
(default) plus optional `--serial <usb serial number>`; several matching watches without `--serial`
is an error. `--exec "<program args>"` talks to a program's stdio instead (simulator, fake device).
`--timeout` (reply, default 10 s), `--connect-timeout` (re-enumeration, default 30 s), `-v` echoes log lines.
Exit status: 0 OK, 1 ERR reply / failed self-test, 2 usage, 3 link, timeout or protocol failure.

## Reconnect
The USB-Serial-JTAG port disappears whenever the watch deep sleeps (ARCHITECTURE section 17). If the
link drops before a reply, `Client.request` reconnects (polling for the VID:PID / serial number or the
path), waits for `ready` (or a probe), then re-sends the same request id once. `run_expecting_reset`
(`sleep`, `reboot`) tolerates the reply being lost, waits for the port to disappear, reconnects and
waits for the new `ready`. DTR/RTS are kept low when opening so the open does not reset the chip
`[ASSUMED]`; udev access to `/dev/ttyACM*` (group `uucp`/`dialout`) is the owner's setup.

## Library
```python
from qzctl import Client
from qzctl.transport import SerialTransport      # or SubprocessTransport([...]) / your Transport
with Client(SerialTransport()) as c:
    c.connect()
    c.wait_ready()
    print(c.request("status").data)
    c.run_expecting_reset("sleep 5")
```
`Transport` (`open/close/read_line/write_line`) is the only hardware seam; tests use an in-memory fake
USB link, a pty serial endpoint and a subprocess fake device (`tests/fakes.py`).

## Tests (no hardware)
```bash
.venv/bin/python -W error -m pytest tools/qzctl/tests -q
```
Covers framing/parse tables, log lines mixed into the stream, id matching, reconnect after a simulated
port disappearance (in-memory and a real pty), `sleep` cycles, timeouts, base64 -> PNG against the
firmware's known frame (CRC `1abd04d4`), and the CLI end to end.
`qz_sim` has no `--console` mode yet; once it does, `--exec "build/host/sim/qz_sim --console"` works unchanged.

## Device suites (owner-run, HARDWARE_BRINGUP B10)
Skeletons in `test_apps/console/` (every console command once, plus a check that `help` lists nothing
uncovered) and `test_apps/wake/` (`sleep 5` -> re-enumeration -> `ready`; repeat; `reboot`). They are
plain pytest driving an already flashed, USB-attached watch through qzctl; nothing runs unless
`QZ_DEVICE=1` (CI never sets it). Flash first (`tools/fw.sh -p PORT flash`), then:
```bash
QZ_DEVICE=1 .venv/bin/python -m pytest test_apps/console -q          # add QZ_PORT=/dev/ttyACM0 / QZ_SERIAL=...
QZ_DEVICE=1 .venv/bin/python -m pytest test_apps/wake -q             # QZ_ALLOW_DESTRUCTIVE=1 adds erasing commands
```
Argument values and field checks are `[ASSUMED]` until run on the watch; tighten them as you confirm output.
