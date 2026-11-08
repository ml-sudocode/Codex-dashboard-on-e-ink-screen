// Panel driver, private to the OS: display.c is its only user. A session: epd_reset, the RAM planes (epd_load) and
// epd_start in either order, pulls, epd_close. The CoG drops every command and RAM write while BUSY is low, so after
// epd_frame, epd_wait before anything else.
#pragma once
#include <stdint.h>

enum { EPD_FAST, EPD_CALM }; // waveforms: the badge team's fast black/white table (ui_wipe), the calm transition one

void epd_init(void);                                   // boot: pins and the SPI bus
void epd_reset(void);                                  // reset pulse and booster setting: a session starts
void epd_load(const uint8_t *p10, const uint8_t *p11); // both RAM planes of both CoGs, whole panel; NULL = all white
void epd_start(int table);                             // register sequence, the table into 0xB0, DC-DC up
int epd_pull(int timeout_ms);                          // one pull on what is in RAM, waited for: ms spent
void epd_frame(const uint8_t *next);                   // calm session: next into 0x10, one pull started (~0.24 s)
void epd_wait(void);                                   // until both CoGs are done: BUSY polled every 1 ms tick
void epd_close(void);                                  // DC-DC soft shutdown: the session ends
