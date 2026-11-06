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

static void screen(const char *l1, const char *l2, int p) // p < 0: no progress bar
{
    memset(fb, 0, sizeof fb);
    text(48, 48, 6, "Online update", true);
    text(48, 200, 4, l1, true);
    if (l2) text(48, 260, 3, l2, true);
    if (p >= 0) {
        outline(96, 340, 768, 96, 6);
        box(108, 352, 744 * p / 100, 72, true);
    }
}

static void done(const char *l1, const char *l2) // final message, then home on any button
{
    screen(l1, l2, -1);
    text(48, 560, 3, "Any button goes home.", true);
    ui_frame(3);
    ui_close();
    btn_flush();
    btn_get(-1);
}

static void run(void)
{
    const char *ours = esp_app_get_description()->version;
    char s[64];
    snprintf(s, sizeof s, "This badge: v%s", ours);
    screen("Checking for updates...", s, -1);
    ui_frame(3);

    if (!wifi_connect(20000)) return done("No Wi-Fi.", "Could not join the badge network.");

    esp_http_client_config_t http = {
        .url = URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .buffer_size = 4096, // GitHub redirects to a very long objects URL
        .buffer_size_tx = 4096,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_handle_t h;
    status = 0;
    esp_err_t err = esp_https_ota_begin(&(esp_https_ota_config_t){.http_config = &http}, &h);
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

    // Download: one frame per 10 %, the download waits during each ~1.07 s pull.
    char l1[96];
    snprintf(l1, sizeof l1, "Updating v%s -> v%s", ours, img.version);
    int size = esp_https_ota_get_image_size(h), shown = -10;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int p = size > 0 ? (long long)esp_https_ota_get_image_len_read(h) * 100 / size : 0;
        if (p >= shown + 10) {
            snprintf(s, sizeof s, "%d%%", p);
            screen(l1, s, p);
            ui_frame(1);
            shown = p;
        }
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) err = esp_https_ota_finish(h);
    else {
        if (err == ESP_OK) err = ESP_FAIL;
        esp_https_ota_abort(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE("ota", "download: %s", esp_err_to_name(err));
        return done("Update failed.", esp_err_to_name(err));
    }

    snprintf(s, sizeof s, "Installed v%s. Restarting.", img.version);
    screen("Done.", s, 100);
    ui_frame(1);
    ui_close();
    ms(2000);
    esp_restart();
}

// The app task's stack (3.5 KB) is far too small for TLS: run on our own task and wait for it.
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
