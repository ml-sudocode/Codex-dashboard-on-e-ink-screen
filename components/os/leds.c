// LED bar: 8x WS2812 on GPIO38, RMT at 10 MHz (0: 0.3 us high, 0.9 us low; 1: 0.9 us high, 0.3 us low).
// The LEDs keep their colours until told otherwise, a reset included: the OS turns them off at boot and back home.
#include <string.h>
#include "driver/rmt_tx.h"
#include "badge/badge.h"
#include "badge/os.h"

#define LED_FLIP 0 // 1 if LED 0 turns out to be on the right as you look at the badge
static rmt_channel_handle_t led_ch;
static rmt_encoder_handle_t led_enc;

static void led_send(const uint8_t grb[3 * LEDS])
{
    rmt_transmit(led_ch, led_enc, grb, 3 * LEDS, &(rmt_transmit_config_t){0});
    rmt_tx_wait_all_done(led_ch, 10);
}

void leds_init(void)
{
    ESP_ERROR_CHECK(rmt_new_tx_channel(&(rmt_tx_channel_config_t){.gpio_num = 38, .clk_src = RMT_CLK_SRC_DEFAULT,
                                                                   .resolution_hz = 10000000, .mem_block_symbols = 64,
                                                                   .trans_queue_depth = 1}, &led_ch));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&(rmt_bytes_encoder_config_t){.bit0 = {.level0 = 1, .duration0 = 3, .duration1 = 9},
                                                                         .bit1 = {.level0 = 1, .duration0 = 9, .duration1 = 3},
                                                                         .flags.msb_first = 1}, &led_enc));
    ESP_ERROR_CHECK(rmt_enable(led_ch));
    static const uint8_t off[3 * LEDS];
    led_send(off);
}

void leds(const uint8_t rgb[LEDS][3])
{
    static uint8_t grb[3 * LEDS]; // what the LEDs show (all off after led_init)
    uint8_t out[3 * LEDS];
    for (int i = 0; i < LEDS; i++) {
        uint8_t *o = out + 3 * (LED_FLIP ? LEDS - 1 - i : i);
        o[0] = rgb[i][1], o[1] = rgb[i][0], o[2] = rgb[i][2];
    }
    if (!memcmp(out, grb, sizeof grb)) return;
    memcpy(grb, out, sizeof grb);
    led_send(grb);
}
