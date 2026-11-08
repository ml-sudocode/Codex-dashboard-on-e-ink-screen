// Pong between two badges over ESP-NOW, in real time: the host steps the game every STEP_MS in pixels and the
// panel shows the latest state as fast as it can (~3.5-4 frames/s). The badge with the lower MAC is the host: it owns
// the ball; the guest shows the host's state and sends its paddle. Both screens show their own player on the left.
// Any button starts pairing (press on both badges at once); in a game hold MENU + to move up, A to move down.
// The AS5600 knob add-on (knob_angle(), plug in any time) moves the paddle too.
// Only a reboot ends a pairing; the other badge goes back to free after LINK_TIMEOUT without a packet.
// Pairing and heartbeat: link.h.
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "badge/badge.h"
#include "link.h"

static const char *TAG = "pong";

// Tuning knobs. The panel lags ~0.2 s behind the game, so the ball has to be slow enough to react to.
#define STEP_MS 15   // game step
#define BALL_V 2     // ball px per step across (133 px/s: ~7 s from paddle to paddle)
#define BALL_DY 3    // steepest bounce, px per step up or down
#define PAD_V 4      // paddle px per step while held (267 px/s)
#define PAUSE_MS 2500 // after a point: the ball waits in the middle
#define CH (H - TOP)  // court height: game y runs 0..CH, drawn TOP px down (the OS top bar is above)
#define BALL 24
#define PADW 16
#define PADH 120
#define PADX 24      // paddle's distance from its edge
#define TRAIL 20     // ball positions kept for the trail, one per step (0.3 s, a frame or two)
#define WIN 4        // first to 4: the LED bar has 4 LEDs per player
#define CHANNEL 1
#define LED_ON 12     // WS2812 brightness, 0-255
#define KNOB_TURN (CH - PADH) // paddle px per full knob turn: one turn crosses the court
#define KNOB_DIR 1    // -1 if turning clockwise moves the paddle up instead of down

static TaskHandle_t game_task;
static link_t lk;
static game_t g; // host: the game; guest: the host's latest, but g.py[1] is its own paddle
static bool host;
static volatile bool in_pong;
static int16_t trail[TRAIL][2]; // ring of recent ball positions, trail_n is the next slot
static int trail_n;
static portMUX_TYPE gmux = portMUX_INITIALIZER_UNLOCKED;

// Ball y after moving dy per step for n steps, bouncing off top and bottom.
static int fold_y(int y, int dy, int n)
{
    int span = CH - BALL, v = abs(y + dy * n) % (2 * span);
    return v > span ? 2 * span - v : v;
}

// mirror: the guest flips the court so its own paddle (player 1) is on the left. live: draw the ball.
static void draw(const game_t *s, const int16_t (*tr)[2], int ntr, bool mirror, bool live)
{
#define X(x, w) (mirror ? W - (x) - (w) : (x))
    memset(fb, 0, PLANE);
    for (int y = TOP; y < H; y += 48) box(W / 2 - 4, y + 12, 8, 24, true); // centre line
    if (live) box(0, TOP, 8, CH, true); // your side: the left one, with a goal line
    for (int p = 0; p < 2; p++) {
        bool left = (p == 0) != mirror;
        int x = left ? PADX : W - PADX - PADW;
        if (left) box(x, TOP + s->py[p], PADW, PADH, true); // yours solid, the other badge's hollow
        else outline(x, TOP + s->py[p], PADW, PADH, 4);
        for (int k = 0; k < s->score[p]; k++) box(left ? W / 2 - 60 - 48 * k : W / 2 + 36 + 48 * k, TOP + 12, 24, 24, true);
    }
    if (!live) return;
    for (int k = 0; k < MIN(ntr, TRAIL); k += 5) // the trail: a dot every 5 steps, where the ball has been
        box(X(tr[k][0] + BALL / 2 - 4, 8), TOP + tr[k][1] + BALL / 2 - 4, 8, 8, true);
    box(X(s->bx, BALL), TOP + s->by, BALL, BALL, true);
#undef X
}

// Host only: one game step. sdx: direction of the next serve; serve_at: 0 while the ball is in play.
static void pong_step(game_t *g, int64_t now, int8_t *sdx, int64_t *serve_at, uint32_t rnd)
{
    if (*serve_at) {
        if (now < *serve_at) return;
        if (g->score[0] == WIN || g->score[1] == WIN) g->score[0] = g->score[1] = 0;
        g->dx = *sdx, g->dy = (1 + rnd / 2 % BALL_DY) * (rnd & 1 ? 1 : -1), *serve_at = 0;
    }
    g->bx += g->dx;
    g->by += g->dy;
    if (g->by < 0 || g->by > CH - BALL) g->by = fold_y(g->by, 0, 0), g->dy = -g->dy; // wall
    int p = g->dx > 0, face = p ? W - PADX - PADW - BALL : PADX + PADW; // ball x where it touches paddle p
    bool crossed = p ? g->bx >= face && g->bx - g->dx < face : g->bx <= face && g->bx - g->dx > face;
    int off = g->by + BALL / 2 - (g->py[p] + PADH / 2); // ball centre from paddle centre
    if (crossed && abs(off) < (PADH + BALL) / 2) { // paddle: back, steeper the further from its centre, +-1 at random
        g->bx = 2 * face - g->bx, g->dx = -g->dx;
        int dy = off * BALL_DY / ((PADH + BALL) / 2) + (int)(rnd >> 8 & 3) % 3 - 1; // plus a random nudge
        g->dy = MIN(MAX(dy, -BALL_DY), BALL_DY) ?: (rnd >> 10 & 1 ? 1 : -1); // never flat: no endless rally
    } else if (g->bx < -BALL || g->bx > W) { // missed: a point, then serve towards whoever lost it
        g->score[!p]++;
        *sdx = p ? BALL_V : -BALL_V;
        g->bx = (W - BALL) / 2, g->by = (CH - BALL) / 2, g->dx = g->dy = 0;
        *serve_at = now + PAUSE_MS;
    }
}

// Renders the latest state as fast as the panel goes (~3.5-4 frames/s); the game runs on the link task's clock.
static void pong(void)
{
    ESP_LOGI(TAG, "%s, %s paddle", host ? "host" : "guest", host ? "left" : "right");
    in_pong = true;
    ui_keys("up", NULL, "down", NULL);
    int frames = 0;
    game_t last = {.score = {255}}; // what the last frame showed (nothing yet)
    while (lk.state == L_PAIRED) {
        taskENTER_CRITICAL(&gmux);
        game_t s = g;
        int16_t tr[TRAIL][2];
        int ntr = trail_n;
        memcpy(tr, trail, sizeof tr);
        taskEXIT_CRITICAL(&gmux);
        if (!memcmp(&s, &last, sizeof s)) { // nothing moved (waiting to serve, paddles still): don't spin
            ms(10);
            continue;
        }
        draw(&s, tr, ntr, !host, true);
        ui_frame(1); // returns while the panel works: the next frame is drawn meanwhile
        last = s, frames++;
    }
    ui_close();
    in_pong = false;
    ui_keys("pair", "pair", "pair", "pair");
    ESP_LOGI(TAG, "over after %d frames: peer gone", frames);
}

// Free: off (red for 0.5 s after a failed pairing or a lost peer). Seeking: blue. Playing: my points green from
// the left, the peer's red from the right, the ball's column white; all green / all red when someone has won.
static void led_update(int64_t now, int64_t red_until)
{
    uint8_t c[8][3] = {0};
    int fill = now < red_until ? 0 : lk.state == L_SEEK ? 2 : -1;
    if (lk.state == L_PAIRED) {
        taskENTER_CRITICAL(&gmux);
        game_t s = g;
        taskEXIT_CRITICAL(&gmux);
        int me = !host;
        fill = s.score[me] == WIN ? 1 : s.score[!me] == WIN ? 0 : -1;
        for (int k = 0; k < s.score[me]; k++) c[k][1] = LED_ON;
        for (int k = 0; k < s.score[!me]; k++) c[7 - k][0] = LED_ON;
        int x = (host ? s.bx : W - BALL - s.bx) + BALL / 2;
        memset(c[MIN(MAX(x * 8 / W, 0), 7)], LED_ON, 3); // live ball position
    }
    if (fill >= 0)
        for (int i = 0; i < 8; i++) memset(c[i], 0, 3), c[i][fill] = LED_ON;
    leds(c);
}

// ---- Radio: ESP-NOW only (no AP), packets go through a queue to the link task.
typedef struct { uint8_t src[6]; pkt_t p; } rx_t;
static QueueHandle_t rxq;

static void radio_rx(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    rx_t r;
    if (len != sizeof r.p) return;
    memcpy(r.src, info->src_addr, 6);
    memcpy(&r.p, data, len);
    xQueueSend(rxq, &r, 0);
}

static void radio_start(void)
{
    wifi_on(); // the OS's driver: station mode, no network joined
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // modem sleep would drop packets
    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(radio_rx));
    esp_now_peer_info_t bc = {.ifidx = WIFI_IF_STA};
    memcpy(bc.peer_addr, link_bcast, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&bc));
    esp_read_mac(lk.me, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "ESP-NOW on channel %d, me " MACSTR, CHANNEL, MAC2STR(lk.me));
}

// Every 10 ms or per packet: buttons, game step, link.h, sends, applies game packets, drives the LEDs.
static void link_task(void *arg)
{
    int was = L_FREE;
    int64_t next_tx = 0, red_until = 0, next_step = 0, serve_at = 0;
    int knob_last = -1, knob_acc = 0; // knob: last angle, turned but not yet moved (x 4096)
    int8_t sdx = 0;
    for (;;) {
        rx_t r;
        bool got = xQueueReceive(rxq, &r, pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time() / 1000;
        while (btn_get(0) >= 0) link_press(&lk, now); // any button pairs; ignored once paired
        if (!in_pong) next_step = now;
        for (; in_pong && now >= next_step; next_step += STEP_MS) { // fixed rate, whenever this loop wakes
            int held = btn_held(), v = (held >> BTN_A & 1) - (held >> BTN_PLUS & 1);
            static int was_held;
            if (held != was_held) ESP_LOGI(TAG, "held 0x%x, knob %d", was_held = held, knob_angle()); // hold-to-move check
            int k = knob_angle();
            if (k >= 0 && knob_last >= 0) knob_acc += ((k - knob_last + 6144) % 4096 - 2048) * KNOB_DIR * KNOB_TURN;
            knob_last = k;
            v = v * PAD_V + knob_acc / 4096; // relative: turning moves the paddle from wherever it is
            knob_acc %= 4096;
            uint32_t rnd = esp_random();
            taskENTER_CRITICAL(&gmux);
            g.py[!host] = MIN(MAX(g.py[!host] + v, 0), CH - PADH);
            if (host) pong_step(&g, now, &sdx, &serve_at, rnd);
            if (g.dx) { // the trail skips the wait before a serve
                int16_t *t = trail[trail_n++ % TRAIL];
                t[0] = g.bx, t[1] = g.by;
            }
            taskEXIT_CRITICAL(&gmux);
        }
        if (got) link_rx(&lk, r.src, &r.p, now);
        link_tick(&lk, now);
        if (lk.state != was) {
            ESP_LOGI(TAG, "link %d -> %d, peer " MACSTR, was, lk.state, MAC2STR(lk.peer));
            if (lk.state == L_PAIRED) {
                esp_now_peer_info_t pi = {.ifidx = WIFI_IF_STA};
                memcpy(pi.peer_addr, lk.peer, 6);
                esp_now_add_peer(&pi);
                taskENTER_CRITICAL(&gmux);
                host = memcmp(lk.me, lk.peer, 6) < 0;
                g = (game_t){.bx = (W - BALL) / 2, .by = (CH - BALL) / 2, .py = {(CH - PADH) / 2, (CH - PADH) / 2}};
                sdx = esp_random() & 1 ? BALL_V : -BALL_V;
                serve_at = now + PAUSE_MS;
                trail_n = 0;
                taskEXIT_CRITICAL(&gmux);
                xTaskNotifyGive(game_task);
            }
            if (was == L_PAIRED) esp_now_del_peer(lk.peer);
            if (lk.state == L_FREE) red_until = now + 500;
            was = lk.state;
        }
        if (got && lk.state == L_PAIRED && r.p.type == P_GAME && !memcmp(r.src, lk.peer, 6)) {
            taskENTER_CRITICAL(&gmux);
            if (host) {
                g.py[1] = MIN(MAX(r.p.g.py[1], 0), CH - PADH);
            } else {
                int16_t mine = g.py[1];
                g = r.p.g;
                g.py[1] = mine;
            }
            taskEXIT_CRITICAL(&gmux);
        }
        pkt_t p;
        const uint8_t *to = link_pkt(&lk, now, &p);
        if (to && now >= next_tx) {
            taskENTER_CRITICAL(&gmux);
            p.g = g;
            taskEXIT_CRITICAL(&gmux);
            esp_now_send(to, (const uint8_t *)&p, sizeof p);
            next_tx = now + (lk.state == L_SEEK ? LINK_PAIR_MS : LINK_GAME_MS);
        }
        led_update(now, red_until);
    }
}

// Never returns: a reboot leaves Pong (and ends a pairing).
static void run(void)
{
    game_task = xTaskGetCurrentTaskHandle();
    rxq = xQueueCreate(16, sizeof(rx_t));
    radio_start();
    xTaskCreate(link_task, "link", 4096, NULL, 6, NULL);
    ui_keys("pair", "pair", "pair", "pair"); // press on both badges at once
    for (;;) { // unpaired: an empty court, then wait for the link task to pair
        draw(&(game_t){.py = {(CH - PADH) / 2, (CH - PADH) / 2}}, NULL, 0, false, false);
        ui_frame(3);
        ui_close();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pong();
    }
}

static void icon(int x, int y) // two paddles and a ball
{
    icon_base(x, y);
    box(x + 28, y + 40, 16, 64, false);
    box(x + 132, y + 76, 16, 64, false);
    box(x + 80, y + 68, 20, 20, false);
    for (int k = 0; k < 6; k++) box(x + 86, y + 18 + k * 26, 6, 12, false);
}

const app_t app_pong = {"Pong", icon, run};
