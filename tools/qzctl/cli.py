"""Command line front end: ``python -m qzctl`` / ``tools/qzctl.sh``.

Exit status: 0 OK reply, 1 ERR reply (or failed self-test), 2 usage error, 3 link / timeout /
protocol failure, 130 interrupted.
"""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from typing import Any, Callable, Optional, Sequence, TextIO

from .client import Client
from .errors import QzError
from .png import dump_to_png
from .protocol import Response, join_command
from .transport import DEFAULT_PID, DEFAULT_VID, SerialTransport, SubprocessTransport, Transport

RESET_COMMANDS = frozenset({"sleep", "reboot"})  # the USB port vanishes after the reply
EXIT_OK, EXIT_ERR, EXIT_USAGE, EXIT_LINK = 0, 1, 2, 3

TransportFactory = Callable[[argparse.Namespace], Transport]


def _vid_pid(text: str) -> tuple[int, int]:
    try:
        vid, pid = text.split(":")
        return int(vid, 16), int(pid, 16)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected VID:PID in hex, got {text!r}") from None


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="qzctl", description="Quartz console client (protocol v1)")
    c = p.add_argument_group("connection (default: USB 303a:1001)")
    c.add_argument("--port", help="serial device path, e.g. /dev/ttyACM0 (disables VID:PID lookup)")
    c.add_argument("--usb", type=_vid_pid, default=(DEFAULT_VID, DEFAULT_PID), metavar="VID:PID")
    c.add_argument("--serial", dest="serial_number", help="USB serial number (several watches)")
    c.add_argument("--exec", dest="exec_cmd", metavar="CMD", help="talk to a program's stdio instead")
    c.add_argument("--connect-timeout", type=float, default=30.0, help="seconds to wait for (re)connect")
    c.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for a reply")
    c.add_argument("-v", "--verbose", action="store_true", help="echo device log lines to stderr")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="list USB serial ports matching VID:PID")
    w = sub.add_parser("wait-ready", help="wait for the ready event (or a live reply)")
    w.add_argument("--wait", type=float, default=30.0, help="seconds to wait (default 30)")
    w.add_argument("--strict", action="store_true", help="require the event, do not probe")
    r = sub.add_parser("run", help="send one command; print the JSON result")
    r.add_argument("words", nargs="+", help='command, quoted whole ("tz set UTC") or as words')
    s = sub.add_parser("screenshot", help="display dump -> 1-bit PNG")
    s.add_argument("out", help="output .png path")
    sub.add_parser("shell", help="interactive line mode")
    t = sub.add_parser("selftest", help="run the on-device self-test and print a table")
    t.add_argument("target", nargs="?", default="all", help="suite or test (default: all)")
    t.add_argument("--wait", type=float, default=180.0, help="seconds to wait for the run")
    return p


def make_transport(args: argparse.Namespace) -> Transport:
    if args.exec_cmd:
        return SubprocessTransport(shlex.split(args.exec_cmd))
    vid, pid = args.usb
    return SerialTransport(args.port, vid, pid, args.serial_number)


def command_line(words: Sequence[str]) -> str:
    return words[0] if len(words) == 1 else join_command(words)


def _emit(obj: Any, out: TextIO) -> None:
    out.write(json.dumps(obj, sort_keys=False) + "\n")


def execute(client: Client, command: str, out: TextIO, err: TextIO, timeout_s: Optional[float] = None) -> int:
    """Run one command line; JSON result on ``out``, ERR code on ``err``."""
    tokens = command.split()
    if tokens and tokens[0] in RESET_COMMANDS:
        extra = 30.0
        if tokens[0] == "sleep" and len(tokens) > 1 and tokens[1].isdigit():
            extra += int(tokens[1])
        result = client.run_expecting_reset(command, ready_timeout_s=extra)
        response = result.response
        if result.ready.event is not None:
            err.write("ready: " + json.dumps(result.ready.event.data) + "\n")
        else:
            err.write("ready event missed; the watch answered a probe\n")
        if response is None:
            err.write("no reply was read before the port vanished\n")
            return EXIT_OK
    else:
        response = client.request(command, timeout_s=timeout_s)
    return _report(response, out, err)


def _report(response: Response, out: TextIO, err: TextIO) -> int:
    _emit(response.data, out)
    if response.ok:
        return EXIT_OK
    err.write(f"qzctl: ERR {response.code}\n")
    return EXIT_ERR


def selftest_table(results: Sequence[dict[str, Any]]) -> str:
    rows = [("TEST", "STATUS", "MS", "DETAIL")]
    rows += [(str(r.get("name", "?")), str(r.get("status", "?")), str(r.get("ms", "")), str(r.get("detail", ""))) for r in results]
    widths = [max(len(row[i]) for row in rows) for i in range(3)]
    return "\n".join(
        f"{a:<{widths[0]}}  {b:<{widths[1]}}  {c:>{widths[2]}}  {d}".rstrip() for a, b, c, d in rows
    )


def _cmd_selftest(client: Client, args: argparse.Namespace, out: TextIO, err: TextIO) -> int:
    response = client.request(f"selftest run {args.target}", timeout_s=args.wait)
    if not response.ok:
        _emit(response.data, out)
        err.write(f"qzctl: ERR {response.code}\n")
        return EXIT_ERR
    results = response.data.get("results", [])
    out.write(selftest_table(results) + "\n")
    failed = int(response.data.get("failed", sum(1 for r in results if r.get("status") == "fail")))
    passed = int(response.data.get("passed", sum(1 for r in results if r.get("status") == "pass")))
    skipped = int(response.data.get("skipped", sum(1 for r in results if r.get("status") == "skip")))
    out.write(f"{passed} passed, {failed} failed, {skipped} skipped\n")
    return EXIT_ERR if failed else EXIT_OK


def _cmd_screenshot(client: Client, args: argparse.Namespace, out: TextIO, err: TextIO) -> int:
    response = client.request("display dump")
    if not response.ok:
        _emit(response.data, out)
        err.write(f"qzctl: ERR {response.code}\n")
        return EXIT_ERR
    png = dump_to_png(response.data)
    with open(args.out, "wb") as fh:
        fh.write(png)
    out.write(f"wrote {args.out} ({len(png)} bytes, crc32 {response.data.get('crc32', '?')})\n")
    return EXIT_OK


def _cmd_shell(client: Client, args: argparse.Namespace, out: TextIO, err: TextIO) -> int:
    err.write("qzctl shell: type a command; 'exit' or Ctrl-D leaves. sleep/reboot reconnect.\n")
    while True:
        try:
            line = input("qz> " if sys.stdin.isatty() else "").strip()
        except EOFError:
            return EXIT_OK
        if line in ("exit", "quit"):
            return EXIT_OK
        if not line:
            continue
        try:
            execute(client, line, out, err, args.timeout)
        except QzError as exc:  # keep the session; the next request reconnects
            err.write(f"qzctl: {exc}\n")
        out.flush()


def _cmd_ports(args: argparse.Namespace, out: TextIO) -> int:
    from serial.tools import list_ports

    vid, pid = args.usb
    found = [p for p in list_ports.comports() if p.vid == vid and p.pid == pid]
    for p in found:
        out.write(f"{p.device}\t{vid:04x}:{pid:04x}\t{p.serial_number or ''}\t{p.description}\n")
    return EXIT_OK if found else EXIT_ERR


def main(
    argv: Optional[Sequence[str]] = None,
    *,
    transport_factory: Optional[TransportFactory] = None,
    out: Optional[TextIO] = None,
    err: Optional[TextIO] = None,
) -> int:
    out = out or sys.stdout
    err = err or sys.stderr
    try:
        args = build_parser().parse_args(argv)
    except SystemExit as exc:  # argparse already printed the usage text
        return EXIT_USAGE if exc.code else EXIT_OK
    if args.cmd == "ports":
        return _cmd_ports(args, out)
    client = Client(
        (transport_factory or make_transport)(args),
        request_timeout_s=args.timeout,
        reconnect_timeout_s=args.connect_timeout,
        on_log=(lambda line: err.write(line + "\n")) if args.verbose else None,
    )
    try:
        client.connect()
        if args.cmd == "wait-ready":
            result = client.wait_ready(args.wait, probe=not args.strict)
            _emit(result.event.data if result.event else {"evt": "alive", "note": "ready event predates connect"}, out)
            return EXIT_OK
        if args.cmd == "run":
            return execute(client, command_line(args.words), out, err, args.timeout)
        if args.cmd == "screenshot":
            return _cmd_screenshot(client, args, out, err)
        if args.cmd == "selftest":
            return _cmd_selftest(client, args, out, err)
        return _cmd_shell(client, args, out, err)
    except QzError as exc:
        err.write(f"qzctl: {exc}\n")
        return EXIT_LINK
    except OSError as exc:
        err.write(f"qzctl: {exc}\n")
        return EXIT_LINK
    except KeyboardInterrupt:
        return 130
    finally:
        client.close()
