#!/usr/bin/env python3
"""Deterministic built-in time-zone table generator for qz_time (WP-03). Python stdlib only.

    python3 tools/tzgen.py            # regenerate components/qz_time/src/tz_table.inc
    python3 tools/tzgen.py --check    # exit 1 if the committed file differs from a fresh generation

Input : the PyPI `tzdata` wheel, pinned by version and SHA-256 below (IANA release 2026e). It is
        downloaded once into .toolchain/tzdata/ (git-ignored) and verified on every run; after that
        the generator works offline. The wheel carries zic-compiled TZif files plus zone.tab and
        zone1970.tab, so no zic and no system zoneinfo (/usr/share/zoneinfo) is ever read.
Method: for every curated zone the TZif v2+ footer (POSIX TZ string, RFC 9636 section 3.3) is
        taken as the rule. The table is a statement about the *future* (the engine in qz/time/tz.hpp
        evaluates footers only), so pre-footer history is deliberately not represented.
Curation (ARCHITECTURE.md section 9): UTC, every Etc/GMT+-N, and a hand-picked list of recognisable
        cities. Coverage is enforced: every distinct footer of zone1970.tab must be served by some
        entry or be listed in UNCOVERED with a reason (the generator fails otherwise), so a new
        tzdata release that adds a rule cannot silently drop a region.
Output: one include file with the tzdata version and a std::array<TzEntry, N> sorted by
        (standard offset, label). Header records the tzdata version, the input hash, the hash of this
        generator and the estimated size. No timestamps or host paths: two runs are byte-identical.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE_DIR = ROOT / ".toolchain" / "tzdata"
OUT_PATH = ROOT / "components" / "qz_time" / "src" / "tz_table.inc"

PIN_VERSION = "2026e"  # IANA release inside the wheel (tzdata.zi first line is checked)
WHEEL_NAME = "tzdata-2026.5-py2.py3-none-any.whl"
WHEEL_URL = "https://files.pythonhosted.org/packages/py2.py3/t/tzdata/" + WHEEL_NAME
WHEEL_SHA256 = "b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac"
ZONEINFO = "tzdata/zoneinfo/"

# Flash estimate for the 32-bit target: sizeof(TzEntry) is 3 string_views (8 B each) + int32.
TZ_ENTRY_BYTES_32 = 28
# Budget enforced here and in tz_db_test.cpp. ARCHITECTURE section 9 said <= 6 KiB; that cannot hold
# ~120-160 entries of this record layout (see docs/STATUS.md TECH-DEBT), so the enforced ceiling is
# the measured design point with headroom.
MAX_TABLE_BYTES = 10 * 1024

# Curated recognisable zones (IANA names). Labels are derived: last path component, '_' -> ' '.
CURATED = """
Africa/Abidjan Africa/Accra Africa/Algiers Africa/Cairo Africa/Casablanca Africa/Johannesburg
Africa/Lagos Africa/Maputo Africa/Nairobi Africa/Tripoli
America/Anchorage America/Argentina/Buenos_Aires America/Bogota America/Caracas America/Chicago
America/Denver America/Edmonton America/Guatemala America/Halifax America/Havana America/La_Paz
America/Lima America/Los_Angeles America/Manaus America/Mexico_City America/Montevideo
America/New_York America/Nuuk America/Panama America/Phoenix America/Puerto_Rico America/Regina
America/Santiago America/Sao_Paulo America/St_Johns America/Toronto America/Vancouver
America/Winnipeg
Atlantic/Azores Atlantic/Reykjavik
Asia/Almaty Asia/Baghdad Asia/Bangkok Asia/Beirut Asia/Colombo Asia/Dhaka Asia/Dubai Asia/Gaza
Asia/Ho_Chi_Minh Asia/Hong_Kong Asia/Irkutsk Asia/Jakarta Asia/Jayapura Asia/Jerusalem
Asia/Kabul Asia/Kamchatka Asia/Karachi Asia/Kathmandu Asia/Kolkata Asia/Kuala_Lumpur
Asia/Makassar Asia/Manila Asia/Novosibirsk Asia/Riyadh Asia/Seoul Asia/Shanghai Asia/Singapore
Asia/Taipei Asia/Tashkent Asia/Tehran Asia/Tokyo Asia/Ulaanbaatar Asia/Vladivostok Asia/Yakutsk
Asia/Yangon Asia/Yekaterinburg
Australia/Adelaide Australia/Brisbane Australia/Darwin Australia/Melbourne Australia/Perth
Australia/Sydney
Europe/Amsterdam Europe/Athens Europe/Belgrade Europe/Berlin Europe/Brussels Europe/Bucharest
Europe/Budapest Europe/Copenhagen Europe/Dublin Europe/Helsinki Europe/Istanbul Europe/Kyiv
Europe/Lisbon Europe/London Europe/Madrid Europe/Moscow Europe/Oslo Europe/Paris Europe/Prague
Europe/Rome Europe/Stockholm Europe/Vienna Europe/Warsaw Europe/Zurich
Pacific/Auckland Pacific/Fiji Pacific/Guam Pacific/Honolulu Pacific/Noumea Pacific/Pago_Pago
""".split()

LABEL_OVERRIDES = {"America/St_Johns": "St. John's"}

# zone1970.tab footers deliberately without an entry. Whole-hour fixed offsets are served by Etc/GMT+-N
# (same footer), so only DST-bearing or half-hour footers of small populations remain here.
UNCOVERED = {
    "<+00>0<+02>-2,M3.5.0/1,M10.5.0/3": "Antarctica/Troll (research station)",
    "<+0845>-8:45": "Australia/Eucla (about 100 people)",
    "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0": "Australia/Lord_Howe (about 400 people)",
    "<+11>-11<+12>,M10.1.0,M4.1.0/3": "Pacific/Norfolk (about 2000 people)",
    "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45": "Pacific/Chatham (about 600 people)",
    "<-03>3<-02>,M3.2.0,M11.1.0": "America/Miquelon (about 6000 people)",
    "<-06>6<-05>,M9.1.6/22,M4.1.6/22": "Pacific/Easter (about 5000 people)",
    "<-0930>9:30": "Pacific/Marquesas (about 9000 people)",
    "HST10HDT,M3.2.0,M11.1.0": "America/Adak (about 300 people)",
}


class GenError(Exception):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_wheel() -> zipfile.ZipFile:
    path = CACHE_DIR / WHEEL_NAME
    if not path.exists():
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(WHEEL_URL, timeout=60) as resp:  # noqa: S310 (pinned https URL)
            data = resp.read()
        if sha256_bytes(data) != WHEEL_SHA256:
            raise GenError(f"downloaded {WHEEL_NAME} has the wrong SHA-256; refusing to cache it")
        path.write_bytes(data)
    if sha256_bytes(path.read_bytes()) != WHEEL_SHA256:
        raise GenError(f"{path} does not match the pinned SHA-256; delete it and rerun")
    return zipfile.ZipFile(path)


def tzif_footer(blob: bytes, name: str) -> str:
    """POSIX TZ footer of a TZif v2+ file (RFC 9636 section 3.1/3.3)."""
    return _tzif_parse(blob, name)[0]


def tzif_last_transition(blob: bytes, name: str) -> int | None:
    """UTC time of the last explicit transition (the footer governs only after it)."""
    return _tzif_parse(blob, name)[1]


def _tzif_parse(blob: bytes, name: str) -> tuple[str, int | None]:
    if blob[:4] != b"TZif":
        raise GenError(f"{name}: not a TZif file")
    if blob[4:5] < b"2":
        raise GenError(f"{name}: TZif version < 2 has no footer")

    def counts(off: int) -> tuple[int, ...]:
        return struct.unpack(">6l", blob[off + 20 : off + 44])  # isutc, isstd, leap, time, type, char

    isutc, isstd, leap, timecnt, typecnt, charcnt = counts(0)
    v1_len = 44 + timecnt * 4 + timecnt + typecnt * 6 + charcnt + leap * 8 + isstd + isutc
    if blob[v1_len : v1_len + 4] != b"TZif":
        raise GenError(f"{name}: v2 header not where expected")
    isutc, isstd, leap, timecnt, typecnt, charcnt = counts(v1_len)
    v2_len = 44 + timecnt * 8 + timecnt + typecnt * 6 + charcnt + leap * 12 + isstd + isutc
    body = v1_len + 44
    last = struct.unpack(">q", blob[body + (timecnt - 1) * 8 : body + timecnt * 8])[0] if timecnt else None
    tail = blob[v1_len + v2_len :]
    if not (tail.startswith(b"\n") and tail.endswith(b"\n")):
        raise GenError(f"{name}: malformed footer")
    footer = tail[1:-1].decode("ascii")
    if not footer:
        raise GenError(f"{name}: empty footer (zone has no recurring rule)")
    return footer, last


_NAME = r"(?:[A-Za-z]{3,}|<[A-Za-z0-9+-]{3,}>)"
_OFFSET = r"[+-]?\d{1,3}(?::\d{2}(?::\d{2})?)?"
_FOOTER_RE = re.compile(rf"^({_NAME})({_OFFSET})(?:({_NAME})({_OFFSET})?)?(?:,.*)?$")


def _offset_s(text: str) -> int:
    """POSIX offset text (positive = west) -> seconds EAST of UTC."""
    sign = -1 if text.startswith("-") else 1
    parts = [int(p) for p in text.lstrip("+-").split(":")] + [0, 0]
    return -sign * (parts[0] * 3600 + parts[1] * 60 + parts[2])


def footer_offsets(footer: str) -> tuple[int, int | None]:
    """(std offset, dst offset or None) in seconds east of UTC."""
    m = _FOOTER_RE.match(footer)
    if not m:
        raise GenError(f"cannot parse footer {footer!r}")
    std = _offset_s(m.group(2))
    if m.group(3) is None:
        return std, None
    dst = _offset_s(m.group(4)) if m.group(4) else std + 3600
    return std, dst


def fmt_utc(offset_s: int) -> str:
    sign = "-" if offset_s < 0 else "+"
    a = abs(offset_s)
    h, m = divmod(a // 60, 60)
    if a % 60:
        raise GenError(f"offset with seconds: {offset_s}")
    return f"UTC{sign}{h}" + (f":{m:02d}" if m else "")


def make_entry(zf: zipfile.ZipFile, name: str, label: str) -> dict:
    footer = tzif_footer(zf.read(ZONEINFO + name), name)
    std, dst = footer_offsets(footer)
    # "Standard" offset for sorting/labels: the lower of the two, so Europe/Dublin (IST-1GMT0, whose
    # tzdata "standard" time is the summer one) reads UTC+0 like every user expects.
    low = std if dst is None else min(std, dst)
    last = tzif_last_transition(zf.read(ZONEINFO + name), name)
    return {"name": name, "label": f"{label} ({fmt_utc(low)})" if "/" in name and
            not name.startswith("Etc/") else label, "posix": footer, "std": low, "last": last}


def etc_names() -> list[str]:
    return [f"Etc/GMT{'+' if n > 0 else '-'}{abs(n)}" for n in range(-14, 13) if n != 0]


def city_of(name: str) -> str:
    return LABEL_OVERRIDES.get(name, name.rsplit("/", 1)[1].replace("_", " "))


def build() -> tuple[list[dict], dict]:
    zf = load_wheel()
    zi = zf.read(ZONEINFO + "tzdata.zi").decode("ascii").splitlines()[0]
    if zi != f"# version {PIN_VERSION}":
        raise GenError(f"wheel carries {zi!r}, pinned {PIN_VERSION!r}")
    zone_tab = {
        line.split("\t")[2]
        for line in zf.read(ZONEINFO + "zone.tab").decode("utf-8").splitlines()
        if line and not line.startswith("#")
    }
    entries = [make_entry(zf, "UTC", "UTC")]
    entries[0]["label"] = "UTC"
    seen = {"UTC"}
    for name in CURATED:
        if name in seen:
            raise GenError(f"duplicate curated zone {name}")
        if name not in zone_tab:
            raise GenError(f"{name} is not in zone.tab of tzdata {PIN_VERSION}")
        seen.add(name)
        entries.append(make_entry(zf, name, city_of(name)))
    for name in etc_names():
        e = make_entry(zf, name, "")
        e["label"] = fmt_utc(e["std"])
        entries.append(e)

    # Coverage of zone1970.tab footers.
    served = {e["posix"] for e in entries}
    tab = zf.read(ZONEINFO + "zone1970.tab").decode("utf-8").splitlines()
    wanted = {tzif_footer(zf.read(ZONEINFO + line.split("\t")[2]), line.split("\t")[2])
              for line in tab if line and not line.startswith("#")}
    missing = sorted(wanted - served - set(UNCOVERED))
    if missing:
        raise GenError("zone1970.tab footers without an entry or an UNCOVERED reason:\n  " +
                       "\n  ".join(missing))
    stale = sorted(set(UNCOVERED) & served)
    if stale:
        raise GenError("UNCOVERED lists footers that are served: " + ", ".join(stale))
    unknown = sorted(set(UNCOVERED) - wanted)
    if unknown:
        raise GenError("UNCOVERED lists footers absent from zone1970.tab: " + ", ".join(unknown))

    entries.sort(key=lambda e: (e["std"], e["label"], e["name"]))
    if len({e["name"] for e in entries}) != len(entries):
        raise GenError("duplicate names")
    if len({e["label"] for e in entries}) != len(entries):
        raise GenError("duplicate labels")
    meta = {"wheel_sha256": WHEEL_SHA256}
    return entries, meta


def footer_caveats(entries: list[dict]) -> list[str]:
    """Entries whose TZif tabulates transitions in or after the release year: before the last one
    the footer (all the engine knows) differs from real time."""
    out = []
    for e in sorted(entries, key=lambda x: x["name"]):
        if e["last"] is not None and e["last"] >= (days_from_ymd(int(PIN_VERSION[:4]), 1, 1) * 86400):
            out.append(f"{e['name']} (footer from {iso_date(e['last'])})")
    return out


def days_from_ymd(y: int, m: int, d: int) -> int:
    import datetime
    return (datetime.date(y, m, d) - datetime.date(1970, 1, 1)).days


def iso_date(t: int) -> str:
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%d")


def estimate_bytes(entries: list[dict]) -> int:
    pool = {s for e in entries for s in (e["name"], e["label"], e["posix"])}
    return len(entries) * TZ_ENTRY_BYTES_32 + sum(len(s) + 1 for s in sorted(pool))


def cpp_str(text: str) -> str:
    if not re.fullmatch(r"[ -~]*", text) or '"' in text or "\\" in text:
        raise GenError(f"unsafe string {text!r}")
    return f'"{text}"'


def render() -> str:
    entries, meta = build()
    size = estimate_bytes(entries)
    if size > MAX_TABLE_BYTES:
        raise GenError(f"table estimate {size} B exceeds MAX_TABLE_BYTES {MAX_TABLE_BYTES}")
    gen_hash = sha256_bytes(Path(__file__).read_bytes())
    out = [
        "// GENERATED by tools/tzgen.py - DO NOT EDIT. Included by tz_db.cpp inside qz::time.",
        f"// tzdata release : {PIN_VERSION} (PyPI tzdata 2026.5 wheel)",
        f"// input sha256   : {meta['wheel_sha256']}",
        f"// generator sha256: {gen_hash}",
        f"// entries        : {len(entries)}; estimated flash on the 32-bit target {size} B "
        f"(budget {MAX_TABLE_BYTES} B)",
        "// Sorted by (standard offset, label); standard offset = the lower of the footer's offsets.",
        "// Footer-only caveat: tzdata tabulates transitions in/after the release year for these zones;",
        "// before the date shown the real rules differ from the footer: " +
        "; ".join(footer_caveats(entries)) + ".",
        f"inline constexpr std::string_view kTzdataVersion = {cpp_str(PIN_VERSION)};",
        f"inline constexpr std::array<TzEntry, {len(entries)}> kZoneTable{{{{",
    ]
    for e in entries:
        out.append(f"    {{{cpp_str(e['name'])}, {cpp_str(e['label'])}, {cpp_str(e['posix'])}, "
                   f"{e['std']}}},")
    out.append("}};")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed table is stale")
    args = ap.parse_args()
    try:
        text = render()
    except GenError as err:
        print(f"tzgen: {err}", file=sys.stderr)
        return 2
    if args.check:
        current = OUT_PATH.read_text() if OUT_PATH.exists() else ""
        if current != text:
            print(f"tzgen: {OUT_PATH.relative_to(ROOT)} is stale; run python3 tools/tzgen.py",
                  file=sys.stderr)
            return 1
        print(">> tz_table.inc up to date")
        return 0
    OUT_PATH.write_text(text)
    print(f">> wrote {OUT_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
