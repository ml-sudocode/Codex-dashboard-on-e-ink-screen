# Device info: Hackaday Supercon 2026 "Billboard Badge"

Everything here was measured on a badge or read from the badge's KiCad schematic
(https://github.com/Hack-a-Day/2026-Billboard_Badge, `hardware/billboard_pcb/`, netlist exported with kicad-cli),
unless marked otherwise. Other sources: the supercon26-badge-idf repo's NOTES.md and the badge team's
`documentation/itc97-driver-reference.pdf` ("badge team doc").

## At a glance
| Part | What |
|---|---|
| MCU | ESP32-S3-WROOM-1 (U1), 8 MB flash, octal PSRAM, Wi-Fi 2.4 GHz / ESP-NOW, native USB |
| Screen | Pervasive Displays E2969JS0B1, 9.7" Spectra R2.0, black/white/red, 960x672, two CoGs ("B1") |
| Buttons | 4 user buttons, two in each top corner (MENU + above A on the left, MENU - above B on the right), plus BOOT and RESET |
| LEDs | 8x WS2812 (XL-2121RGBC) bar, 1 status LED, 1 charge LED (charger-driven) |
| Power | LiPo (JST PH 2.0 or pads), MCP73831 charger 500 mA, AP2112K-3.3 LDO, slide switch |
| USB | USB-C, USB 2.0 full speed on the ESP32-S3 native USB; power sink only |
| Expansion | 3 SAO v2 ports (shared I2C + 2 GPIOs each), test pads for UART0, GPIO9, GPIO41 |

## Pin map (ESP32-S3 GPIO)
| GPIO | Net | Use |
|---|---|---|
| 0 | SW2 | BOOT button (strapping pin: held at reset = download mode) |
| 1 | STATUS_LED | status LED D6 |
| 2 / 42 | SAO1_GPIO1 / SAO1_GPIO2 | SAO port 1 (J8) |
| 4 / 5 | SAO2_GPIO1 / SAO2_GPIO2 | SAO port 2 (J9) |
| 7 / 6 | SAO3_GPIO1 / SAO3_GPIO2 | SAO port 3 (J10) |
| 3 | EPD_CS2 | panel, right CoG (slave) chip select. Strapping pin, fine as output after boot |
| 8 | EPD_BUSY2 | right CoG BUSY, low = busy |
| 9 | TP8 | test pad |
| 10 | EPD_DC | panel data/command (low = command index) |
| 11 | EPD_RST | panel reset, shared by both CoGs |
| 12 | EPD_BUSY1 | left CoG (master) BUSY, low = busy |
| 13 | EPD_CS1 | left CoG chip select |
| 14 | BAT_SENS | supply sense: 10k/10k divider, reads VIN / 2 (ADC2 channel 3) |
| 15 | Button3 | printed **MENU -** (right corner, upper) |
| 16 | Button4 | printed **B** (right corner, lower) |
| 17 / 18 | SAO_SDA / SAO_SCL | I2C to all three SAO ports, 5.1k pull-ups |
| 19 / 20 | USB D- / D+ | USB-C data (native USB: USB-Serial-JTAG console or USB OTG host) |
| 21 | EPD_SCL | panel SPI clock |
| 38 | WS2812_data | LED bar (8 LEDs, chained) |
| 39 | Button1 | printed **A** (left corner, lower) |
| 40 | Button2 | printed **MENU +** (left corner, upper) |
| 41 | TP9 | test pad |
| 43 / 44 | TXD0 / RXD0 | UART0 test pads TP3 / TP4 |
| 47 | EPD_SDA | panel SPI data. Bidirectional (3-wire): the CoG's OTP is read back on it |
| 48 | EPD_PWR_ENABLE | gate of P-FET Q2 that switches the panel supply (VCC_EPD) from 3V3: **low = panel on** |
| EN | SW1 | RESET button (5.1k pull-up) |

GPIO45 is listed as MISO in the stock driver but is not connected; it is a strapping pin, leave it alone.

## Buttons
- Physical layout: two in each top corner, **MENU +** above **A** on the left, **MENU -** above **B** on the right.
- Active low, no external pull-ups on the user buttons: enable the internal pull-ups. Polling every 20 ms with
  edge detection is enough debouncing.
- The stock MicroPython names are swapped against the silkscreen ("plus/minus mislabeled on the silkscreen"):
  its `MENU_PLUS` is GPIO15 (printed MENU -) and `MENU_MINUS` is GPIO40 (printed MENU +). badge os uses the
  printed names: `BTN_PLUS` = GPIO40, `BTN_MINUS` = GPIO15.
- BOOT (GPIO0) is free to read at run time. RESET is the chip enable.

## Screen
### What it is
- E2969JS0B1, Spectra R2.0 film, operating range 0-40 C, typical stock refresh 31.2 s (PDi spec). The spec also
  asks for a refresh at least every 24 h and storage with a white image.
- Two CoGs behind one FPC each: the left (master, CS1/BUSY1) drives columns 0-479, the right (slave, CS2/BUSY2)
  columns 480-959, all 672 rows. Register writes go to both at once (both CS low).
- The CoG's high voltages come from an external booster (Q1, L1 47 uH, D2) driven by the master CoG (NGDRV).
- Two 1-bit RAM planes per CoG: black/white (index 0x10) and red (0x11). A pixel picks a waveform table by
  `(red << 1) | bw`: 0 white, 1 black, 2 red, 3 unused (both bits set: nothing drives it in the stock tables).
- On the wire: per row 60 bytes to the left CoG and 60 to the right, every byte bit-reversed (this "B1" panel;
  the older "084" variant is not reversed). badge os keeps planes MSB-left and reverses while sending.
- SPI mode 0, 20 MHz (PDi's maximum for this CoG family). One ESP32-S3 SPI transaction carries at most 32 KB: send
  in pieces with CS held low.

### Refresh modes (measured)
| Mode | Cost | Looks | Use for |
|---|---|---|---|
| **Clean full**: the panel's own OTP waveform | pull 33 s (BUSY 32.96 s), ~35 s total | true black/white/red, removes all ghosting and red | never in badge os (owner's rule) |
| **Fast full**: badge team table `lutb_base` at 0xB0, 3 pulls | ~3.5 s with the quick open | the whole panel flashes; black is grey-brown to maroon; no red | a clean-ish black/white screen |
| **Fast frame** in an open session: planes + 1 pull | 1.07 s (946 ms pull + ~120 ms transfer) | whole panel flashes each frame; ghosting builds up | badge os `ui_wipe` (one black, one white frame) |
| **Refresh window** (DRFW 0x90 y range) | ~490 ms pull for a y window | copies rows 0..y1 down the panel | never: keep DRFW `00 3B 00 A9` |
| **Calm partial** (`epd_partial`): film state in 0x11, new frame in 0x10, 20-frame transition table | ~0.48 s per frame, however much changed | only changed pixels move; ink dark red, erased ink leaves a light ghost | badge os `ui_frame` |

### Timing details
- A pull (`0x15 3C`) plays the latched waveform; its length follows the table: frames x ~23.7 ms. The badge team
  table is 40 frames = 949 ms. Doubling every count doubles the pull (1898 ms).
- **Never use a frame count of 0.** It hangs the CoG with BUSY low (seen for 180 s); only a reset recovers it.
  So 40 frames (~0.95 s) is the floor for a full-height pull.
- A DRFW window narrower in y roughly halves the pull (481-492 ms); a narrower x window changes nothing.
  DUW (0x13) + RAM_RW (0x12) window writes work, so a small change costs a few bytes of SPI instead of 4 x 40 KB.
- Session open (reset, `0x70`, planes, register sequence, `0xB0`, DC-DC soft start): 2443 ms with the PDi GU_large
  waits, **348 ms with the quick waits** (reset 2/4/20 ms, `0x05 7D` 50 ms, `0x05 00` 1 ms, load strobes 2/10 ms,
  `0xB0` 10 ms after the last strobe). badge os only uses the quick waits. Close (DC-DC soft shutdown): ~0.2 s.
- `0xB0` has one window: after the last `0xA7 00` load strobe, before the DC-DC soft start. Rewriting it inside an
  open session drops the panel back to its own 33 s waveform.
- Full plane transfer: 80 KB per plane; all 4 planes (2 per CoG) row by row took 301 ms at 8 MHz.
- `0x45` after `0x44 06` is the temperature input, `0x50 + 2 x degrees C` (0x82 = 25 C, 0x84 = 26 C). From 26 to
  50 C our fast frames stay at 23.7 ms; at 15 C they are 26.3 ms. It is not a speed lever. Values below 0x80 give
  the stock waveform 45.7 s.

### Ghosting: what causes it, how to live with it
- Short waveforms do not fully drive the particles: every fast frame leaves some of the previous picture behind,
  and anything that moves leaves a light trail.
- The red pigment moves at a lower drive than the black one: weak or long-but-chopped drives pull red along, so
  "black" from a fast table comes out brownish to dark red, worse further from the CoGs (bottom of the panel).
- Red already on the film (an old nametag) survives every fast waveform; only the clean full refresh removes it.
  badge os never runs it (owner's rule), so an old nametag's red can stay.
- Unbalanced drives (more VDH than VDL on average) push the whole panel one way: the white background turns pink
  within a few updates (badge team doc). Long runs of ground frames also left a faint red tint here.
- The full cure is the clean refresh (35 s), which badge os never runs. `ui_wipe` (fast black, then white) takes
  most ghosting off at scene changes and on every boot.

### Rules that keep the panel alive
- Send `0x70 71 36` to both CoGs after every reset: without it a B1 refresh draws about 2 A (badge team errata 9).
- Never send a command while BUSY is low; always end with the DC-DC soft shutdown (`0x09 7F`, `0x05 7D`, `0x09 00`).
- Do not cut power or reset in the middle of a refresh unless something is physically wrong.
- Do not drive the panel bus pins while the panel supply (GPIO48 / Q2) is off: it backfeeds the CoG through its
  ESD diodes, and the badge team broke a panel that way. All firmware so far leaves GPIO48 alone (panel on).
- Only GND / VDH / VDL / VDHR levels from the known tables; no level held for long; balanced shakes.
- Partial updates open a fresh session every 60 frames (sessions of 60 are proven on the badge; the badge team
  used 20 for its fast frames, reason not documented).

## LEDs
- 8x WS2812 on GPIO38, chained, powered from 3V3 (keep brightness low, ~12/255 is plenty). GRB order, 800 kHz:
  the Pong app drives them with the RMT peripheral at 10 MHz (0 = 0.3/0.9 us, 1 = 0.9/0.3 us).
- Status LED D6 on GPIO1. The charge LED D4 is driven by the charger, not the MCU.

## Power
- Battery: 1-cell LiPo on J1 (JST PH 2.0) or the battery pads. Charger MCP73831, 500 mA (PROG 2k).
- Power path: from USB through Schottky D5; from the battery through P-FET Q3 (on when VBUS is absent).
- The slide switch only enables the 3.3 V LDO (AP2112K, 600 mA); the battery still charges with the switch off.
- GPIO14 (BAT_SENS) reads half of the LDO input: the battery voltage on battery, about VBUS - 0.3 V on USB.
  It is on ADC2, which shares hardware with Wi-Fi: expect failed reads while the radio is busy.
  badge os turns it into a percentage with a typical LiPo rest-voltage curve (3.3 V = 0 %, 4.2 V = 100 %),
  and shows "USB" above 4.4 V (`battery_pct()` in main.c).

## USB
- USB-C wired to the ESP32-S3's native USB (no hub, no switch). Our firmware's console and esptool use the
  USB-Serial-JTAG device (VID 303A PID 1001); the stock MicroPython used TinyUSB (PID 4001).
- **Sink only:** nothing drives VBUS out. The badge can be a USB host (the Update app flashes another badge over a
  USB-C to USB-C cable), but both badges must then run on their own batteries.
- Download mode: firmware with USB-Serial-JTAG (ours) is reset into the ROM loader by esptool itself. The stock
  MicroPython ignores esptool's reset: hold BOOT and tap RESET, or run
  `mpremote exec "import machine; machine.mem32[0x6000812C] = 1; machine.reset()"`.
- Reading the log on macOS: opening the port raises DTR and RTS, and dropping DTR first is the download-mode reset.
  Open with DTR high and RTS low, then drop DTR (pyserial: `s.dtr = True; s.rts = False; s.open(); s.dtr = False`).

## Radio
- Ranging runs on every screen: Wi-Fi in APSTA mode on channel 1, a hidden SoftAP as the FTM responder, ESP-NOW
  beacons about once a second, power save off. FTM distances read right with no offset on our badges.
- Pong uses ESP-NOW on channel 1 with Wi-Fi in station mode and power save off (modem sleep drops packets).
  ~250-byte packets, broadcast for pairing, unicast after.
- Apps that join a network (Online update, Hackaday, News, Weather) stop ranging; the home screen restarts it.

## Flash
- 8 MB. badge os: bootloader at 0x0, partition table at 0x8000, NVS at 0x9000 (24 KB), otadata at 0xF000,
  two 3 MB app slots (ota_0 at 0x20000, ota_1 at 0x320000) for online updates with rollback: a new version that
  does not reach the home screen once is rolled back at the next boot. Coredump at 0x620000 (64 KB), the Badge
  app's picture at 0x630000 (128 KB, used by address: badges updated over the air keep their old partition table).
- The stock MicroPython firmware kept a LittleFS filesystem at 0x310000; it survives flashing a smaller app.
