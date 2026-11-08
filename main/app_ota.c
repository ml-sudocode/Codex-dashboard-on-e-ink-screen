// Online update: fetches badge_os.bin from the repo's latest GitHub release and installs it into the other OTA slot
// if its version is newer than ours. The new version is kept once it reaches the home screen (rollback, main.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "badge.h"

#define URL "https://github.com/yigitabi5444/supercon26-badge-os/releases/latest/download/badge_os.bin"

static int status; // HTTP status of the last response (after GitHub's redirects)

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE) status = esp_http_client_get_status_code(e->client);
    return ESP_OK;
}

// "0.10" > "0.9": compare dot-separated numbers one by one.
static bool newer(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x > y;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
        if (ea == a && eb == b) break; // ponytail: non-numeric tails ("-rc1") compare equal
    }
    return false;
}

// The screen is a log: every message goes on the next empty line, never text over text (erased ink ghosts).
// A draw task owns the panel and shows the latest log, so the network never waits for a refresh.
static struct {
    char s[64];
    int scale;
} lines[8];
static volatile int nlines, pct = -1, want; // pct < 0: no bar
static volatile bool stop;

static void draw_task(void *caller)
{
    ui_wipe(); // we come right after the Update chooser; runs while Wi-Fi joins
    for (;;) {
        int g = want;
        memset(fb, 0, sizeof fb);
        for (int i = 0, y = TOP + 24; i < nlines; i++) {
            text(48, y, lines[i].scale, lines[i].s, true);
            y += 8 * lines[i].scale + 24;
        }
        if (pct >= 0) { // at the bottom: frames that change only low rows are the quickest
            outline(48, H - 112, 864, 64, 6);
            box(60, H - 100, 840 * pct / 100, 40, true); // only grows: ink is added, never erased
        }
        ui_frame(1);
        if (stop && g == want) break;
        if (want == g) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    ui_close();
    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

static TaskHandle_t drawer;

static void redraw(void)
{
    want++;
    xTaskNotifyGive(drawer);
}

static void say(int scale, const char *l)
{
    if (nlines == sizeof lines / sizeof *lines) return;
    snprintf(lines[nlines].s, sizeof lines[0].s, "%s", l);
    lines[nlines].scale = scale;
    nlines++;
    redraw();
}

static void finish_screen(void) // show the last lines, then the draw task ends
{
    stop = true;
    redraw();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void done(const char *l1, const char *l2) // final message, then back on any button
{
    ui_keys("Back", "Back", "Back", "Back");
    say(4, l1);
    if (l2) say(3, l2);
    finish_screen();
    btn_flush();
    btn_get(-1);
}

static int64_t t0;

static void lap(const char *what) // timings on the log, to see where the time goes
{
    int64_t t = esp_timer_get_time();
    ESP_LOGI("ota", "%s: %lld ms", what, (t - t0) / 1000);
    t0 = t;
}

static void run(void)
{
    const char *ours = esp_app_get_description()->version;
    char s[64];
    nlines = want = 0;
    pct = -1;
    stop = false;
    xTaskCreate(draw_task, "ota draw", 4096, xTaskGetCurrentTaskHandle(), 5, &drawer);
    say(6, "Online update");
    snprintf(s, sizeof s, "This badge: v%s", ours);
    say(3, s);
    say(4, "Checking for updates...");

    t0 = esp_timer_get_time();
    bool joined = wifi_connect(20000);
    lap("wifi_connect");
    if (!joined) return done("No Wi-Fi.", "Could not join the badge network.");

    esp_http_client_config_t http = {
        .url = URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .buffer_size = 16384, // big reads for the download; also holds GitHub's very long redirect URL
        .buffer_size_tx = 4096,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_handle_t h;
    status = 0;
    esp_err_t err = esp_https_ota_begin(&(esp_https_ota_config_t){.http_config = &http}, &h);
    lap("esp_https_ota_begin");
    if (err != ESP_OK) {
        ESP_LOGE("ota", "begin: %s, HTTP %d", esp_err_to_name(err), status);
        if (status == 404) return done("No release yet.", "Nothing to install.");
        snprintf(s, sizeof s, "%s, HTTP %d", esp_err_to_name(err), status);
        return done("Could not reach GitHub.", s);
    }

    esp_app_desc_t img;
    err = esp_https_ota_get_img_desc(h, &img);
    if (err != ESP_OK || !newer(img.version, ours)) {
        esp_https_ota_abort(h);
        if (err != ESP_OK) return done("Bad download.", esp_err_to_name(err));
        snprintf(s, sizeof s, "v%s is the latest.", ours);
        return done("Badge is up to date.", s);
    }

    snprintf(s, sizeof s, "Updating to v%s", img.version);
    say(4, s);
    pct = 0;
    int size = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int p = size > 0 ? (long long)esp_https_ota_get_image_len_read(h) * 100 / size : 0;
        if (p != pct) {
            pct = p;
            redraw();
        }
    }
    snprintf(s, sizeof s, "download (%d bytes)", esp_https_ota_get_image_len_read(h));
    lap(s);
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) err = esp_https_ota_finish(h);
    else {
        if (err == ESP_OK) err = ESP_FAIL;
        esp_https_ota_abort(h);
    }
    lap("finish");
    if (err != ESP_OK) {
        ESP_LOGE("ota", "download: %s", esp_err_to_name(err));
        return done("Update failed.", esp_err_to_name(err));
    }

    pct = 100;
    say(4, "Installed. Restarting.");
    finish_screen();
    esp_restart();
}

static void task(void *caller)
{
    run();
    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

void app_ota(void)
{
    xTaskCreate(task, "ota", 16384, xTaskGetCurrentTaskHandle(), 4, NULL);
    ulTaskNotifyTake(true, portMAX_DELAY);
}
