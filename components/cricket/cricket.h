// Noisy Cricket SAO (github.com/yigitabi5444/noisy_cricket_sao, branch speaker) as the badge's speaker:
// an I2C client for its speaker registers, and an SWD programmer that puts the speaker firmware on a stock Cricket.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#define CRICKET_FW 1 // speaker firmware version embedded in this component (cricket_fw.bin)

// OS, at boot: the SAO I2C bus, and how to read an SAO port's GPIO1 in mV with the badge's internal pull-up
// (pullup = true) or pull-down, the pin floating again after (port 0-2 = SAO1-3; -1 on error).
void cricket_init(i2c_master_bus_handle_t bus, int (*gpio1_mv)(int port, bool pullup));

// -1 = no Cricket on I2C, 0 = Cricket with stock firmware (no speaker), else its speaker firmware version.
// Cheap: probes I2C at most every 2 s.
int cricket_version(void);

// SAO port (0-2) the Cricket sits in, or -1 if not exactly one port looks like one. Passive: it only reads each
// port's GPIO1 (the Cricket's SWCLK, 40k pull-down inside) through the badge's weak pulls; nothing is driven.
int cricket_port(void);

// Flash the embedded speaker firmware over SWD on that port's GPIO1 (SWCLK) / GPIO2 (SWDIO): halt, erase, program,
// verify, reset, then wait for the new firmware on I2C. ~10 s. progress (may be NULL) gets 0-100 and a short note.
// Returns NULL on success, else what failed.
const char *cricket_update(int port, void (*progress)(int pct, const char *what));

// Square wave, hz, ms (0 = until cricket_quiet), vol 0-255. Back to back calls just change the tone.
void cricket_tone(int hz, int ms, int vol);
void cricket_quiet(void); // stop host sound now (otherwise the Cricket goes back to chirping 1 s after the last)

// The OS snd_* calls, routed to the Cricket: mono 16-bit at any rate, played at 11.7 kHz 8-bit. write blocks while
// the Cricket's FIFO is full. start returns false if there is no speaker Cricket.
bool cricket_snd_start(int rate);
void cricket_snd_write(const int16_t *pcm, int n);
void cricket_snd_stop(void);
