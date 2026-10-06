# Release checklist

Rules from `docs/SPEC.md` "Release": semantic versioning, git hash embedded, pinned ESP-IDF,
USB flashing with documented recovery, changelog per release. Only the lead tags and pushes.
Automation: `tools/release.sh` (skill `.claude/skills/release`). It never tags, pushes or publishes.

## Versioning
- `version.txt` is the only place the version is written. ESP-IDF reads it as `PROJECT_VER`
  [IDF:tools/cmake/project.cmake]; it appears in the console `version` / `ready` event and the
  About screen. The short git hash (`-dirty` when tracked files differ) is added by CMake when it
  configures (`CMakeLists.txt`); `tools/release.sh` reconfigures first so it is exact.
- **0.x until the hardware release gate passes.** 0.MINOR for features, 0.x.PATCH for fixes. 1.0.0 is
  declared by the owner, never by an agent. After 1.0: MAJOR = incompatible change to persisted
  data (RTC state, NVS schema, settings ids such as `face`) or the console protocol (`proto`),
  MINOR = new features/commands/faces, PATCH = fixes.
- Tags are `vMAJOR.MINOR.PATCH` on `main`; the changelog entry's version must equal `version.txt`.
- Reproducible build inputs: `.idf-version` (ESP-IDF tag; CI container `espressif/idf:<tag>` must
  match), `dependencies.lock` (managed components, committed), `sdkconfig.defaults*`.

## Hardware release gate (must pass for 1.0.0; recommended before any 0.x tag users flash)
1. `docs/HARDWARE_BRINGUP.md` B1-B12 completed by the owner; report in `docs/bringup/YYYY-MM-DD.md`.
2. B9 power run repeated for the candidate; every row of `docs/POWER_BUDGET.md` section 3 within
   10 % of the recorded baseline (P-04), no `NOT MEASURED` cell left.
3. `[ASSUMED]`/`[TUNE]` items hit by bring-up resolved in `docs/ARCHITECTURE.md`; open items listed
   in `docs/STATUS.md` tech debt are accepted explicitly in the changelog.
Until then every 0.x changelog entry must say what is verified on hardware and what is not.

## Checklist
1. [ ] Branch is `main`, tree clean, everything reviewed and committed by the lead.
2. [ ] `version.txt` bumped; `CHANGELOG.md`: `[Unreleased]` moved to `## [X.Y.Z] - YYYY-MM-DD`
       (Added / Changed / Fixed / Removed / Known limitations).
3. [ ] `tools/release.sh --dry-run` passes.
4. [ ] `tools/release.sh` passes: it runs `tools/check.sh --fw` (contract, generated fonts/tz table,
       format, host tests under ASan+UBSan, clang-tidy, radio + offline firmware, no radio symbols in
       the offline image, radio stack present in the radio image), enforces image budgets (radio
       <= 1.6 MiB, offline <= 600 KiB, static DRAM <= 96 KiB: check `size-radio.txt`) and fills
       `dist/quartz-X.Y.Z/`.
5. [ ] CI is green on GitHub for the release commit (CI cannot have run while the repo is unpublished;
       `tools/check.sh --fw` is the local equivalent).
6. [ ] Artifact smoke test by the owner: flash `dist/quartz-X.Y.Z/offline` (then `radio`) per
       `docs/FIRST_FLASH.md`; the About screen shows `X.Y.Z` and the expected hash.
7. [ ] Tag and push (lead, owner-approved): `git tag -a vX.Y.Z -m "Quartz X.Y.Z"`, `git push origin main vX.Y.Z`.
8. [ ] Attach `dist/quartz-X.Y.Z/` (`SHA256SUMS` included) to the GitHub release; paste the changelog
       entry and the recovery notes below.

## `dist/quartz-X.Y.Z/` layout
`radio/` and `offline/`, each: `quartz.bin`, `bootloader/bootloader.bin`,
`partition_table/partition-table.bin`, `ota_data_initial.bin`, `flash_args`, `sdkconfig`. Top level:
`SHA256SUMS`, `MANIFEST.txt` (version, git, ESP-IDF, build time), `size-radio.txt`, `size-offline.txt`,
`version.txt`, `.idf-version`, `dependencies.lock`, `CHANGELOG.md`. Flash from a variant directory:
`tools/idf.sh python -m esptool --chip esp32s3 -p $P write-flash @flash_args` (offsets: bootloader
0x0, partition table 0x10000, otadata 0x21000, app 0x40000).

## Recovery notes (include in every release)
- Download mode: plug in USB, hold BACK + UP for more than 4 s, release BACK first while still holding UP,
  then release UP. Plain reset: the same chord, releasing UP first. Details: `docs/FIRST_FLASH.md` s2.
- A bad image or a crash loop: download mode, `erase-flash`, flash again (NVS loss = factory reset).
- Back to the stock firmware: only possible if the owner took the 8 MB backup first (FIRST_FLASH s2).
- Two 3 MiB app slots (`docs/PARTITIONS.md`) leave room for OTA later; v1 releases are USB-flashed.
- The board has no battery protection: never leave the cell below 3.0 V, disconnect it for storage.
