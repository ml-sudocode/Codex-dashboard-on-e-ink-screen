#pragma once
#include "board_feed.h"

typedef enum { CB_STREAM_NONE, CB_STREAM_VALID, CB_STREAM_INVALID } cb_stream_result_t;
typedef struct {
    char body[CB_BODY_MAX + 1];
    size_t length, header_match;
    bool active;
} cb_stream_t;

// Accept bytes across arbitrary USB packet boundaries. A new header resynchronizes
// even after a cut-off line or oversized frame. Only complete END\n frames commit.
void cb_stream_reset(cb_stream_t *stream);
cb_stream_result_t cb_stream_byte(cb_stream_t *stream, char byte, cb_feed_t *out);
