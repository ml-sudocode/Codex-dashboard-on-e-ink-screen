// Rocks: a ship in open space below the OS top bar. Hold MENU + / MENU - to turn left / right, hold A to thrust,
// B fires (press, or hold for a steady stream). The AS5600 knob add-on, when turned, sets the heading directly.
// Rocks drift and wrap around the edges; a hit rock splits in two smaller ones, the smallest vanish. A cleared
// field shakes the ghosts off the panel and brings more rocks. Three ships; game over shows the score for 3 s,
// then a new game starts by itself.
// Like Snake: the game ticks on its own clock (25/s), a second task hands the latest state to the panel whenever
// it takes a frame (~3.5-4/s).
// Everything is drawn as 3 px outlines that wrap around the play field, so a frame changes few pixels.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "badge/badge.h"

#define PH (H - TOP)  // play field: W x PH, y = 0 is the row under the top bar
#define TICK_MS 40
#define DT (TICK_MS / 1000.0f)
#define TURN 2.4f     // rad/s while MENU +/- is held
#define THRUST 360.0f // px/s^2
#define VMAX 360.0f   // px/s
#define DRAG 0.5f     // fraction of speed kept per second while coasting
#define SHOT_V 640.0f // px/s, on top of the ship's speed
#define SHOT_LIFE 25  // ticks (1 s)
#define SHOT_GAP 6    // ticks between shots while B is held
#define SHIELD 75     // ticks of invulnerability after a (re)spawn
#define RESPAWN 38    // ticks without a ship after a crash
#define KNOB_DIR 1    // flip to -1 if turning the knob clockwise turns the ship the wrong way
#define NV 10         // corners per rock
#define MAXR 64
#define MAXB 8

typedef struct { float x, y, vx, vy; int size; uint8_t r[NV]; } rock_t; // size 0 = no rock
typedef struct { float x, y, vx, vy; int life; } shot_t;
static const float RAD[] = {0, 18, 36, 64}; // rock radius by size
static const int PTS[] = {0, 100, 50, 20};  // score by size

static struct {
    float x, y, vx, vy, a; // ship; a = heading, 0 = up, clockwise
    int gone, shield;      // ticks until the ship is back; ticks of invulnerability left
    bool thrust;
    rock_t rk[MAXR];
    shot_t sh[MAXB];
    int score, lives, over, t; // over: true once the last ship crashed
} g, d; // g: the game's state; d: the drawer's copy
static SemaphoreHandle_t lock, panel, shown;
static TaskHandle_t drawer;

static float frand(float lo, float hi) { return lo + (hi - lo) * (esp_random() % 10000) / 10000.0f; }
static float wrap(float v, float span) { return v - span * floorf(v / span); }
static float near(float d, float span) { return wrap(d + span / 2, span) - span / 2; } // shortest way round
static bool hit(float x0, float y0, float x1, float y1, float r)
{
    float dx = near(x1 - x0, W), dy = near(y1 - y0, PH);
    return dx * dx + dy * dy < r * r;
}

// ---- Drawing: 3x3 dots along lines, every dot wrapped onto the play field
static void dot(int x, int y)
{
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            int px = ((x + i) % W + W) % W, py = ((y + j) % PH + PH) % PH + TOP;
            fb[(py * W + px) >> 3] |= 0x80 >> (px & 7);
        }
}

static void line(float x0, float y0, float x1, float y1)
{
    int n = fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= n; i++) dot(lroundf(x0 + (x1 - x0) * i / n), lroundf(y0 + (y1 - y0) * i / n));
}

// polygon of n corners given in the ship's frame (f forward, s to the right), heading a, at x, y
static void shape(float x, float y, float a, int n, const float (*p)[2])
{
    float c = cosf(a), s = sinf(a);
    for (int k = 0; k < n; k++) {
        const float *u = p[k], *v = p[(k + 1) % n];
        line(x + u[0] * s + u[1] * c, y - u[0] * c + u[1] * s, x + v[0] * s + v[1] * c, y - v[0] * c + v[1] * s);
    }
}

static void ship(float x, float y, float a, float k) // k: scale
{
    const float p[4][2] = {{26 * k, 0}, {-18 * k, 15 * k}, {-9 * k, 0}, {-18 * k, -15 * k}};
    shape(x, y, a, 4, p);
}

static void draw(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    d = g;
    xSemaphoreGive(lock);
    memset(fb, 0, sizeof fb);
    for (int i = 0; i < MAXR; i++) {
        rock_t *r = &d.rk[i];
        if (!r->size) continue;
        float p[NV][2];
        for (int k = 0; k < NV; k++) {
            float q = 2 * (float)M_PI * k / NV, rr = RAD[r->size] * r->r[k] / 255;
            p[k][0] = rr * cosf(q), p[k][1] = rr * sinf(q);
        }
        shape(r->x, r->y, 0, NV, p);
    }
    for (int i = 0; i < MAXB; i++)
        if (d.sh[i].life) line(d.sh[i].x, d.sh[i].y, d.sh[i].x - d.sh[i].vx * 0.02f, d.sh[i].y - d.sh[i].vy * 0.02f);
    if (!d.gone) {
        ship(d.x, d.y, d.a, 1);
        if (d.thrust) {
            const float f[3][2] = {{-13, 6}, {-30 - (d.t & 1) * 8, 0}, {-13, -6}};
            shape(d.x, d.y, d.a, 3, f);
        }
        if (d.shield) {
            float p[12][2];
            for (int k = 0; k < 12; k++) p[k][0] = 36 * cosf(k * (float)M_PI / 6), p[k][1] = 36 * sinf(k * (float)M_PI / 6);
            shape(d.x, d.y, 0, 12, p);
        }
    }
    char s[24];
    snprintf(s, sizeof s, "%d", d.score);
    text(12, TOP + 12, 3, s, true);
    for (int i = 0; i < d.lives; i++) ship(W - 30 - i * 36, 32, 0, 0.6f);
    if (d.over) {
        int n = snprintf(s, sizeof s, " GAME OVER  %d ", d.score), x = (W - n * 32) / 2;
        box(x - 8, H / 2 - 32, n * 32 + 16, 64, false);
        outline(x - 8, H / 2 - 32, n * 32 + 16, 64, 4);
        text(x, H / 2 - 16, 4, s, true);
    }
}

static void draw_task(void *arg) // one frame per wake-up, of the latest state
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(panel, portMAX_DELAY);
        draw();
        ui_frame(1);
        xSemaphoreGive(panel);
        if (d.over) xSemaphoreGive(shown);
    }
}

// ---- Game
static void add_rock(float x, float y, int size, float v)
{
    for (int i = 0; i < MAXR; i++) {
        rock_t *r = &g.rk[i];
        if (r->size) continue;
        float q = frand(0, 2 * (float)M_PI);
        *r = (rock_t){.x = x, .y = y, .vx = v * cosf(q), .vy = v * sinf(q), .size = size};
        for (int k = 0; k < NV; k++) r->r[k] = frand(170, 255);
        return;
    }
}

static void wave(int level)
{
    for (int n = level < 6 ? 3 + level : 8; n--;) {
        float x, y;
        do x = frand(0, W), y = frand(0, PH); while (hit(x, y, g.x, g.y, 260));
        add_rock(x, y, 3, frand(40, 70 + 8 * level));
    }
}

static void spawn(void)
{
    g.x = W / 2, g.y = PH / 2, g.vx = g.vy = 0, g.a = 0, g.gone = 0, g.shield = SHIELD;
}

static void run(void)
{
    lock = xSemaphoreCreateMutex();
    panel = xSemaphoreCreateMutex();
    shown = xSemaphoreCreateBinary();
    xTaskCreate(draw_task, "rocks draw", 6144, NULL, 5, &drawer);
    ui_keys("turn left", "turn right", "thrust", "fire");
    int ref = knob_angle(); // knob angle when it last steered, -1 = no knob
    for (;;) {
        memset(&g, 0, sizeof g);
        g.lives = 3;
        spawn();
        int level = 1, cool = 0;
        wave(level);
        btn_flush();
        TickType_t t = xTaskGetTickCount();
        for (;;) {
            vTaskDelayUntil(&t, pdMS_TO_TICKS(TICK_MS));
            int held = btn_held(), b;
            bool fire = false;
            while ((b = btn_get(0)) >= 0) fire |= b == BTN_B; // a short tap between two ticks still fires
            int a = knob_angle();
            xSemaphoreTake(lock, portMAX_DELAY);
            g.t++;
            // ship
            if (a >= 0 && ref >= 0 && fabsf(near(a - ref, 4096)) > 24) ref = a; // moved: the knob steers
            if (a < 0) ref = -1;
            else if (ref < 0) ref = a;
            if (!g.gone) {
                g.a += ((held >> BTN_MINUS & 1) - (held >> BTN_PLUS & 1)) * TURN * DT;
                if (a >= 0 && ref == a) g.a = KNOB_DIR * a * 2 * (float)M_PI / 4096;
                g.thrust = held >> BTN_A & 1;
                if (g.thrust) {
                    g.vx += THRUST * DT * sinf(g.a), g.vy -= THRUST * DT * cosf(g.a);
                    float v = hypotf(g.vx, g.vy);
                    if (v > VMAX) g.vx *= VMAX / v, g.vy *= VMAX / v;
                } else g.vx *= powf(DRAG, DT), g.vy *= powf(DRAG, DT);
                g.x = wrap(g.x + g.vx * DT, W), g.y = wrap(g.y + g.vy * DT, PH);
                if (g.shield) g.shield--;
                if (cool) cool--;
                if ((fire || (held >> BTN_B & 1)) && !cool)
                    for (int i = 0; i < MAXB; i++)
                        if (!g.sh[i].life) {
                            float sx = sinf(g.a), sy = -cosf(g.a);
                            g.sh[i] = (shot_t){g.x + 26 * sx, g.y + 26 * sy, g.vx + SHOT_V * sx, g.vy + SHOT_V * sy,
                                               SHOT_LIFE};
                            cool = SHOT_GAP;
                            break;
                        }
            } else if (!--g.gone) spawn();
            // shots and rocks
            for (int i = 0; i < MAXB; i++) {
                shot_t *s = &g.sh[i];
                if (s->life) s->life--, s->x = wrap(s->x + s->vx * DT, W), s->y = wrap(s->y + s->vy * DT, PH);
            }
            bool any = false;
            for (int i = 0; i < MAXR; i++) {
                rock_t *r = &g.rk[i];
                if (!r->size) continue;
                r->x = wrap(r->x + r->vx * DT, W), r->y = wrap(r->y + r->vy * DT, PH);
                for (int j = 0; j < MAXB && r->size; j++) {
                    shot_t *s = &g.sh[j];
                    if (!s->life || !hit(s->x, s->y, r->x, r->y, RAD[r->size])) continue;
                    s->life = 0;
                    g.score += PTS[r->size];
                    int size = r->size - 1;
                    float v = hypotf(r->vx, r->vy) * 1.3f + 15;
                    r->size = 0;
                    if (size) add_rock(r->x, r->y, size, v), add_rock(r->x, r->y, size, v);
                }
                if (r->size && !g.gone && !g.shield && hit(g.x, g.y, r->x, r->y, RAD[r->size] * 0.85f + 12)) {
                    g.gone = RESPAWN, g.thrust = false;
                    if (!--g.lives) g.over = 1;
                }
                any |= r->size;
            }
            bool over = g.over, clear = !any && !g.gone;
            xSemaphoreGive(lock);
            xTaskNotifyGive(drawer);
            if (over) break;
            if (clear) { // a breather: shake off the ghosts, then the next wave
                xSemaphoreTake(panel, portMAX_DELAY);
                ui_wipe();
                xSemaphoreGive(panel);
                xSemaphoreTake(lock, portMAX_DELAY);
                wave(++level);
                memset(g.sh, 0, sizeof g.sh);
                g.shield = SHIELD;
                xSemaphoreGive(lock);
                t = xTaskGetTickCount();
            }
        }
        xSemaphoreTake(shown, portMAX_DELAY); // the score is on the panel
        xSemaphoreTake(panel, portMAX_DELAY);
        ui_close();
        ms(3000);
        ui_wipe();
        xSemaphoreGive(panel);
    }
}

static void icon(int x, int y) // a little ship among rocks
{
    icon_base(x, y);
    for (int r = 0; r < 24; r++) box(x + 88 - r / 2, y + 76 + r, r + 1, 1, false); // ship, nose up
    outline(x + 28, y + 28, 40, 36, 6), outline(x + 116, y + 40, 32, 28, 6), outline(x + 112, y + 120, 44, 36, 6);
    box(x + 36, y + 124, 20, 20, false);
}

const app_t app_rocks = {"Asteroids", icon, run};
