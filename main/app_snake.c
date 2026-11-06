// Snake: 20 x 14 grid of 48 px cells, one move per panel frame (~1.07 s), walls wrap around. Nonstop: after
// the snake bites itself its length shows for 3 s, then a new game starts by itself. MENU - turns clockwise,
// MENU + counter-clockwise; one turn per move, the last press wins. A reboot goes home.
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "badge.h"

#define CELL 48
#define GW (W / CELL)
#define GH (H / CELL)

static void fill_cell(int c, int inset, bool ink)
{
    box(c % GW * CELL + inset, c / GW * CELL + inset, CELL - 2 * inset, CELL - 2 * inset, ink);
}

void app_snake(void)
{
    enum { N = GW * GH };
    static int q[N]; // ring buffer of body cells, q[tl] = tail .. q[hd] = head
    static bool occ[N];
    for (;;) {
        int hd = 2, tl = 0, dir = 1, food; // dir clockwise from up: 0 up, 1 right, 2 down, 3 left
        memset(fb, 0, sizeof fb);
        memset(occ, 0, sizeof occ);
        for (int k = 0; k <= hd; k++) {
            q[k] = GH / 2 * GW + 4 + k;
            occ[q[k]] = true;
            fill_cell(q[k], 3, true);
        }
        do food = esp_random() % N; while (occ[food]);
        fill_cell(food, 16, true);
        btn_flush();
        ui_frame(3);
        for (;;) {
            int turn = 0;
            for (int b; (b = btn_get(0)) >= 0;)
                if (b == BTN_MINUS) turn = 1;
                else if (b == BTN_PLUS) turn = -1;
            dir = (dir + turn + 4) % 4;
            int x = (q[hd] % GW + (dir == 1) - (dir == 3) + GW) % GW;
            int y = (q[hd] / GW + (dir == 2) - (dir == 0) + GH) % GH;
            int h = y * GW + x;
            bool eat = h == food;
            if (!eat) { // the tail moves away first, so following it is allowed
                occ[q[tl]] = false;
                fill_cell(q[tl], 0, false);
                tl = (tl + 1) % N;
            }
            if (occ[h]) break;
            hd = (hd + 1) % N;
            q[hd] = h;
            occ[h] = true;
            fill_cell(h, 3, true);
            if (eat) {
                if ((hd - tl + N) % N == N - 1) break; // board full: you win
                do food = esp_random() % N; while (occ[food]);
                fill_cell(food, 16, true);
            }
            ui_frame(1);
        }
        char s[16];
        int n = snprintf(s, sizeof s, " LENGTH %d ", (hd - tl + N) % N + 1);
        box((W - n * 32) / 2 - 8, H / 2 - 32, n * 32 + 16, 64, false);
        outline((W - n * 32) / 2 - 8, H / 2 - 32, n * 32 + 16, 64, 4);
        text((W - n * 32) / 2, H / 2 - 16, 4, s, true);
        ui_frame(1); // the crash and the length, in the frame time the old code spent idle
        ui_close();
        ms(3000);
        ui_wipe(); // a fresh session and less ghosting for the next game
    }
}
