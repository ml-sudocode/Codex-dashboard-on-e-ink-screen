// Display: apps draw into fb, ui_frame shows it with one calm partial update (only changed pixels move), with the OS
// top bar drawn over rows 0..TOP-1. The panel itself is epd.c.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cricket.h"
#include "badge/badge.h"
#include "badge/os.h"
#include "epd.h"

EXT_RAM_BSS_ATTR uint8_t fb[PLANE]; // in PSRAM: internal RAM is needed for Wi-Fi and TLS (Online update)
static const char *bar_app = "Home"; // the name in the top bar
static const char *keys[4];           // ui_keys: what BTN_PLUS, BTN_MINUS, BTN_A, BTN_B do now
volatile bool in_app;                 // an app is running: MENU + and MENU - held together go home
static SemaphoreHandle_t panel_lock;  // recursive: one panel user at a time
static EXT_RAM_BSS_ATTR uint8_t part_next[PLANE]; // the frame ui_frame handed over
static volatile bool part_pending;    // ... and the panel task has not started it yet
static TaskHandle_t panel_task;

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

#define LOCK() xSemaphoreTakeRecursive(panel_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(panel_lock)

// The panel task starts every frame, then waits for the pull without the lock, so ui_frame can hand over the next
// frame meanwhile and the app draws while the panel pulls. While the screen sits still it does the 60-pull
// session reopen, so a frame does not pay for it.
static void panel_run(void *arg)
{
    for (;;) {
        bool idle = !ulTaskNotifyTake(pdTRUE, part_n >= 40 ? pdMS_TO_TICKS(500) : portMAX_DELAY);
        LOCK();
        if (part_pending) part_pending = false, part_start(part_next);
        else if (idle && part_n >= 40) epd_sleep(), part_open();
        UNLOCK();
        part_wait();
    }
}

// Hands the frame over and returns while the panel works; waits only while the previous frame has not started, so
// an app that calls it in a loop runs at the panel's pace with its drawing overlapping the pulls.
void ui_frame(int pulls)
{
    (void)pulls; // a partial update is one pull whatever was asked
    while (part_pending) ms(1);
    LOCK();
    draw_bar();
    memcpy(part_next, fb, PLANE);
    part_pending = true;
    UNLOCK();
    xTaskNotifyGive(panel_task);
}

// The frame handed over, if any, on the panel; nothing running. Under the lock.
static void panel_flush(void)
{
    part_wait();
    if (part_pending) part_pending = false, part_start(part_next), part_wait();
}

// Partial updates leave light ghosts of erased ink; a scene change shakes the film with the short black/white
// waveform (all black, then all white, ~2.8 s) and leaves the panel and the driver agreeing on white.
void ui_wipe(void)
{
    LOCK();
    part_pending = false; // drawn before the wipe: gone with it
    epd_sleep();
    memset(fb, 0xFF, sizeof fb);
    epd_open(fb);
    epd_pull(5000);
    epd_load(NULL); // both planes: after the pull 0x11 holds the black frame, which would select red
    epd_pull(5000);
    epd_close();
    memset(fb, 0, sizeof fb);
    memset(part_shown, 0, sizeof part_shown);
    UNLOCK();
}

void ui_close(void)
{
    LOCK();
    panel_flush();
    epd_sleep();
    UNLOCK();
}

// Every restart (going home, an app's esp_restart) finishes the panel's work and closes the session: never cut the
// panel off mid-refresh.
static void panel_shutdown(void)
{
    if (!xSemaphoreTakeRecursive(panel_lock, pdMS_TO_TICKS(3000))) return;
    panel_flush(); // a last frame handed over just before the restart ("Saved, restarting") still shows
    epd_sleep();
}

void os_app(const char *name)
{
    bar_app = name ? name : "Home";
    in_app = name != NULL;
}

void display_init(void)
{
    epd_init();
    panel_lock = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(panel_run, "panel", 4096, NULL, 5, &panel_task);
    esp_register_shutdown_handler(panel_shutdown);
}
