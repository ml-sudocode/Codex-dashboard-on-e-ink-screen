// badge os internals: what the OS modules share, and what main/ calls to boot and to run the home screen.
// Apps include badge/badge.h, never this.
#pragma once
#include <stdbool.h>

// Boot (main/main.c), in this order.
void display_init(void); // panel pins and SPI, the panel lock, the restart hook that lets the panel finish
void sao_init(void);     // the SAO I2C bus (logs what answers), the Cricket, the knob
void buttons_init(void); // the 20 ms polling task
void leds_init(void);    // all off: they still show whatever ran before the reset

// The home screen (main/home.c).
void os_app(const char *name); // an app starts: its name in the top bar, MENU+/- go home; NULL = back home
bool ranging_running(void);

// Shared by the OS modules.
extern volatile bool in_app;     // an app runs (os_app)
extern const int sao_gpio[3][2]; // GPIO1, GPIO2 of SAO1..3
