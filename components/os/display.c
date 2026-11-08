// Display: apps draw into fb, ui_frame shows it with one calm whole-screen update and the OS top bar drawn over rows
// 0..TOP-1. The panel itself is epd.c.
//
// A frame: wait for the previous pull, send the whole frame, start its pull, return. So ui_frame blocks only until
// the previous pull is done (~0.24 s after it started), plus ~35 ms of SPI: ~3.5-4 frames/s, with the app drawing
// the next frame meanwhile. The session (DC-DC up, the calm table latched) stays open between frames; ui_close ends
// it.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cricket.h"
#include "badge/badge.h"
#include "badge/os.h"
#include "epd.h"

static const char *TAG = "os";
EXT_RAM_BSS_ATTR uint8_t fb[PLANE]; // in PSRAM: internal RAM is needed for Wi-Fi and TLS (Online update)
static EXT_RAM_BSS_ATTR uint8_t shown[PLANE]; // what the film shows; ui_wipe leaves both all white
static int pulls = -1;               // pulls in the open session, -1 = no session
static bool pulling;                 // a pull may still run: epd_wait before the next command
static SemaphoreHandle_t lock;       // one panel user at a time: apps draw from more than one task
static const char *bar_app = "Home"; // the name in the top bar
static const char *keys[4];          // ui_keys: what BTN_PLUS, BTN_MINUS, BTN_A, BTN_B do now
volatile bool in_app;                // an app is running: MENU + and MENU - held together go home

// Top bar (rows 0..TOP-1) on every screen, drawn over fb by ui_frame. The buttons sit two in each top corner,
// MENU + above A on the left, MENU - above B on the right: each label (ui_keys) sits beside its button, with a tab at
// the edge pointing at it. In the middle: the app's name, then the username, OS version and battery.
static void draw_bar(void)
{
    static const int ky[4] = {4, 4, 26, 26}; // label rows of BTN_PLUS, BTN_MINUS, BTN_A, BTN_B
    static int pct = -2;
    static int64_t next;
    static char name[13];
    if (esp_timer_get_time() >= next) { // every 30 s, so the bar sits still
        pct = battery_pct();
        cfg_get("name", name, sizeof name);
        next = esp_timer_get_time() + 30000000;
    }
    char mid[64], bat[16] = "USB";
    int n = snprintf(mid, sizeof mid, "%s%sv%s", name, *name ? "  " : "", esp_app_get_description()->version);
    if (pct >= 0) snprintf(bat, sizeof bat, "%d%%", pct);
    bool spk = !in_app && cricket_version() >= 1; // home screen: a speaker Cricket is plugged in
    int x = (W - n * 16 - (pct == -2 ? 0 : 32 + 38 + (int)strlen(bat) * 16) - (spk ? 40 : 0)) / 2; // text, battery, speaker
    box(0, 0, W, TOP, false);
    for (int k = 0; k < 4; k++) {
        if (!keys[k] || !*keys[k]) continue;
        bool left = k == BTN_PLUS || k == BTN_A;
        box(left ? 0 : W - 6, ky[k], 6, 16, true);
        text(left ? 14 : W - 14 - (int)strlen(keys[k]) * 16, ky[k], 2, keys[k], true);
    }
    text((W - (int)strlen(bar_app) * 16) / 2, 4, 2, bar_app, true);
    text(x, 26, 2, mid, true);
    x += n * 16;
    if (pct != -2) { // a battery outline filled to the charge (full on USB), then "87%" or "USB"
        x += 32;
        outline(x, 27, 30, 14, 2);
        box(x + 30, 31, 3, 6, true);
        box(x + 4, 31, pct < 0 ? 22 : 22 * pct / 100, 6, true);
        text(x + 38, 26, 2, bat, true);
        x += 38 + (int)strlen(bat) * 16;
    }
    if (spk) { // a speaker: body, cone, one sound wave
        x += 18;
        box(x, 31, 6, 8, true);
        for (int k = 0; k < 8; k++) box(x + 6 + k, 31 - k / 2, 1, 8 + k, true);
        box(x + 18, 29, 2, 12, true);
    }
    box(0, TOP - 4, W, 2, true);
}

void ui_keys(const char *plus, const char *minus, const char *a, const char *b)
{
    keys[BTN_PLUS] = plus, keys[BTN_MINUS] = minus, keys[BTN_A] = a, keys[BTN_B] = b;
}

// The pull in flight done, the session closed. Under the lock.
static void panel_off(void)
{
    if (pulling) epd_wait(), pulling = false;
    if (pulls >= 0) epd_close();
    pulls = -1;
}

void ui_frame(int unused)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    draw_bar();
    if (memcmp(fb, shown, PLANE)) { // no change, no refresh
        int64_t t0 = esp_timer_get_time();
        if (pulling) epd_wait(), pulling = false;
        int64_t t1 = esp_timer_get_time();
        if (pulls == 60) panel_off(); // a fresh session every 60 pulls: sessions of 60 are proven, longer ones are not
        if (pulls < 0) {
            epd_reset();
            epd_start(EPD_CALM);
            epd_load(shown, shown); // 0x11 must hold what the film shows
            pulls = 0;
        }
        epd_frame(fb);
        memcpy(shown, fb, PLANE);
        pulls++, pulling = true;
        ESP_LOGD(TAG, "frame %d: %d ms waiting for the panel, %d ms sending", pulls, (int)((t1 - t0) / 1000),
                 (int)((esp_timer_get_time() - t1) / 1000));
    }
    xSemaphoreGive(lock);
}

// Frames leave light ghosts of erased ink; a scene change shakes the film with the short black/white waveform (all
// black, then all white, ~2.8 s) and leaves the panel and the driver agreeing on white.
void ui_wipe(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    panel_off();
    memset(fb, 0xFF, sizeof fb);
    epd_reset();
    epd_load(fb, NULL);
    epd_start(EPD_FAST);
    epd_pull(5000);
    epd_load(NULL, NULL); // both planes: after the pull 0x11 holds the black frame, which would select red
    epd_pull(5000);
    epd_close();
    memset(fb, 0, sizeof fb);
    memset(shown, 0, sizeof shown);
    xSemaphoreGive(lock);
}

void ui_close(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    panel_off();
    xSemaphoreGive(lock);
}

// Every restart (going home, an app's esp_restart) lets the pull in flight finish and closes the session: never cut
// the panel off mid-refresh. The lock stays taken: no frame after this one.
static void panel_shutdown(void)
{
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(3000))) panel_off();
}

void os_app(const char *name)
{
    bar_app = name ? name : "Home";
    in_app = name != NULL;
}

void display_init(void)
{
    epd_init();
    lock = xSemaphoreCreateMutex();
    esp_register_shutdown_handler(panel_shutdown);
}
