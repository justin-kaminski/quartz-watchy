#!/usr/bin/env python3
"""Enforces the component contract in docs/COMPONENTS.md (stdlib only).

Checks:
  1. host/components.cmake lists exactly the pure components.
  2. every directory under components/ is in the table, and vice versa.
  3. CMake REQUIRES/PRIV_REQUIRES (qz_* names) stay within the declared dependencies.
  4. `#include "qz/<name>/..."` in include/ and src/ of a component stays within its own name and
     declared dependencies (tests may include anything the component can reach).
  5. pure components never include ESP-IDF / FreeRTOS / libc-vendor headers.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE_ROW = re.compile(r"^\|\s*`(qz_[a-z0-9_]+|main)`\s*\|(.*)\|\s*$")
FORBIDDEN_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](esp_[a-z0-9_]*\.h|esp32[a-z0-9]*/|freertos/|driver/|nvs[a-z_]*\.h|'
    r"sdkconfig\.h|soc/|rom/|lwip/|mbedtls/|xtensa/|hal/[a-z_]+\.h|esp_[a-z_]+/)"
)
QZ_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]qz/([a-z0-9_]+)/')


def parse_table() -> dict[str, dict]:
    comps: dict[str, dict] = {}
    for line in (ROOT / "docs" / "COMPONENTS.md").read_text().splitlines():
        m = TABLE_ROW.match(line)
        if not m:
            continue
        cells = [c.strip() for c in m.group(2).split("|")]
        if len(cells) < 5:
            continue
        kind, deps_cell = cells[0], cells[3]
        comps[m.group(1)] = {"kind": kind, "deps_cell": deps_cell}
    for name, info in comps.items():
        info["pure"] = info["kind"].lower().startswith("pure")
    pure = [n for n, i in comps.items() if i["pure"]]
    for name, info in comps.items():
        cell = info["deps_cell"]
        if cell.startswith("all pure"):
            info["deps"] = {p for p in pure if p not in (name, "qz_testkit")}
        elif cell in ("—", "-", ""):
            info["deps"] = set()
        else:
            info["deps"] = {"qz_" + d.strip() for d in cell.split(",") if d.strip()}
    return comps


def cmake_requires(path: Path) -> set[str]:
    text = path.read_text()
    found: set[str] = set()
    for m in re.finditer(r"\b(PRIV_REQUIRES|REQUIRES)\b([^)]*?)(?=\b(?:SRC_DIRS|INCLUDE_DIRS|PRIV_INCLUDE_DIRS|"
                         r"PRIV_REQUIRES|REQUIRES|PLATFORM)\b|\))", text, re.S):
        found |= {t for t in m.group(2).split() if t.startswith("qz_")}
    return found


def host_components() -> set[str]:
    text = (ROOT / "host" / "components.cmake").read_text()
    body = re.search(r"set\(QZ_HOST_COMPONENTS(.*?)\)", text, re.S)
    return set(body.group(1).split()) if body else set()


def main() -> int:
    comps = parse_table()
    errors: list[str] = []
    pure = {n for n, i in comps.items() if i["pure"]}

    if host_components() != pure:
        errors.append(
            f"host/components.cmake mismatch: missing={sorted(pure - host_components())} "
            f"extra={sorted(host_components() - pure)}"
        )

    on_disk = {p.name for p in (ROOT / "components").iterdir() if p.is_dir()}
    table_comps = {n for n in comps if n.startswith("qz_")}
    if on_disk != table_comps:
        errors.append(f"components/ vs table mismatch: only-on-disk={sorted(on_disk - table_comps)} "
                      f"only-in-table={sorted(table_comps - on_disk)}")

    for name in sorted(on_disk & table_comps):
        info, cdir = comps[name], ROOT / "components" / name
        cml = cdir / "CMakeLists.txt"
        if cml.exists():
            extra = cmake_requires(cml) - info["deps"]
            if extra:
                errors.append(f"{name}: CMake REQUIRES not allowed by COMPONENTS.md: {sorted(extra)}")
        allowed = info["deps"] | {name}
        for sub in ("include", "src"):
            for src in sorted((cdir / sub).rglob("*")) if (cdir / sub).exists() else []:
                if src.suffix not in (".hpp", ".h", ".cpp", ".cc", ".inc") or "third_party" in src.parts:
                    continue
                for n, line in enumerate(src.read_text(errors="replace").splitlines(), 1):
                    m = QZ_INCLUDE.match(line)
                    if m and "qz_" + m.group(1) not in allowed:
                        errors.append(f"{src.relative_to(ROOT)}:{n}: includes qz/{m.group(1)}/ "
                                      f"but {name} may only use {sorted(allowed)}")
                    if info["pure"] and FORBIDDEN_INCLUDE.match(line):
                        errors.append(f"{src.relative_to(ROOT)}:{n}: pure component includes a "
                                      f"platform header: {line.strip()}")

    if errors:
        print("component contract violations:", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    print(f">> component contract OK ({len(table_comps)} components, {len(pure)} pure)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
