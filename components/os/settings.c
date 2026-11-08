// Settings (the Settings app writes them): short strings in NVS namespace "cfg"; the Badge app's picture raw in flash
// at IMG_ADDR, right after the coredump partition. partitions.csv lists it as "img", but badges updated over the air
// keep their old partition table, so the code uses the address, not the table.
#include <stdio.h>
#include <string.h>
#include "esp_flash.h"
#include "nvs.h"
#include "badge/badge.h"

#if __has_include("../../main/wifi_secret.h")
#include "../../main/wifi_secret.h" // WIFI_SSID / WIFI_PASS of the event Wi-Fi, gitignored; the release workflow writes it
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

#define IMG_ADDR 0x630000
#define IMG_MAGIC 0x31474D49 // "IMG1", written last: a cut-off write reads as no picture

int cfg_get(const char *key, char *out, int n)
{
    nvs_handle_t h;
    size_t len = n;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_str(h, key, out, &len);
        nvs_close(h);
        if (e == ESP_OK) return strlen(out);
    }
    const char *def = !strcmp(key, "ssid") ? WIFI_SSID : !strcmp(key, "pass") ? WIFI_PASS : NULL; // event Wi-Fi
    if (n) *out = 0;
    if (!def) return -1;
    snprintf(out, n, "%s", def);
    return strlen(out);
}

bool cfg_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = val ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool cfg_get_image(uint8_t *img)
{
    uint32_t magic = 0;
    return esp_flash_read(NULL, &magic, IMG_ADDR, 4) == ESP_OK && magic == IMG_MAGIC &&
           esp_flash_read(NULL, img, IMG_ADDR + 4, IMG_BYTES) == ESP_OK;
}

bool cfg_set_image(const uint8_t *img)
{
    if (esp_flash_erase_region(NULL, IMG_ADDR, (4 + IMG_BYTES + 4095) & ~4095) != ESP_OK) return false;
    uint32_t magic = IMG_MAGIC;
    return !img || (esp_flash_write(NULL, img, IMG_ADDR + 4, IMG_BYTES) == ESP_OK &&
                    esp_flash_write(NULL, &magic, IMG_ADDR, 4) == ESP_OK);
}
