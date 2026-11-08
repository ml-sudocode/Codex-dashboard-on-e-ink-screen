// The knob add-on: an AS5600 magnetic encoder on an SAO port (I2C 0x36; its analog out on SAO1's GPIO1 also works).
// Plug it in any time.
#pragma once

int knob_angle(void); // 0-4095 per turn, -1 if none or no magnet
