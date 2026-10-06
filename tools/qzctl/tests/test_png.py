import base64
import struct
import zlib

import pytest

from fakes import KNOWN_CRC, KNOWN_FRAME
from qzctl.errors import ProtocolError
from qzctl.png import Frame, decode_dump, dump_to_png, encode_png


def read_png(data: bytes):
    """Independent minimal decoder: returns (w, h, depth, color_type, rows) and checks CRCs."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks, idat = 8, [], b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length : pos + 12 + length])
        assert crc == zlib.crc32(kind + payload) & 0xFFFFFFFF, kind
        chunks.append(kind)
        if kind == b"IDAT":
            idat += payload
        if kind == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", payload)
            assert (comp, filt, inter) == (0, 0, 0)
        pos += 12 + length
    assert chunks[0] == b"IHDR" and chunks[-1] == b"IEND" and pos == len(data)
    raw = zlib.decompress(idat)
    stride = (w * depth + 7) // 8
    assert len(raw) == (stride + 1) * h
    rows = []
    for y in range(h):
        assert raw[y * (stride + 1)] == 0  # filter None
        rows.append(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
    return w, h, depth, ctype, rows


def dump(frame: bytes, w=200, h=200, **over):
    d = {"w": w, "h": h, "fmt": "1bpp-msb", "crc32": f"{zlib.crc32(frame) & 0xFFFFFFFF:08x}",
         "b64": base64.b64encode(frame).decode()}
    d.update(over)
    return d


def test_known_frame_crc_matches_firmware_vector():
    assert zlib.crc32(KNOWN_FRAME) == int(KNOWN_CRC, 16)


def test_known_frame_round_trip():
    png = dump_to_png(dump(KNOWN_FRAME, crc32=KNOWN_CRC))
    w, h, depth, ctype, rows = read_png(png)
    assert (w, h, depth, ctype) == (200, 200, 1, 0)
    # device bit 1 = black ink = PNG gray 0, so rows are the bitwise inverse of the dump
    assert b"".join(rows) == bytes(b ^ 0xFF for b in KNOWN_FRAME)


def test_png_is_deterministic():
    assert dump_to_png(dump(KNOWN_FRAME)) == dump_to_png(dump(KNOWN_FRAME))


def test_tiny_frame_exact_pixels():
    # 8x2: row0 = ink on the left four pixels, row1 = ink on the right four
    _, _, _, _, rows = read_png(encode_png(Frame(8, 2, b"\xf0\x0f")))
    assert rows == [b"\x0f", b"\xf0"]


def test_width_not_multiple_of_eight_pads_white():
    # 3 wide: ink pattern 101 followed by garbage padding bits that must come out white
    _, _, _, _, rows = read_png(encode_png(Frame(3, 1, bytes([0b10100111]))))
    assert rows == [bytes([0b01011111])]


def test_blank_frame_is_all_white():
    _, _, _, _, rows = read_png(dump_to_png(dump(bytes(5000))))
    assert set(b"".join(rows)) == {0xFF}


def test_dump_without_crc_is_accepted():
    d = dump(KNOWN_FRAME)
    del d["crc32"]
    assert decode_dump(d).bits == KNOWN_FRAME


@pytest.mark.parametrize("over", [
    {"fmt": "8bpp"}, {"b64": "!!!not base64!!!"}, {"b64": base64.b64encode(b"\x00" * 10).decode()},
    {"crc32": "deadbeef"}, {"crc32": "zz"}, {"w": 0}, {"w": "x"},
])
def test_bad_dumps_rejected(over):
    with pytest.raises(ProtocolError):
        decode_dump(dump(KNOWN_FRAME, **over))


def test_missing_field_rejected():
    d = dump(KNOWN_FRAME)
    del d["b64"]
    with pytest.raises(ProtocolError):
        decode_dump(d)
