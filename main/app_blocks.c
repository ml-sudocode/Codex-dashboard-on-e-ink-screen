// Blocks: four-cell pieces fall into a 10 x 20 well of 28 px cells. Full rows clear; every 10 rows the level
// rises and the pieces fall faster (one row per second at level 0). A / B move left / right (hold to repeat),
// MENU - turns clockwise, MENU + drops. The AS5600 knob add-on moves the piece too: a sixteenth of a turn is one
// cell. When a new piece cannot enter, the score shows for 3 s, then a new game starts by itself. A reboot goes home.
// The game runs on its own 20 ms clock; a second task shows the latest state whenever the panel is free, so the game
// speed does not depend on the refresh speed (same scheme as app_snake.c).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "badge.h"

#define C 28                    // cell size
#define GW 10
#define GH 20
#define WX ((W - GW * C) / 2)   // well on screen: 340..620
#define WY (TOP + 32)           //                 80..640
#define TICK_MS 20
#define REPEAT_AFTER 300        // hold A / B this long, then it repeats ...
#define REPEAT_MS 100           // ... every this often
#define KNOB_STEP 256           // knob: a sixteenth of a turn moves one cell
#define KNOB_DIR 1              // flip to -1 if turning the knob clockwise moves the piece left

typedef struct { int8_t x, y; } pt;
static const struct { int8_t n; pt c[4]; } SHAPE[7] = { // cells in an n x n box that turns around its middle
    {4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}}, // I
    {2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}}, // O
    {3, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}}, // T
    {3, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}}, // S
    {3, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}}, // Z
    {3, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}}, // J
    {3, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}}, // L
};

typedef struct {
    uint8_t well[GH][GW]; // 0 empty, else piece type + 1
    pt c[4];              // the falling piece: its cells in its n x n box at (x, y)
    int8_t type, n, x, y, next;
    int score, rows;
    bool over;
} game;
static game g; // written by the game, read by the drawer, under lock
static SemaphoreHandle_t lock, panel, shown;
static TaskHandle_t drawer;

static void dot(int x, int y)
{
    fb[(y * W + x) / 8] |= 0x80 >> (x & 7);
}

static void cell(int px, int py, int t) // one block of piece type t (0..6): a frame and a pattern of its own
{
    outline(px + 1, py + 1, C - 2, C - 2, 2);
    for (int j = 0; j < C - 6; j++)
        for (int i = 0; i < C - 6; i++) {
            bool on = t == 0 ? 1
                    : t == 1 ? (i / 2 + j / 2) & 1
                    : t == 2 ? j % 4 < 2
                    : t == 3 ? i % 4 < 2
                    : t == 4 ? (i + j) % 6 < 3
                    : t == 5 ? (i - j + 60) % 6 < 3
                    : i >= 7 && i < 15 && j >= 7 && j < 15;
            if (on) dot(px + 3 + i, py + 3 + j);
        }
}

static bool fits(const game *s, const pt *c, int x, int y)
{
    for (int k = 0; k < 4; k++) {
        int X = x + c[k].x, Y = y + c[k].y;
        if (X < 0 || X >= GW || Y < 0 || Y >= GH || s->well[Y][X]) return false;
    }
    return true;
}

static void label(int x, int y, const char *name, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    text(x, y, 3, name, true);
    text(x, y + 36, 4, s, true);
}

static bool draw(void) // g -> fb; returns true when the game-over box is shown
{
    game s;
    xSemaphoreTake(lock, portMAX_DELAY);
    s = g;
    xSemaphoreGive(lock);
    memset(fb, 0, sizeof fb);
    outline(WX - 7, WY - 7, GW * C + 14, GH * C + 14, 3);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if (s.well[y][x]) cell(WX + x * C, WY + y * C, s.well[y][x] - 1);
    if (!s.over) {
        int gy = s.y; // where the piece would land: thin outlines
        while (fits(&s, s.c, s.x, gy + 1)) gy++;
        for (int k = 0; k < 4; k++) {
            if (gy > s.y) outline(WX + (s.x + s.c[k].x) * C + 4, WY + (gy + s.c[k].y) * C + 4, C - 8, C - 8, 1);
            cell(WX + (s.x + s.c[k].x) * C, WY + (s.y + s.c[k].y) * C, s.type);
        }
    }
    text(72, 100, 3, "NEXT", true); // left: the next piece and the controls
    outline(56, 140, 4 * C + 32, 4 * C + 32, 3);
    int o = (4 - SHAPE[s.next].n) * C / 2;
    for (int k = 0; k < 4; k++)
        cell(72 + o + SHAPE[s.next].c[k].x * C, 156 + o + SHAPE[s.next].c[k].y * C, s.next);
    text(40, 440, 2, "A / B   move", true);
    text(40, 476, 2, "MENU -  turn", true);
    text(40, 512, 2, "MENU +  drop", true);
    label(680, 100, "SCORE", s.score); // right: the numbers
    label(680, 220, "ROWS", s.rows);
    label(680, 340, "LEVEL", s.rows / 10);
    if (s.over) {
        char a[16];
        int n = snprintf(a, sizeof a, "%d", s.score), bw = GW * C - 24;
        box(WX + 12, WY + 200, bw, 150, false);
        outline(WX + 12, WY + 200, bw, 150, 4);
        text(WX + (GW * C - 9 * 24) / 2, WY + 230, 3, "GAME OVER", true);
        text(WX + (GW * C - n * 32) / 2, WY + 290, 4, a, true);
    }
    return s.over;
}

static void draw_task(void *arg) // one frame per wake-up, of the latest state; moves made meanwhile show together
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(panel, portMAX_DELAY);
        bool done = draw();
        ui_frame(1);
        xSemaphoreGive(panel);
        if (done) xSemaphoreGive(shown);
    }
}

static int bag(void) // each of the seven pieces once in every seven, in a shuffled order
{
    static int b[7], left;
    if (!left) {
        for (int i = 0; i < 7; i++) b[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = esp_random() % (i + 1), t = b[i];
            b[i] = b[j], b[j] = t;
        }
        left = 7;
    }
    return b[--left];
}

static void spawn(void) // the next piece enters at the top; over when it does not fit
{
    g.type = g.next;
    g.next = bag();
    g.n = SHAPE[g.type].n;
    memcpy(g.c, SHAPE[g.type].c, sizeof g.c);
    g.x = (GW - g.n) / 2;
    g.y = 0;
    g.over = !fits(&g, g.c, g.x, g.y);
}

static void settle(void) // the piece joins the well, full rows clear
{
    static const int pts[5] = {0, 100, 300, 500, 800};
    for (int k = 0; k < 4; k++) g.well[g.y + g.c[k].y][g.x + g.c[k].x] = g.type + 1;
    int cleared = 0;
    for (int y = GH - 1; y >= 0; y--) {
        if (!memchr(g.well[y], 0, GW)) {
            memmove(g.well[1], g.well[0], y * GW);
            memset(g.well[0], 0, GW);
            cleared++;
            y++; // look at the row that moved down into this one
        }
    }
    g.score += pts[cleared] * (g.rows / 10 + 1);
    g.rows += cleared;
    spawn();
}

static bool shift(int dx)
{
    if (!fits(&g, g.c, g.x + dx, g.y)) return false;
    g.x += dx;
    return true;
}

static bool turn(void) // clockwise; if the turned piece hits something, try it a cell or two to the side
{
    pt r[4];
    for (int k = 0; k < 4; k++) r[k] = (pt){g.n - 1 - g.c[k].y, g.c[k].x};
    static const int kick[5] = {0, -1, 1, -2, 2};
    for (int i = 0; i < 5; i++)
        if (fits(&g, r, g.x + kick[i], g.y)) {
            memcpy(g.c, r, sizeof r);
            g.x += kick[i];
            return true;
        }
    return false;
}

static int fall_ms(int level) // 1000 ms per row at level 0, 15 % faster per level, never under 100 ms
{
    int t = 1000;
    while (level-- > 0 && t > 100) t = t * 85 / 100;
    return t < 100 ? 100 : t;
}

void app_blocks(void)
{
    lock = xSemaphoreCreateMutex();
    panel = xSemaphoreCreateMutex();
    shown = xSemaphoreCreateBinary();
    xTaskCreate(draw_task, "blocks draw", 4096, NULL, 5, &drawer);
    int ref = knob_angle(); // knob angle at the last step, -1 = no knob
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        memset(&g, 0, sizeof g);
        g.next = bag();
        spawn();
        xSemaphoreGive(lock);
        xTaskNotifyGive(drawer);
        btn_flush();
        xSemaphoreTake(shown, 0); // a stale give from a second game-over frame
        int fell = 0, hold[2] = {0, 0}; // ms since the last row fell; ms A / B held
        TickType_t t = xTaskGetTickCount();
        while (!g.over) {
            vTaskDelayUntil(&t, pdMS_TO_TICKS(TICK_MS));
            bool moved = false;
            int a = knob_angle();
            xSemaphoreTake(lock, portMAX_DELAY);
            for (int b; !g.over && (b = btn_get(0)) >= 0;) {
                if (b == BTN_A) moved |= shift(-1);
                else if (b == BTN_B) moved |= shift(1);
                else if (b == BTN_MINUS) moved |= turn();
                else if (b == BTN_PLUS) { // drop: 2 points per row
                    while (fits(&g, g.c, g.x, g.y + 1)) g.y++, g.score += 2;
                    settle();
                    fell = 0;
                    moved = true;
                }
            }
            int h = btn_held();
            for (int i = 0; i < 2; i++) { // hold to repeat; the press itself came through btn_get
                if (!(h & 1 << (BTN_A + i))) hold[i] = 0;
                else if ((hold[i] += TICK_MS) >= REPEAT_AFTER && (hold[i] - REPEAT_AFTER) % REPEAT_MS == 0)
                    moved |= shift(i ? 1 : -1);
            }
            if (a < 0 || ref < 0) ref = a;
            else
                for (int d; (d = ((a - ref) % 4096 + 6144) % 4096 - 2048) >= KNOB_STEP || d <= -KNOB_STEP;) {
                    if (!g.over) moved |= shift(d > 0 ? KNOB_DIR : -KNOB_DIR);
                    ref = (ref + (d > 0 ? KNOB_STEP : -KNOB_STEP) + 4096) % 4096;
                }
            if (!g.over && (fell += TICK_MS) >= fall_ms(g.rows / 10)) {
                fell = 0;
                if (fits(&g, g.c, g.x, g.y + 1)) g.y++;
                else settle();
                moved = true;
            }
            xSemaphoreGive(lock);
            if (moved) xTaskNotifyGive(drawer);
        }
        xTaskNotifyGive(drawer);
        xSemaphoreTake(shown, portMAX_DELAY); // the game-over box and the score are on the panel
        xSemaphoreTake(panel, portMAX_DELAY);
        ui_close();
        ms(3000);
        ui_wipe(); // a fresh session and less ghosting for the next game
        xSemaphoreGive(panel);
    }
}
