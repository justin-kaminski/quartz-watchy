# Quartz phone page

`index.html` is the companion page for **Phone sync** (docs/ARCHITECTURE.md section 13a). One
self-contained file: no build step, no external scripts, no trackers. It talks to the watch with
Web Bluetooth and to `api.open-meteo.com` for weather; nothing else.

## Requirements
- Chrome on Android 6+ (or desktop Chrome/Edge with Bluetooth). Firefox and iOS Safari have no
  Web Bluetooth.
- A secure context: the page must be served over `https://` (or from `http://localhost`). Opening
  the file directly is not enough on Android.

## Hosting options
- Any static HTTPS host (GitHub Pages from a public repository that holds just this file,
  Cloudflare Pages, Netlify). The page contains no secrets.
- Local testing on a desktop: `python3 -m http.server -d web/phone 8000`, then open
  `http://localhost:8000` in Chrome.

## Use
1. Watch: Menu > Phone > Sync with phone.
2. Page: Connect, pick `Quartz-XXXX`. The first time, the phone asks for a code: type the six digits
   the watch shows. Later connections need no code.
3. The page syncs time, time zone and (with a location set) weather, then shows status and
   settings. Tap Done when finished; the watch turns Bluetooth off.
