# badge os

A small operating system for the Hackaday Supercon 2026 "Billboard Badge" (ESP32-S3 with 8 MB PSRAM and a 9.7"
960x672 black/white/red e-paper), written in C on ESP-IDF. The badge boots to an iOS-style home screen of app icons.
Apps take over the badge; hold MENU + and MENU - together to get back home.

## Features
- **Home screen**: 8 app icons per page, OS version bottom right.
- **Calm e-paper frames**: about 0.27 s per frame (3.5-4 frames/s), only the pixels that change move, nothing
  flashes. A custom waveform for the panel's two Pervasive Displays B1 controllers.
- **Top bar on every screen**: what each button does right now, printed beside the button; app name, username,
  OS version and battery in the middle.
- **Online update**: pulls the latest GitHub release over Wi-Fi (OTA, with rollback if the new version doesn't
  boot).
- **Badge-to-badge update**: one badge flashes its own firmware onto another over a USB-C cable (USB host mode).
- **Ranging**: badges find each other with ESP-NOW and measure their distance with Wi-Fi FTM (802.11mc round-trip
  time), on every screen. Each badge shares its measurements, so every badge learns the distances between the others.
  An app that needs the radio otherwise takes it over.
- **Settings**: saved in flash and kept across updates; edited from a phone through the Settings app.
- **Add-ons**: an AS5600 magnetic knob on an SAO port as a controller, a Noisy Cricket SAO as the speaker.

## Apps
| App | What it does |
|---|---|
| Badge | Name tag: "HELLO my name is", or your own text or picture from Settings |
| Clock | A huge 24-hour dial moving past a fixed hand, your name beside it, the Supercon talks on con days |
| Update | Online update from GitHub, flash another badge over USB-C, or put the speaker firmware on a Cricket |
| Pong | Two-badge Pong over ESP-NOW, buttons or knob |
| Snake | Nonstop snake with wrap-around walls |
| Life | Conway's Game of Life, 2340 cells, runs by itself |
| SpaceInv, Tetris, Asteroids | Single-player arcade games: descending aliens, falling blocks, drifting rocks |
| Radar | Nearby badges running badge os, with distances, and a map once 3+ badges are in range |
| Source | A QR code to this repo |
| Hackaday | Hackaday's latest posts with their images, one per screen; open one to read it |
| Video | A 15 s clip at 4 frames/s, to show off the panel |
| Weather | Current weather and a 5-day forecast (Open-Meteo) for a saved city or coordinates |
| Settings | Opens a Wi-Fi hotspot "Badge-XXXX" (password `supercon`) with a settings page: username, Wi-Fi, Badge text or picture, weather place and units |
| Doom | The shareware episode, downloaded once into flash |

## Controls
The four buttons sit two in each top corner: MENU + above A on the left, MENU - above B on the right. The top bar
shows what each one does, right beside it. On the home screen MENU + / MENU - move left / right and A or B opens
the app. Hold MENU + and MENU - together for half a second to go back home from any app.

## Layout
| Path | What |
|---|---|
| [`main/`](main) | boot (`main.c`), the home screen (`home.c`), the app list (`apps.c`) |
| [`components/os/`](components/os) | the OS services: display driver, drawing, buttons, Wi-Fi, ranging, settings, sound, LEDs, SAO add-ons, battery |
| [`components/os/include/badge/`](components/os/include/badge) | the app API, one documented header per service; apps include `badge/badge.h` |
| [`apps/<name>/`](apps) | one folder and component per app, with its assets and tools; [apps/README.md](apps/README.md): how to write one |
| [`components/`](components) | also `stb` (image decoding), `cricket` (Noisy Cricket SAO), `doomgeneric` (the Doom engine) |
| [`docs/os.md`](docs/os.md) | how the OS works: boot, the display driver, who owns what, the radio rules, flash layout |
| [`docs/DEVICE_INFO.md`](docs/DEVICE_INFO.md) | the hardware: pins, buttons, the panel's refresh modes and timings, ghosting, power, flash |

## Install
Download `badge_os_full.bin` from the [latest release](https://github.com/yigitabi5444/supercon26-badge-os/releases/latest)
and flash it at 0x0:
```sh
esptool --chip esp32s3 -p /dev/cu.usbmodem101 write-flash 0x0 badge_os_full.bin
```
A badge running the stock MicroPython firmware has to be put into download mode first: hold BOOT, tap RESET.
After that, use Update -> Online update, or flash other badges from one badge with Update -> Flash another badge.

## Build
ESP-IDF 6.0 (tested with v6.0.2):
```sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```
Online update needs Wi-Fi credentials at build time in `main/wifi_secret.h` (gitignored):
```c
#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"
```
Without the file everything else works, and Online update says it has no Wi-Fi.

Releases: pushing a `v*` tag runs `.github/workflows/release.yml`, which builds that version and attaches
`badge_os.bin` (what Online update downloads) and `badge_os_full.bin` to the GitHub release.

## License
GPL-2.0 (`LICENSE`), because the Doom app links the doomgeneric engine (`components/doomgeneric`, GPL-2.0). Bundled
third-party parts keep their own terms: stb_image (public domain), the 8x8 font (public domain), the Clock's DejaVu
fonts (DejaVu license). Doom's game data is id Software's unmodified shareware `DOOM1.WAD`, downloaded by the badge
from the `doom-wad` release, not part of this source. The panel driver started in
[supercon26-badge-idf](https://github.com/yigitabi5444/supercon26-badge-idf).

Unofficial. Not affiliated with Hackaday or Pervasive Displays.
