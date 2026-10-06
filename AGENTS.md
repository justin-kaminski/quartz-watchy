# AGENTS.md - Quartz (Watchy v3 firmware)

Production-grade ESP-IDF (v6.1) C++ firmware for the SQFMI Watchy v3 (ESP32-S3, 200x200 e-paper).
Stable, power-efficient, fully tested, wearable every day. No hacks, no shortcuts.

## Where things are
| What | Where |
|---|---|
| Requirements (the owner's brief) | `docs/SPEC.md` |
| Architecture + coding conventions (read section 2 first) | `docs/ARCHITECTURE.md` |
| Component contract (names, layers, allowed dependencies) | `docs/COMPONENTS.md` (enforced by `tools/check_deps.py`) |
| Work packages, waves, acceptance criteria | `docs/ROADMAP.md` |
| Decisions, open questions, pushback | `docs/DECISIONS.md`, `docs/OPEN_QUESTIONS.md`, `docs/PUSHBACK.md` |
| Hardware / datasheet facts (sourced) | `docs/research/*.md` |
| Owner steps that need the physical watch | `docs/HARDWARE_BRINGUP.md` |
| Current status and known tech debt | `docs/STATUS.md` |

## Ground rules
- Contracts are the public headers in `components/*/include/qz/**`. Changing a public signature
  needs a note in your report (and a `docs/COMPONENTS.md` "Changes" entry if a dependency changes).
- Pure components (everything except `qz_platform`, `qz_net`, `main`) never include ESP-IDF or
  FreeRTOS headers and never read `CONFIG_*`. They build and test on the Linux host.
- Write drivers from datasheets (`docs/research/`); never copy code from other driver libraries.
- Fact tags in docs and code comments: `[IDF:path]` verified in `.toolchain/esp-idf`, `[R1]` from
  `docs/research`, `[ASSUMED]` unconfirmed, `[TUNE]` calibrate on hardware. Nothing hardware-facing
  counts as tested until the owner runs `docs/HARDWARE_BRINGUP.md`. Never claim otherwise.
- Every public function has a test; warnings are errors; no TODO without a `[TECH-DEBT]` entry in
  `docs/STATUS.md`. Do not weaken a test to make it pass; fix the code or flag the contradiction.
- Credentials never reach logs, console output or panic messages.
- Keep commands quiet: pipe long output through `tail`/`grep`; never `cat` raw progress-bar logs.
- Git: only the lead commits (after a review). Agents do not run `git add/commit/stash/checkout/reset`.

## Commands
```bash
tools/bootstrap.sh              # once: pinned ESP-IDF into .toolchain/ (git-ignored)
tools/setup-dev.sh              # once: .venv with pinned clang-format / clang-tidy / pytest / pyserial
tools/check.sh [--fast] [--fw]  # THE gate: contract, format, host tests (ASan+UBSan), clang-tidy [, firmware]
tools/host.sh [configure|build|test|all] [default|release|coverage]
tools/format.sh [--check]       # apply / verify clang-format
tools/tidy.sh [files...]        # clang-tidy on the host compile database
tools/fw.sh build               # firmware (also: -p PORT flash monitor | size | menuconfig | clean)
tools/idf.sh <cmd>              # run any command inside the pinned ESP-IDF environment
python3 tools/check_deps.py     # component dependency contract
```
Parallel agents must use private build directories: `QZ_BUILD_DIR=build/<name> tools/host.sh all`,
`QZ_FW_BUILD_DIR=build/<name>-fw tools/fw.sh build`. Run only the tests of your component while
iterating: `QZ_BUILD_DIR=build/<name> tools/host.sh build && build/<name>/qz_<c>_test`.

## Conventions (summary of ARCHITECTURE section 2)
C++23, no exceptions/RTTI, `qz::Status`/`qz::Result<T>`, no heap on the wake path, integer-only
rendering, `PascalCase` types, `snake_case` functions, `snake_case_` members, `kPascalCase`
constants, unit suffixes (`_ms`, `_us`, `_mv`), `int64_t` for all time arithmetic, tunables in one
`constexpr` table per component.
