# How badge os works

badge os is an ESP-IDF project. The OS is one component (`components/os`), every app is one component
(`apps/<name>`), and `main/` only boots and runs the home screen. The hardware it drives (pins, panel timings,
power, flash) is in [DEVICE_INFO.md](DEVICE_INFO.md).

## Layout
| Path | What |
|---|---|
| `main/main.c` | `app_main`: boot, then the home screen |
| `main/home.c` | the home screen: icon grid, pages, opening apps, what the badge gets back after one |
| `main/apps.c` | the app list, in home screen order |
| `components/os/include/badge/` | the app API, one header per service; `badge.h` includes them all |
| `components/os/include/badge/os.h` | what `main/` and the OS modules share; apps never include it |
| `components/os/*.c` | the OS services (table below); `epd.h` is private to the panel driver |
| `components/stb` | stb_image (JPEG, PNG, zlib) for the apps, buffers in PSRAM |
| `components/cricket` | Noisy Cricket SAO: the speaker over I2C, and an SWD flasher for its firmware |
| `components/doomgeneric` | the Doom engine (GPL-2), used by `apps/doom` |
| `apps/<name>/` | one app: sources, assets, tools, its icon; how to write one: [apps/README.md](../apps/README.md) |
| `partitions.csv`, `sdkconfig.defaults` | flash layout, build options |

## Boot
`app_main` (`main/main.c`) runs these in order on the main task (16 KB stack, `CONFIG_ESP_MAIN_TASK_STACK_SIZE`):

1. NVS: Wi-Fi keeps its calibration there, settings live there. Erased and set up again if it is full or from a newer
   format.
2. `display_init`: the panel's pins, SPI at 20 MHz, the panel lock, the restart hook.
3. `sao_init`: the SAO I2C bus (logs every address that answers), the Cricket, one knob reading in the log.
4. `buttons_init`: the button task.
5. `leds_init`: the LED bar off (the LEDs keep their colours through a reset).
6. `ui_wipe`: the film to a known all-white state, which the display driver needs to start from.
7. `battery_pct`: one reading before Wi-Fi starts sharing ADC2.
8. `ranging_start`: other badges' Radar sees this badge from now on.
9. `home`: the first home screen frame marks this firmware good (`esp_ota_mark_app_valid_cancel_rollback`). A new
   version that never gets here is rolled back by the bootloader at the next reset.

## Tasks
| Task | Stack | Prio | What |
|---|---|---|---|
| main | 16 KB | 1 | boot, the home screen, and every app's `run()` |
| buttons | 3 KB | 3 | polls the buttons every 20 ms, MENU + and MENU - held 0.4 s in an app: `esp_restart` |
| ranging | 4 KB | 4 | ESP-NOW beacons and FTM sessions, while ranging runs |
| apps' own | | | draw tasks, network tasks: any task that calls `ui_frame`, `ui_close` or `ui_wipe` needs 6 KB or more |

There is no panel task: the panel work happens in the task that calls `ui_frame`.

## Display
`components/os/display.c` keeps the frame buffer `fb` (PSRAM), a copy of what the film shows (`shown`), the panel lock
and the session state. `epd.c` talks to the panel: two CoGs on one SPI bus, each with its own CS and BUSY.

A frame (`ui_frame`):
1. Draw the top bar into rows 0..`TOP`-1 of `fb`.
2. `fb` equal to `shown`: nothing to do, return.
3. Wait for the previous frame's pull: BUSY polled every 1 ms tick.
4. No session open, or 60 pulls in this one: close it and open a fresh one. Opening: reset, the register sequence
   with the quick waits, the calm transition table into `0xB0`, DC-DC up, `shown` into both RAM planes (~0.4 s).
5. DUW whole panel, the whole of `fb` into `0x10` of both CoGs (80 KB, 4 KB SPI pieces, ~35 ms), DRFW whole panel
   (`00 3B 00 A9`, never shorter: a short DRFW copies rows down the panel), trigger (`0x15 3C`).
6. `shown` = `fb`, return. The pull runs on its own for ~0.24 s.

The CoG keeps the frame it just showed in `0x11` after every pull, and the transition table drives only the pixels
where `0x10` and `0x11` differ, so nothing else moves and nothing flashes. `0x11` is never written after the session
opens. So `ui_frame` costs ~35 ms plus whatever is left of the previous pull: ~3.5-4 frames/s, and the app draws the
next frame while the panel pulls.

- `ui_close`: waits for the pull, closes the session (DC-DC soft shutdown). For screens that sit still.
- `ui_wipe`: closes the session, then a session of its own on the badge team's fast table: an all-black frame, an
  all-white frame (~2.8 s), closed. `fb` and `shown` end all white, like the film.
- The restart hook (`esp_register_shutdown_handler`): every `esp_restart` (going home, an app restarting) takes the
  lock, lets the pull in flight finish and closes the session. Never cut the panel off mid-refresh.
- One mutex around all of it, so apps may draw on several tasks.
- Never the panel's own slow refresh (~35 s, true black and red): the owner does not want it on the badge.

## Which module owns what
| File | Owns |
|---|---|
| `epd.c` | SPI2 and the panel pins (CS 13/3, BUSY 12/8, DC 10, RST 11, SCK 21, MOSI 47), the waveform tables, `ms()` |
| `display.c` | `fb`, the shown frame, sessions, the lock, the restart hook, the top bar, the running app's name (`os_app`) |
| `draw.c` | `box`, `outline`, `text` (8x8 font), `icon_base` |
| `buttons.c` | GPIO 40/15/39/16, the press queue, the MENU + / MENU - chord |
| `wifi.c` | the Wi-Fi driver (set up once), joining the saved network |
| `ranging.c` | ESP-NOW, FTM, APSTA mode, the hidden SoftAP `badge-os`, the table of ranges |
| `settings.c` | NVS namespace `cfg`, the Badge picture at 0x630000, the event Wi-Fi defaults |
| `sound.c` | `snd_*`: to the Cricket if one with the speaker firmware is plugged in, else PDM on I2S0 to an SAO port |
| `leds.c` | the LED bar: RMT on GPIO38 |
| `sao.c` | the SAO I2C bus (GPIO17/18), ADC1 on the SAO ports' GPIO1, the knob, the Cricket's start |
| `power.c` | ADC2 on GPIO14: `battery_pct` |
| `main/home.c` | which app runs; after it returns: USB, sound, the radio, the LEDs |

## Radio
- Ranging runs on every screen: Wi-Fi in APSTA mode on channel 1, a hidden SoftAP (`badge-os`) as this badge's FTM
  responder, power save off. Two ESP-NOW broadcasts: `BOSR` about once a second (the SoftAP MAC, up to 8 own ranges,
  the username; byte for byte what v0.8 badges understand) and `BOSF` about every 250 ms (raw FTM results, so the
  measured badge needs not measure back). Details and measurements: `apps/radar/RADAR_RESEARCH.md`.
- An app that needs the radio otherwise calls `wifi_on` (ESP-NOW on its own channel, e.g. Pong) or `wifi_connect`
  (the saved network: Online update, Clock, Hackaday, Weather). Both stop ranging first. Never use ESP-NOW while
  ranging runs.
- An app done with the network but staying open: `wifi_off()`, then `ranging_start()`.
- Back home, `home.c` gives the radio back: if ranging is not running, `wifi_off()` and `ranging_start()`.
- The Settings app runs its own SoftAP and restarts the badge when it is done.

## Back home
After an app's `run()` returns, `home.c`:
1. restarts the badge if the app made USB a host (`usb_host_lib_info`): on the home screen the badge must always be
   flashable over USB;
2. stops sound;
3. gives the radio back to ranging;
4. turns the LEDs off;
5. wipes the panel.

Most apps never return. MENU + and MENU - held together restart the badge, which boots back to the home screen.

## Flash
| Offset | Size | What |
|---|---|---|
| 0x0 | | bootloader |
| 0x8000 | | partition table |
| 0x9000 | 24 KB | NVS: settings (`cfg`), Wi-Fi calibration |
| 0xF000 | 8 KB | otadata |
| 0x20000 | 3 MB | ota_0 |
| 0x320000 | 3 MB | ota_1 |
| 0x620000 | 64 KB | coredump (`idf.py coredump-info` after a crash) |
| 0x630000 | 128 KB | the Badge app's picture (`cfg_set_image`) |
| 0x650000 | 1.69 MB | Doom's packed game data, to the end of flash |

The picture and the Doom data are used by address, not through the partition table: badges updated over the air
keep their old table. Online update writes the other OTA slot; a version that does not reach the home screen once
is rolled back.

## Logs
The OS logs under the tag `os` (session open and close, button presses on the home screen, opening apps, new badges
in ranging). Apps use their own tags, e.g. `ota`, `rss` (Hackaday), `video`, `clock`, `doom`. The per-frame line
(`os`, how long a frame waited and took to send) is at debug level.
