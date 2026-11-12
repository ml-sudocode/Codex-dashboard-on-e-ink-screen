#pragma once
#include "board_feed.h"

#define CB_SEEN_MAX 32
#define CB_SEEN_TEXT (CB_SEEN_MAX * 33 + 1)
typedef struct { unsigned count; char ids[CB_SEEN_MAX][33]; } cb_seen_t;

// NVS format: one 32-character hex event ID per line, oldest first.
bool cb_seen_load(cb_seen_t *seen, const char *saved);
void cb_seen_save(const cb_seen_t *seen, char out[CB_SEEN_TEXT]);
// Only fresh terminal snapshots produce alerts. An event on REPLY_READY can
// flash without making that row urgent. Protect active IDs from eviction.
unsigned cb_seen_observe(cb_seen_t *seen, const cb_feed_t *feed);
