# Status

Maintained by the lead. Read this first when resuming. Last updated: 2026-10-06 09:20 CDT.

## Blockers needing the owner
| # | Item | Detail |
|---|---|---|
| B1 | GitHub push | This machine has no `gh`, no GitHub credentials and no SSH keys. The repo is complete locally (`git log`); to publish: `gh auth login && gh repo create quartz-watchy --private --source=. --push` (or create an empty private repo in the web UI, then `git remote add origin <url> && git push -u origin main`). CI cannot have run on GitHub yet; `tools/check.sh --fw` is the local equivalent. |
| B2 | Physical-watch steps | Everything hardware-facing is UNVERIFIED until `docs/HARDWARE_BRINGUP.md` has been run. |
| B3 | Owner decisions | `docs/OPEN_QUESTIONS.md` (Q-01..Q-13; defaults implemented). Most important: Q-12 (panel partial-refresh waveform, P-11) and Q-13 (BMA423 blob provenance, P-12). |

## Milestones (docs/ROADMAP.md section 2)
| Milestone | State |
|---|---|
| Skeleton (build system, tooling, CI, stubs) | done |
| Planning (architecture, roadmap, research, headers) | done (docs/*.md, 37 headers compile standalone under strict C++23) |
| M1 host face (simulator renders a face PNG) | not started |
| M2 firmware builds (radio + offline variants) | skeleton builds; no real code yet |
| M3 face on the watch | needs owner bring-up |
| M4 a week in 10 s (virtual-time suite) | not started |
| M5 full feature | not started |

## Work packages
| WP | State |
|---|---|
| WP-01 core + base fakes, WP-02 civil time + POSIX-TZ, WP-03 TZ table, WP-04 TimeKeeper | committed |
| WP-05 gfx + PNG, WP-06 fonts (Spleen), WP-16 faces (default + minimal) | committed |
| WP-07 SSD1681 driver + panel model, WP-08 BMA423 wrapper + fake | committed |
| WP-09 settings + KV fake, WP-10 steps, WP-11 power, WP-12 weather, WP-13 connectivity logic | committed |
| WP-17 console protocol + dispatcher, WP-28 firmware config | committed |
| WP-23 face-only simulator (milestone M1) | committed (`--press/--scene/--console` need the app) |
| Not started | WP-14 UI framework, 15 system screens, 18 console catalog, 19 self-test + goldens, 20 app core, 21 app wiring, 22 virtual-time suite, 24-27 platform/radio (IDF-only), 29 qzctl + device tests, 30 skills/docs/release |

Full gate at 09:20 CDT: `tools/check.sh --fw` = 1067 host tests (ASan+UBSan), tidy, both firmware variants, offline-symbol check: all green.
Milestone M1 (host face) is done: `build/host/sim/qz_sim --face default --time ... --out face.png` (see host/sim/main.cpp).

## Resume playbook (do this after every usage-window reset)
1. `get_usage`; proceed only if the 5-hour window is < 50% used.
2. (No agents are in flight.)
3. For every finished WP the lead reviews and commits alone: `QZ_BUILD_DIR=build/lead tools/host.sh configure`,
   build + run only that component's `qz_<c>_test`, `clang-format --dry-run --Werror` and `tools/tidy.sh <files>` on its files,
   `python3 tools/check_deps.py`, one `tools/fw.sh build` if firmware-linked, read the non-test source, then
   `git add components/qz_<c>` and commit (message: what, tests, "Reviewed: ..."). Other agents' half-written files make
   the repo-wide gate unreliable until they finish.
4. Next WPs, in order: WP-14 (UI framework) -> WP-15 (system screens) and WP-18 (console catalog, needs 17+09)
   -> WP-20 (app core: RTC state, tether, wake planner) -> WP-21 (app wiring + DeviceApi) -> WP-22 (virtual-time suite)
   -> WP-19 (selftest/goldens) -> WP-23 rest (--press/--scene/--console); platform in parallel: WP-24, 25 (opus review),
   26, 27, then WP-29/30. Reviews: read the non-test source of critical modules (wake planner, sleep/GPIO, app wiring).
5. Pacing (measured): 4 parallel Sonnet agents burn ~2.7 % of the 5-hour window per minute (the first burst used 96 % in
   35 min and 13 % of the week). Run at most 3 agents, stop launching at ~85 %, and when the window is nearly spent let the
   running agents finish, then END the turn after scheduling the next wake-up with `CronCreate` (one-shot, local time,
   reset time + 6 min). Never idle in repeated waiting turns (overage cache TTL = 5 min -> ~$1 per turn).
6. Opus is only reachable via `Agent(model="opus")`; reserve it for reviews of the TZ engine, SSD1681 driver and wake planner.

## Budget and pacing (read before spawning agents)
- Plan: Pro (5-hour window, resets 03:40 CDT then every 5 h) + extra usage capped at $40/month. **$36.46 of the $40 is
  spent (91 %)**: ~$20 by agents with 150+ tool calls over big contexts while the window was exhausted, and ~$13.5 by my own
  idle waiting turns (overage drops the prompt-cache TTL to 5 min, so every 10-minute wait re-billed a ~450k-token context).
  Treat the remaining $3.54 as untouchable.
- Weekly usage was 41 % at 23:20 CDT on 2026-10-05 (resets 07:00 CDT on 2026-10-07).
- Agents must use <= 60 tool calls and read only their sections (AGENTS.md "Working efficiently"); WP-11 (42 calls) shows it works.

## Tech debt / follow-ups
- [TECH-DEBT] `FixedString(const char*)` documents truncation as a programmer error but clears silently (WP-01).
- [TECH-DEBT] `QZ_LOGW` ignores `QZ_LOG_MAX_LEVEL`; there is no `QZ_LOGV` (WP-01).
- [TECH-DEBT] Header contract vs panel vendor guidance (P-05) and the partial-refresh waveform (P-11) are open until bring-up experiment E1.
- [TECH-DEBT] BMA423 blob authenticity (P-12/Q-13).
- [TECH-DEBT] TZ table (WP-03): 139 entries = ~9 KB (the 6 KiB goal is unreachable with `TzEntry`; ceiling is now 10 KiB). POSIX footers only apply after a zone's last tabulated transition, so Casablanca (until 2026-09-20) and Edmonton/Vancouver/Winnipeg (until 2026-11-01) are WRONG until then, and Gaza's Ramadan shifts are ignored; Chicago/Berlin/Sydney-style zones are unaffected. Consider flagging these in the zone picker.
- [TECH-DEBT] SSD1681: outside 0-50 C the controller refuses an update but BUSY still falls; the app must gate on `temperature_dc()` (WP-07).
- [TECH-DEBT] SSD1681: 0x1B/0x2D read framing (no dummy byte) is [ASSUMED]; platform must set 10 MHz SPI and the reset timing (WP-25) (WP-07).
- [TECH-DEBT] BMA423: step-counter byte order and low-power counter read are [ASSUMED] until bring-up B7; I2C HAL must accept 64-byte writes (WP-25) (WP-08).
- [TECH-DEBT] WP-26: the console dispatcher needs a line buffer of >= 257 bytes (WP-17).
- [TECH-DEBT] qz_power: curve/thresholds unmeasured [TUNE B6]; EWMA not re-seeded on USB plug/unplug (WP-11).

## Environment notes
- ESP-IDF v6.1 in `.toolchain/` (bootstrap: `tools/bootstrap.sh`); dev venv in `.venv/` (`tools/setup-dev.sh`).
- The machine hard-rebooted once (nvidia-drm pageflip timeout, unrelated to builds); a btrfs crash artifact truncated one FetchContent checkout - `rm -rf build/` fixes any such corruption.
- Parallel agents need private build dirs: `QZ_BUILD_DIR=build/<name>`, `QZ_FW_BUILD_DIR=build/<name>-fw`.
