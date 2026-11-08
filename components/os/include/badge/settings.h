// Settings, persisted across restarts and updates (NVS namespace "cfg"; the Settings app edits them from a phone).
// cfg_get copies the value into out (n bytes) and returns its length, or -1 if unset; "ssid" and "pass" fall back to
// the event Wi-Fi. cfg_set(key, NULL) deletes. Keys:
//   "name"        username, <= 12 chars: top bar, Radar, ranging beacons
//   "ssid" "pass" the Wi-Fi wifi_connect joins
//   "badge_text"  the Badge app's text (lines split by '\n'), also the Clock's name column
//   "tz"          Clock: a POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0"; unset = US Pacific
//   "wx_city"     Weather: a place name as typed, e.g. "Istanbul"; the app looks it up online (Open-Meteo geocoding)
//   "wx_lat" "wx_lon" Weather: decimal degrees as text; default Pasadena when unset
//   "wx_place"    Weather: the looked-up "Name, Country" for display
//   "wx_unit"     "F" = Fahrenheit and mph; anything else or unset = Celsius and km/h
//   "snd_sao"     sound port without a Cricket: "1"-"3", "0" = none; unset = SAO2 (badge/sound.h)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "badge/display.h"

int cfg_get(const char *key, char *out, int n);
bool cfg_set(const char *key, const char *val);

// The Badge app's picture: W x (H - TOP) pixels, 1 bit, same layout as fb (1 = black), IMG_BYTES bytes. Raw in
// flash at 0x630000 (by address: badges updated over the air keep their old partition table).
#define IMG_BYTES (W * (H - TOP) / 8)
bool cfg_get_image(uint8_t *img);       // false if none saved
bool cfg_set_image(const uint8_t *img); // NULL deletes it
