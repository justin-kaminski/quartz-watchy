---
name: add-console-command
description: Add or change a USB console command (qz_console catalog, DeviceApi, tests, protocol docs). Use when a new `@QZ1` command or DeviceApi capability is needed.
---

# Add a console command

Protocol and the command table: `docs/ARCHITECTURE.md` section 16. Commands live in
`components/qz_console/src/commands_*.cpp` as `Command` rows (`name`, `usage`, `help`, `min_args`,
`max_args`, flags, `handler`); a name is one word (`vibrate`) or two (`log wakes`).

## Pick the file
`commands_time.cpp` time/tz/settings; `commands_device.cpp` btn, steps, battery, weather, screen,
face, display; `commands_radio.cpp` wifi, sync, provision (flag `kFlagNeedsRadio`, use `require_radio`);
`commands_platform.cpp` log, diag, selftest, vibrate, sleep, reboot, factory-reset. Each file ends
with one `constexpr auto kCommands = std::to_array<Command>({...})`; `commands.cpp` registers them.

## Steps
1. Handler signature: `Status my_cmd(DeviceApi& api, Args args, JsonWriter& out)`. Validate with
   `parse_integer(args[0], min, max)` (-> `Errc::kBadArgs`), call only `DeviceApi`, write result with
   `out.field(...)`, `return ok();` or return an `Errc` (the dispatcher builds `ERR <code> {"msg":...}`).
   Ranges/limits go in `CatalogTuning` (`commands_internal.hpp`). Never echo secrets: flag
   `kFlagSensitive`; destructive actions need `kFlagDestructive` and a literal `confirm` argument.
2. Add the row to that file's `kCommands`.
3. New device capability: add a pure virtual (or defaulted) method to `DeviceApi`
   (`components/qz_console/include/qz/console/device_api.hpp`) and implement it in **all three**:
   `components/qz_app/src/device_api.cpp` (real device + simulator), and the test doubles
   `components/qz_console/test/fake_device_api.hpp` and `null_device_api.hpp`. A changed public
   signature needs a report note (AGENTS.md ground rules).
4. Tests, `components/qz_console/test/commands_test.cpp`: add one `OkCase` (exact JSON) **and** one
   `ErrCase` (error code) with `.command` set to the registry name. The registry-introspection
   tests `Catalog.EveryRegisteredCommandIsTested`, `NoCaseNamesAnUnregisteredCommand` and
   `RegistersTheWholeV1Catalog` (add the name to `kExpected`) fail until you do. Radio commands
   also need an `unsupported` case in `CatalogRadio`.
5. Docs: add the row to the command table in `docs/ARCHITECTURE.md` section 16 (args, result fields,
   flags) and, if it is user-visible in bring-up, to `docs/FIRST_FLASH.md` section 5.
6. Device-side test coverage lands with `test_apps/console` (WP-29, pytest: every command).
7. Run:
   ```bash
   tools/format.sh >/dev/null; QZ_BUILD_DIR=build/cmd tools/host.sh configure >/dev/null \
     && cmake --build build/cmd --target qz_console_test 2>&1 | tail -30 && build/cmd/qz_console_test 2>&1 | tail -15
   tools/tidy.sh components/qz_console/src/commands_<file>.cpp
   ```
   then `tools/check.sh --fast`; `tools/fw.sh build` if `qz_app` changed.

Error codes are the closed `qz::Errc` token set (`qz/core/result.hpp`); do not invent new ones.
Responses are one line, <= 16 KiB (`log wakes` clamps for that reason).
