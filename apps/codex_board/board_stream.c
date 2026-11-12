#include "board_stream.h"
#include <string.h>

void cb_stream_reset(cb_stream_t *s)
{
    s->length = s->header_match = 0;
    s->active = false;
}

cb_stream_result_t cb_stream_byte(cb_stream_t *s, char byte, cb_feed_t *out)
{
    static const char header[] = "CODEXBOARD\t1\n";
    // The header has no self-overlapping prefix except its initial C.
    if (byte == header[s->header_match]) s->header_match++;
    else s->header_match = byte == header[0] ? 1 : 0;
    if (s->header_match == sizeof header - 1) {
        memcpy(s->body, header, sizeof header - 1);
        s->length = sizeof header - 1;
        s->header_match = 0;
        s->active = true;
        return CB_STREAM_NONE;
    }
    if (!s->active) return CB_STREAM_NONE;
    if (s->length == CB_BODY_MAX) {
        s->active = false;
        s->length = 0;
        return CB_STREAM_INVALID;
    }
    s->body[s->length++] = byte;
    if (s->length >= 5 && !memcmp(s->body + s->length - 5, "\nEND\n", 5)) {
        bool valid = cb_feed_parse(s->body, s->length, out);
        cb_stream_reset(s);
        return valid ? CB_STREAM_VALID : CB_STREAM_INVALID;
    }
    return CB_STREAM_NONE;
}
