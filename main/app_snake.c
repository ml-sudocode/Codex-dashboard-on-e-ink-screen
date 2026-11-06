// Snake: 20 x 14 grid of 48 px cells, a move every MOVE_MS, walls wrap around. Nonstop: after
// the snake bites itself its length shows for 3 s, then a new game starts by itself. MENU - turns clockwise,
// MENU + counter-clockwise; presses queue, one turn per move. The AS5600 knob add-on steers too: an eighth
// of a turn either way is one turn. A reboot goes home.
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "badge.h"

#define CELL 48
#define GW (W / CELL)
#define GH (H / CELL)
#define MOVE_MS 350 // the speed: one move every MOVE_MS; frames come as fast as the changed rows allow
#define KNOB_STEP 512 // knob: an eighth of a turn turns the snake once
#define KNOB_DIR 1    // flip to -1 if turning the knob clockwise turns the snake the wrong way

static void fill_cell(int c, int inset, bool ink)
{
    box(c % GW * CELL + inset, c / GW * CELL + inset, CELL - 2 * inset, CELL - 2 * inset, ink);
}

void app_snake(void)
{
    enum { N = GW * GH };
    static int q[N]; // ring buffer of body cells, q[tl] = tail .. q[hd] = head
    static bool occ[N];
    int ref = knob_angle(); // knob angle at the last turn, -1 = no knob
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
        int64_t next = esp_timer_get_time() / 1000 + MOVE_MS;
        for (;;) {
            // one queued turn per move, so two quick presses make two turns within a frame
            int b = btn_get(0), turn = b == BTN_MINUS ? 1 : b == BTN_PLUS ? -1 : 0;
            int a = knob_angle(), d = ((a - ref) % 4096 + 6144) % 4096 - 2048; // -2048..2047
            if (a < 0 || ref < 0) ref = a;
            else if (!turn && (d >= KNOB_STEP || d <= -KNOB_STEP)) { // one knob step per move, the rest waits
                turn = d > 0 ? KNOB_DIR : -KNOB_DIR;
                ref = (ref + (d > 0 ? KNOB_STEP : -KNOB_STEP) + 4096) % 4096;
            }
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
            // a frame costs ~949 ms x (lowest changed row / 672): when it runs past a move, the next moves catch
            // up undrawn, so the game keeps its speed and the snake jumps a few cells on the low rows
            int64_t now = esp_timer_get_time() / 1000;
            if (now > next + 2000) next = now; // a session reopen: do not race to catch up
            if (now < next) {
                ui_frame(1);
                now = esp_timer_get_time() / 1000;
                if (now < next) ms(next - now);
            }
            next += MOVE_MS;
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
