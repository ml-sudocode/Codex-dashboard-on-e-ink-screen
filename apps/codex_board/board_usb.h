#pragma once
#include "board_stream.h"

bool cb_usb_start(void);
// Processes at most 1024 bytes per call, without waiting for input. Complete
// frames return immediately; unread bytes are kept for the next call.
cb_stream_result_t cb_usb_poll(cb_feed_t *out);
void cb_usb_ack(const char *revision);
