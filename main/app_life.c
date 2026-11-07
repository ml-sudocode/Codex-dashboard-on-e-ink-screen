// Conway's Game of Life: 60 x 39 = 2340 cells of 16 px below the top bar, edges wrap around. Nobody plays: a random
// soup runs one generation per frame (~0.5 s) until it dies out, settles (same as 1 or 2 generations ago) or hits
// MAX_GEN, then a fresh soup starts after a wipe. Any button goes home.
#include <string.h>
#include "esp_random.h"
#include "badge.h"

#define CELL 16
#define GW (W / CELL)
#define GH ((H - TOP) / CELL)
#define MAX_GEN 500 // ponytail: a cap instead of cycle detection beyond period 2 (gliders on a torus run forever)

static uint8_t g[3][GH][GW]; // this generation, the one before, the one before that

static void step(void)
{
    memcpy(g[2], g[1], sizeof g[1]);
    memcpy(g[1], g[0], sizeof g[0]);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) n += g[1][(y + dy + GH) % GH][(x + dx + GW) % GW];
            n -= g[1][y][x];
            g[0][y][x] = n == 3 || (n == 2 && g[1][y][x]);
        }
}

void app_life(void)
{
    for (;;) {
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) g[0][y][x] = esp_random() % 100 < 30; // 30 % alive
        for (int gen = 0; gen < MAX_GEN; gen++) {
            memset(fb, 0, sizeof fb);
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++)
                    if (g[0][y][x]) box(x * CELL + 1, TOP + y * CELL + 1, CELL - 2, CELL - 2, true);
            ui_frame(1);
            if (btn_get(0) >= 0) return;
            step();
            if (!memcmp(g[0], g[1], sizeof g[0]) || !memcmp(g[0], g[2], sizeof g[0])) break; // still or blinking
        }
        ms(3000); // let the last picture stand
        ui_wipe();
    }
}
