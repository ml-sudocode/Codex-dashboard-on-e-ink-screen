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
#include "badge.h"
#include "clock_fonts.h"
#include "clock_schedule.h"

static const char *TAG = "clock";

#define TZ_DEFAULT "PST8PDT,M3.2.0,M11.1.0"
#define CON_NAME "Hackaday Supercon X"
#define CON_Y 2026 // Day 1
#define CON_M 11
#define CON_D 6
#define CON_DAYS 3
#define RESYNC_S (6 * 3600)
#define WIPE_S 3600 // partial updates leave ghosts: shake them off this often

#define LEFT 288       // name column width; the dial window is the rest, below the top bar
#define WX LEFT
#define WY TOP
#define WW (W - LEFT)
#define WH (H - TOP)
#define R 1100         // radius of the virtual dial, px (about 18 inches across)
#define RING_OFF 140   // the ring passes this far from the window centre, on the outer side
#define WINDOW_MIN 30  // the window moves along the dial every 30 minutes
#define DEG 0.017453293f

// ---- Drawing: 1-bit bitmaps like fb (1 = black, MSB = leftmost pixel), clipped. Everything draws black.
typedef struct {
    uint8_t *p;
    int rb, x0, y0, x1, y1; // bytes per row, clip rectangle (x1, y1 exclusive)
} cv_t;
static cv_t scr = {fb, W / 8, 0, TOP, W, H};

static void dot(cv_t *c, int x, int y)
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

static void line(cv_t *c, float x0, float y0, float x1, float y1, float width)
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

static int text_w(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int i = (uint8_t)*s - f->first;
        if (i >= 0 && i < f->count) w += f->w[i];
    }
    return w;
}

static void put(cv_t *c, const font_t *f, const char *s, int x, int y) // (x, y) = top left
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

// ---- The dial: the window shows a small arc of a circle of radius R whose centre is far off screen.
static float cx, cy, view, hand; // virtual centre (screen px); dial angles (deg) at the window middle and of the hand

static void pt(float r, float deg, float *x, float *y) // point at radius r and angle deg (0 = top, clockwise)
{
    *x = cx + r * sinf(deg * DEG), *y = cy - r * cosf(deg * DEG);
}

static bool in_window(float x, float y, float margin)
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

// ---- Schedule: one dashed ray per start time, the talks written along it on the side of later times (upcoming)
// or earlier times (past). Labels never cover the hand, a ray or another label; fewer talks plus "+N more" if needed.
#define LINE_H 22           // label line height, px
#define GAP 5               // between ray and text
#define R0 (R - 82)         // text starts here, inside the ticks
#define RAY_END (R - 600)
#define MIN_LEN 160         // shortest label worth drawing, px
#define MAX_GROUPS 16
#define MAX_LINES 5

typedef struct {
    int first, count; // talks schedule[first .. first + count - 1] start at the same time
    float deg;
} group_t;

typedef struct {
    float lo, hi;   // radians (when half == 0)
    float centre;   // radians
    float half;     // > 0: a line this many px wide each side, so its angle depends on the radius
    int owner;      // group, or -1
} taken_t;

static int con_day(const struct tm *t) // 1 on Day 1, by calendar date
{
    struct tm a = {.tm_year = t->tm_year, .tm_mon = t->tm_mon, .tm_mday = t->tm_mday, .tm_hour = 12, .tm_isdst = -1};
    struct tm b = {.tm_year = CON_Y - 1900, .tm_mon = CON_M - 1, .tm_mday = CON_D, .tm_hour = 12, .tm_isdst = -1};
    return (int)floor(difftime(mktime(&a), mktime(&b)) / 86400.0 + 0.5) + 1;
}

static void fit(const font_t *f, const char *s, int maxw, char *out, int n) // s, cut short with "..." to fit maxw
{
    snprintf(out, n, "%s", s);
    for (int len = strlen(out); text_w(f, out) > maxw; ) {
        while (len > 0 && out[len - 1] == ' ') len--; // no space before the dots
        if (--len <= 0) { out[0] = 0; return; }
        strcpy(out + len, "...");
    }
}

static void dashed_ray(float deg)
{
    for (float r = R - 2; r > RAY_END; r -= 20) { // 12 on, 8 off
        float x0, y0, x1, y1;
        pt(r, deg, &x0, &y0);
        pt(fmaxf(r - 12, RAY_END), deg, &x1, &y1);
        line(&scr, x0, y0, x1, y1, 3);
    }
}

static int room(float deg, float offset) // how far inward from R0 a label can run before its far edge leaves the window
{
    float tx = cosf(deg * DEG), ty = sinf(deg * DEG); // towards later times
    int len = 0;
    for (int l = 0; l <= 610; l += 10) {
        float x, y;
        pt(R0 - l, deg, &x, &y);
        if (!in_window(x, y, 6) || !in_window(x + tx * offset, y + ty * offset, 6)) break;
        len = l;
    }
    return len;
}

static bool is_free(const taken_t *t, int n, float lo, float hi, float rmin, int owner)
{
    for (int i = 0; i < n; i++) {
        if (t[i].owner >= 0 && t[i].owner == owner) continue;
        float a = t[i].half > 0 ? t[i].centre - t[i].half / rmin : t[i].lo;
        float b = t[i].half > 0 ? t[i].centre + t[i].half / rmin : t[i].hi;
        if (!(hi < a || lo > b)) return false;
    }
    return true;
}

// The label is laid out flat in a scratch bitmap, then copied through the rotation onto the ray at deg, on side
// (+1 = towards later times), len px long. Text always reads left to right or top down.
static void draw_label(float deg, int side, int len, char lines[][160], int n, int indent)
{
    static uint8_t buf[(610 + 7) / 8 * (LINE_H * MAX_LINES + 4)];
    int bh = LINE_H * n + 4, rb = (len + 7) / 8;
    cv_t lab = {buf, rb, 0, 0, len, bh};
    memset(buf, 0, rb * bh);
    for (int i = 0; i < n; i++) {
        if (i == 0) { // "HH:MM " in bold, then the talk
            char hhmm[7];
            snprintf(hhmm, sizeof hhmm, "%.6s", lines[0]);
            put(&lab, &f_labelb, hhmm, 0, 2);
            put(&lab, &f_label, lines[0] + 6, indent, 2);
        } else put(&lab, &f_label, lines[i], indent, 2 + i * LINE_H);
    }
    float a = deg * DEG, inx = -sinf(a), iny = cosf(a); // along the ray, inward
    bool inward = inx > 0.02f || (fabsf(inx) <= 0.02f && iny > 0);
    float dx = inward ? inx : -inx, dy = inward ? iny : -iny; // text direction
    float nx = -dy, ny = dx;                                  // text "down"
    float sx = side * cosf(a), sy = side * sinf(a);           // towards the chosen side
    float px, py, ox, oy;
    pt(inward ? R0 : R0 - len, deg, &px, &py);
    float off = nx * sx + ny * sy > 0 ? GAP : GAP + bh;
    ox = px + off * sx, oy = py + off * sy;
    float xs[4] = {ox, ox + len * dx, ox + bh * nx, ox + len * dx + bh * nx};
    float ys[4] = {oy, oy + len * dy, oy + bh * ny, oy + len * dy + bh * ny};
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) x0 = fminf(x0, xs[i]), x1 = fmaxf(x1, xs[i]), y0 = fminf(y0, ys[i]), y1 = fmaxf(y1, ys[i]);
    for (int y = (int)y0; y <= (int)y1 + 1; y++)
        for (int x = (int)x0; x <= (int)x1 + 1; x++) {
            float rx = x + 0.5f - ox, ry = y + 0.5f - oy;
            int u = (int)floorf(rx * dx + ry * dy), v = (int)floorf(rx * nx + ry * ny);
            if (u >= 0 && u < len && v >= 0 && v < bh && buf[v * rb + u / 8] & 0x80 >> (u & 7)) dot(&scr, x, y);
        }
}

static float now_deg;
static group_t groups[MAX_GROUPS];

static float order_key(int g) // upcoming (from 10 minutes ago) nearest first, then past ones
{
    return (groups[g].deg < now_deg - 2.5f ? 1000 : 0) + fabsf(groups[g].deg - now_deg);
}

static void draw_events(const struct tm *t)
{
    int day = con_day(t), ng = 0;
    if (day < 1 || day > CON_DAYS) return; // the schedule only names weekdays
    for (int i = 0, j; i < NEVENTS && ng < MAX_GROUPS; i = j) {
        const event_t *e = &schedule[i];
        for (j = i + 1; j < NEVENTS && schedule[j].wday == e->wday && schedule[j].hour == e->hour &&
                        schedule[j].minute == e->minute; j++) {}
        float deg = (e->hour + e->minute / 60.0f) * 15, x, y;
        pt(R, deg, &x, &y);
        if (e->wday == t->tm_wday && in_window(x, y, 10)) groups[ng++] = (group_t){i, j - i, deg};
    }
    taken_t taken[1 + 2 * MAX_GROUPS];
    int nt = 0, order[MAX_GROUPS];
    taken[nt++] = (taken_t){0, 0, hand * DEG, 8, -1}; // the hand: 10 px plus a margin
    for (int g = 0; g < ng; g++) {
        dashed_ray(groups[g].deg);
        taken[nt++] = (taken_t){0, 0, groups[g].deg * DEG, 3, g};
        order[g] = g;
    }
    now_deg = (t->tm_hour + t->tm_min / 60.0f) * 15;
    for (int i = 1; i < ng; i++)
        for (int k = i; k > 0 && order_key(order[k]) < order_key(order[k - 1]); k--) {
            int o = order[k];
            order[k] = order[k - 1], order[k - 1] = o;
        }
    static char lines[MAX_LINES][160];
    for (int oi = 0; oi < ng; oi++) {
        const group_t *G = &groups[order[oi]];
        int sides[2] = {G->deg >= now_deg ? 1 : -1, G->deg >= now_deg ? -1 : 1};
        bool done = false;
        for (int n = G->count < MAX_LINES ? G->count : MAX_LINES; n >= 1 && !done; n--) {
            int more = G->count - n, nl = n + (more ? 1 : 0), bh = LINE_H * nl + 4;
            if (nl > MAX_LINES) continue;
            for (int si = 0; si < 2 && !done; si++) {
                int side = sides[si], len = room(G->deg, side * (GAP + bh));
                if (len < MIN_LEN) continue;
                float rmin = R0 - len, a = G->deg * DEG, hw = (GAP + bh) / rmin;
                float lo = side > 0 ? a : a - hw, hi = side > 0 ? a + hw : a;
                if (!is_free(taken, nt, lo, hi, rmin, order[oi])) continue;
                char hhmm[12], full[160];
                snprintf(hhmm, sizeof hhmm, "%02d:%02d ", schedule[G->first].hour, schedule[G->first].minute);
                int indent = text_w(&f_labelb, hhmm);
                for (int k = 0; k < n; k++) {
                    const event_t *e = &schedule[G->first + k];
                    snprintf(full, sizeof full, "%.40s: %.110s", e->stage, e->talk);
                    fit(&f_label, full, len - indent - 4, lines[k] + (k ? 0 : 6), 154);
                    if (!k) memcpy(lines[0], hhmm, 6);
                }
                if (more) snprintf(lines[n], sizeof lines[n], "+%d more", more);
                draw_label(G->deg, side, len, lines, nl, indent);
                if (nt < (int)(sizeof taken / sizeof *taken)) taken[nt++] = (taken_t){lo, hi, 0, 0, -1};
                done = true;
            }
        }
    }
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

void app_clock(void)
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
