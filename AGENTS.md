# Working on badge os

- **OS maintainer:** the herdr agent `idfport`. Owns `components/os/` (the OS and its app API), `main/`, the build,
  `docs/`, the release tags and the badge's serial port (it flashes; nobody else does).
- **App agents:** one per app. Each owns its own folder:
  - `app-update`: `apps/update/` (wired update, Cricket update and Online update, `ota.c`), `.github/workflows/`
  - `app-pong`: `apps/pong/`
  - `app-snake`: `apps/snake/`
  - `app-aliens`: `apps/aliens/`
  - `app-blocks`: `apps/blocks/`
  - `app-rocks`: `apps/rocks/`
  - `app-radar`: `apps/radar/`
  - `app-hackaday`: `apps/hackaday/`
  - `app-doom`: `apps/doom/`, `components/doomgeneric/` (the engine as its own component)
  - `app-badge`: `apps/badge/`
  - `app-settings`: `apps/settings/`
  - `app-weather`: `apps/weather/`
  - `sao-cricket`: `components/cricket/` (Noisy Cricket SAO as the speaker: its firmware, an SWD flasher, I2C sound)
  - `idfport` (OS maintainer): `apps/life/`, `apps/source/`, `apps/video/`, `apps/clock/` (contributed in pull
    request #2)

## Rules for app agents
- Work in your own git worktree on your own branch (`~/Documents/hackaday26/badge_os-<app>`, branch `app/<app>`).
  Commit there. When a version is ready to try, ask the OS maintainer to merge and flash it:
  `herdr agent prompt idfport "<app>: branch app/<app> is ready to merge and flash: <one line on what changed>"`
- Need something from the OS (a new function in a `badge/*.h` header, a change in `components/os` or `main/`, a
  Kconfig option)? Ask the same way; do not edit OS files yourself. Your app's own `CMakeLists.txt` and
  `idf_component.yml` are yours: add what your app requires there.
- Do not flash and do not open `/dev/cu.usbmodem*`. Do not push or create repos unless the owner says so.
- Build to check: `source ~/.espressif/tools/activate_idf_v6.0.2.sh && idf.py build` in your worktree. Warnings
  count as errors.
- Anything that takes more than ~15 s to test on hardware: ask the humans to try it (the owner follows you over
  Remote Control) instead of waiting on it yourself.
- Keep it minimal, in the style of the code around it, no source file much over 400 lines. Never commit `.bin` files
  or personal data (names, photos).

## The app contract
How to write an app, its folder, `CMakeLists.txt`, the `app_t` descriptor and the rules (draw below `TOP`,
`ui_keys`, MENU + and MENU - held = home, Wi-Fi and ranging, tasks that draw need 6 KB of stack): `apps/README.md`.
The API is `components/os/include/badge/`, one documented header per service. How the OS works: `docs/os.md`.

## The panel (measured; pins and timings in docs/DEVICE_INFO.md)
- `ui_frame` is one calm whole-screen update: ~0.27 s, ~3.5-4 frames/s. Only changed pixels move, nothing flashes.
  It blocks only until the previous frame's pull is done, then returns while the panel works: draw the next frame
  meanwhile, and run game logic on its own clock (see `apps/snake`) rather than one step per frame.
  Ink is dark red, erased ink leaves a light ghost. No red. `ui_wipe()` (~2.8 s) shakes the ghosts off.
- Never run the slow 3-colour refresh (~35 s): the owner does not want it on the badge.
