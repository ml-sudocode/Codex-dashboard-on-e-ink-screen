// Aliens ("Saucer Siege"): a 8 x 5 formation of three kinds of aliens marches side to side, steps down at the edges
// and speeds up as it thins out. Hold A / B to move the gun left / right, MENU + or MENU - to fire (one shot in the air
// at a time; holding fires again as soon as it lands). The AS5600 knob add-on moves the gun too: a full turn is the
// screen width. Three lives; a cleared wave brings a lower, faster one. Game over shows the score for 3 s, then a new
// game starts by itself.
// The game runs on its own clock (TICK_MS); a second task shows the latest state whenever the panel is free, as in
// app_snake.c, so the game speed does not depend on the refresh speed.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "badge.h"

#define TICK_MS 50
#define COLS 8
#define ROWS 5
#define NA (COLS * ROWS)
#define S 4             // sprite pixel size: 12 x 8 aliens are 48 x 32 px
#define AW (12 * S)
#define AH (8 * S)
#define CW 80           // formation cell
#define RH 56
#define DX 16           // a march step
#define DY 24           // a step down at the edge
#define GW (16 * S)     // the gun, 64 x 32
#define GH (8 * S)
#define PY (H - 64)     // gun top
#define GUN_V 12        // px per tick while A / B is held
#define SHOT_V 32       // player shot, px per tick
#define BOMB_V 12       // alien shots, px per tick
#define NB 3            // alien shots in the air at most
#define BOOM_TICKS 10   // a hit alien shows a burst this long (about one frame)
#define DEAD_TICKS 30   // a hit gun freezes the game this long
#define KNOB_DIR 1      // flip to -1 if turning the knob clockwise moves the gun the wrong way

// Sprites: '#' is ink. Two frames each, swapped on every march step.
static const char *const kinds[3][2][8] = {
    { // horned, top row, 30 points
        { "#..........#", ".#..####..#.", "..########..", ".##.####.##.", "############", "..##....##..",
          ".##..##..##.", "##........##" },
        { "..#......#..", "..#.####.#..", "..########..", ".##.####.##.", "############", "..##....##..",
          "...##..##...", "..##....##.." },
    },
    { // jelly, rows 2-3, 20 points
        { "....####....", "..##.##.##..", ".##########.", "############", "#.#.#..#.#.#", "#.#.#..#.#.#",
          ".#..#..#..#.", "#...#..#...#" },
        { "....####....", "..##.##.##..", ".##########.", "############", ".#.#.##.#.#.", ".#.#.##.#.#.",
          "..#..##..#..", ".#...##...#." },
    },
    { // eye, rows 4-5, 10 points
        { "....####....", "..########..", ".###....###.", "###..##..###", ".###....###.", "..########..",
          ".#.#.##.#.#.", "#.#......#.#" },
        { "....####....", "..########..", ".###....###.", "###.##...###", ".###....###.", "..########..",
          "..#.#..#.#..", ".#.#....#.#." },
    },
};
static const char *const gun[8] = { "......####......", ".....######.....", "....########....", ".##############.",
                                    "################", "################", "####.######.####", "###..........###" };
static const char *const burst[8] = { "#..#....#..#", ".#..#..#..#.", "..#......#..", "##........##",
                                      "..#......#..", ".#..#..#..#.", "#..#....#..#", "....#..#...." };

static struct {
    uint8_t alive[NA], boom[NA];
    int fx, fy, dir, frame, march_ms; // formation top left, march direction, sprite frame, time to the next step
    int px, sx, sy;                   // gun x; player shot (sy < 0: none)
    int bx[NB], by[NB];               // alien shots (by < 0: none)
    int score, lives, wave, dead, over;
} g; // the game's state, written by the game, read by the drawer
static SemaphoreHandle_t lock, panel, shown;
static TaskHandle_t drawer;

static void sprite(const char *const *rows, int x, int y)
{
    for (int r = 0; r < 8; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#') box(x + c * S, y + r * S, S, S, true);
}

static int kind(int row) { return row == 0 ? 0 : row < 3 ? 1 : 2; }

static int draw(void) // g -> fb; returns nonzero once the game-over box is in it
{
    xSemaphoreTake(lock, portMAX_DELAY);
    typeof(g) s = g;
    xSemaphoreGive(lock);
    memset(fb, 0, sizeof fb);
    char t[32];
    snprintf(t, sizeof t, "SCORE %d", s.score);
    text(16, TOP + 8, 3, t, true);
    snprintf(t, sizeof t, "WAVE %d", s.wave);
    text(W / 2 - 72, TOP + 8, 3, t, true);
    for (int k = 0; k < s.lives; k++) // lives left, the gun in play included
        for (int r = 0; r < 8; r++)
            for (int c = 0; gun[r][c]; c++)
                if (gun[r][c] == '#') box(W - 16 - (k + 1) * 40 + c * 2, TOP + 12 + r * 2, 2, 2, true);
    for (int i = 0; i < NA; i++) {
        int x = s.fx + i % COLS * CW, y = s.fy + i / COLS * RH;
        if (s.alive[i]) sprite(kinds[kind(i / COLS)][s.frame], x, y);
        else if (s.boom[i]) sprite(burst, x, y);
    }
    if (s.sy >= 0) box(s.sx - 2, s.sy, 4, 24, true);
    for (int k = 0; k < NB; k++)
        if (s.by[k] >= 0) { // a zigzag bolt
            box(s.bx[k] - 4, s.by[k], 4, 8, true);
            box(s.bx[k], s.by[k] + 8, 4, 8, true);
            box(s.bx[k] - 4, s.by[k] + 16, 4, 8, true);
        }
    if (s.dead) sprite(burst, s.px + 8, PY);
    else sprite(gun, s.px, PY);
    box(0, H - 20, W, 4, true); // the ground
    if (s.over) {
        int n = snprintf(t, sizeof t, " GAME OVER  SCORE %d ", s.score), x = (W - n * 32) / 2;
        box(x - 8, H / 2 - 32, n * 32 + 16, 64, false);
        outline(x - 8, H / 2 - 32, n * 32 + 16, 64, 4);
        text(x, H / 2 - 16, 4, t, true);
    }
    return s.over;
}

static void draw_task(void *arg) // one frame per wake-up, of the latest state; ticks made meanwhile show together
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

static void new_wave(void)
{
    g.wave++;
    memset(g.alive, 1, NA);
    memset(g.boom, 0, NA);
    g.fx = (W - (COLS - 1) * CW - AW) / 2;
    g.fy = TOP + 56 + (g.wave - 1 < 5 ? g.wave - 1 : 5) * DY; // each wave starts a bit lower
    g.dir = 1;
    g.march_ms = 0;
    g.sy = -1;
    for (int k = 0; k < NB; k++) g.by[k] = -1;
}

static bool hits(int x, int y, int w, int h, int rx, int ry, int rw, int rh)
{
    return x < rx + rw && rx < x + w && y < ry + rh && ry < y + h;
}

static void tick(int fire, int move) // one TICK_MS step of the game
{
    if (g.dead) { // the gun was hit: everything waits
        if (--g.dead == 0) {
            if (g.lives == 0) g.over = 1;
            for (int k = 0; k < NB; k++) g.by[k] = -1;
        }
        return;
    }
    g.px += move;
    if (g.px < 0) g.px = 0;
    if (g.px > W - GW) g.px = W - GW;
    if (fire && g.sy < 0) g.sx = g.px + GW / 2, g.sy = PY - 24;

    int n = 0, lo = COLS, hi = -1, bottom = 0;
    for (int i = 0; i < NA; i++) {
        if (g.boom[i]) g.boom[i]--;
        if (!g.alive[i]) continue;
        n++;
        if (i % COLS < lo) lo = i % COLS;
        if (i % COLS > hi) hi = i % COLS;
        bottom = g.fy + i / COLS * RH + AH;
    }
    if (n == 0) { // wave cleared, once the last burst has shown
        bool booms = false;
        for (int i = 0; i < NA; i++) booms |= g.boom[i];
        if (!booms) new_wave();
        return;
    }

    // the formation marches: fewer aliens and later waves march faster
    g.march_ms -= TICK_MS;
    if (g.march_ms <= 0) {
        g.march_ms = (60 + n * 22) * 6 / (5 + g.wave);
        g.frame ^= 1;
        int nx = g.fx + g.dir * DX;
        if (nx + lo * CW < 8 || nx + hi * CW + AW > W - 8) g.fy += DY, g.dir = -g.dir, bottom += DY;
        else g.fx = nx;
        if (bottom >= PY) g.lives = 0, g.dead = DEAD_TICKS; // they landed
    }

    if (g.sy >= 0) { // the player's shot
        g.sy -= SHOT_V;
        if (g.sy < TOP + 40) g.sy = -1;
        for (int i = 0; i < NA && g.sy >= 0; i++)
            if (g.alive[i] && hits(g.sx - 2, g.sy, 4, 24 + SHOT_V, g.fx + i % COLS * CW, g.fy + i / COLS * RH, AW, AH)) {
                g.alive[i] = 0;
                g.boom[i] = BOOM_TICKS;
                g.score += 30 - 10 * kind(i / COLS);
                g.sy = -1;
            }
    }

    for (int k = 0; k < NB; k++) { // alien shots fall; a free slot may fire from the lowest alien of a random column
        if (g.by[k] >= 0) {
            g.by[k] += BOMB_V;
            if (g.by[k] > H - 44) g.by[k] = -1;
            else if (hits(g.bx[k] - 4, g.by[k], 8, 24, g.px + 4, PY + 8, GW - 8, GH - 8)) {
                g.by[k] = -1;
                g.lives--;
                g.dead = DEAD_TICKS;
            }
        } else if ((int)(esp_random() % 100) < 2 + g.wave) {
            int c = esp_random() % COLS;
            for (int r = ROWS - 1; r >= 0; r--)
                if (g.alive[r * COLS + c]) {
                    g.bx[k] = g.fx + c * CW + AW / 2;
                    g.by[k] = g.fy + r * RH + AH;
                    break;
                }
        }
    }
}

void app_aliens(void)
{
    lock = xSemaphoreCreateMutex();
    panel = xSemaphoreCreateMutex();
    shown = xSemaphoreCreateBinary();
    xTaskCreate(draw_task, "aliens draw", 4096, NULL, 5, &drawer);
    ui_keys("FIRE", "FIRE", "LEFT", "RIGHT");
    int ref = knob_angle(); // knob angle the gun last followed, -1 = no knob
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        memset(&g, 0, sizeof g);
        g.lives = 3;
        g.px = (W - GW) / 2;
        new_wave();
        xSemaphoreGive(lock);
        xSemaphoreTake(panel, portMAX_DELAY);
        draw();
        ui_frame(1);
        xSemaphoreGive(panel);
        btn_flush();
        TickType_t t = xTaskGetTickCount();
        while (!g.over) {
            vTaskDelayUntil(&t, pdMS_TO_TICKS(TICK_MS));
            int held = btn_held(), fire = held & (1 << BTN_PLUS | 1 << BTN_MINUS), b;
            while ((b = btn_get(0)) >= 0) fire |= b == BTN_PLUS || b == BTN_MINUS; // taps shorter than a tick
            int move = ((held >> BTN_B & 1) - (held >> BTN_A & 1)) * GUN_V;
            int a = knob_angle(), d = ((a - ref) % 4096 + 6144) % 4096 - 2048; // -2048..2047
            if (a < 0 || ref < 0) ref = a;
            else if (d >= 8 || d <= -8) move += KNOB_DIR * d * (W - GW) / 4096, ref = a; // small jitter waits
            xSemaphoreTake(lock, portMAX_DELAY);
            tick(fire, move);
            xSemaphoreGive(lock);
            xTaskNotifyGive(drawer);
        }
        xSemaphoreTake(shown, portMAX_DELAY); // the game-over box is on the panel
        xSemaphoreTake(panel, portMAX_DELAY);
        ui_close();
        ms(3000);
        ui_wipe(); // a fresh session and less ghosting for the next game
        xSemaphoreGive(panel);
    }
}
