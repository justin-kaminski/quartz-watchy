"""Protocol client: request/response matching by id, events, reconnect across re-enumeration."""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Optional

from .errors import ProtocolError, RequestTimeout, TransportClosed
from .protocol import Event, Response, build_request, parse_line
from .transport import Transport

_PROBE_COMMAND = "version"


@dataclass(frozen=True)
class ReadyResult:
    """``event`` is the ``ready`` event; None when the watch was already up and answered a
    probe instead (the event was emitted before we connected)."""

    event: Optional[Event]


@dataclass(frozen=True)
class ResetResult:
    response: Optional[Response]  # None if the link dropped before the reply was read
    ready: ReadyResult


class Client:
    def __init__(
        self,
        transport: Transport,
        *,
        request_timeout_s: float = 10.0,
        reconnect_timeout_s: float = 30.0,
        probe_interval_s: float = 1.5,
        on_log: Optional[Callable[[str], None]] = None,
        on_event: Optional[Callable[[Event], None]] = None,
    ) -> None:
        self.transport = transport
        self.request_timeout_s = request_timeout_s
        self.reconnect_timeout_s = reconnect_timeout_s
        self.probe_interval_s = probe_interval_s
        self.on_log = on_log
        self.on_event = on_event
        self.events: list[Event] = []  # every event seen, in order
        self.logs: deque[str] = deque(maxlen=2000)  # non-protocol lines
        self.malformed: list[str] = []  # @QZ1 lines that failed to parse
        self.stale: list[Response] = []  # replies whose id matched no pending request
        self.retries = 0  # requests re-sent after a reconnect
        self._counter = 0
        self._ready: Optional[Event] = None

    # -- lifecycle ---------------------------------------------------------------------------

    def connect(self, timeout_s: Optional[float] = None) -> None:
        self.transport.open(self.reconnect_timeout_s if timeout_s is None else timeout_s)

    def close(self) -> None:
        self.transport.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def reconnect(self, timeout_s: Optional[float] = None) -> None:
        """Drop the link and wait for the device to enumerate again."""
        self.transport.close()
        self._ready = None
        self.connect(timeout_s)

    # -- reading -----------------------------------------------------------------------------

    def _next_id(self) -> str:
        self._counter = self._counter % 0xFFFFFF + 1  # <= 6 hex digits
        return format(self._counter, "x")

    def _pump(self, deadline: float, until_ready: bool = False) -> Optional[Response]:
        """Process lines until a Response arrives (returned) or the deadline passes (None).
        Logs, events and malformed lines are recorded and skipped. With ``until_ready`` it also
        returns None as soon as a ``ready`` event has been seen. May raise TransportClosed."""
        while True:
            remaining = deadline - time.monotonic()
            line = self.transport.read_line(max(0.0, remaining))
            if line is None:
                return None
            try:
                msg = parse_line(line)
            except ProtocolError:
                self.malformed.append(line)
                continue
            if msg is None:
                self.logs.append(line)
                if self.on_log:
                    self.on_log(line)
            elif isinstance(msg, Event):
                self.events.append(msg)
                if msg.name == "ready":
                    self._ready = msg
                if self.on_event:
                    self.on_event(msg)
                if until_ready and self._ready is not None:
                    return None
            else:
                return msg

    def _await_response(self, rid: str, deadline: float) -> Response:
        while True:
            msg = self._pump(deadline)
            if msg is None:
                raise RequestTimeout("no reply before the deadline")
            if msg.id == rid:
                return msg
            self.stale.append(msg)

    # -- requests ----------------------------------------------------------------------------

    def _send(self, command: str, rid: str, timeout_s: float) -> Response:
        line = build_request(command, rid)
        deadline = time.monotonic() + timeout_s
        self.transport.write_line(line)
        try:
            return self._await_response(rid, deadline)
        except RequestTimeout:
            raise RequestTimeout(f"no reply to {command!r} within {timeout_s:g} s") from None

    def request(self, command: str, *, timeout_s: Optional[float] = None, retry: bool = True) -> Response:
        """Send one request with a fresh id and return its reply (OK or ERR).

        If the link drops before the reply, reconnect, wait for the watch (``ready`` event or a
        probe) and re-send the *same id* once (``retry=False`` for non-idempotent commands).
        """
        timeout = self.request_timeout_s if timeout_s is None else timeout_s
        rid = self._next_id()
        try:
            return self._send(command, rid, timeout)
        except TransportClosed:
            if not retry:
                raise
        self.reconnect()
        self.wait_ready(self.reconnect_timeout_s)
        self.retries += 1
        return self._send(command, rid, timeout)

    # -- ready / reset -----------------------------------------------------------------------

    def _probe(self) -> bool:
        try:
            self._send(_PROBE_COMMAND, self._next_id(), min(1.0, self.probe_interval_s))
        except RequestTimeout:
            return False
        return True

    def wait_ready(self, timeout_s: float = 30.0, *, probe: bool = True) -> ReadyResult:
        """Wait for the ``ready`` event. With ``probe`` (default), also accept a reply to a
        periodic ``version`` request: a tethered watch that is already running emitted
        ``ready`` before we connected. ``probe=False`` insists on the event."""
        deadline = time.monotonic() + timeout_s
        next_probe = time.monotonic() + self.probe_interval_s
        while True:
            if self._ready is not None:
                event, self._ready = self._ready, None
                return ReadyResult(event)
            now = time.monotonic()
            if now >= deadline:
                raise RequestTimeout(f"no ready event within {timeout_s:g} s")
            if probe and now >= next_probe:
                if self._probe():
                    if self._ready is not None:  # the event arrived while probing; prefer it
                        continue
                    return ReadyResult(None)
                next_probe = time.monotonic() + self.probe_interval_s
                continue
            wake = min(deadline, next_probe) if probe else deadline
            self._pump_events(wake)

    def _pump_events(self, until: float) -> None:
        # _pump only returns on a Response; with nothing pending any reply is stale.
        msg = self._pump(until, until_ready=True)
        if msg is not None:
            self.stale.append(msg)

    def run_expecting_reset(
        self,
        command: str,
        *,
        vanish_timeout_s: float = 10.0,
        ready_timeout_s: float = 30.0,
    ) -> ResetResult:
        """For ``sleep <s>`` / ``reboot``: send, read the reply if it arrives, ride out the USB
        port disappearing and re-enumerating, then wait for the new ``ready`` event."""
        rid = self._next_id()
        line = build_request(command, rid)
        response: Optional[Response] = None
        self._ready = None
        self.transport.write_line(line)
        try:
            response = self._await_response(rid, time.monotonic() + self.request_timeout_s)
            deadline = time.monotonic() + vanish_timeout_s
            while self._ready is None:  # ready on the same link means it never dropped
                extra = self._pump(deadline, until_ready=True)
                if extra is not None:
                    self.stale.append(extra)
                elif self._ready is None:
                    raise RequestTimeout("link did not drop and no ready event after the reply")
        except TransportClosed:
            self.reconnect()
        return ResetResult(response, self.wait_ready(ready_timeout_s))
