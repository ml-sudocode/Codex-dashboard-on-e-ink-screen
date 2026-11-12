#include "board_view.h"
#include "badge/badge.h"
#include "badge/font8x8_basic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint8_t fb[PLANE];
static unsigned frames, closes, low_labels, quote_chars, task_chars;
static bool fetched, resets, zone, needs_you, approval, led_button, reply_ready;
static bool status_column, quota_column;

void box(int x, int y, int w, int h, bool ink)
{
    assert(x >= 0 && y >= TOP && w >= 0 && h >= 0 && x + w <= W && y + h <= H);
    for (int r = y; r < y + h; r++)
        for (int c = x; c < x + w; c++) {
            uint8_t bit = (uint8_t)(0x80 >> (c & 7));
            if (ink) fb[r * (W / 8) + c / 8] |= bit;
            else fb[r * (W / 8) + c / 8] &= (uint8_t)~bit;
        }
}

void text(int x, int y, int scale, const char *s, bool ink)
{
    assert(scale >= 2);
    assert(x >= 0 && y >= TOP && x + (int)strlen(s) * 8 * scale <= W && y + 8 * scale <= H);
    if (!strcmp(s, "LOW")) low_labels++;
    if (!strcmp(s, "2026-11-10 15:00:00")) fetched = true;
    if (!strcmp(s, "2026-11-11 10:00:00")) resets = true;
    if (!strcmp(s, "PST (-0800)")) zone = true;
    if (strstr(s, "INPUT NEEDED")) needs_you = true;
    if (!strcmp(s, "STATUS")) { status_column = true; assert(x == 408); }
    if (!strcmp(s, "QUOTA")) { quota_column = true; assert(x == 632); }
    if (!strcmp(s, "INPUT NEEDED") && y > 170) { assert(x == 408 && !ink); }
    if (!strcmp(s, "REPLY READY")) { reply_ready = true; assert(x == 408 && ink); }
    if (!strcmp(s, "LOW")) assert(x == 838);
    if (x == 24 && y >= 194 && y < 500 && scale == 2 && s[0] == 'T')
        task_chars += (unsigned)strlen(s);
    if (strstr(s, "APPROVAL")) approval = true;
    if (y >= 614) quote_chars += (unsigned)strlen(s);
    for (; *s; s++, x += 8 * scale)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (font8x8_basic[(unsigned char)*s][r] >> c & 1)
                    box(x + c * scale, y + r * scale, scale, scale, ink);
}

void ui_keys(const char *plus, const char *minus, const char *a, const char *b)
{
    assert(!plus || strlen(plus) <= 16);
    assert(!minus || strlen(minus) <= 16);
    assert(a && strlen(a) <= 16 && b && !strcmp(b, "Home"));
    led_button = !strcmp(a, "Test LEDs");
}
void ui_frame(int pulls) { assert(pulls == 1); frames++; }
void ui_close(void) { closes++; }

int main(int argc, char **argv)
{
    cb_feed_t f = {.pane_count = 16, .window_count = 8, .demo = true};
    memset(f.zone, 'Z', sizeof f.zone - 1);
    strcpy(f.sampled, "2026-11-10 15:00:00 PST (-0800)");
    strcpy(f.fetched, f.sampled);
    memset(f.footer, 'F', sizeof f.footer - 1);
    for (unsigned i = 0; i < f.pane_count; i++) {
        snprintf(f.panes[i].id, sizeof f.panes[i].id, "%%%u", i);
        memset(f.panes[i].task, 'T', sizeof f.panes[i].task - 1);
        f.panes[i].status = (cb_status_t)(i % 7);
    }
    for (unsigned i = 0; i < f.window_count; i++) {
        memset(f.windows[i].label, 'Q', sizeof f.windows[i].label - 1);
        f.windows[i].remaining = i == 0 ? 29 : 30;
        strcpy(f.windows[i].resets, "2026-11-11 10:00:00 PST (-0800)");
    }
    assert(cb_pages(&f) == 6);
    for (unsigned p = 0; p < 6; p++) {
        quote_chars = 0;
        cb_view(&f, p, "STALE FEED: collector has stopped updating", false);
        assert(quote_chars == 160 && !led_button);
    }
    assert(low_labels == 2); // 29% on pages 0 and 4 is low; 30% is not
    assert(task_chars == 16 * (sizeof f.panes[0].task - 1));
    assert(fetched && resets && zone && needs_you && approval && reply_ready);
    assert(status_column && quota_column);
    assert(frames == 6 && closes == 6);
    if (argc == 2) {
        cb_view(&f, 0, "", false);
        FILE *image = fopen(argv[1], "wb");
        assert(image);
        fprintf(image, "P4\n%d %d\n", W, H);
        assert(fwrite(fb, 1, sizeof fb, image) == sizeof fb);
        fclose(image);
    }
    memset(f.message, 'M', sizeof f.message - 1);
    cb_view(&f, 0, "", false);
    assert(led_button);
    cb_view(&f, 0, "USB STALE: no complete feed for 30 seconds", true);
    assert(led_button);
    f = (cb_feed_t){0};
    f.pane_count = f.window_count = 1;
    f.panes[0].status = CB_REPLY_READY;
    strcpy(f.windows[0].label, "Weekly");
    f.windows[0].remaining = 77;
    low_labels = 0;
    cb_view(&f, 0, "", true);
    assert(low_labels == 0 && cb_pending(&f) == 0);
    f = (cb_feed_t){0};
    assert(cb_pages(&f) == 1);
    cb_view(&f, 0, "", true);
    cb_setup_view("Set a valid http:// feed URL in Settings", false);
    cb_setup_view("USB waiting: send a Codex Board feed", true);
    puts("PASS: renderer bounds, exact timestamps, all pages, full footer, LOW threshold, smoke LED control");
    return 0;
}
