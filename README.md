# badge os

A small operating system for the Hackaday Supercon 2026 "Billboard Badge" (ESP32-S3 with 8 MB PSRAM and a 9.7"
960x672 black/white/red e-paper), written in C on ESP-IDF. The badge boots to an iOS-style home screen of app icons.
Apps take over the badge, and you get back home with a reboot or when an app returns.

## Features
- **Home screen**: 8 app icons per page, battery level top right, OS version bottom right.
- **Calm partial e-paper updates**: about 0.5 s per frame, only the pixels that change move, nothing flashes.
  This is a custom waveform for the panel's two Pervasive Displays B1 controllers. The panel can't refresh faster
  than about 2 frames per second.
- **Top bar in every app**: app name, OS version, battery.
- **Online update**: pulls the latest GitHub release over Wi-Fi (OTA, with rollback if the new version doesn't
  boot).
- **Badge-to-badge update**: one badge flashes its own firmware onto another over a USB-C cable (USB host mode).
- **Wi-Fi only on demand**: the radio is off unless an app needs it.
- **Ranging service**: badges find each other with ESP-NOW and measure their distance with Wi-Fi FTM (802.11mc
  round-trip time). Each badge shares its measurements, so every badge learns the distances between the others.
- **Add-ons**: AS5600 magnetic knob on an SAO port (I2C 0x36) as an optional controller.

## Apps
| App | What it does |
|---|---|
| Update | Online update from GitHub, or flash another badge over USB-C |
| Pong | Two-badge Pong over ESP-NOW, buttons or knob |
| Snake | Nonstop snake with wrap-around walls |
| Life | Conway's Game of Life, 2340 cells, runs by itself |
| Arcade games | Three single-player games: falling blocks, a wave of descending aliens, and a field of drifting rocks to shoot |
| Radar | Nearby badges running badge os, with distances, and a map once 3+ badges are in range |
| Source | A QR code to this repo |

## Controls
The four buttons form a 2x2 block. MENU + (top left) and MENU - (top right) move left and right on the home screen;
A (bottom left) or B (bottom right) opens the app.

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
