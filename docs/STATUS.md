# Status

Maintained by the lead. Read this first when resuming. Last updated: 2026-10-05 23:25 CDT.

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
| WP-01 core runtime + base fakes | committed | 200 tests; lead-reviewed |
| WP-11 power model + policy | committed | 71 tests; lead-reviewed; header defaults aligned (170 mAh, refresh every 30) |
| WP-28 firmware config (partitions, sdkconfig, Kconfig) | committed | both variants build; offline-symbol guard in CI |
| WP-02 civil time + POSIX-TZ | IN FLIGHT, uncommitted | agent a196a3f8f29b7b879 stopped at the last step (full 40-zone glibc oracle run, then gates + report) |
| WP-05 graphics core + PNG | IN FLIGHT, uncommitted | agent abdc55d4504945955: 72 tests pass; remaining lint gate, mutation sanity checks, report |
| WP-17 console protocol + dispatcher | IN FLIGHT, uncommitted | agent a25ca3eca507d425f: sources + header edits in; remaining tests, gates, report |
| WP-08 BMA423 wrapper + fake | IN FLIGHT, nothing written | agent a62afeed68deb45ed had only read AGENTS.md; vendoring from the pinned SQFMI mirror still to do |
| remaining WPs | not started | docs/ROADMAP.md |

The four stopped agents keep their transcripts: `SendMessage` to the agent id resumes it with context intact.

## Resume playbook (do this after every usage-window reset)
1. `get_usage`; proceed only if the 5-hour window is < 50% used.
2. Resume the four agents above with: "Usage window reset. Finish only the remaining steps with minimal tool calls
   (no extra mutation testing), run your gates, report <= 200 words."
3. For every finished WP the lead reviews and commits alone: `QZ_BUILD_DIR=build/lead tools/host.sh configure`,
   build + run only that component's `qz_<c>_test`, `clang-format --dry-run --Werror` and `tools/tidy.sh <files>` on its files,
   `python3 tools/check_deps.py`, one `tools/fw.sh build` if firmware-linked, read the non-test source, then
   `git add components/qz_<c>` and commit (message: what, tests, "Reviewed: ..."). Other agents' half-written files make
   the repo-wide gate unreliable until they finish.
4. Next WPs by dependency: after 05 -> WP-06 (fonts), WP-07 (SSD1681); after 02 -> WP-03 (tz table), WP-04 (TimeKeeper),
   WP-09 (settings) -> WP-10 (steps), WP-13 (conn), WP-14 (UI) -> 15/16 -> WP-23 (simulator = milestone M1);
   after 17 -> WP-18; then 12, 19, 20, 21, 22, platform 24-27, 29, 30.
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

## Environment notes
- ESP-IDF v6.1 in `.toolchain/` (bootstrap: `tools/bootstrap.sh`); dev venv in `.venv/` (`tools/setup-dev.sh`).
- The machine hard-rebooted once (nvidia-drm pageflip timeout, unrelated to builds); a btrfs crash artifact truncated one FetchContent checkout - `rm -rf build/` fixes any such corruption.
- Parallel agents need private build dirs: `QZ_BUILD_DIR=build/<name>`, `QZ_FW_BUILD_DIR=build/<name>-fw`.
