// badge os: boot, then the home screen. The OS services are components/os, the apps apps/<name>.
#include "esp_err.h"
#include "nvs_flash.h"
#include "badge/badge.h"
#include "badge/os.h"
#include "home.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init(); // Wi-Fi (ESP-NOW) keeps its calibration here
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    display_init();
    sao_init();
    buttons_init();
    leds_init(); // off: they still show whatever ran before the reset

    ui_wipe(); // the film still shows whatever ran before the reset; frames need a known start
    battery_pct(); // one reading before Wi-Fi starts sharing ADC2
    ranging_start(); // on every screen, so other badges' Radar sees this one; apps that take the radio stop it
    home();
}
