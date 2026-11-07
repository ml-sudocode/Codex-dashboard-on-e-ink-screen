// Snake: 20 x 13 grid of 48 px cells, a move every MOVE_MS, walls wrap around. Nonstop: after the snake bites
// itself its length shows for 3 s, then a new game starts by itself. MENU - turns clockwise, MENU + counter-
// clockwise; presses queue, one turn per move. The AS5600 knob add-on steers too: an eighth of a turn either way
// is one turn. A reboot goes home.
// The game runs on its own clock; a second task shows the latest board whenever the panel is free (a frame costs
// ~949 ms x lowest changed row / 672), so the game speed does not depend on the refresh speed.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "badge.h"

#define CELL 48
#define GW (W / CELL)
#define GH ((H - TOP) / CELL) // 13 rows below the OS top bar
#define N (GW * GH)
#define MOVE_MS 350   // the game speed
#define KNOB_STEP 512 // knob: an eighth of a turn turns the snake once
#define KNOB_DIR 1    // flip to -1 if turning the knob clockwise turns the snake the wrong way

enum { EMPTY, BODY, FOOD };
static uint8_t board[N];  // the game's state, written by the game, read by the drawer
static int over;          // the final length once the snake has crashed, 0 while playing
static SemaphoreHandle_t lock, panel, shown;
static TaskHandle_t drawer;

static void fill_cell(int c, int inset, bool ink)
{
    box(c % GW * CELL + inset, TOP + c / GW * CELL + inset, CELL - 2 * inset, CELL - 2 * inset, ink);
}

static int draw(void) // board -> fb; returns the length shown in the crash box, 0 for none
{
    uint8_t b[N];
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(b, board, N);
    int len = over;
    xSemaphoreGive(lock);
    memset(fb, 0, sizeof fb);
    for (int c = 0; c < N; c++)
        if (b[c]) fill_cell(c, b[c] == BODY ? 3 : 16, true);
    if (len) {
        char s[16];
        int n = snprintf(s, sizeof s, " LENGTH %d ", len), x = (W - n * 32) / 2;
        box(x - 8, H / 2 - 32, n * 32 + 16, 64, false);
        outline(x - 8, H / 2 - 32, n * 32 + 16, 64, 4);
        text(x, H / 2 - 16, 4, s, true);
    }
    return len;
}

static void draw_task(void *arg) // one frame per wake-up, of the latest board; moves made meanwhile show together
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

void app_snake(void)
{
    static int q[N]; // ring buffer of body cells, q[tl] = tail .. q[hd] = head
    lock = xSemaphoreCreateMutex();
    panel = xSemaphoreCreateMutex();
    shown = xSemaphoreCreateBinary();
    xTaskCreate(draw_task, "snake draw", 4096, NULL, 5, &drawer);
    int ref = knob_angle(); // knob angle at the last turn, -1 = no knob
    for (;;) {
        int hd = 2, tl = 0, dir = 1, food; // dir clockwise from up: 0 up, 1 right, 2 down, 3 left
        memset(board, EMPTY, N);
        for (int k = 0; k <= hd; k++) board[q[k] = GH / 2 * GW + 4 + k] = BODY;
        do food = esp_random() % N; while (board[food]);
        board[food] = FOOD;
        over = 0;
        xSemaphoreTake(panel, portMAX_DELAY);
        draw();
        ui_frame(3);
        xSemaphoreGive(panel);
        btn_flush();
        TickType_t t = xTaskGetTickCount();
        for (;;) {
            vTaskDelayUntil(&t, pdMS_TO_TICKS(MOVE_MS));
            // one queued turn per move, so two quick presses make two turns in a row
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
            bool eat = h == food, end = false;
            xSemaphoreTake(lock, portMAX_DELAY);
            if (!eat) { // the tail moves away first, so following it is allowed
                board[q[tl]] = EMPTY;
                tl = (tl + 1) % N;
            }
            if (board[h] == BODY) end = true;
            else {
                hd = (hd + 1) % N;
                board[q[hd] = h] = BODY;
                if (eat) {
                    if ((hd - tl + N) % N == N - 1) end = true; // board full: you win
                    else {
                        do food = esp_random() % N; while (board[food]);
                        board[food] = FOOD;
                    }
                }
            }
            if (end) over = (hd - tl + N) % N + 1;
            xSemaphoreGive(lock);
            xTaskNotifyGive(drawer);
            if (end) break;
        }
        xSemaphoreTake(shown, portMAX_DELAY); // the crash and the length are on the panel
        xSemaphoreTake(panel, portMAX_DELAY);
        ui_close();
        ms(3000);
        ui_wipe(); // a fresh session and less ghosting for the next game
        xSemaphoreGive(panel);
    }
}
