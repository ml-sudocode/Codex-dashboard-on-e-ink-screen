// Clock app internals: clock.c draws the dial and the name column and runs the app; schedule.c writes the con's
// talks on the dial.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "clock_fonts.h"

#define CON_Y 2026 // Day 1
#define CON_M 11
#define CON_D 6
#define CON_DAYS 3
#define R 1100     // radius of the virtual dial, px (about 18 inches across)
#define DEG 0.017453293f

// Drawing: 1-bit bitmaps like fb (1 = black, MSB = leftmost pixel), clipped. Everything draws black.
typedef struct {
    uint8_t *p;
    int rb, x0, y0, x1, y1; // bytes per row, clip rectangle (x1, y1 exclusive)
} cv_t;
extern cv_t scr;   // fb, clipped to what is being drawn
extern float hand; // the hand's dial angle, deg (0 = top, clockwise)

void dot(cv_t *c, int x, int y);
void line(cv_t *c, float x0, float y0, float x1, float y1, float width);
int text_w(const font_t *f, const char *s);
void put(cv_t *c, const font_t *f, const char *s, int x, int y);     // (x, y) = top left
void fit(const font_t *f, const char *s, int maxw, char *out, int n); // s, cut short with "..." to fit maxw
void pt(float r, float deg, float *x, float *y); // point at radius r and angle deg (0 = top, clockwise)
bool in_window(float x, float y, float margin);  // inside the dial window, margin px from its edges
int con_day(const struct tm *t);                 // 1 on Day 1, by calendar date
void draw_events(const struct tm *t);            // schedule.c: the talks of t's day near the window
