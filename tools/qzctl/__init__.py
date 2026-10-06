"""qzctl: host-side client for the Quartz console protocol v1 (docs/ARCHITECTURE.md section 16).

Stdlib plus pyserial. The client talks to a ``Transport`` (USB CDC serial, or the stdio of any
subprocess such as a simulator), so it is testable without hardware.
"""

from .client import Client, ReadyResult, ResetResult
from .errors import ConnectTimeout, ProtocolError, QzError, RequestTimeout, TransportClosed
from .protocol import Event, Response, parse_line

__all__ = [
    "Client",
    "ConnectTimeout",
    "Event",
    "ProtocolError",
    "QzError",
    "ReadyResult",
    "RequestTimeout",
    "Response",
    "ResetResult",
    "TransportClosed",
    "parse_line",
]
