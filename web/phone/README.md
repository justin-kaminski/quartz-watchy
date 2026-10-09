# Quartz phone page

`index.html` is the companion page for **Phone sync** (docs/ARCHITECTURE.md section 13a). One
self-contained file: no build step, no external scripts, no trackers. It talks to the watch with
Web Bluetooth and to `api.open-meteo.com` for weather; nothing else.

## Requirements
- Chrome on Android 6+ (or desktop Chrome/Edge with Bluetooth). Firefox and iOS Safari have no
  Web Bluetooth.
- A secure context: the page must be served over `https://` (or from `http://localhost`). Opening
  the file directly is not enough on Android.

## Hosting
- **GitHub Pages (intended):** `.github/workflows/pages.yml` deploys this folder on pushes to main.
  It is dormant while the repository is private; after making it public, enable Settings > Pages >
  Source: "GitHub Actions". The page contains no secrets.
- **Before that, on the phone over USB:** with USB debugging on, run
  `python3 -m http.server -d web/phone 8000` on the PC and `adb reverse tcp:8000 tcp:8000`, then open
  `http://localhost:8000` in Chrome on the phone (localhost counts as secure).
- **Desktop Chrome:** the same `http.server` command, then `http://localhost:8000`.
- **PC without a browser:** `tools/phone_link_test.py` pairs and exercises the link over BlueZ.

## Use
1. Watch: Menu > Phone > Sync with phone.
2. Page: Connect, pick `Quartz-XXXX`. The first time, the phone asks for a code: type the six digits
   the watch shows. Later connections need no code.
3. The page syncs time, time zone and (with a location set) weather, then shows status and
   settings. Tap Done when finished; the watch turns Bluetooth off.
