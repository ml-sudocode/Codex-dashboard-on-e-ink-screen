# Working on badge os

- **OS maintainer:** the herdr agent `idfport`. Owns `main/main.c`, `main/badge.h`, the build, the release tags and
  the badge's serial port (it flashes; nobody else does).
- **App agents:** one per app. Each owns only its own files:
  - `app-update`: `main/app_update.c`
  - `app-pong`: `main/app_pong.c`, `main/link.h`, `tools/test_link.c`
  - `app-ota`: `main/app_ota.c`, `.github/workflows/`
  - `app-snake`: `main/app_snake.c`
  - `idfport` (OS maintainer): `main/app_life.c` (Game of Life), `main/app_source.c` (QR code to this repo),
    `main/app_video.c` (+ `main/rickroll.vid` from `tools/make_video.py`)
  - `app-aliens`: `main/app_aliens.c`
  - `app-blocks`: `main/app_blocks.c`
  - `app-rocks`: `main/app_rocks.c`
  - `app-radar`: `main/app_radar.c`
  - `app-hackaday`: `main/app_hackaday.c`
  - `app-doom`: `main/app_doom.c`, `components/doom/` (the engine as its own component)
  - `sao-cricket`: `components/cricket/` (Noisy Cricket SAO as the speaker: its firmware, an SWD flasher, I2C sound)
  - `app-badge`: `main/app_badge.c`
  - `app-settings`: `main/app_settings.c`
  - `app-weather`: `main/app_weather.c`

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
- On the home screen the badge must always be flashable over USB: an app that installs the USB host and returns
  gets the badge restarted by the OS (better: restart it yourself when you are done).
- The OS has closed its panel session when your app starts. Draw into `fb` (960x672, 1 bit, 1 = black) with
  `box`, `outline`, `text`, then `ui_frame(1)` (the pulls argument is ignored now). The screen starts white.
  `ui_close()` when the screen will sit still.
- Rows 0..`TOP`-1 (48 px) are the OS top bar (button labels, app name, username, OS version, battery); `ui_frame`
  draws it over `fb`.
  Keep your picture below `TOP`.
- Wi-Fi: the OS keeps ranging running on every screen. `wifi_connect` joins the saved network (Settings, the event
  Wi-Fi by default) and stops ranging; when done, `wifi_off()` then `ranging_start()`. Settings live behind
  `cfg_get`/`cfg_set` (keys in `badge.h`).
- Buttons: `btn_get(timeout_ms)` returns `BTN_PLUS`, `BTN_MINUS`, `BTN_A`, `BTN_B` (as printed; two in each top
  corner: MENU + above A on the left, MENU - above B on the right), or -1. Label them with
  `ui_keys(plus, minus, a, b)`: the OS shows each label beside its button in the top bar. Holding MENU + and
  MENU - together for 0.4 s goes home from any app (the OS restarts the badge between frames). The press that opened your app has
  been flushed.

## The panel (measured; the full story with pins and timings is in DEVICE_INFO.md)
- `ui_frame` is a calm partial update over the rows that changed: ~0.24 s if they reach the top of the screen,
  ~0.13 s if they all lie in the lower half (so keep fast-changing things low). Only changed pixels move, nothing
  flashes. It returns while the panel works and waits only if the previous frame has not started yet: draw the next
  frame meanwhile, and run game logic on its own clock (see app_snake.c) rather than one step per frame.
  Ink is dark red, erased ink leaves a light ghost. No red. `ui_wipe()` (~2.8 s) shakes the ghosts off.
- Never run the slow 3-colour refresh (~35 s): the owner does not want it on the badge.
