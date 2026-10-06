# Status

Maintained by the lead. Read this first when resuming. Last updated: 2026-10-05 20:30 CDT.

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
| WP | State | Notes |
|---|---|---|
| WP-01 core runtime + base fakes | done | 200 tests; lead-reviewed |
| WP-02..WP-30 | not started | see docs/ROADMAP.md; run them as Sonnet implementations, Opus review only for TZ/time, SSD1681, wake planner/app wiring, platform sleep |

## Budget and pacing (read before spawning agents)
- Plan: Pro (5-hour window) + extra usage capped at $40/month. The window hit 100% at ~19:45 CDT and
  extra usage had reached $20.26 by 20:20 CDT: agents with 150+ tool calls over large contexts are
  what burn it. **Tell agents to minimise tool calls (target <= 60), batch writes, run the gate in
  one command, and never re-read files.**
- Windows reset at 22:40 CDT, then every 5 hours. Run work in bursts right after a reset (<= 3-4
  agents in parallel), idle while the window is spent, keep the remaining extra-usage credit as a
  reserve for finishing an in-flight WP.
- A session cannot switch its own model/effort; Opus is only reachable through `Agent(model="opus")`.

## Tech debt / follow-ups
- [TECH-DEBT] `FixedString(const char*)` documents truncation as a programmer error but clears silently (WP-01).
- [TECH-DEBT] `QZ_LOGW` ignores `QZ_LOG_MAX_LEVEL`; there is no `QZ_LOGV` (WP-01).
- [TECH-DEBT] Header contract vs panel vendor guidance (P-05) and the partial-refresh waveform (P-11) are open until bring-up experiment E1.
- [TECH-DEBT] BMA423 blob authenticity (P-12/Q-13).

## Environment notes
- ESP-IDF v6.1 in `.toolchain/` (bootstrap: `tools/bootstrap.sh`); dev venv in `.venv/` (`tools/setup-dev.sh`).
- The machine hard-rebooted once (nvidia-drm pageflip timeout, unrelated to builds); a btrfs crash artifact truncated one FetchContent checkout - `rm -rf build/` fixes any such corruption.
- Parallel agents need private build dirs: `QZ_BUILD_DIR=build/<name>`, `QZ_FW_BUILD_DIR=build/<name>-fw`.
