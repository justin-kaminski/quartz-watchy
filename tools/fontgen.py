#!/usr/bin/env python3
"""Deterministic BDF -> C++ font generator for qz_gfx (WP-06). Python stdlib only.

    python3 tools/fontgen.py            # regenerate components/qz_gfx/src/generated/fonts_data.hpp
    python3 tools/fontgen.py --check    # exit 1 if the committed file differs from a fresh generation

Input : components/qz_gfx/fonts/*.bdf (Spleen, BSD-2-Clause; see fonts/PROVENANCE.md). Every source
        file is pinned by SHA-256 below; a mismatch is an error, so the output is a pure function
        of known bytes.
Output: one header holding, per font, a bitmap blob (tight per-glyph bounding boxes, rows padded to
        whole bytes, MSB first) and a contiguous glyph table [first, first + n). Code points inside
        the range that the font does not provide share the fallback glyph's data.
Scaling: a font entry may carry integer (sx, sy) scale factors, applied by pixel replication to
        bitmap, offsets, advance, ascent and line height. Only kHuge uses it (3x4 of Spleen 12x24):
        no Spleen size gives digits >= 48 px tall AND "88:88" within 200 px unscaled.
No timestamps, no host paths, fixed iteration order: two runs give byte-identical output.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "components" / "qz_gfx" / "fonts"
OUT_PATH = ROOT / "components" / "qz_gfx" / "src" / "generated" / "fonts_data.hpp"

# SHA-256 of the pinned upstream files (Spleen 2.2.0, commit 0493c34e22791824767c618fed42c434b477662c).
PINNED_SHA256 = {
    "spleen-6x12.bdf": "fc0743d164690f99b7e2e1b9d503180e4c719a9831ae03fd8f6da18c857dee27",
    "spleen-8x16.bdf": "4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae",
    "spleen-12x24.bdf": "181989f2c5f47b34c2474cd424b387b0002aa77fc970e59ee31e16733af24687",
    "spleen-16x32.bdf": "f6db2549d46c5699ceca7ccc26e747c8bdad3af76239f86aae6ed995bd2f3d37",
}


def _latin1() -> list[int]:
    return [*range(0x20, 0x7F), *range(0xA0, 0x100)]


@dataclass(frozen=True)
class FontSpec:
    ident: str  # C++ identifier stem (kSmall -> "Small")
    bdf: str
    codes: tuple[int, ...]  # code points to render; the table spans [min, max]
    fallback: int  # code point whose glyph backs every missing entry
    sx: int = 1
    sy: int = 1


FONTS = (
    FontSpec("Small", "spleen-6x12.bdf", tuple(_latin1()), ord("?")),
    FontSpec("Medium", "spleen-8x16.bdf", tuple(_latin1()), ord("?")),
    FontSpec("Large", "spleen-16x32.bdf", tuple(_latin1()), ord("?")),
    # Digits and clock punctuation only. Spleen 12x24 scaled 3 (x) by 4 (y): digit ink height
    # 15 * 4 = 60 px, advance 36 px, "88:88" = 180 px.
    FontSpec(
        "Huge",
        "spleen-12x24.bdf",
        tuple([ord(c) for c in " %-.0123456789:"]),
        ord(" "),
        sx=3,
        sy=4,
    ),
    # Stacked face: two digits per row. Spleen 16x32 digits scaled 4 (x) by 3 (y): advance 64 px,
    # so "88" = 128 px; ink height ~66 px, two rows fit with room for status and date.
    FontSpec(
        "Giant",
        "spleen-16x32.bdf",
        tuple([ord(c) for c in " -0123456789"]),
        ord(" "),
        sx=4,
        sy=3,
    ),
)


@dataclass
class BdfGlyph:
    advance: int
    w: int
    h: int
    xoff: int
    yoff: int
    rows: list[list[int]]  # h rows of w pixels (0/1)


@dataclass
class BdfFont:
    ascent: int
    descent: int
    glyphs: dict[int, BdfGlyph]


def parse_bdf(path: Path) -> BdfFont:
    data = path.read_bytes()
    want = PINNED_SHA256[path.name]
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        sys.exit(f"fontgen: {path.name} sha256 {got} != pinned {want}")
    ascent = descent = None
    glyphs: dict[int, BdfGlyph] = {}
    enc = adv = bbx = None
    rows: list[str] | None = None
    for line in data.decode("ascii").splitlines():
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if rows is not None:
            if key == "ENDCHAR":
                if enc is not None and enc >= 0:
                    assert adv is not None and bbx is not None
                    w, h, xo, yo = bbx
                    px = [
                        [(int(r, 16) >> (len(r) * 4 - 1 - c)) & 1 for c in range(w)]
                        for r in rows
                    ]
                    assert len(px) == h, f"{path.name}: bitmap height mismatch for U+{enc:04X}"
                    glyphs[enc] = BdfGlyph(adv, w, h, xo, yo, px)
                rows = None
            else:
                rows.append(key)
        elif key == "FONT_ASCENT":
            ascent = int(parts[1])
        elif key == "FONT_DESCENT":
            descent = int(parts[1])
        elif key == "STARTCHAR":
            enc = adv = bbx = None
        elif key == "ENCODING":
            enc = int(parts[1])
        elif key == "DWIDTH":
            adv = int(parts[1])
        elif key == "BBX":
            bbx = tuple(int(p) for p in parts[1:5])
        elif key == "BITMAP":
            rows = []
    assert ascent is not None and descent is not None
    return BdfFont(ascent, descent, glyphs)


@dataclass
class OutGlyph:
    offset: int
    w: int
    h: int
    xo: int
    yo: int
    adv: int


def crop(g: BdfGlyph) -> tuple[list[list[int]], int, int]:
    """Tight ink box: returns (rows, x_offset, y_offset_of_bottom_row); no ink gives no rows."""
    ink_rows = [i for i, r in enumerate(g.rows) if any(r)]
    if not ink_rows:
        return [], g.xoff, g.yoff
    ink_cols = [c for c in range(g.w) if any(r[c] for r in g.rows)]
    top, bottom = ink_rows[0], ink_rows[-1]
    left, right = ink_cols[0], ink_cols[-1]
    rows = [r[left : right + 1] for r in g.rows[top : bottom + 1]]
    return rows, g.xoff + left, g.yoff + (g.h - 1 - bottom)


def scale(rows: list[list[int]], sx: int, sy: int) -> list[list[int]]:
    out = []
    for r in rows:
        wide = [v for v in r for _ in range(sx)]
        out.extend([list(wide) for _ in range(sy)])
    return out


def pack(rows: list[list[int]]) -> bytes:
    out = bytearray()
    for r in rows:
        for i in range(0, len(r), 8):
            chunk = r[i : i + 8]
            b = 0
            for k, v in enumerate(chunk):
                b |= v << (7 - k)
            out.append(b)
    return bytes(out)


def fits(v: int, lo: int, hi: int, what: str) -> int:
    if not lo <= v <= hi:
        sys.exit(f"fontgen: {what}={v} outside [{lo}, {hi}]")
    return v


def build(spec: FontSpec) -> tuple[BdfFont, list[OutGlyph], bytes, int]:
    font = parse_bdf(FONT_DIR / spec.bdf)
    first, last = min(spec.codes), max(spec.codes)
    blob = bytearray()
    seen: dict[bytes, int] = {}
    table: dict[int, OutGlyph] = {}
    for cp in sorted(set(spec.codes) | {spec.fallback}):
        if cp not in font.glyphs:
            sys.exit(f"fontgen: {spec.bdf} lacks U+{cp:04X}")
        g = font.glyphs[cp]
        rows, xo, yo_bottom = crop(g)
        if rows:
            rows = scale(rows, spec.sx, spec.sy)
            data = pack(rows)
            if data not in seen:
                seen[data] = len(blob)
                blob += data
            off = seen[data]
            w, h = len(rows[0]), len(rows)
            y_top = -(yo_bottom * spec.sy + h)  # y-down from baseline to the top row
        else:
            off, w, h, y_top = 0, 0, 0, 0
        table[cp] = OutGlyph(
            offset=off,
            w=fits(w, 0, 255, f"{spec.bdf} U+{cp:04X} width"),
            h=fits(h, 0, 255, f"{spec.bdf} U+{cp:04X} height"),
            xo=fits(xo * spec.sx, -128, 127, f"{spec.bdf} U+{cp:04X} x_offset"),
            yo=fits(y_top, -128, 127, f"{spec.bdf} U+{cp:04X} y_offset"),
            adv=fits(g.advance * spec.sx, 0, 255, f"{spec.bdf} U+{cp:04X} advance"),
        )
    wanted = set(spec.codes)
    glyphs = [
        table[cp] if cp in wanted else table[spec.fallback] for cp in range(first, last + 1)
    ]
    return font, glyphs, bytes(blob), spec.fallback - first


def emit() -> str:
    o: list[str] = []
    o.append("// GENERATED by tools/fontgen.py from components/qz_gfx/fonts/*.bdf - DO NOT EDIT.")
    o.append("// Spleen 2.2.0 (BSD-2-Clause, (c) Frederic Cambus); see components/qz_gfx/fonts/PROVENANCE.md.")
    o.append("// Regenerate: python3 tools/fontgen.py   Verify: python3 tools/fontgen.py --check")
    o.append("// NOLINTBEGIN")
    o.append("#pragma once")
    o.append("")
    o.append('#include "qz/gfx/framebuffer.hpp"')
    o.append("")
    o.append("#include <cstdint>")
    o.append("")
    o.append("namespace qz::gfx::generated {")
    o.append("")
    fonts_decl: list[str] = []
    scaled = lambda sp: "" if sp.sx == sp.sy == 1 else f"-x{sp.sx}y{sp.sy}"  # noqa: E731
    for spec in FONTS:
        font, glyphs, blob, fb_index = build(spec)
        stem = spec.ident
        first = min(spec.codes)
        o.append(f"// {spec.ident}: {spec.bdf}, scale {spec.sx}x{spec.sy}, "
                 f"U+{first:04X}..U+{max(spec.codes):04X}, {len(glyphs)} glyphs, "
                 f"{len(blob)} bitmap bytes.")
        o.append(f"inline constexpr std::uint8_t k{stem}Bitmap[] = {{")
        if not blob:
            o.append("    0")
        for i in range(0, len(blob), 16):
            o.append("    " + ", ".join(f"0x{b:02x}" for b in blob[i : i + 16]) + ",")
        o.append("};")
        o.append(f"inline constexpr Glyph k{stem}Glyphs[] = {{")
        for i, g in enumerate(glyphs):
            o.append(
                f"    {{{g.offset}, {g.w}, {g.h}, {g.xo}, {g.yo}, {g.adv}}},"
                f" // U+{first + i:04X}"
            )
        o.append("};")
        o.append("")
        fonts_decl.append(
            f'    Font{{.name = "{spec.bdf[:-4]}{scaled(spec)}",'
            f" .line_height = {(font.ascent + font.descent) * spec.sy},"
            f" .ascent = {font.ascent * spec.sy},"
            f" .first = U'\\x{first:04x}',"
            f" .glyphs = k{stem}Glyphs, .bitmap = k{stem}Bitmap,"
            f" .fallback_index = {fb_index}}},"
        )
    o.append("/// Indexed by FontId.")
    o.append("inline constexpr Font kFonts[] = {")
    o.extend(fonts_decl)
    o.append("};")
    o.append("")
    o.append("} // namespace qz::gfx::generated")
    o.append("// NOLINTEND")
    return "\n".join(o) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed output is stale")
    ap.add_argument("--out", type=Path, default=OUT_PATH)
    args = ap.parse_args()
    text = emit().encode("ascii")
    if args.check:
        if not args.out.exists() or args.out.read_bytes() != text:
            print(f"fontgen: {args.out} is stale; run python3 tools/fontgen.py", file=sys.stderr)
            return 1
        print("fontgen: generated fonts up to date")
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(text)
    print(f"fontgen: wrote {args.out.relative_to(ROOT)} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
