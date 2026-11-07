// Wired update: this badge turns its USB-C port into a USB host, waits for another badge on a USB-C cable and copies
// its own firmware (bootloader, partition table, running app) onto it, then resets it. Core from idf_port clone.c.
// Both badges must run on their batteries: the USB-C port is sink-only and cannot power the other badge.
// A target on our firmware drops into its ROM loader by itself; one on the stock MicroPython needs BOOT held while
// RESET is tapped. After a good update this badge restarts.
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp32_usb_cdc_acm_port.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "badge.h"

static volatile int pct;   // -1 waiting for a badge, 0-100 copying, 101 done, -2 failed, -3 stopped
static volatile bool stop; // the app asks the waiting task to give up

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// Our own flash [src, src + len) to dst on the target; flash_finish checks the MD5.
static bool copy(esp_loader_t *ld, uint32_t src, uint32_t dst, uint32_t len, uint32_t *done, uint32_t total)
{
    static uint8_t buf[1024];
    esp_loader_flash_cfg_t cfg = {.offset = dst, .image_size = len, .block_size = sizeof buf};
    if (esp_loader_flash_start(ld, &cfg) != ESP_LOADER_SUCCESS) return false;
    for (uint32_t off = 0; off < len; off += sizeof buf) {
        uint32_t n = MIN(sizeof buf, len - off);
        if (esp_flash_read(NULL, buf, src + off, n) != ESP_OK) return false;
        if (esp_loader_flash_write(ld, &cfg, buf, n) != ESP_LOADER_SUCCESS) return false;
        *done += n;
        pct = MIN((uint64_t)*done * 100 / total, 100);
    }
    return esp_loader_flash_finish(ld, &cfg) == ESP_LOADER_SUCCESS;
}

// One try: wait for a badge (until stop), copy, report in pct.
static void update_task(void *arg)
{
    static esp32_usb_cdc_acm_port_t port; // outlives the loader; VID/PID 0 = any CDC device
    port = (esp32_usb_cdc_acm_port_t){.port.ops = &esp32_usb_cdc_acm_ops, .connection_timeout_ms = 1000,
                                      .out_buffer_size = 4096};
    const esp_partition_t *app = esp_ota_get_running_partition();
    esp_partition_pos_t pos = {.offset = app->address, .size = app->size};
    esp_image_metadata_t img;
    bool ok = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &img) == ESP_OK;

    esp_loader_t ld;
    while (ok && esp_loader_init_serial(&ld, &port.port) != ESP_LOADER_SUCCESS) // fails after 1 s without a device
        if (stop) {
            pct = -3;
            vTaskDelete(NULL);
        }
    uint32_t done = 0, total = 0x8000 + 0x1000 + img.image_len;
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    ok = ok && esp_loader_connect(&ld, &args) == ESP_LOADER_SUCCESS;
    if (ok) pct = 0;
    // Our app goes to the target's ota_0 whichever slot runs here; erased otadata makes the target boot ota_0.
    ok = ok && copy(&ld, 0x0, 0x0, 0x8000, &done, total) &&         // bootloader
         copy(&ld, 0x8000, 0x8000, 0x1000, &done, total) &&        // partition table
         copy(&ld, app->address, 0x20000, img.image_len, &done, total) && // app -> ota_0
         esp_loader_flash_erase_region(&ld, 0xF000, 0x2000) == ESP_LOADER_SUCCESS; // otadata
    // On success the target resets and drops off USB; the port's disconnect handler (USB host task) closes the
    // device then, and a deinit here would race it. So no deinit: the app restarts after "Done".
    if (ok) esp_loader_reset_target(&ld);
    else esp_loader_deinit(&ld);
    ESP_LOGI("update", "%s, %u bytes", ok ? "done" : "failed", (unsigned)done);
    pct = ok ? 101 : -2;
    vTaskDelete(NULL);
}

static void message(const char *s)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 24, 6, "Wired update", true);
    text(48, 300, 6, s, true);
    ui_frame(3);
    ui_close();
}

static void wired(void)
{
    // usb_host_install takes the PHY from USB-Serial-JTAG until reboot, so this app never returns: it restarts.
    usb_host_install(&(usb_host_config_t){.intr_flags = ESP_INTR_FLAG_LEVEL1});
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 20, NULL);
    cdc_acm_host_install(NULL);
    for (;;) {
        pct = -1;
        stop = false;
        xTaskCreate(update_task, "update", 8192, NULL, 4, NULL);

        memset(fb, 0, sizeof fb);
        text(48, TOP + 24, 6, "Wired update", true);
        text(48, 200, 3, "Connect another badge with a", true);
        text(48, 240, 3, "USB-C cable. Both on battery.", true);
        text(48, 320, 3, "Badge on stock MicroPython:", true);
        text(48, 360, 3, "hold BOOT, tap RESET.", true);
        ui_keys("Home", "Home", "Home", "Home");
        ui_frame(3);
        ui_close();
        while (pct == -1)
            if (btn_get(200) >= 0) stop = true;
        if (pct == -3) esp_restart(); // home, with the badge back on the Mac

        ui_keys(NULL, NULL, NULL, NULL);
        // Copying: redraw whenever the number moved; each frame takes ~0.5 s while the copy task keeps going.
        for (int shown = -1, pulls = 3; pct >= 0 && pct <= 100;) {
            int p = pct;
            if (p == shown) {
                ms(100);
                continue;
            }
            char s[8];
            snprintf(s, sizeof s, "%d%%", p);
            memset(fb, 0, sizeof fb);
            // Only changed pixels move: the bar only grows, so just the number leaves a light ghost as it changes.
            outline(48, TOP + 24, 720, 96, 6);
            box(60, TOP + 36, 696 * p / 100, 72, true);
            text(800, TOP + 56, 4, s, true);
            text(48, 300, 4, "Updating connected badge", true);
            text(48, 560, 3, "Wired update", true);
            ui_frame(pulls);
            pulls = 1;
            shown = p;
        }
        ui_close();
        if (pct == 101) {
            message("Done");
            ms(3000);
            esp_restart(); // boots to the home screen, USB back to USB-Serial-JTAG
        }
        message("Update failed"); // then keep scanning: unplug, fix, plug again
        ms(3000);
    }
}

// Chooser: online update (app_ota.c) or the wired update above. Same keys as the home screen.
void app_update(void)
{
    static const char *name[2] = {"Online update", "Flash another badge"};
    for (int sel = 0, pulls = 3;;) {
        memset(fb, 0, sizeof fb);
        text(48, TOP + 24, 6, "Update", true);
        for (int i = 0; i < 2; i++) {
            text(112, 240 + i * 200, 5, name[i], true);
            if (i == sel) outline(80, 208 + i * 200, 800, 104, 8);
        }
        ui_keys("Up", "Down", "Pick", "Pick");
        ui_frame(pulls);
        pulls = 1;
        int b = btn_get(10000);
        if (b < 0) { // the screen sits still
            ui_close();
            b = btn_get(-1);
        }
        if (b == BTN_PLUS || b == BTN_MINUS) {
            sel = b == BTN_MINUS;
            continue;
        }
        ui_close();
        btn_flush();
        if (sel == 1) wired(); // never returns
        app_ota();             // returning goes home; no USB host was installed on this path
        return;
    }
}
