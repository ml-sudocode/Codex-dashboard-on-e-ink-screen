// Battery: the LiPo's charge from the supply voltage on GPIO14 and a typical LiPo curve (good to ~10 %).
#pragma once

int battery_pct(void); // 0-100 on battery, -1 on USB power, -2 unknown; a failed read (ADC2 vs Wi-Fi) repeats the last
