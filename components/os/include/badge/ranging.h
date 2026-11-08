// Ranging (Radar and any app): badges running badge os find each other (ESP-NOW beacons, channel 1) and measure
// their distance with Wi-Fi FTM (802.11mc round-trip time); each badge shares its own ranges, so you also get the
// distances between others. The OS starts it at boot and keeps it on (APSTA, hidden SoftAP as the FTM responder,
// ESP-NOW on channel 1), so apps just call ranging_get. ranging_start/ranging_stop exist for apps that stopped it.
// Don't use ESP-NOW yourself while it runs; wifi_on/wifi_connect stop it first. Background: apps/radar/RADAR_RESEARCH.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t a[6], b[6]; // a measured b; a = ranging_me() for this badge's own ranges
    int16_t cm;         // distance, -1 = b heard but not measured yet; FTM: ~1-2 m typical error, 5-10 m outliers
    uint32_t age_ms;    // age of cm (or of the last beacon while cm is -1)
} range_t;

void ranging_start(void);
void ranging_stop(void);
int ranging_get(range_t *out, int max); // pairs heard of in the last 30 s; returns how many it wrote
void ranging_me(uint8_t mac[6]);        // this badge's id: its Wi-Fi station MAC (what other badges list it as)
bool ranging_name(const uint8_t mac[6], char name[13]); // that badge's username from Settings, false if none (yet)
void badge_id(char id[5]); // this badge's 4-hex-digit id, e.g. "9FCC": the last two bytes of ranging_me()
