#include "board_feed.h"
#include <string.h>

static bool copy(char *dst, size_t capacity, const char *src, bool required)
{
    size_t n = strlen(src);
    if (n >= capacity || (required && !n)) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool hex(const char *s, size_t minimum, size_t maximum)
{
    size_t n = strlen(s);
    if (n < minimum || n > maximum) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

static int choice(const char *s, const char *const *choices, unsigned count)
{
    for (unsigned i = 0; i < count; i++) if (!strcmp(s, choices[i])) return (int)i;
    return -1;
}

static int health(const char *s)
{
    static const char *const names[] = {"OK", "STALE", "UNAVAILABLE"};
    return choice(s, names, 3);
}

static bool percent(const char *s, unsigned *out)
{
    size_t n = strlen(s);
    unsigned value = 0;
    if (!n || n > 3) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (unsigned)(s[i] - '0');
    }
    if (value > 100) return false;
    *out = value;
    return true;
}

static bool pane(cb_feed_t *feed, char **v)
{
    static const char *const names[] = {
        "WORKING", "IDLE", "DONE", "NEEDS_YOU", "APPROVAL", "UNKNOWN", "REPLY_READY"
    };
    int status = choice(v[3], names, 7);
    if (feed->pane_count == CB_PANES_MAX || status < 0 ||
        (v[4][0] && !hex(v[4], 32, 32))) return false;
    cb_pane_t *p = &feed->panes[feed->pane_count];
    if (!copy(p->id, sizeof p->id, v[1], true) ||
        !copy(p->task, sizeof p->task, v[2], false) ||
        !copy(p->event, sizeof p->event, v[4], false)) return false;
    for (unsigned i = 0; i < feed->pane_count; i++)
        if (!strcmp(p->id, feed->panes[i].id)) return false;
    p->status = (cb_status_t)status;
    feed->pane_count++;
    return true;
}

static bool window(cb_feed_t *feed, char **v)
{
    if (feed->window_count == CB_WINDOWS_MAX) return false;
    cb_window_t *w = &feed->windows[feed->window_count];
    if (!copy(w->label, sizeof w->label, v[1], true) ||
        !copy(w->resets, sizeof w->resets, v[3], true) ||
        !percent(v[2], &w->remaining)) return false;
    feed->window_count++;
    return true;
}

// All required records except END. Ordering after the header is deliberately flexible.
enum { HEADER = 1, META = 2, SOURCE = 4, QUOTA = 8, FOOTER = 16, MESSAGE = 32 };

static bool record(cb_feed_t *f, char **v, unsigned n, unsigned *seen)
{
    unsigned bit = 0;
    if (!strcmp(v[0], "CODEXBOARD")) {
        if (n != 2 || strcmp(v[1], "1") || *seen) return false;
        bit = HEADER;
    } else if (!(*seen & HEADER)) {
        return false;
    } else if (!strcmp(v[0], "META")) {
        if (n != 5 || !hex(v[1], 1, 32) ||
            (strcmp(v[2], "DEMO") && strcmp(v[2], "LIVE")) ||
            !copy(f->revision, sizeof f->revision, v[1], true) ||
            !copy(f->zone, sizeof f->zone, v[3], true) ||
            !copy(f->sampled, sizeof f->sampled, v[4], true)) return false;
        f->demo = !strcmp(v[2], "DEMO");
        bit = META;
    } else if (!strcmp(v[0], "SOURCE")) {
        if (n != 2 || health(v[1]) < 0) return false;
        f->source = (cb_health_t)health(v[1]);
        bit = SOURCE;
    } else if (!strcmp(v[0], "QUOTA")) {
        if (n != 3 || health(v[1]) < 0 ||
            !copy(f->fetched, sizeof f->fetched, v[2], false)) return false;
        f->quota = (cb_health_t)health(v[1]);
        bit = QUOTA;
    } else if (!strcmp(v[0], "MESSAGE")) {
        if (n != 2 || !copy(f->message, sizeof f->message, v[1], false)) return false;
        bit = MESSAGE;
    } else if (!strcmp(v[0], "FOOTER")) {
        if (n != 3 || (strcmp(v[1], "quote") && strcmp(v[1], "joke")) ||
            !copy(f->footer, sizeof f->footer, v[2], false)) return false;
        f->joke = !strcmp(v[1], "joke");
        bit = FOOTER;
    } else if (!strcmp(v[0], "PANE")) {
        return n == 5 && pane(f, v);
    } else if (!strcmp(v[0], "WINDOW")) {
        return n == 4 && window(f, v);
    } else {
        return false;
    }
    if (*seen & bit) return false;
    *seen |= bit;
    return true;
}

bool cb_feed_parse(char *body, size_t length, cb_feed_t *out)
{
    if (!body || !out || !length || length > CB_BODY_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c != '\t' && c != '\n' && (c < 32 || c > 126)) return false;
    }
    body[length] = '\0';
    cb_feed_t next = {0};
    unsigned seen = 0;
    char *line = body, *end = body + length;
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        if (nl) *nl = '\0';
        if (!strcmp(line, "END")) {
            if ((seen & 31) != 31 || (nl && nl + 1 != end)) return false;
            *out = next;
            return true;
        }
        if (!nl || !line[0]) return false;
        char *v[5] = {line};
        unsigned n = 1;
        for (char *p = line; p < nl; p++) {
            if (*p != '\t') continue;
            if (n == 5) return false;
            *p = '\0';
            v[n++] = p + 1;
        }
        if (!record(&next, v, n, &seen)) return false;
        line = nl + 1;
    }
    return false;
}

bool cb_attention(cb_status_t status)
{
    return status == CB_NEEDS_YOU || status == CB_APPROVAL;
}

void cb_feed_retain_terminals(cb_feed_t *next, const cb_feed_t *previous)
{
    if (next->source == CB_OK) return;
    next->pane_count = previous->pane_count;
    memcpy(next->panes, previous->panes, sizeof next->panes);
}

unsigned cb_pending(const cb_feed_t *feed)
{
    unsigned count = 0;
    for (unsigned i = 0; i < feed->pane_count; i++)
        if (cb_attention(feed->panes[i].status)) count++;
    return count;
}
