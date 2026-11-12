// Minimal host doubles. These exercise app behavior, not ESP-IDF API compatibility.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define W 960
#define H 672
#define TOP 48
#define PLANE (W * H / 8)
#define LEDS 8
extern uint8_t fb[PLANE];
enum { BTN_PLUS, BTN_MINUS, BTN_A, BTN_B };
typedef struct { const char *name; void (*icon)(int, int); void (*run)(void); } app_t;
void box(int x, int y, int w, int h, bool ink);
void text(int x, int y, int scale, const char *s, bool ink);
void icon_base(int x, int y);
void ui_keys(const char *plus, const char *minus, const char *a, const char *b);
void ui_frame(int pulls);
void ui_close(void);
void leds(const uint8_t rgb[LEDS][3]);
void ms(int t);
int btn_get(int timeout_ms);
int cfg_get(const char *key, char *out, int n);
bool cfg_set(const char *key, const char *val);
bool wifi_connect(int timeout_ms);
void wifi_off(void);
