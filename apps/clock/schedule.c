// The con's schedule on the dial: one dashed ray per start time, the talks written along it on the side of later
// times (upcoming) or earlier times (past). Labels never cover the hand, a ray or another label; fewer talks plus
// "+N more" if needed. clock_schedule.h comes from tools/make_clock_schedule.py.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "badge/badge.h"
#include "clock.h"
#include "clock_schedule.h"

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

void draw_events(const struct tm *t)
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
