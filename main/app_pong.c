// Pong between two badges over ESP-NOW. The game runs on a 10 x 7 grid of 96 px cells, one step per panel frame.
// The badge with the lower MAC is the host: it owns the game and steps it; the guest shows the host's state and
// sends its paddle. Both screens show their own player on the left. Pairing and heartbeat: link.h.
// Any button starts pairing (press on both badges at once); in a game MENU + moves your paddle up, A down.
// Only a reboot ends a pairing; the other badge goes back to free after LINK_TIMEOUT without a packet.
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

#define CELL 96
#define PW (W / CELL) // 10 x 7
#define PH (H / CELL)
#define PAD 2         // paddle height in cells
#define WIN 4         // first to 4: the LED bar has 4 LEDs per player
#define CHANNEL 1
#define LED_GPIO 38
#define LED_FLIP 0    // 1 if LED 0 turns out to be on the right as you look at the badge
#define LED_ON 12     // WS2812 brightness, 0-255

static TaskHandle_t game_task;
static link_t lk;
static game_t g;     // host: the game; guest: the host's latest, but g.py[1] is its own paddle
static uint8_t seen; // last tick the guest has shown (host: as the guest reported it)
static bool host;
static volatile bool in_pong;
static portMUX_TYPE gmux = portMUX_INITIALIZER_UNLOCKED;

// mirror: the guest flips the court so its own paddle (player 1) is on the left.
static void draw(const game_t *s, bool mirror)
{
    memset(fb, 0, PLANE);
    for (int y = 0; y < H; y += 48) box(W / 2 - 4, y + 12, 8, 24, true); // centre line
    if (s->tick) box(0, 0, 8, H, true); // your side: the left one, with a goal line
    for (int p = 0; p < 2; p++) {
        bool left = (p == 0) != mirror;
        int x = (left ? 0 : PW - 1) * CELL + 32, y = s->py[p] * CELL + 8, h = PAD * CELL - 16;
        if (left) box(x, y, 32, h, true); // yours solid, the other badge's hollow
        else outline(x, y, 32, h, 8);
        for (int k = 0; k < s->score[p]; k++) box(left ? W / 2 - 60 - 48 * k : W / 2 + 36 + 48 * k, 12, 24, 24, true);
    }
    if (!s->tick) return; // not served yet
    box((mirror ? PW - 1 - s->bx : s->bx) * CELL + 12, s->by * CELL + 12, CELL - 24, CELL - 24, true);
    if (s->bx == 0 || s->bx == PW - 1) return; // a point: no next position
    int nx = s->bx + s->dx, ny = s->by + s->dy;
    if (ny < 0 || ny >= PH) ny = s->by - s->dy;
    outline((mirror ? PW - 1 - nx : nx) * CELL + 18, ny * CELL + 18, CELL - 36, CELL - 36, 6); // where it goes next
}

static void serve(game_t *g, int dir)
{
    g->bx = dir > 0 ? PW / 2 - 1 : PW / 2;
    g->by = esp_random() % PH;
    g->dx = dir;
    g->dy = esp_random() & 1 ? 1 : -1;
}

// Host only: one game step.
static void pong_tick(game_t *g)
{
    if (!++g->tick) g->tick = 1; // 0 means "not started"
    if (g->bx == 0 || g->bx == PW - 1) { // last step was a point: serve towards whoever lost it
        if (g->score[0] == WIN || g->score[1] == WIN) g->score[0] = g->score[1] = 0;
        serve(g, g->bx ? 1 : -1);
        return;
    }
    int y = g->by + g->dy;
    if (y < 0 || y >= PH) g->dy = -g->dy, y = g->by + g->dy; // wall
    int x = g->bx + g->dx;
    if (x == 0 || x == PW - 1) {
        int p = x != 0;
        if (y >= g->py[p] && y < g->py[p] + PAD) g->dx = -g->dx, x = g->bx + g->dx; // paddle: back again
        else g->score[!p]++; // missed: the ball goes into the gutter
    }
    g->bx = x;
    g->by = y;
}

// Lockstep at the panel's ~1 frame/s: the host steps only after the guest has shown the last step, so both
// players see every step for a whole frame. After a point frame both badges close the panel session, so the
// reopen (~0.85 s) lands in that pause and the OS's forced reopen every 20 frames rarely hits a rally.
static void pong(void)
{
    ESP_LOGI(TAG, "%s, %s paddle", host ? "host" : "guest", host ? "left" : "right");
    in_pong = true;
    int frames = 0;
    for (uint8_t drawn = 0; lk.state == L_PAIRED;) {
        taskENTER_CRITICAL(&gmux);
        bool go = host ? seen == g.tick : g.tick != drawn;
        if (go && host) pong_tick(&g);
        game_t s = g;
        taskEXIT_CRITICAL(&gmux);
        if (!go) {
            ms(20);
            continue;
        }
        draw(&s, !host);
        ui_frame(frames++ ? 1 : 3);
        if (s.bx == 0 || s.bx == PW - 1) ui_close(); // a point: restart the session in the pause
        drawn = s.tick;
        if (!host) seen = drawn;
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
        int col = host ? s.bx : PW - 1 - s.bx;
        if (s.tick) memset(c[(col * 7 + 4) / 9], LED_ON, 3);
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

// Every 10 ms or per packet: buttons, link.h, sends, applies game packets, drives the LEDs.
static void link_task(void *arg)
{
    int was = L_FREE;
    int64_t next_tx = 0, red_until = 0;
    for (;;) {
        rx_t r;
        bool got = xQueueReceive(rxq, &r, pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time() / 1000;
        for (int b; (b = btn_get(0)) >= 0;) {
            link_press(&lk, now); // ignored once paired
            if ((b == BTN_PLUS || b == BTN_A) && in_pong) {
                taskENTER_CRITICAL(&gmux);
                uint8_t *y = &g.py[!host];
                *y = b == BTN_PLUS ? MAX(*y - 1, 0) : MIN(*y + 1, PH - PAD);
                taskEXIT_CRITICAL(&gmux);
            }
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
                g = (game_t){.py = {2, 2}};
                seen = 0;
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
                g.py[1] = MIN(r.p.g.py[1], PH - PAD);
                seen = r.p.seen;
            } else {
                uint8_t mine = g.py[1];
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
            p.seen = seen;
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
        draw(&(game_t){.py = {2, 2}}, false);
        ui_frame(3);
        ui_close();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pong();
    }
}
