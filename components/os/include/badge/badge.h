// badge os: everything the OS gives the apps. Include this one header; each part is documented in its own:
//   display.h  fb, ui_frame, ui_close, ui_wipe, ui_keys     draw.h     box, outline, text
//   buttons.h  btn_get, btn_flush, btn_held                  wifi.h     wifi_on, wifi_connect, wifi_off
//   ranging.h  badges nearby and their distances             settings.h cfg_get, cfg_set, the Badge picture
//   sound.h    snd_start, snd_write, snd_stop                leds.h     the LED bar
//   knob.h     the AS5600 knob add-on                        power.h    battery_pct
//   app.h      app_t, the home screen's icons
// How to write an app: apps/README.md.
#pragma once
#include "badge/app.h"
#include "badge/buttons.h"
#include "badge/display.h"
#include "badge/draw.h"
#include "badge/knob.h"
#include "badge/leds.h"
#include "badge/power.h"
#include "badge/ranging.h"
#include "badge/settings.h"
#include "badge/sound.h"
#include "badge/wifi.h"

void ms(int t); // sleep at least t ms
