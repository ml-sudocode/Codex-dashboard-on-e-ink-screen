// badge os: what main.c gives the apps.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define W 960
#define H 672
#define PLANE (W * H / 8) // MONO_HLSB, MSB = leftmost pixel, 1 = black

extern uint8_t fb[PLANE]; // what the next ui_frame() shows
void ms(int t);           // sleep at least t ms

void box(int x, int y, int w, int h, bool ink); // filled rectangle, black or white
void outline(int x, int y, int w, int h, int t); // black frame, t px thick
void text(int x, int y, int scale, const char *s, bool ink); // 8x8 font, 8 * scale px per character

// Panel: ui_frame shows fb with one calm partial update, ~0.48 s however much changed (~2 frames/s is the hardware
// floor). Only pixels that changed are driven, nothing else flashes. Ink comes out dark red, erased ink leaves a light
// ghost. pulls is ignored (kept so old apps build). No change since the last frame = no refresh.
// Call ui_close() when the screen will sit still for a while.
void ui_frame(int pulls);
void ui_close(void);
void ui_wipe(void); // scene change: an all-black then an all-white frame shake the ghosts off (~2.8 s), fb all white

// Buttons as printed on the badge, a 2x2 block: MENU + top left, MENU - top right, A bottom left, B bottom right.
enum { BTN_PLUS, BTN_MINUS, BTN_A, BTN_B };
int btn_get(int timeout_ms); // next press, -1 on timeout; timeout -1 waits forever
void btn_flush(void);
int btn_held(void); // buttons held right now: bit (1 << BTN_x) set while pressed, updated every 20 ms

// Wi-Fi is off unless an app turns it on; the home screen turns it off again when an app returns.
void wifi_on(void);                 // radio on, station mode, no network joined (ESP-NOW apps: set the channel next)
bool wifi_connect(int timeout_ms); // wifi_on + join the badge Wi-Fi (main/wifi_secret.h); true once there is an IP
void wifi_off(void);                // radio off
int knob_angle(void); // AS5600 knob add-on on an SAO port: 0-4095 per turn, -1 if none (plug in any time)
int battery_pct(void); // 0-100 on battery, -1 on USB power, -2 unknown (ADC2: fails while Wi-Fi is busy)

// Apps: run() takes over the badge. Returning goes back to the home screen; most apps never return.
void app_update(void);
void app_pong(void);
void app_ota(void);   // not on the home screen: the Update app offers it next to the wired update
void app_snake(void);
