"""`display dump` frame decoding and a small stdlib PNG encoder.

The dump is ``{"w":200,"h":200,"fmt":"1bpp-msb","crc32":"xxxxxxxx","b64":"..."}``: row-major,
MSB = leftmost pixel, bit 1 = black ink (qz/gfx/framebuffer.hpp). PNG grayscale 1-bit uses
0 = black, so the bits are inverted on the way out.
"""

from __future__ import annotations

import base64
import binascii
import struct
import zlib
from dataclasses import dataclass
from typing import Any, Mapping

from .errors import ProtocolError

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@dataclass(frozen=True)
class Frame:
    width: int
    height: int
    bits: bytes  # packed rows, MSB first, bit 1 = black; each row padded to whole bytes

    @property
    def row_bytes(self) -> int:
        return (self.width + 7) // 8


def decode_dump(data: Mapping[str, Any]) -> Frame:
    """Validate and decode the JSON object returned by ``display dump``."""
    try:
        width, height = int(data["w"]), int(data["h"])
        fmt, b64 = data["fmt"], data["b64"]
    except (KeyError, TypeError, ValueError) as exc:
        raise ProtocolError(f"display dump: missing or bad field ({exc})") from exc
    if fmt != "1bpp-msb":
        raise ProtocolError(f"display dump: unsupported format {fmt!r}")
    if width <= 0 or height <= 0:
        raise ProtocolError("display dump: bad dimensions")
    try:
        raw = base64.b64decode(b64, validate=True)
    except (binascii.Error, TypeError, ValueError) as exc:
        raise ProtocolError(f"display dump: bad base64 ({exc})") from exc
    expected = ((width + 7) // 8) * height
    if len(raw) != expected:
        raise ProtocolError(f"display dump: {len(raw)} bytes, expected {expected}")
    crc = data.get("crc32")
    if crc is not None:
        try:
            want = int(str(crc), 16)
        except ValueError as exc:
            raise ProtocolError(f"display dump: bad crc32 {crc!r}") from exc
        got = zlib.crc32(raw) & 0xFFFFFFFF
        if got != want:
            raise ProtocolError(f"display dump: crc32 mismatch (device {want:08x}, host {got:08x})")
    return Frame(width, height, raw)


def _chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def encode_png(frame: Frame) -> bytes:
    """Encode a Frame as a 1-bit grayscale PNG (black ink = 0)."""
    rb = frame.row_bytes
    pad_bits = rb * 8 - frame.width
    rows = bytearray()
    for y in range(frame.height):
        row = bytearray(b ^ 0xFF for b in frame.bits[y * rb : (y + 1) * rb])
        if pad_bits:
            row[-1] |= (1 << pad_bits) - 1  # padding bits are white, whatever the source said
        rows.append(0)  # filter type: None
        rows += row
    header = struct.pack(">IIBBBBB", frame.width, frame.height, 1, 0, 0, 0, 0)
    return (
        _PNG_SIGNATURE
        + _chunk(b"IHDR", header)
        + _chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + _chunk(b"IEND", b"")
    )


def dump_to_png(data: Mapping[str, Any]) -> bytes:
    return encode_png(decode_dump(data))
