// Home screen: an iOS-style grid of app icons, 8 to a page. MENU + / MENU - move left / right, A or B opens the app.
// An app runs on this task; when it returns, the badge gets back what the app may have taken: USB (a restart if the
// app made it a host, so the badge stays flashable), sound, the radio (ranging) and the LEDs.
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "usb/usb_host.h"
#include "badge/badge.h"
#include "badge/os.h"
#include "home.h"

static const char *TAG = "os";

#define PITCH 224 // icon to icon
#define X0 56     // (960 - 4 * 176 - 3 * 48) / 2: four icons per row
#define Y0 80     // below the top bar
#define ROW 300

static void draw_home(int sel)
{
    char ver[48];
    snprintf(ver, sizeof ver, "badge os v%s", esp_app_get_description()->version);
    memset(fb, 0, sizeof fb);
    int page = sel / 8, pages = (napps + 7) / 8; // 8 icons a page; moving past the last one turns the page
    for (int i = page * 8; i < MIN(napps, page * 8 + 8); i++) {
        int x = X0 + i % 4 * PITCH, y = Y0 + i % 8 / 4 * ROW;
        apps[i]->icon(x, y);
        text(x + ICON / 2 - (int)strlen(apps[i]->name) * 12, y + ICON + 24, 3, apps[i]->name, true);
        if (i == sel) outline(x - 16, y - 16, ICON + 32, ICON + 32, 8);
    }
    text(W - (int)strlen(ver) * 24 - 24, H - 48, 3, ver, true);
    for (int p = 0; pages > 1 && p < pages; p++) { // page dots, bottom centre: the current one filled
        int x = W / 2 - pages * 16 + p * 32;
        if (p == page) box(x, H - 44, 16, 16, true);
        else outline(x, H - 44, 16, 16, 3);
    }
}

void home(void)
{
    ui_keys("< Left", "Right >", "Open", "Open");
    btn_flush();
    for (int sel = 0, first = 1;;) {
        draw_home(sel);
        ui_frame(1);
        if (first) esp_ota_mark_app_valid_cancel_rollback(), first = 0; // this version boots: keep it (rollback)
        int b = btn_get(10000), open = 0;
        if (b < 0) { // the screen sits still: end the panel session, then wait as long as it takes
            ui_close();
            b = btn_get(-1);
        }
        // Every press counts at once, also those made during the last refresh; the next frame shows where they led.
        for (; b >= 0 && !open; b = btn_get(0)) {
            ESP_LOGI(TAG, "home: button %d on %s", b, apps[sel]->name);
            if (b == BTN_PLUS) sel = (sel + napps - 1) % napps; // MENU + is the left one: move left
            else if (b == BTN_MINUS) sel = (sel + 1) % napps; // MENU - is the right one: move right
            else open = 1;
        }
        if (!open) continue;
        ESP_LOGI(TAG, "opening %s", apps[sel]->name);
        ui_wipe(); // the home screen would ghost through the app's first frame
        ui_close();
        btn_flush();
        os_app(apps[sel]->name);
        ui_keys(NULL, NULL, NULL, NULL);
        apps[sel]->run();
        os_app(NULL);
        ui_keys("< Left", "Right >", "Open", "Open");
        usb_host_lib_info_t usb;
        if (usb_host_lib_info(&usb) == ESP_OK) esp_restart(); // the app made USB a host: home must stay flashable
        snd_stop();
        if (!ranging_running()) wifi_off(), ranging_start(); // the app took the radio: ranging gets it back
        leds((const uint8_t[LEDS][3]){{0}});
        ui_wipe(); // and the app through the home screen
        btn_flush();
    }
}
