# Provenance of the vendored Bosch BMA423 SensorAPI

Vendored **unmodified**. Do not edit these files; update this record if they are ever replaced.

| Item | Value |
|---|---|
| Source repo | https://github.com/sqfmi/BMA423-Sensor-API (SQFMI fork; preserves Bosch Sensortec's commit history) |
| Tag | `bma423_v2.14.13` |
| Commit | `df5c8ee95f7544451090f70eec4911fb6c9a7c72` (2020-05-25, "Refactored code and fixed bugs") |
| Fetched from | https://raw.githubusercontent.com/sqfmi/BMA423-Sensor-API/df5c8ee95f7544451090f70eec4911fb6c9a7c72/<file> (and again via the tag URL; both byte-identical) |
| Licence | BSD-3-Clause, "Copyright (c) 2020 Bosch Sensortec GmbH" (`LICENSE`, retain it with any redistribution) |
| Fetched | 2026-10-05 |

## Upstream is gone; authenticity is unconfirmed

`github.com/boschsensortec/BMA423_SensorAPI` returns 404 and Bosch no longer lists the BMA423 (docs/PUSHBACK.md P-12).
This fork is the only surviving copy found. The SHA-256 values below were recorded by research
(docs/research/bma423.md section 12) and re-verified at vendoring time; they prove the files did not change since
then, **not** that they equal Bosch's original release. The feature-config blob is therefore unconfirmed
(docs/OPEN_QUESTIONS.md Q-13): ask Bosch Sensortec support for the official driver and compare. Bring-up B7
validates it functionally (INTERNAL_STATUS init_ok, step counter counts).

## Files

| File | Bytes | SHA-256 |
|---|---|---|
| LICENSE | 1519 | c6c5f610dfce4c6a325b9d245a0aad2c8c9edc4a99e8cfa953a81883224ded51 |
| bma4.c | 173801 | 9070106780bc4343f00030e807d904b3e19f895a68d23a96252fd49a24822965 |
| bma4.h | 76885 | fb69ea52a0666a10778ed5aac25e6a1d173439b3bd0071d18c0872c668e5bd49 |
| bma4_defs.h | 41810 | 80b01827a6f56780d899f3af110426641262d7030d1d2ec78dc49eeec8f34a0a |
| bma423.c | 81636 | 3103cfa12beb2b31a57981362084c4d27c019c012fe3caf6c744e0537ce75611 |
| bma423.h | 38598 | 160c775c42ff27e624e8bd38b7f2f9e5d6d3656c3104607ec67585d12a6546a1 |

Verify: `cd components/qz_bma423/third_party/bosch && sha256sum LICENSE bma4.c bma4.h bma4_defs.h bma423.c bma423.h`

## Feature-config blob

`bma423_config_file[]` inside `bma423.c`: 6144 bytes, SHA-256 over the decoded bytes `112f81c8baba6d8abbf000c01e24fa56abd9f0c55e0415600d49109c189a2d3e`
(CRC-32 0xe08e8a46, asserted by `test/accel_test.cpp`). Only the BMA423 blob may be used on this chip; the BMA456 blob is a
different chip's firmware.

## Build notes

Compiled as C by qz_bma423 on host and firmware. Warnings are suppressed for these files only (`-w` via
`set_source_files_properties` in `../../CMakeLists.txt`); Quartz warning flags apply to the C++ sources. Known upstream
quirks worked around by the wrapper (src/accel.cpp): `bma423_write_config_file` requires 2 <= read_write_len <= 70 and
reads past the blob unless config_size is a multiple of it (wrapper uses 64); it ignores an error in all but the last chunk.
