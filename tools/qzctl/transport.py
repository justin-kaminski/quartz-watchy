"""Transports: a line-oriented byte link to the device.

``Transport`` is the only thing ``Client`` knows about. ``SerialTransport`` talks to the
USB-Serial-JTAG CDC port (and finds it again after re-enumeration); ``SubprocessTransport``
talks to the stdio of any program that speaks the protocol (a simulator, a fake device).
"""

from __future__ import annotations

import abc
import os
import queue
import subprocess
import threading
import time
from typing import Any, Callable, Iterable, Optional, Sequence

from .errors import ConnectTimeout, QzError, TransportClosed

DEFAULT_VID = 0x303A
DEFAULT_PID = 0x1001
POLL_INTERVAL_S = 0.1


class LineBuffer:
    """Splits a byte stream into text lines (``\\n`` terminated, trailing ``\\r`` removed)."""

    def __init__(self) -> None:
        self._pending = bytearray()

    def feed(self, data: bytes) -> list[str]:
        self._pending += data
        lines: list[str] = []
        while True:
            idx = self._pending.find(b"\n")
            if idx < 0:
                return lines
            raw = bytes(self._pending[:idx])
            del self._pending[: idx + 1]
            lines.append(raw.rstrip(b"\r").decode("utf-8", errors="replace"))

    def clear(self) -> None:
        self._pending.clear()


class Transport(abc.ABC):
    """A reopenable line link. All timeouts are in seconds."""

    @abc.abstractmethod
    def open(self, timeout_s: float) -> None:
        """(Re)open the link, waiting up to ``timeout_s`` for the device. Raises ConnectTimeout."""

    @abc.abstractmethod
    def close(self) -> None:
        """Close the link; safe to call repeatedly."""

    @abc.abstractmethod
    def read_line(self, timeout_s: float) -> Optional[str]:
        """Next received line, or None on timeout. Raises TransportClosed once the link is gone
        and all buffered lines have been delivered."""

    @abc.abstractmethod
    def write_line(self, line: str) -> None:
        """Send one line (the transport appends ``\\n``). Raises TransportClosed."""

    def describe(self) -> str:
        return type(self).__name__


# ---------------------------------------------------------------------------------------------
# Serial


def find_port(
    path: Optional[str] = None,
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    serial_number: Optional[str] = None,
    lister: Optional[Callable[[], Iterable[Any]]] = None,
) -> Optional[str]:
    """Device node of the watch if it is currently enumerated, else None.

    With ``path`` the node merely has to exist. Otherwise match USB ``vid:pid`` (and
    ``serial_number`` when given); several matches without a serial number are ambiguous.
    """
    if path:
        return path if os.path.exists(path) else None
    if lister is None:
        from serial.tools import list_ports

        lister = list_ports.comports
    matches = sorted(
        str(p.device)
        for p in lister()
        if p.vid == vid and p.pid == pid and (serial_number is None or p.serial_number == serial_number)
    )
    if len(matches) > 1:
        raise QzError(
            f"{len(matches)} devices match {vid:04x}:{pid:04x} ({', '.join(matches)}); "
            "pass --port or --serial"
        )
    return matches[0] if matches else None


class SerialTransport(Transport):
    """USB CDC serial. Reopening polls ``find_port`` until the device is back."""

    def __init__(
        self,
        path: Optional[str] = None,
        vid: int = DEFAULT_VID,
        pid: int = DEFAULT_PID,
        serial_number: Optional[str] = None,
        *,
        baud: int = 115200,
        lister: Optional[Callable[[], Iterable[Any]]] = None,
    ) -> None:
        self._path, self._vid, self._pid, self._sn = path, vid, pid, serial_number
        self._baud, self._lister = baud, lister
        self._ser: Any = None
        self._buf = LineBuffer()
        self._lines: list[str] = []

    def describe(self) -> str:
        if self._path:
            return f"serial {self._path}"
        sn = f" sn={self._sn}" if self._sn else ""
        return f"serial {self._vid:04x}:{self._pid:04x}{sn}"

    def open(self, timeout_s: float) -> None:
        import serial

        self.close()
        deadline = time.monotonic() + timeout_s
        last_error: Optional[Exception] = None
        while True:
            port = find_port(self._path, self._vid, self._pid, self._sn, self._lister)
            if port is not None:
                try:
                    ser = serial.Serial()
                    ser.port, ser.baudrate, ser.timeout = port, self._baud, 0
                    # Never touch DTR/RTS. Linux asserts both on open; pyserial then applying
                    # dtr=False before rts=False passes through "RTS high, DTR low", which the
                    # ESP32-S3 USB-Serial/JTAG treats as a chip reset (measured on a Watchy v3,
                    # 2026-10-06: every such open rebooted the watch). Leaving pyserial's
                    # defaults (both asserted) causes no transition and no reset.
                    ser.open()
                except (serial.SerialException, OSError) as exc:
                    last_error = exc  # node exists but is not usable yet (udev, still booting)
                else:
                    self._ser = ser
                    self._buf.clear()
                    self._lines.clear()
                    return
            if time.monotonic() >= deadline:
                what = f" (last error: {last_error})" if last_error else ""
                raise ConnectTimeout(f"{self.describe()} did not appear within {timeout_s:g} s{what}")
            time.sleep(POLL_INTERVAL_S)

    def close(self) -> None:
        ser, self._ser = self._ser, None
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001 - a vanished port may fail to close cleanly
                pass

    def _closed(self, why: Exception) -> TransportClosed:
        self.close()
        return TransportClosed(f"{self.describe()} disconnected ({why})")

    def read_line(self, timeout_s: float) -> Optional[str]:
        import serial

        deadline = time.monotonic() + timeout_s
        while True:
            if self._lines:
                return self._lines.pop(0)
            if self._ser is None:
                raise TransportClosed(f"{self.describe()} is closed")
            remaining = deadline - time.monotonic()
            try:
                self._ser.timeout = max(0.0, min(remaining, 0.05))
                data = self._ser.read(max(1, self._ser.in_waiting))
            except (serial.SerialException, OSError, ValueError) as exc:
                raise self._closed(exc) from exc
            if data:
                self._lines.extend(self._buf.feed(data))
            elif time.monotonic() >= deadline:
                return None

    def write_line(self, line: str) -> None:
        import serial

        if self._ser is None:
            raise TransportClosed(f"{self.describe()} is closed")
        try:
            self._ser.write(line.encode("utf-8") + b"\n")
            self._ser.flush()
        except (serial.SerialException, OSError, ValueError) as exc:
            raise self._closed(exc) from exc


# ---------------------------------------------------------------------------------------------
# Subprocess / stdio


class SubprocessTransport(Transport):
    """stdio of a child process. ``open`` (re)spawns it when it is not running, so a restart of
    the simulated device looks like a re-enumeration."""

    def __init__(self, argv: Sequence[str], *, cwd: Optional[str] = None) -> None:
        self._argv, self._cwd = list(argv), cwd
        self._proc: Optional[subprocess.Popen[bytes]] = None
        self._queue: "queue.Queue[Optional[str]]" = queue.Queue()
        self._thread: Optional[threading.Thread] = None

    def describe(self) -> str:
        return f"subprocess {' '.join(self._argv)}"

    def open(self, timeout_s: float) -> None:  # noqa: ARG002 - spawning is immediate
        if self._proc is not None and self._proc.poll() is None:
            return
        self.close()
        try:
            proc = subprocess.Popen(
                self._argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                cwd=self._cwd,
            )
        except OSError as exc:
            raise ConnectTimeout(f"cannot start {self._argv[0]!r}: {exc}") from exc
        self._proc = proc
        self._queue = queue.Queue()
        self._thread = threading.Thread(target=self._pump, args=(proc, self._queue), daemon=True)
        self._thread.start()

    @staticmethod
    def _pump(proc: "subprocess.Popen[bytes]", out: "queue.Queue[Optional[str]]") -> None:
        assert proc.stdout is not None
        buf = LineBuffer()
        with proc.stdout:
            while True:
                chunk = proc.stdout.readline()
                if not chunk:
                    break
                for line in buf.feed(chunk):
                    out.put(line)
        out.put(None)  # EOF sentinel

    def close(self) -> None:
        proc, self._proc = self._proc, None
        thread, self._thread = self._thread, None
        if proc is None:
            return
        try:
            if proc.stdin is not None:
                proc.stdin.close()
        except OSError:
            pass
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if thread is not None:
            thread.join(timeout=2)

    def read_line(self, timeout_s: float) -> Optional[str]:
        if self._proc is None:
            raise TransportClosed(f"{self.describe()} is not running")
        try:
            item = self._queue.get(timeout=max(0.0, timeout_s))
        except queue.Empty:
            return None
        if item is None:
            self._queue.put(None)  # keep reporting EOF
            raise TransportClosed(f"{self.describe()} exited")
        return item

    def write_line(self, line: str) -> None:
        proc = self._proc
        if proc is None or proc.stdin is None:
            raise TransportClosed(f"{self.describe()} is not running")
        try:
            proc.stdin.write(line.encode("utf-8") + b"\n")
            proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            raise TransportClosed(f"{self.describe()} exited ({exc})") from exc
