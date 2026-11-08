// Wi-Fi. The OS runs ranging (badge/ranging.h) on every screen, which keeps the radio busy. An app that calls wifi_on
// or wifi_connect takes the radio over: ranging stops until the app returns home or the badge restarts. Done with the
// network but staying in the app: wifi_off() then ranging_start(), so other badges' Radar sees this one again.
#pragma once
#include <stdbool.h>

void wifi_on(void);                // radio on, station mode, no network joined (ESP-NOW apps: set the channel next)
bool wifi_connect(int timeout_ms); // wifi_on + join the saved Wi-Fi ("ssid"/"pass"); true once there is an IP
void wifi_off(void);               // radio off
