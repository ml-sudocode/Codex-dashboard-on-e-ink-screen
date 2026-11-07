# badge os

A small "OS" for the Hackaday Supercon 2026 "Billboard Badge" (ESP32-S3, 9.7" black/white/red e-paper), in C on
ESP-IDF. The badge boots to a home screen of app icons; the only way back to it is a reboot, or an app that
returns. The version (`version.txt`) is shown at the bottom right of the home screen.

## Use
- MENU + / MENU - (the top two buttons) move left / right; A or B (the bottom two) open the app.
- A screen update takes about a second and the whole panel flashes: that is the e-paper's fast mode.

## Apps
| App | What it does |
|---|---|
| Update | Wired badge-to-badge update: copies this badge's firmware onto another badge on a USB-C cable |
| Pong | Two-badge Pong over ESP-NOW |

## Build and flash
ESP-IDF 6.0 (tested with v6.0.2):
```sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```
Every boot shakes the panel black, then white (~2.8 s); after that every frame is a calm ~0.5 s partial update.

Panel driver, pins and timings come from https://github.com/yigitabi5444/supercon26-badge-idf (see its NOTES.md).
Hardware, pins, screen modes, timings and ghosting: [DEVICE_INFO.md](DEVICE_INFO.md).
How apps and agents work on this repo: [AGENTS.md](AGENTS.md).

Unofficial. Not affiliated with Hackaday or Pervasive Displays.
