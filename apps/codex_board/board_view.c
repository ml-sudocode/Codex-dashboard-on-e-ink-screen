#include "board_view.h"
#include "badge/badge.h"
#include <stdio.h>
#include <string.h>

static unsigned pages(unsigned count, unsigned per_page)
{
    return count ? (count + per_page - 1) / per_page : 1;
}

unsigned cb_pages(const cb_feed_t *feed)
{
    unsigned p = pages(feed->pane_count, 3), q = pages(feed->window_count, 2);
    return p > q ? p : q;
}

// Prefer word boundaries when the remaining lines can still fit every character.
static void wrapped_ink(int x, int y, int scale, unsigned columns, const char *s, unsigned lines, bool ink)
{
    char line[113];
    for (unsigned i = 0; *s && i < lines; i++) {
        size_t remaining = strlen(s), n = remaining;
        if (n > columns) n = columns;
        if (remaining > columns && i + 1 < lines) {
            for (size_t split = n; split; split--)
                if (s[split - 1] == ' ' && remaining - split <= (lines - i - 1) * columns) {
                    n = split;
                    break;
                }
        }
        memcpy(line, s, n);
        line[n] = '\0';
        text(x, y + (int)i * (8 * scale + 2), scale, line, ink);
        s += n;
    }
}

static void wrapped(int x, int y, int scale, unsigned columns, const char *s, unsigned lines)
{
    wrapped_ink(x, y, scale, columns, s, lines, true);
}

static void timestamp(int x, int y, const char *s)
{
    if (strlen(s) > 20 && s[10] == ' ' && s[19] == ' ') {
        char date[20];
        memcpy(date, s, 19); date[19] = '\0';
        text(x, y, 2, date, true);
        text(x, y + 18, 2, s + 20, true);
    } else wrapped(x, y, 2, 22, s, 2);
}

static void footer(const cb_feed_t *f, const char *notice)
{
    char label[58];
    snprintf(label, sizeof label, "%s%s%.48s", f->joke ? "JOKE" : "QUOTE",
             notice[0] ? " | " : "", notice);
    box(24, 595, 912, 1, true);
    text(24, 600, 2, label, true);
    wrapped(24, 618, 2, 57, f->footer, 3);
}

static void heading(const cb_feed_t *f)
{
    char line[80];
    text(24, 64, 3, "Codex Board", true);
    text(808, 64, 3, f->demo ? "DEMO" : "LIVE", true);
    snprintf(line, sizeof line, "Zone: %s", f->zone);
    text(24, 100, 2, line, true);
    unsigned pending = cb_pending(f);
    unsigned ready = 0;
    for (unsigned i = 0; i < f->pane_count; i++) ready += f->panes[i].status == CB_REPLY_READY;
    if (pending) {
        box(24, 120, 912, 36, true);
        snprintf(line, sizeof line, "INPUT NEEDED: %u | REPLIES READY: %u%s", pending, ready,
                 f->source == CB_OK ? "" : " (last known)");
        text(40, 130, 2, line, false);
    } else if (ready) {
        snprintf(line, sizeof line, "REPLIES READY: %u%s", ready,
                 f->source == CB_OK ? "" : " (last known)");
        text(24, 130, 2, line, true);
    } else {
        text(24, 130, 2, f->source == CB_OK ? "No input requests detected" : "Terminal status unavailable", true);
    }
}

static void terminals(const cb_feed_t *f, unsigned page)
{
    static const char *const labels[] = {"WORKING", "IDLE", "DONE", "INPUT NEEDED", "APPROVAL", "UNKNOWN", "REPLY READY"};
    char line[80];
    text(24, 170, 2, "TERMINALS", true);
    text(408, 170, 2, "STATUS", true);
    unsigned start = (page % pages(f->pane_count, 3)) * 3;
    if (!f->pane_count) text(24, 210, 2, "No sessions", true);
    for (unsigned i = start; i < f->pane_count && i < start + 3; i++) {
        const cb_pane_t *p = &f->panes[i];
        int y = 194 + (int)(i - start) * 104;
        bool urgent = cb_attention(p->status);
        const char *task = p->task[0] ? p->task : p->id;
        if (urgent) box(404, y - 2, 208, 100, true);
        // Keep long task titles complete in the narrower terminal column.
        if (strlen(task) > 45) wrapped(24, y, 2, 23, task, 3);
        else wrapped(24, y, 3, 15, task, 3);
        text(408, y + 20, 2, labels[p->status], !urgent);
        snprintf(line, sizeof line, "%.12s", p->id);
        text(408, y + 52, 2, line, !urgent);
        box(24, y + 100, 588, 1, true);
    }
    snprintf(line, sizeof line, "Page %u/%u | %u terminal%s", page + 1, cb_pages(f),
             f->pane_count, f->pane_count == 1 ? "" : "s");
    text(24, 510, 2, line, true);
    text(24, 532, 2, "Snapshot at:", true);
    timestamp(24, 550, f->sampled);
}

static void quota(const cb_feed_t *f, unsigned page)
{
    char line[32];
    text(632, 170, 2, "QUOTA", true);
    if (f->quota != CB_OK)
        text(752, 170, 2, f->quota == CB_STALE ? "STALE" : "UNAVAILABLE", true);
    text(632, 196, 2, "Quota fetched at:", true);
    timestamp(632, 218, f->fetched[0] ? f->fetched : "Not fetched");
    unsigned start = (page % pages(f->window_count, 2)) * 2;
    if (!f->window_count) text(632, 270, 2, "No quota windows", true);
    for (unsigned i = start; i < f->window_count && i < start + 2; i++) {
        const cb_window_t *w = &f->windows[i];
        int y = 270 + (int)(i - start) * 164;
        wrapped(632, y, 2, 19, w->label, 2);
        snprintf(line, sizeof line, "%u%%", w->remaining);
        text(632, y + 38, 4, line, true);
        if (w->remaining < 30) text(838, y + 40, 3, "LOW", true);
        text(632, y + 72, 2, "remaining", true);
        text(632, y + 92, 2, "Resets at:", true);
        timestamp(632, y + 114, w->resets);
    }
}

void cb_view(const cb_feed_t *f, unsigned page, const char *notice, bool usb)
{
    memset(fb, 0, sizeof fb);
    ui_keys(f->message[0] ? NULL : "Next page", f->message[0] ? NULL : "Previous page",
            f->message[0] ? "Test LEDs" : usb ? "Redraw" : "Refresh", "Home");
    heading(f);
    if (f->message[0]) {
        text(24, 190, 2, usb ? "USB CONNECTION TEST" : "WI-FI CONNECTION TEST", true);
        wrapped(24, 248, 3, 38, f->message, 4);
        text(24, 408, 2, "A: test red LEDs", true);
        text(24, 444, 2, usb ? "Feed updates over USB. Keep the cable connected."
                           : "Feed updates automatically over Wi-Fi", true);
        text(24, 480, 2, "Sampled at:", true);
        timestamp(24, 502, f->sampled);
    } else {
        box(396, 170, 1, 412, true);
        box(616, 170, 1, 412, true);
        terminals(f, page);
        quota(f, page);
    }
    const char *warning = notice[0] ? notice :
        f->source != CB_OK ? "Terminal data is stale or unavailable" :
        f->quota == CB_STALE ? "Quota is from the last successful fetch" : "";
    footer(f, warning);
    ui_frame(1);
    ui_close();
}

void cb_setup_view(const char *notice, bool usb)
{
    memset(fb, 0, sizeof fb);
    ui_keys(NULL, NULL, "Retry", "Home");
    text(24, 80, 4, "Codex Board", true);
    text(24, 160, 2, notice, true);
    if (usb) {
        text(24, 236, 2, "Keep the USB cable connected to the Linux guest.", true);
        text(24, 276, 2, "Start the Codex Board USB sender on that machine.", true);
        text(24, 316, 2, "Feed URL is blank: receiving over USB.", true);
    } else {
        text(24, 236, 2, "Set the feed URL in Settings on your phone.", true);
        text(24, 276, 2, "Use the collector's http:// LAN address.", true);
        text(24, 316, 2, "Connect this badge to 2.4 GHz Wi-Fi.", true);
    }
    ui_frame(1);
    ui_close();
}
