# badge os

A small operating system for the Hackaday Supercon 2026 "Billboard Badge" (ESP32-S3 with 8 MB PSRAM and a 9.7"
960x672 black/white/red e-paper), written in C on ESP-IDF. The badge boots to an iOS-style home screen of app icons.
Apps take over the badge; hold MENU + and MENU - together to get back home.

## Features
- **Home screen**: 8 app icons per page, OS version bottom right.
- **Calm partial e-paper updates**: about 0.5 s per frame, only the pixels that change move, nothing flashes.
  This is a custom waveform for the panel's two Pervasive Displays B1 controllers. The panel can't refresh faster
  than about 2 frames per second.
- **Top bar on every screen**: what each button does right now, printed beside the button; app name, username,
  OS version and battery in the middle.
- **Online update**: pulls the latest GitHub release over Wi-Fi (OTA, with rollback if the new version doesn't
  boot).
- **Badge-to-badge update**: one badge flashes its own firmware onto another over a USB-C cable (USB host mode).
- **Wi-Fi**: the radio runs ranging in the background; an app that needs it otherwise (Pong, Online update, the
  feed and weather apps) takes it over.
- **Ranging service**: badges find each other with ESP-NOW and measure their distance with Wi-Fi FTM (802.11mc
  round-trip time). Each badge shares its measurements, so every badge learns the distances between the others.
- **Settings**: saved in flash and kept across updates; edited from a phone through the Settings app.
- **Add-ons**: AS5600 magnetic knob on an SAO port (I2C 0x36) as an optional controller.

## Apps
| App | What it does |
|---|---|
| Badge | Name tag: "HELLO my name is", or your own text or picture from Settings |
| Update | Online update from GitHub, or flash another badge over USB-C |
| Pong | Two-badge Pong over ESP-NOW, buttons or knob |
| Snake | Nonstop snake with wrap-around walls |
| Life | Conway's Game of Life, 2340 cells, runs by itself |
| Arcade games | Three single-player games: falling blocks, a wave of descending aliens, and a field of drifting rocks to shoot |
| Radar | Nearby badges running badge os, with distances, and a map once 3+ badges are in range |
| Source | A QR code to this repo |
| Hackaday | Hackaday's latest posts with their images, one per screen; open one to read it |
| Video | A 15 s clip at 4 frames/s, to show off the panel |
| Weather | Current weather and a 5-day forecast (Open-Meteo) for a saved city or coordinates |
| Settings | Opens a Wi-Fi hotspot "Badge-XXXX" (password `supercon`) with a settings page: username, Wi-Fi, Badge text or picture, weather place and units |

## Controls
The four buttons sit two in each top corner: MENU + above A on the left, MENU - above B on the right. The top bar
shows what each one does, right beside it. On the home screen MENU + / MENU - move left / right and A or B opens
the app. Hold MENU + and MENU - together for half a second to go back home from any app.

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

## Writing an app
Apps draw into a 1-bit frame buffer and call `ui_frame()`. The OS handles the panel, buttons, battery, Wi-Fi,
ranging and the knob. The whole API is in [main/badge.h](main/badge.h). Rules and the app contract are in
[AGENTS.md](AGENTS.md).

## Hardware notes
Pins, buttons, LEDs, power, the panel's refresh modes, timings, ghosting and the rules that keep it alive:
[DEVICE_INFO.md](DEVICE_INFO.md). The panel driver started in
[supercon26-badge-idf](https://github.com/yigitabi5444/supercon26-badge-idf).

Unofficial. Not affiliated with Hackaday or Pervasive Displays.
