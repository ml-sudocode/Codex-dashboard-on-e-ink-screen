// Clock: a zoomed-in arc of a huge 24-hour dial (0 at the top, 12 at the bottom, clockwise) moves past a fixed hand,
// beside a name column made from the Badge app's text. On con days the Supercon schedule shows as dashed rays from
// the ring at each start time, with the talks written along them.
// The time comes from NTP over the saved Wi-Fi when the app opens and every 6 hours (the radio goes back to ranging
// in between). Settings key "tz" (a POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0") sets the time zone; unset = US
// Pacific, the con's. MENU + / MENU - look 30 minutes ahead / back, A wipes the ghosts (and goes back to now), B home.
// Fonts and schedule are generated: tools/make_clock_fonts.py, tools/make_clock_schedule.py.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "badge/badge.h"
#include "clock.h"

static const char *TAG = "clock";

#define TZ_DEFAULT "PST8PDT,M3.2.0,M11.1.0"
#define CON_NAME "Hackaday Supercon X"
#define RESYNC_S (6 * 3600)
#define WIPE_S 3600 // partial updates leave ghosts: shake them off this often

#define LEFT 288       // name column width; the dial window is the rest, below the top bar
#define WX LEFT
#define WY TOP
#define WW (W - LEFT)
#define WH (H - TOP)
#define RING_OFF 140   // the ring passes this far from the window centre, on the outer side
#define WINDOW_MIN 30  // the window moves along the dial every 30 minutes

// ---- Drawing
cv_t scr = {fb, W / 8, 0, TOP, W, H};

void dot(cv_t *c, int x, int y)
{
    if (x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1) c->p[y * c->rb + x / 8] |= 0x80 >> (x & 7);
}

static void span(cv_t *c, int x0, int x1, int y) // x0..x1 inclusive
{
    for (int x = x0; x <= x1; x++) dot(c, x, y);
}

static void rect(cv_t *c, int x, int y, int w, int h)
{
    for (int r = y; r < y + h; r++) span(c, x, x + w - 1, r);
}

static void convex(cv_t *c, const float *xs, const float *ys, int n) // fills the pixel centres inside
{
    float lo = ys[0], hi = ys[0];
    for (int i = 1; i < n; i++) lo = fminf(lo, ys[i]), hi = fmaxf(hi, ys[i]);
    int y0 = (int)ceilf(lo - 0.5f), y1 = (int)floorf(hi - 0.5f);
    if (y0 < c->y0) y0 = c->y0;
    if (y1 >= c->y1) y1 = c->y1 - 1;
    for (int y = y0; y <= y1; y++) {
        float yc = y + 0.5f, l = 1e9f, r = -1e9f;
        for (int i = 0; i < n; i++) {
            float ax = xs[i], ay = ys[i], bx = xs[(i + 1) % n], by = ys[(i + 1) % n];
            if ((ay <= yc && yc < by) || (by <= yc && yc < ay)) {
                float x = ax + (yc - ay) * (bx - ax) / (by - ay);
                l = fminf(l, x), r = fmaxf(r, x);
            }
        }
        if (l <= r) span(c, (int)ceilf(l - 0.5f), (int)floorf(r - 0.5f), y);
    }
}

void line(cv_t *c, float x0, float y0, float x1, float y1, float width)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    float xs[4] = {x0 + nx, x1 + nx, x1 - nx, x0 - nx}, ys[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
    convex(c, xs, ys, 4);
}

static const uint8_t *glyph(const font_t *f, char ch, int *w) // unpacked, NULL if the font hasn't got it
{
    static uint8_t g[GLYPH_MAX];
    int i = (uint8_t)ch - f->first;
    if (i < 0 || i >= f->count) return NULL;
    *w = f->w[i];
    const uint8_t *p = f->bits + f->off[i];
    for (int k = 0, n = (*w + 7) / 8 * f->h; k < n;) {
        uint8_t b = *p++;
        if (b == 0x00 || b == 0xFF)
            for (int run = *p++; run--;) g[k++] = b;
        else g[k++] = b;
    }
    return g;
}

int text_w(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int i = (uint8_t)*s - f->first;
        if (i >= 0 && i < f->count) w += f->w[i];
    }
    return w;
}

void put(cv_t *c, const font_t *f, const char *s, int x, int y) // (x, y) = top left
{
    for (int w; *s; s++) {
        const uint8_t *g = glyph(f, *s, &w);
        if (!g) continue;
        for (int r = 0, rb = (w + 7) / 8; r < f->h; r++)
            for (int k = 0; k < w; k++)
                if (g[r * rb + k / 8] & 0x80 >> (k & 7)) dot(c, x + k, y + r);
        x += w;
    }
}

static void centred(const font_t *f, const char *s, int x, int y) // (x, y) = centre
{
    put(&scr, f, s, x - text_w(f, s) / 2, y - f->h / 2);
}

int con_day(const struct tm *t) // 1 on Day 1, by calendar date
{
    struct tm a = {.tm_year = t->tm_year, .tm_mon = t->tm_mon, .tm_mday = t->tm_mday, .tm_hour = 12, .tm_isdst = -1};
    struct tm b = {.tm_year = CON_Y - 1900, .tm_mon = CON_M - 1, .tm_mday = CON_D, .tm_hour = 12, .tm_isdst = -1};
    return (int)floor(difftime(mktime(&a), mktime(&b)) / 86400.0 + 0.5) + 1;
}

void fit(const font_t *f, const char *s, int maxw, char *out, int n) // s, cut short with "..." to fit maxw
{
    snprintf(out, n, "%s", s);
    for (int len = strlen(out); text_w(f, out) > maxw; ) {
        while (len > 0 && out[len - 1] == ' ') len--; // no space before the dots
        if (--len <= 0) { out[0] = 0; return; }
        strcpy(out + len, "...");
    }
}

// ---- The dial: the window shows a small arc of a circle of radius R whose centre is far off screen.
static float cx, cy, view; // virtual centre (screen px), dial angle (deg) at the window middle
float hand;

void pt(float r, float deg, float *x, float *y) // point at radius r and angle deg (0 = top, clockwise)
{
    *x = cx + r * sinf(deg * DEG), *y = cy - r * cosf(deg * DEG);
}

bool in_window(float x, float y, float margin)
{
    return x >= WX + margin && x <= WX + WW - margin && y >= WY + margin && y <= WY + WH - margin;
}

static void layout(int hour, int min)
{
    float h = hour + min / 60.0f, win = WINDOW_MIN / 60.0f;
    view = (floorf(h / win) * win + win / 2) * 15; // middle of this window period
    hand = h * 15;
    float d = R - RING_OFF;
    cx = WX + WW / 2.0f - d * sinf(view * DEG), cy = WY + WH / 2.0f + d * cosf(view * DEG);
}

static void draw_dial(void)
{
    float px = 0, py = 0, x, y, x2, y2;
    for (int i = -120; i <= 120; i++, px = x, py = y) { // ring: 60 degrees in quarter-degree segments
        pt(R, view + i / 4.0f, &x, &y);
        if (i > -120) line(&scr, px, py, x, y, 4);
    }
    for (int m = (int)floorf((view - 30) * 4), end = m + 240; m <= end; m++) { // ticks, inward from the ring
        int mm = (m % 1440 + 1440) % 1440;
        float len = mm % 60 == 0 ? 70 : mm % 30 == 0 ? 55 : mm % 15 == 0 ? 40 : 16;
        float w = mm % 60 == 0 ? 7 : mm % 15 == 0 ? 4 : 2;
        pt(R, m / 4.0f, &x, &y);
        pt(R - len, m / 4.0f, &x2, &y2);
        line(&scr, x, y, x2, y2, w);
    }
    for (int h = (int)floorf(view / 15) - 3, end = h + 7; h <= end; h++) { // hour numbers, upright, outside the ring
        char s[4];
        snprintf(s, sizeof s, "%d", (h % 24 + 24) % 24);
        pt(R + 75, h * 15.0f, &x, &y);
        int w = text_w(&f_hour, s), lx = (int)(x - w / 2.0f), ly = (int)(y - f_hour.h / 2.0f);
        if (lx >= WX + 4 && ly >= WY + 4 && lx + w <= WX + WW - 4 && ly + f_hour.h <= WY + WH - 4) // a cut "12" reads "2"
            put(&scr, &f_hour, s, lx, ly);
    }
}

static void draw_hand(void)
{
    float x0, y0, x1, y1;
    pt(R - 700, hand, &x0, &y0); // from beyond the window edge
    pt(R, hand, &x1, &y1);       // to the ring
    line(&scr, x0, y0, x1, y1, 10);
}

// ---- Name column: the Badge app's text, line 1 large, line 2 medium, the rest small, each smaller if it must be;
// a line too long even when small wraps at spaces. With a picture (too big for the column) or no text: the username,
// else "Badge XXXX".
#define MAX_NAME 8
#define COL_W (LEFT - 24)
static const font_t *const ladder[] = {&f_big, &f_mid, &f_small, &f_day, &f_con}; // the sizes, largest first
#define NSIZES ((int)(sizeof ladder / sizeof *ladder))
static char name[MAX_NAME][64];
static int nname, start[MAX_NAME]; // lines, and the size each starts from

static void add_line(const char *s, int size)
{
    if (nname < MAX_NAME) snprintf(name[nname], sizeof name[0], "%s", s), start[nname++] = size;
}

static void load_name(void)
{
    char t[256];
    bool pic = cfg_get_image(fb + TOP * (W / 8)); // fb as scratch: only whether there is one matters
    int n = pic ? -1 : cfg_get("badge_text", t, sizeof t);
    if (n <= 0 && cfg_get("name", t, 13) <= 0) {
        char id[5];
        badge_id(id);
        snprintf(t, sizeof t, "Badge %s", id);
    }
    nname = 0;
    for (char *s = t, *end; s; s = end ? end + 1 : NULL) {
        int size = nname < 2 ? nname : 2;
        if ((end = strchr(s, '\n'))) *end = 0;
        for (int len = strlen(s); len && (s[len - 1] == '\r' || s[len - 1] == ' ');) s[--len] = 0;
        if (text_w(ladder[2], s) <= COL_W) { add_line(s, size); continue; }
        char cur[64] = "", try[64];
        for (char *w = strtok(s, " "); w; w = strtok(NULL, " ")) { // wrap, word by word, at the small size
            bool over = snprintf(try, sizeof try, "%s%s%s", cur, *cur ? " " : "", w) >= (int)sizeof try ||
                        text_w(ladder[2], try) > COL_W;
            if (*cur && over) add_line(cur, 2), snprintf(cur, sizeof cur, "%s", w);
            else strcpy(cur, try);
        }
        add_line(cur, 2);
    }
    while (nname > 1 && !name[nname - 1][0]) nname--;
    memset(fb, 0, sizeof fb);
}

static void draw_column(const struct tm *t) // t NULL: no date yet
{
    const int nl = NSIZES, top = TOP + 12, bottom = 390, gap = 14, cxm = LEFT / 2;
    int size[MAX_NAME], n = nname, total;
    for (int i = 0; i < n; i++) // each line from its own size down until it fits the width
        for (size[i] = i && start[i] < size[i - 1] ? size[i - 1] : start[i]; // never larger than the line above
             size[i] < nl - 1 && text_w(ladder[size[i]], name[i]) > COL_W; size[i]++) {}
    for (;;) { // then shrink the largest lines a step, or drop the last line, until the lines fit the height
        total = -gap;
        for (int i = 0; i < n; i++) total += ladder[size[i]]->h + gap;
        if (total <= bottom - top) break;
        int big = nl - 1;
        for (int i = 0; i < n; i++)
            if (size[i] < big) big = size[i];
        if (big == nl - 1) n--;
        for (int i = 0; i < n; i++) size[i] += size[i] == big;
    }
    for (int i = 0, y = top + (bottom - top - total) / 2; i < n; y += ladder[size[i]]->h + gap, i++) {
        char s[64];
        fit(ladder[size[i]], name[i], COL_W, s, sizeof s);
        put(&scr, ladder[size[i]], s, cxm - text_w(ladder[size[i]], s) / 2, y);
    }
    rect(&scr, 30, 399, LEFT - 60, 2);
    rect(&scr, LEFT - 3, TOP, 3, H - TOP);
    centred(&f_con, CON_NAME, cxm, 570);
    if (!t) return;
    char s[32];
    strftime(s, 20, "%a %b ", t);
    snprintf(s + strlen(s), 12, "%d", t->tm_mday); // no leading zero
    centred(&f_small, s, cxm, 460);
    int d = con_day(t);
    if (d >= 1 && d <= CON_DAYS) {
        snprintf(s, sizeof s, "Day %d", d);
        centred(&f_day, s, cxm, 610);
    }
}

static void draw(const struct tm *t, const char *msg) // the clock at t, or msg where the dial goes
{
    memset(fb, 0, sizeof fb);
    draw_column(t);
    scr.x0 = WX, scr.y0 = WY, scr.x1 = WX + WW, scr.y1 = WY + WH;
    if (t) {
        layout(t->tm_hour, t->tm_min);
        draw_dial();
        draw_events(t);
        draw_hand();
    } else centred(&f_day, msg, WX + WW / 2, WY + WH / 2);
    scr.x0 = 0, scr.y0 = TOP, scr.x1 = W, scr.y1 = H;
}

// ---- Time
static bool valid(time_t t) { return t > 1700000000; } // after Nov 2023: set

static bool sync_time(void)
{
    bool ok = false;
    if (wifi_connect(20000)) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&c);
        ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK;
        esp_netif_sntp_deinit();
    }
    wifi_off();
    ranging_start(); // give the radio back
    ESP_LOGI(TAG, "time sync %s", ok ? "done" : "failed");
    return ok;
}

static void run(void)
{
    char tz[64];
    setenv("TZ", cfg_get("tz", tz, sizeof tz) > 0 ? tz : TZ_DEFAULT, 1);
    tzset();
    load_name();
    ui_keys("+30 min", "-30 min", "Wipe", "Home");
    time_t next_sync = 0, next_wipe = time(NULL) + WIPE_S; // the OS wiped the panel when the app opened
    long shown = -1, offset = 0;                            // minute on the screen; look-ahead, s
    const char *msg = "Getting the time...";
    for (;;) {
        time_t now = time(NULL);
        if (now >= next_wipe) ui_wipe(), next_wipe = now + WIPE_S, shown = -1;
        long key = valid(now) ? (long)((now + offset) / 60) : -2;
        if (key != shown) {
            struct tm t;
            time_t at = now + offset;
            localtime_r(&at, &t);
            draw(key >= 0 ? &t : NULL, msg);
            ui_frame(1);
            ui_close();
            shown = key;
        }
        if (now >= next_sync) {
            bool ok = sync_time();
            now = time(NULL);
            next_sync = now + (ok ? RESYNC_S : valid(now) ? 600 : 60);
            msg = "No Wi-Fi or time: retrying";
            shown = -1;
            continue;
        }
        int b = btn_get(valid(now) ? (60 - (now + offset) % 60) * 1000 + 50 : (next_sync - now) * 1000 + 50);
        if (b == BTN_PLUS) offset += 30 * 60;
        else if (b == BTN_MINUS) offset -= 30 * 60;
        else if (b == BTN_A) offset = 0, ui_wipe(), next_wipe = time(NULL) + WIPE_S, shown = -1;
        else if (b == BTN_B) return;
    }
}

static void icon(int x, int y) // a dial: ring, hour ticks, two hands
{
    icon_base(x, y);
    int c = ICON / 2;
    for (int r = 0; r < ICON; r++)
        for (int k = 0; k < ICON; k++) {
            int dx = k - c, dy = r - c, d2 = dx * dx + dy * dy;
            bool ring = d2 > 60 * 60 && d2 < 68 * 68;
            bool tick = d2 > 46 * 46 && d2 < 58 * 58 && (abs(dx) < 4 || abs(dy) < 4);
            if (ring || tick || d2 < 8 * 8) box(x + k, y + r, 1, 1, false);
        }
    box(x + c - 4, y + c - 44, 8, 44, false); // minute hand, to 12
    box(x + c, y + c - 4, 32, 8, false);      // hour hand, to 3
}

const app_t app_clock = {"Clock", icon, run};

