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

// Panel: fast black/white refresh, ~1.07 s per pull, the whole panel flashes. Frames share one panel session;
// call ui_close() when the screen will sit still for a while.
void ui_frame(int pulls);
void ui_close(void);

// Buttons as printed on the badge, a 2x2 block: MENU + top left, MENU - top right, A bottom left, B bottom right.
enum { BTN_PLUS, BTN_MINUS, BTN_A, BTN_B };
int btn_get(int timeout_ms); // next press, -1 on timeout; timeout -1 waits forever
void btn_flush(void);
int btn_held(void); // buttons held right now: bit (1 << BTN_x) set while pressed, updated every 20 ms

int battery_pct(void); // 0-100 on battery, -1 on USB power, -2 unknown (ADC2: fails while Wi-Fi is busy)

// Apps: run() takes over the badge. Returning goes back to the home screen; most apps never return.
void app_update(void);
void app_pong(void);
