// badge os: what main.c gives the apps.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define W 960
#define H 672
#define PLANE (W * H / 8) // MONO_HLSB, MSB = leftmost pixel, 1 = black
#define TOP 48 // rows 0..TOP-1 are the OS top bar in apps (name, version, battery): ui_frame draws over them

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
// What each button does now, shown beside it in the top bar (two buttons in each top corner: MENU + above A on the
// left, MENU - above B on the right). Up to ~16 characters each; NULL or "" = no label. Set it again when it changes;
// it shows from the next ui_frame. Every app starts with no labels.
void ui_keys(const char *plus, const char *minus, const char *a, const char *b);
void ui_wipe(void); // scene change: an all-black then an all-white frame shake the ghosts off (~2.8 s), fb all white

// Buttons as printed on the badge, two in each top corner: MENU + above A on the left, MENU - above B on the right.
// MENU + and MENU - held together for 0.4 s in any app go home (the OS restarts the badge between frames).
enum { BTN_PLUS, BTN_MINUS, BTN_A, BTN_B };
int btn_get(int timeout_ms); // next press, -1 on timeout; timeout -1 waits forever
void btn_flush(void);
int btn_held(void); // buttons held right now: bit (1 << BTN_x) set while pressed, updated every 20 ms

// The OS runs ranging (below) on every screen. An app that calls wifi_on or wifi_connect takes the radio over:
// ranging stops until the app returns home or the badge restarts.
void wifi_on(void);                 // radio on, station mode, no network joined (ESP-NOW apps: set the channel next)
bool wifi_connect(int timeout_ms); // wifi_on + join the badge Wi-Fi (main/wifi_secret.h); true once there is an IP
void wifi_off(void);                // radio off
// Ranging (Radar and any app): badges running badge os find each other (ESP-NOW beacons, channel 1) and measure
// their distance with Wi-Fi FTM; each badge shares its own ranges, so you also get the distances between others.
// The OS starts it at boot and keeps it on (APSTA, hidden SoftAP as the FTM responder, ESP-NOW on channel 1), so
// apps just call ranging_get. ranging_start/ranging_stop exist for apps that stopped it.
// Don't use ESP-NOW yourself while it runs; wifi_on/wifi_connect stop it first.
typedef struct {
    uint8_t a[6], b[6]; // a measured b; a = ranging_me() for this badge's own ranges
    int16_t cm;         // distance, -1 = b heard but not measured yet; FTM: ~1-2 m typical error, 5-10 m outliers
    uint32_t age_ms;    // age of cm (or of the last beacon while cm is -1)
} range_t;
void ranging_start(void);
void ranging_stop(void);
int ranging_get(range_t *out, int max); // pairs heard of in the last 30 s; returns how many it wrote
void ranging_me(uint8_t mac[6]);        // this badge's id: its Wi-Fi station MAC (what other badges list it as)
int knob_angle(void); // AS5600 knob add-on on an SAO port: 0-4095 per turn, -1 if none or no magnet (plug in any time)
int battery_pct(void); // 0-100 on battery, -1 on USB power, -2 unknown (ADC2: fails while Wi-Fi is busy)

// Apps: run() takes over the badge. Returning goes back to the home screen; most apps never return.
void app_update(void);
void app_pong(void);
void app_ota(void);   // not on the home screen: the Update app offers it next to the wired update
void app_snake(void);
void app_life(void);
void app_aliens(void);
void app_blocks(void);
void app_rocks(void);
void app_radar(void);
void app_source(void);
void app_hackaday(void);
