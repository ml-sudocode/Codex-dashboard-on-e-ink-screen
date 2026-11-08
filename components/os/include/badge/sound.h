// Sound. A Noisy Cricket SAO with the speaker firmware (components/cricket) plays it when one is plugged in.
// Otherwise it goes out as PDM (1 bit at ~64x the sample rate) on an SAO port's two GPIOs, GPIO1 = data and
// GPIO2 = clock: Settings "snd_sao" picks the port, SAO2 by default. Simplest add-on there: a passive piezo from GPIO1
// to GND. Louder: a PDM-input amp (e.g. MAX98358) on both pins, or an analog amp (e.g. PAM8302) on the data pin
// through an RC low-pass (~1k + 10 nF).
// snd_start(rate) -> false if there is no sound; then snd_write mono 16-bit samples at that rate (blocks while the
// ~30 ms queue is full; gaps play silence); snd_stop when done (the OS also stops it back home).
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool snd_start(int rate);
void snd_write(const int16_t *pcm, int n);
void snd_stop(void);
