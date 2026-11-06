// Pong between two badges over ESP-NOW, in real time: the host steps the game every STEP_MS in pixels and the
// panel shows the latest state as fast as it can (~1 frame/s). The badge with the lower MAC is the host: it owns
// the ball; the guest shows the host's state and sends its paddle. Both screens show their own player on the left.
// Any button starts pairing (press on both badges at once); in a game hold MENU + to move up, A to move down.
// Only a reboot ends a pairing; the other badge goes back to free after LINK_TIMEOUT without a packet.
// Pairing and heartbeat: link.h.
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "badge.h"
#include "link.h"

static const char *TAG = "pong";

// Tuning knobs. The panel lags ~1 s behind the game, so the ball has to be slow enough to react to.
#define STEP_MS 20   // game step
#define BALL_V 3     // ball px per step across (150 px/s: ~6 s from paddle to paddle)
#define BALL_DY 4    // steepest bounce, px per step up or down
#define PAD_V 5      // paddle px per step while held (250 px/s)
#define FRAME_MS 1100 // one panel frame: the hollow marker shows where the ball will be when it appears
#define PAUSE_MS 2500 // after a point: the ball waits in the middle
#define BALL 24
#define PADW 16
#define PADH 120
#define PADX 24      // paddle's distance from its edge
#define WIN 4        // first to 4: the LED bar has 4 LEDs per player
#define CHANNEL 1
#define LED_GPIO 38
#define LED_FLIP 0    // 1 if LED 0 turns out to be on the right as you look at the badge
#define LED_ON 12     // WS2812 brightness, 0-255

static TaskHandle_t game_task;
static link_t lk;
static game_t g; // host: the game; guest: the host's latest, but g.py[1] is its own paddle
static bool host;
static volatile bool in_pong;
static portMUX_TYPE gmux = portMUX_INITIALIZER_UNLOCKED;

// Ball y after moving dy per step for n steps, bouncing off top and bottom.
static int fold_y(int y, int dy, int n)
{
    int span = H - BALL, v = abs(y + dy * n) % (2 * span);
    return v > span ? 2 * span - v : v;
}

// mirror: the guest flips the court so its own paddle (player 1) is on the left. live: draw the ball.
static void draw(const game_t *s, bool mirror, bool live)
{
#define X(x, w) (mirror ? W - (x) - (w) : (x))
    memset(fb, 0, PLANE);
    for (int y = 0; y < H; y += 48) box(W / 2 - 4, y + 12, 8, 24, true); // centre line
    if (live) box(0, 0, 8, H, true); // your side: the left one, with a goal line
    for (int p = 0; p < 2; p++) {
        bool left = (p == 0) != mirror;
        int x = left ? PADX : W - PADX - PADW;
        if (left) box(x, s->py[p], PADW, PADH, true); // yours solid, the other badge's hollow
        else outline(x, s->py[p], PADW, PADH, 4);
        for (int k = 0; k < s->score[p]; k++) box(left ? W / 2 - 60 - 48 * k : W / 2 + 36 + 48 * k, 12, 24, 24, true);
    }
    if (!live) return;
    box(X(s->bx, BALL), s->by, BALL, BALL, true);
    if (!s->dx) return; // waiting to serve
    int n = FRAME_MS / STEP_MS, fx = MIN(MAX(s->bx + s->dx * n, PADX + PADW), W - PADX - PADW - BALL);
    outline(X(fx, BALL), fold_y(s->by, s->dy, n), BALL, BALL, 3); // where it is when this frame shows
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
    if (g->by < 0 || g->by > H - BALL) g->by = fold_y(g->by, 0, 0), g->dy = -g->dy; // wall
    int p = g->dx > 0, face = p ? W - PADX - PADW - BALL : PADX + PADW; // ball x where it touches paddle p
    bool crossed = p ? g->bx >= face && g->bx - g->dx < face : g->bx <= face && g->bx - g->dx > face;
    int off = g->by + BALL / 2 - (g->py[p] + PADH / 2); // ball centre from paddle centre
    if (crossed && abs(off) < (PADH + BALL) / 2) { // paddle: back, steeper the further from its centre
        g->bx = 2 * face - g->bx, g->dx = -g->dx;
        g->dy = off * BALL_DY / ((PADH + BALL) / 2);
    } else if (g->bx < -BALL || g->bx > W) { // missed: a point, then serve towards whoever lost it
        g->score[!p]++;
        *sdx = p ? BALL_V : -BALL_V;
        g->bx = (W - BALL) / 2, g->by = (H - BALL) / 2, g->dx = g->dy = 0;
        *serve_at = now + PAUSE_MS;
    }
}

// Renders the latest state as fast as the panel goes. After a point both badges close the panel session, so
// the reopen (~0.85 s) lands in the pause and the OS's forced reopen every 20 frames rarely hits a rally.
static void pong(void)
{
    ESP_LOGI(TAG, "%s, %s paddle", host ? "host" : "guest", host ? "left" : "right");
    in_pong = true;
    int frames = 0, points = 0;
    while (lk.state == L_PAIRED) {
        taskENTER_CRITICAL(&gmux);
        game_t s = g;
        taskEXIT_CRITICAL(&gmux);
        draw(&s, !host, true);
        ui_frame(frames++ ? 1 : 3);
        if (s.score[0] + s.score[1] != points) ui_close(), points = s.score[0] + s.score[1];
    }
    ui_close();
    in_pong = false;
    ESP_LOGI(TAG, "over after %d frames: peer gone", frames);
}

// ---- LED bar: 8x WS2812, RMT at 10 MHz (0: 0.3 us high, 0.9 us low; 1: 0.9 us high, 0.3 us low).
static rmt_channel_handle_t led_ch;
static rmt_encoder_handle_t led_enc;

static void led_init(void)
{
    ESP_ERROR_CHECK(rmt_new_tx_channel(&(rmt_tx_channel_config_t){.gpio_num = LED_GPIO, .clk_src = RMT_CLK_SRC_DEFAULT,
                                                                   .resolution_hz = 10000000, .mem_block_symbols = 64,
                                                                   .trans_queue_depth = 1}, &led_ch));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&(rmt_bytes_encoder_config_t){.bit0 = {.level0 = 1, .duration0 = 3, .duration1 = 9},
                                                                         .bit1 = {.level0 = 1, .duration0 = 9, .duration1 = 3},
                                                                         .flags.msb_first = 1}, &led_enc));
    ESP_ERROR_CHECK(rmt_enable(led_ch));
}

// LED 0 is the left end, as the court is seen on this badge.
static void leds(const uint8_t rgb[8][3])
{
    static uint8_t grb[24];
    uint8_t out[24];
    for (int i = 0; i < 8; i++) {
        uint8_t *o = out + 3 * (LED_FLIP ? 7 - i : i);
        o[0] = rgb[i][1], o[1] = rgb[i][0], o[2] = rgb[i][2];
    }
    if (!memcmp(out, grb, sizeof grb)) return;
    memcpy(grb, out, sizeof grb);
    rmt_transmit(led_ch, led_enc, grb, sizeof grb, &(rmt_transmit_config_t){0});
    rmt_tx_wait_all_done(led_ch, 10);
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
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
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
    int8_t sdx = 0;
    for (;;) {
        rx_t r;
        bool got = xQueueReceive(rxq, &r, pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time() / 1000;
        while (btn_get(0) >= 0) link_press(&lk, now); // any button pairs; ignored once paired
        if (in_pong && now >= next_step) {
            next_step = now + STEP_MS;
            int held = btn_held(), v = (held >> BTN_A & 1) - (held >> BTN_PLUS & 1);
            uint32_t rnd = esp_random();
            taskENTER_CRITICAL(&gmux);
            g.py[!host] = MIN(MAX(g.py[!host] + v * PAD_V, 0), H - PADH);
            if (host) pong_step(&g, now, &sdx, &serve_at, rnd);
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
                g = (game_t){.bx = (W - BALL) / 2, .by = (H - BALL) / 2, .py = {(H - PADH) / 2, (H - PADH) / 2}};
                sdx = esp_random() & 1 ? BALL_V : -BALL_V;
                serve_at = now + PAUSE_MS;
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
                g.py[1] = MIN(MAX(r.p.g.py[1], 0), H - PADH);
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
void app_pong(void)
{
    game_task = xTaskGetCurrentTaskHandle();
    led_init();
    rxq = xQueueCreate(16, sizeof(rx_t));
    radio_start();
    xTaskCreate(link_task, "link", 4096, NULL, 6, NULL);
    for (;;) { // unpaired: an empty court, then wait for the link task to pair
        draw(&(game_t){.py = {(H - PADH) / 2, (H - PADH) / 2}}, false, false);
        ui_frame(3);
        ui_close();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pong();
    }
}
