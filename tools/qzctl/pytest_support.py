"""Helpers for the owner-run device suites (test_apps/console, test_apps/wake).

The suites are plain pytest: they drive an already flashed watch through qzctl. Environment:
``QZ_DEVICE=1`` enables them (CI never sets it), ``QZ_PORT`` / ``QZ_SERIAL`` select the device
(default: USB 303a:1001), ``QZ_ALLOW_DESTRUCTIVE=1`` also runs commands that erase state.
"""

from __future__ import annotations

import os

from .client import Client
from .transport import SerialTransport


def device_enabled() -> bool:
    return os.environ.get("QZ_DEVICE") == "1"


def destructive_enabled() -> bool:
    return os.environ.get("QZ_ALLOW_DESTRUCTIVE") == "1"


def client_from_env() -> Client:
    """A connected Client for the attached watch (raises ConnectTimeout if absent)."""
    transport = SerialTransport(os.environ.get("QZ_PORT") or None, serial_number=os.environ.get("QZ_SERIAL") or None)
    client = Client(transport)
    client.connect(15.0)
    return client
