#pragma once
#include "board_feed.h"

unsigned cb_pages(const cb_feed_t *feed);
void cb_view(const cb_feed_t *feed, unsigned page, const char *notice, bool usb);
void cb_setup_view(const char *notice, bool usb);
