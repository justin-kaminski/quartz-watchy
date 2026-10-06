"""Console protocol v1: line framing, parsing and request building (ARCHITECTURE section 16).

Only lines starting with ``@QZ1`` are protocol; everything else is a log line and is ignored
by the parser (``parse_line`` returns None for it).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any, Optional, Sequence, Union

from .errors import ProtocolError

PREFIX = "@QZ1"
MAX_REQUEST_BYTES = 256
_ID_RE = re.compile(r"[A-Za-z0-9]{1,8}")
_CODE_RE = re.compile(r"[a-z_]+")
_NEEDS_QUOTES = re.compile(r'[\s"\\]')


@dataclass(frozen=True)
class Response:
    """One reply to one request. ``id`` is None when the device answered ``-``."""

    id: Optional[str]
    ok: bool
    code: Optional[str]  # error code (qz::Errc token) for ERR, None for OK
    data: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Event:
    """Asynchronous ``@QZ1 ! EVT {...}`` line."""

    data: dict[str, Any]

    @property
    def name(self) -> Optional[str]:
        value = self.data.get("evt")
        return value if isinstance(value, str) else None


Message = Union[Response, Event]


def _json_object(text: str, what: str) -> dict[str, Any]:
    try:
        value = json.loads(text)
    except ValueError as exc:
        raise ProtocolError(f"{what}: invalid JSON ({exc})") from exc
    if not isinstance(value, dict):
        raise ProtocolError(f"{what}: JSON payload is not an object")
    return value


def parse_line(line: str) -> Optional[Message]:
    """Parse one received line.

    Returns None for log lines (anything not starting with ``@QZ1``). Raises ProtocolError
    for a line that claims to be protocol but is malformed.
    """
    line = line.rstrip("\r\n")
    if not line.startswith(PREFIX):
        return None
    if not line.startswith(PREFIX + " "):
        raise ProtocolError(f"malformed protocol line: {line[:60]!r}")
    parts = line[len(PREFIX) + 1 :].split(" ", 2)
    if len(parts) < 3:
        raise ProtocolError(f"truncated protocol line: {line[:60]!r}")
    ident, kind, tail = parts
    if kind == "EVT":
        if ident != "!":
            raise ProtocolError("EVT line must use id '!'")
        return Event(_json_object(tail, "EVT"))
    if ident == "!" or not (ident == "-" or _ID_RE.fullmatch(ident)):
        raise ProtocolError(f"bad response id {ident!r}")
    rid = None if ident == "-" else ident
    if kind == "OK":
        return Response(rid, True, None, _json_object(tail, "OK"))
    if kind == "ERR":
        code, _, payload = tail.partition(" ")
        if not _CODE_RE.fullmatch(code):
            raise ProtocolError(f"bad error code {code!r}")
        return Response(rid, False, code, _json_object(payload, "ERR"))
    raise ProtocolError(f"unknown response kind {kind!r}")


def quote_arg(arg: str) -> str:
    """Quote one argument the way the device tokenizer expects (``"..."`` with \\" and \\\\)."""
    if arg and not _NEEDS_QUOTES.search(arg):
        return arg
    return '"' + arg.replace("\\", "\\\\").replace('"', '\\"') + '"'


def join_command(args: Sequence[str]) -> str:
    """Join argv-style words into one command line, quoting where needed."""
    return " ".join(quote_arg(a) for a in args)


def build_request(command: str, req_id: Optional[str]) -> str:
    """Build the request line (without the trailing newline); validates framing limits."""
    if not command.strip():
        raise ProtocolError("empty command")
    if "\n" in command or "\r" in command:
        raise ProtocolError("command contains a line break")
    if req_id is not None and not _ID_RE.fullmatch(req_id):
        raise ProtocolError(f"invalid request id {req_id!r} (1-8 chars [A-Za-z0-9])")
    line = f"#{req_id} {command}" if req_id is not None else command
    if len(line.encode("utf-8")) > MAX_REQUEST_BYTES:
        raise ProtocolError(f"request longer than {MAX_REQUEST_BYTES} bytes")
    return line
