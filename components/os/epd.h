// Panel driver, private to the OS (display.c uses it). Every call needs the panel idle: the CoG drops commands and RAM
// writes while BUSY is low.
#pragma once
#include <stdint.h>
#include "badge/display.h"

void epd_init(void);                    // pins and the SPI bus, at boot
void epd_open(const uint8_t *black);    // a full session on the fast black/white table, black in RAM (NULL = white)
void epd_load(const uint8_t *black);    // black into 0x10, 0x11 all white (NULL = white)
int epd_pull(int timeout_ms);           // run the waveform once on what is in RAM, wait for it; ms spent
void epd_close(void);                   // DC-DC soft shutdown: ends a session

// Calm partial updates: see epd.c.
extern uint8_t part_shown[PLANE];
extern int part_n;
void part_open(void);
void part_start(const uint8_t *black);
void part_wait(void);
void epd_sleep(void);
