---
name: release
description: Cut a Quartz release - version bump, changelog, checks, both firmware variants, dist/ artifacts and the tag command. Use when asked to prepare, verify or tag a release. Never tags, pushes or publishes on its own.
---

# Release

Full checklist: `docs/RELEASE.md`. Only the lead commits, tags and pushes; this skill prepares them.

## Rules
- Semantic versioning. **0.x until the hardware release gate passes** (docs/HARDWARE_BRINGUP.md
  B1-B12 pass, `docs/POWER_BUDGET.md` has no `NOT MEASURED` cell, drift < 10 %). 1.0.0 needs the owner.
- The version lives in `version.txt` only (ESP-IDF reads it into the app descriptor; console
  `version`, `ready` event and About screen show it). The git hash is added by CMake at configure time.
- ESP-IDF is pinned by `.idf-version` (CI container tag must match; CI job `pins` checks it).

## Steps
1. Edit `version.txt` (`MAJOR.MINOR.PATCH`) and move the `## [Unreleased]` notes in `CHANGELOG.md` to
   a `## [X.Y.Z] - YYYY-MM-DD` entry (Keep a Changelog sections; say what is verified on hardware and
   what is not). Commit on `main`.
2. Dry run (safe, builds nothing): `tools/release.sh --dry-run` - validates version, changelog entry,
   branch, clean tree, tag, and prints what it would do.
3. Real run: `tools/release.sh`. It runs `tools/check.sh --fw` (host tests, tidy, both firmware
   variants, offline/radio symbol checks), reconfigures so the embedded hash is exact, enforces the image
   size budgets, and writes `dist/quartz-X.Y.Z/` (git-ignored): `radio/` and `offline/` each with
   `quartz.bin`, `bootloader/bootloader.bin`, `partition_table/partition-table.bin`,
   `ota_data_initial.bin`, `flash_args`, `sdkconfig`; plus `SHA256SUMS`, `size-*.txt`, `MANIFEST.txt`.
4. Print the tag command: `git tag -a vX.Y.Z -m "Quartz X.Y.Z"`. The script does not run it; the lead
   tags the reviewed commit, then `git push origin main vX.Y.Z` (owner-approved).
5. Flash check by the owner from the artifacts: `cd dist/quartz-X.Y.Z/radio && python -m esptool --chip esp32s3 -p $P write-flash @flash_args`
   (use `tools/idf.sh python -m esptool ...`), see `docs/FIRST_FLASH.md`.

## Recovery notes for release text
Download mode: hold BACK + UP > 4 s, release BACK first, then UP. `erase-flash` + flash restores
everything (NVS loss = factory reset). The two 3 MiB OTA slots leave room for a future OTA.
