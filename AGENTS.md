# Working on badge os

- **OS maintainer:** the herdr agent `idfport`. Owns `main/main.c`, `main/badge.h`, the build, the release tags and
  the badge's serial port (it flashes; nobody else does).
- **App agents:** one per app. Each owns only its own files:
  - `app-update`: `main/app_update.c`
  - `app-pong`: `main/app_pong.c`, `main/link.h`, `tools/test_link.c`

## Rules for app agents
- Work in your own git worktree on your own branch (`~/Documents/hackaday26/badge_os-<app>`, branch `app/<app>`).
  Commit there. When a version is ready to try, ask the OS maintainer to merge and flash it:
  `herdr agent prompt idfport "<app>: branch app/<app> is ready to merge and flash: <one line on what changed>"`
- Need something from the OS (a new function in `badge.h`, a change in `main.c`, a Kconfig option, a dependency)?
  Ask the same way; do not edit OS files yourself.
- Do not flash and do not open `/dev/cu.usbmodem*`. Do not push or create repos unless the owner says so.
- Build to check: `source ~/.espressif/tools/activate_idf_v6.0.2.sh && idf.py build` in your worktree.
- Anything that takes more than ~15 s to test on hardware: ask the humans to try it (the owner follows you over
  Remote Control) instead of waiting on it yourself.
- Keep it minimal, in the style of `main.c`. Never commit `.bin` files or personal data (names, photos).

## The app contract (`main/badge.h`)
- `void app_<name>(void)` runs on the home screen's task. It takes over the badge; returning goes back to the
  home screen (most apps never return; a reboot is the other way home).
- The OS has closed its panel session when your app starts. Draw into `fb` (960x672, 1 bit, 1 = black) with
  `box`, `outline`, `text`, then `ui_frame(pulls)`. Use 3 pulls for a clean first picture, 1 afterwards.
  `ui_close()` when the screen will sit still.
- Buttons: `btn_get(timeout_ms)` returns `BTN_PLUS`, `BTN_MINUS`, `BTN_A`, `BTN_B` (as printed; 2x2 block:
  MENU + top left, MENU - top right, A bottom left, B bottom right), or -1. The press that opened your app has
  been flushed.

## The panel (measured; the full story with pins and timings is in DEVICE_INFO.md)
- A fast frame is plane data + one ~1.07 s pull, black/white only, and the whole panel flashes. A slow 3-colour
  refresh is 35 s. Red cannot be drawn or removed in fast mode. `ui_frame` starts a fresh panel session every
  20 frames (~5 s extra) as the badge team's driver does.
- Partial, no-flash updates are being researched in `~/Documents/hackaday26/panel_lab`; they are not in the OS yet.
