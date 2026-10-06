# Font provenance (WP-06)

All fonts are **Spleen 2.2.0** by Frederic Cambus, licensed **BSD-2-Clause** (full text in `LICENSE`
next to the sources; the generated header names the licence and copyright holder).

- Upstream: https://github.com/fcambus/spleen
- Tag `2.2.0`, commit `0493c34e22791824767c618fed42c434b477662c`
- Fetched from `https://raw.githubusercontent.com/fcambus/spleen/0493c34e22791824767c618fed42c434b477662c/<file>`

| File | SHA-256 | Used for |
|---|---|---|
| `LICENSE` | `f33fe8679d5b2abecc4f1313ce6c6bfa58262964de5f7bca146596a7318047af` | licence text |
| `spleen-6x12.bdf` | `fc0743d164690f99b7e2e1b9d503180e4c719a9831ae03fd8f6da18c857dee27` | `kSmall`, Latin-1 |
| `spleen-8x16.bdf` | `4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae` | `kMedium`, Latin-1 |
| `spleen-16x32.bdf` | `f6db2549d46c5699ceca7ccc26e747c8bdad3af76239f86aae6ed995bd2f3d37` | `kLarge`, Latin-1 |
| `spleen-12x24.bdf` | `181989f2c5f47b34c2474cd424b387b0002aa77fc970e59ee31e16733af24687` | `kHuge`, scaled 3x (x) by 4y, digits and clock punctuation |

`tools/fontgen.py` pins the four `.bdf` hashes and refuses to run on any other bytes.

## Why kHuge is scaled
Spleen's largest size (32x64) has digits only 40 px tall (ink), below the 48 px readability floor
(ROADMAP WP-16). 12x24 digits are 15 px tall of ink; replicated 3x horizontally and 4x vertically
(pixel replication, deterministic, integer-only) they are 60 px tall, 36 px advance, and `88:88`
is 180 px wide. A uniform 5x of 8x16 also qualifies (50 px) but looks much blockier.

## Regenerating
`python3 tools/fontgen.py` writes `components/qz_gfx/src/generated/fonts_data.hpp` (committed);
`python3 tools/fontgen.py --check` fails if the committed file is stale. Output is byte-identical
across runs (no timestamps or paths).
