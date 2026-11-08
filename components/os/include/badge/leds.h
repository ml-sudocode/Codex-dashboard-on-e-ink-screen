// LED bar: 8 WS2812, LED 0 at the left end as you look at the badge, {r, g, b} each 0-255 (~12 is plenty: they run
// from 3V3). They keep their colours until set again, a reset included: the OS turns them off at boot and back home.
#pragma once
#include <stdint.h>

#define LEDS 8
void leds(const uint8_t rgb[LEDS][3]);
