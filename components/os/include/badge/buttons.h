// Buttons as printed on the badge, two in each top corner: MENU + above A on the left, MENU - above B on the right.
// Polled every 20 ms; every new press goes into a queue of 8. The press that opened your app has been flushed.
// MENU + and MENU - held together for 0.4 s go home from any app (the OS restarts the badge between frames), so a
// game should not need that chord.
#pragma once

enum { BTN_PLUS, BTN_MINUS, BTN_A, BTN_B };
int btn_get(int timeout_ms); // next press, -1 on timeout; timeout -1 waits forever
void btn_flush(void);
int btn_held(void); // buttons held right now: bit (1 << BTN_x) set while pressed, updated every 20 ms
