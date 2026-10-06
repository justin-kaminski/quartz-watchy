"""A fake Quartz console (no hardware): protocol device model + in-memory and stdio links.

Run as a script it serves the protocol on stdin/stdout, so SubprocessTransport and the CLI
can be tested end to end. ``sleep``/``reboot`` end the process after the reply; starting it
again (a reconnect) prints ``ready`` like a wake.
"""

from __future__ import annotations

import base64
import json
import os
import sys
import time
import zlib
from collections import deque
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from qzctl.errors import ConnectTimeout, TransportClosed  # noqa: E402
from qzctl.transport import Transport  # noqa: E402

KNOWN_FRAME = bytes((i * 7 + 3) & 0xFF for i in range(5000))  # firmware's fake framebuffer
KNOWN_CRC = "1abd04d4"  # zlib CRC-32 of KNOWN_FRAME, from components/qz_console/test/commands_test.cpp
READY = '@QZ1 ! EVT {"evt":"ready","proto":1,"fw":"1.0.0","git":"abc1234","reset":"deepsleep"}'


def ok(rid: str, obj: dict) -> str:
    return f"@QZ1 {rid} OK {json.dumps(obj, separators=(',', ':'))}"


def err(rid: str, code: str, msg: str) -> str:
    return f'@QZ1 {rid} ERR {code} {json.dumps({"msg": msg}, separators=(",", ":"))}'


class FakeDevice:
    """Protocol model. ``handle`` maps one request line to the lines the device prints."""

    def __init__(self) -> None:
        self.received: list[str] = []
        self.after_reply: Optional[str] = None  # "vanish" once the reply has been sent

    def handle(self, line: str) -> list[str]:
        self.received.append(line)
        text = line.strip()
        rid = "-"
        if text.startswith("#"):
            rid, _, text = text[1:].partition(" ")
        cmd = text.split()
        if not cmd:
            return [err(rid, "bad_args", "empty")]
        name = cmd[0]
        if name == "version":
            return ["I (12) qz: version requested", ok(rid, {"fw": "1.0.0", "git": "abc1234", "proto": 1})]
        if name == "status":
            return [ok(rid, {"valid": True, "battery": {"mv": 3900, "pct": 80}})]
        if name == "noisy":  # logs before, between and after the reply; a log mentioning the prefix
            return ["I (1) a: working", "W (2) b: saw @QZ1 in the middle", ok(rid, {"n": 1}), "I (3) c: done"]
        if name == "display" and cmd[1:] == ["dump"]:
            return [ok(rid, {"w": 200, "h": 200, "fmt": "1bpp-msb", "crc32": KNOWN_CRC,
                             "b64": base64.b64encode(KNOWN_FRAME).decode()})]
        if name == "selftest":
            return [ok(rid, {"results": [
                {"name": "rtc/xtal", "status": "pass", "ms": 12, "detail": ""},
                {"name": "radio/scan", "status": "skip", "ms": 0, "detail": "no radio"},
                {"name": "panel/crc", "status": "fail" if "failme" in cmd else "pass", "ms": 340, "detail": "crc mismatch"},
            ], "passed": 1 if "failme" in cmd else 2, "failed": 1 if "failme" in cmd else 0, "skipped": 1})]
        if name in ("sleep", "reboot"):
            self.after_reply = "vanish"
            return [ok(rid, {})]
        return [err(rid, "unknown_cmd", name)]


class FakeLink(Transport):
    """In-memory transport simulating USB CDC: after ``vanish`` the port is gone until
    ``reappear_after_s`` has passed, then it returns (emitting ``ready`` unless suppressed)."""

    def __init__(self, device: Optional[FakeDevice] = None, *, reappear_after_s: float = 0.1,
                 emit_ready: bool = True, drop_before_reply: bool = False, never_reappear: bool = False) -> None:
        self.device = device or FakeDevice()
        self.reappear_after_s = reappear_after_s
        self.emit_ready = emit_ready
        self.drop_before_reply = drop_before_reply  # lose the next request, then vanish
        self.never_reappear = never_reappear
        self.rx: deque[str] = deque()
        self.opened = False
        self.gone_until: Optional[float] = None  # port absent until this time
        self.open_calls = 0
        self.mute = False  # swallow requests (device not answering)

    def _vanish(self) -> None:
        self.gone_until = float("inf") if self.never_reappear else time.monotonic() + self.reappear_after_s

    def _present(self) -> bool:
        return self.gone_until is None or time.monotonic() >= self.gone_until

    def open(self, timeout_s: float) -> None:
        self.open_calls += 1
        deadline = time.monotonic() + timeout_s
        while not self._present():
            if time.monotonic() >= deadline:
                raise ConnectTimeout("fake port did not reappear")
            time.sleep(0.005)
        if self.gone_until is not None:  # re-enumerated just now
            self.gone_until = None
            self.rx.clear()
            if self.emit_ready:
                self.rx.append("I (1) boot: waking")
                self.rx.append(READY)
        self.opened = True

    def close(self) -> None:
        self.opened = False

    def read_line(self, timeout_s: float) -> Optional[str]:
        deadline = time.monotonic() + timeout_s
        while True:
            if self.rx:
                return self.rx.popleft()
            if not self.opened or not self._present() or self.gone_until is not None:
                raise TransportClosed("fake port gone")
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.002)

    def write_line(self, line: str) -> None:
        if not self.opened or self.gone_until is not None:
            raise TransportClosed("fake port gone")
        if self.mute:
            return
        if self.drop_before_reply:
            self.drop_before_reply = False
            self.device.received.append(line)  # arrived, but the reply is lost
            self._vanish()
            return
        self.rx.extend(self.device.handle(line))
        if self.device.after_reply == "vanish":
            self.device.after_reply = None
            self._vanish()


def serve_stdio() -> int:
    device = FakeDevice()
    print("I (0) boot: starting", flush=True)
    print(READY, flush=True)
    for line in sys.stdin:
        for out in device.handle(line.rstrip("\r\n")):
            print(out, flush=True)
        if device.after_reply == "vanish":
            os._exit(0)  # the "USB port" disappears
    return 0


if __name__ == "__main__":
    sys.exit(serve_stdio())
