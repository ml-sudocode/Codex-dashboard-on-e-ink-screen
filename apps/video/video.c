// Video: a 15 s clip at 4 fps in a loop, to show what the panel can do. rickroll.vid comes from
// tools/make_video.py: full-resolution 1-bit frames, each a zlib stream (stb_image's inflate unpacks it). Frames keep
// to the clip's clock: when the panel falls behind, frames are skipped rather than slowing the clip down.
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "badge/badge.h"
#include "stb_image.h"

extern const uint8_t vid[] asm("_binary_rickroll_vid_start");

static int u16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void run(void)
{
    static EXT_RAM_BSS_ATTR uint8_t img[W / 8 * (H - TOP)];
    int w = u16(vid + 4), h = u16(vid + 6), n = u16(vid + 8), fps = u16(vid + 10), rb = w / 8;
    if (memcmp(vid, "VID2", 4) || w > W || h > H - TOP) return;
    ui_keys(NULL, NULL, "Restart", "Home");
    for (;;) {
        ui_wipe();
        const uint8_t *p = vid + 12;
        int64_t t0 = esp_timer_get_time();
        int shown = 0, b = -1;
        for (int f = 0; f < n && b < 0; f++) {
            int len = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
            const uint8_t *z = p + 4;
            p = z + len;
            int64_t due = t0 + (int64_t)f * 1000000 / fps, now = esp_timer_get_time();
            if (now > due + 1000000 / fps) continue; // a frame late: skip to keep the clip's pace
            if (stbi_zlib_decode_buffer((char *)img, rb * h, (const char *)z, len) != rb * h) continue;
            if (due > now) ms((due - now) / 1000);
            for (int y = 0; y < h; y++) // centred below the top bar
                memcpy(fb + (TOP + (H - TOP - h) / 2 + y) * (W / 8) + (W - w) / 16, img + y * rb, rb);
            ui_frame(1);
            shown++;
            b = btn_get(0);
        }
        ESP_LOGI("video", "%d of %d frames shown in %d ms", shown, n, (int)((esp_timer_get_time() - t0) / 1000));
        if (b == BTN_B) {
            ui_close();
            return;
        }
    }
}

static void icon(int x, int y) // a play button on a screen
{
    icon_base(x, y);
    outline(x + 24, y + 40, 128, 96, 8);
    for (int r = 0; r < 48; r++) box(x + 72, y + 64 + r, r < 24 ? r + 1 : 48 - r, 1, false);
}

const app_t app_video = {"Video", icon, run};
