---
name: add-watch-face
description: Add a new watch face to components/qz_faces - source file, registry row, tests, golden scenes and images - and preview it with the host simulator. Use when asked to create or change a face.
---

# Add a watch face

A face is a pure function `void render_<name>_face(const ui::WatchState&, gfx::Canvas&) noexcept`
(integer-only, no heap, no ESP-IDF; see `docs/ARCHITECTURE.md` section 15.1). Ids are persisted in
the `face` setting: **never renumber or reuse one**. Read `components/qz_faces/src/minimal_face.cpp`
(short, complete) and `face_common.hpp` (helpers: `format_time`, `format_battery`, `sync_word`,
`power_tag`, `draw_weather_icon`, `draw_sync_icon`, `draw_tag`, `weather_visible`, `layout::k*`).

Every face must show: time (12/24 h), date, steps with goal, battery, weather when fresh/stale,
sync indicator, time-invalid state, saver/charging marks. 200x200, 1 bit, black/white.

## Steps
1. Pick the next free id (`components/qz_faces/src/registry.cpp`, `kTable`). Id <= 255.
2. Create `components/qz_faces/src/<name>_face.cpp` (the component globs `src/*.cpp`; re-run
   `tools/host.sh configure` so CMake sees it). Start with `c.reset_clip(); c.clear(Color::kWhite);`.
3. Declare `void render_<name>_face(const ui::WatchState& state, gfx::Canvas& canvas) noexcept;` in
   `components/qz_faces/src/face_common.hpp` (next to `render_minimal_face`).
4. Add one row to `kTable` in `registry.cpp` and bump the `std::array<FaceDescriptor, N>` size:
   `{2, "<name>", &render_<name>_face},` (names are unique, lowercase, no spaces: console and
   simulator select by name).
5. Tests in `components/qz_faces/test/`: `registry_test.cpp` `IdsAreStablePersistedValues` gets an
   `EXPECT_EQ` for the new id/name; `faces_test.cpp` already loops over every descriptor for the
   variant tests (time validity, weather, sync, power, goal, long strings), so add face-specific
   checks there (e.g. `<Name>FaceTimeDigitsAreAtLeast48PixelsTall`, layout bounds).
6. Golden scenes: in `components/qz_selftest/src/scenes.cpp` add rows next to the `// minimal face`
   block, e.g. `{"face_<name>_24h", ScreenId::kFace, &on_face<v_24h, <id>>, {}},` for at least
   24h, 12h_pm, time_invalid, goal_met, wx_stale, sync_failed, power_saver, charging.
7. Generate and review the images:
   ```bash
   tools/host.sh build
   build/host/golden/qz_golden --update      # writes components/qz_selftest/golden/*.png + src/golden_crc.inc
   build/host/golden/qz_golden --check       # must exit 0
   ```
   Open the new `face_<name>_*.png` files and look at them. Commit PNGs and `golden_crc.inc` together.
8. Preview any state with the simulator:
   ```bash
   build/host/sim/qz_sim --face <name> --time 2026-10-06T08:15:00 --tz America/Chicago \
     --steps 7421 --goal 10000 --battery 76 --weather 'temp_c=18,code=61,age_min=20' --scale 2 --out face.png
   ```
   Also try `--hour-format 12`, `--time-invalid`, `--power saver`, `--charging`, `--sync failed`.
   Update the `--face NAME (default|minimal)` help text in `host/sim/sim.cpp` to list the new name.
9. Gate: `tools/format.sh; tools/check.sh --fast` (then `tools/check.sh` before handing over).
   Optional review PNGs from the face tests: `QZ_FACE_PNG_DIR=/tmp/faces build/host/qz_faces_test`.

Do not edit golden PNGs by hand and do not run `--update` to hide an unintended change to another
face: read the diff of every PNG that changed.
