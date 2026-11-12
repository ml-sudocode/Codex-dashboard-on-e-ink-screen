#include "board_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char valid[] =
    "CODEXBOARD\t1\nMETA\tabc\tDEMO\tUTC\t2026-11-10 15:00:00 UTC\n"
    "SOURCE\tOK\nQUOTA\tUNAVAILABLE\t\nMESSAGE\tHello from USB\n"
    "FOOTER\tjoke\tTesting\nEND\n";

static unsigned received, rejected;
static void bytes(cb_stream_t *stream, const char *data, size_t n, cb_feed_t *out)
{
    for (size_t i = 0; i < n; i++) {
        cb_stream_result_t result = cb_stream_byte(stream, data[i], out);
        if (result == CB_STREAM_VALID) received++;
        if (result == CB_STREAM_INVALID) rejected++;
    }
}

int main(void)
{
    cb_stream_t stream = {0};
    cb_feed_t out = {0};
    size_t length = strlen(valid);
    // Every possible cut in an incomplete frame must recover at a fresh header,
    // including a cut in the middle of a record without an intervening newline.
    for (size_t cut = 0; cut < length; cut++) {
        cb_stream_reset(&stream);
        received = rejected = 0;
        bytes(&stream, valid, cut, &out);
        assert(!received);
        bytes(&stream, valid, length, &out);
        assert(received == 1 && !strcmp(out.message, "Hello from USB"));
    }
    cb_stream_reset(&stream);
    received = rejected = 0;
    bytes(&stream, "boot log\nCCC", 12, &out);
    bytes(&stream, valid, length, &out);
    bytes(&stream, valid, length, &out);
    assert(received == 2 && !rejected); // back-to-back frames
    cb_feed_t saved = out;
    static const char bad[] = "CODEXBOARD\t1\nSOURCE\tWRONG\nEND\n";
    bytes(&stream, bad, strlen(bad), &out);
    assert(rejected == 1 && !memcmp(&saved, &out, sizeof saved));
    cb_stream_reset(&stream);
    bytes(&stream, "CODEXBOARD\t1\n", 13, &out);
    for (unsigned i = 0; i < CB_BODY_MAX + 20; i++)
        bytes(&stream, "x", 1, &out);
    assert(rejected == 2 && !memcmp(&saved, &out, sizeof saved));
    bytes(&stream, valid, length, &out);
    assert(received == 3); // recover after oversize
    // A partial END is never committed. An invalid control byte cannot sneak in.
    cb_stream_reset(&stream);
    received = rejected = 0;
    bytes(&stream, valid, length - 1, &out);
    assert(!received);
    bytes(&stream, "\n", 1, &out);
    assert(received == 1);
    cb_stream_reset(&stream);
    bytes(&stream, valid, 13, &out);
    bytes(&stream, "\0", 1, &out);
    bytes(&stream, valid + 13, length - 13, &out);
    assert(rejected == 1 && !memcmp(&saved, &out, sizeof saved));
    puts("PASS: USB framing, every truncation recovery, split headers, oversize recovery, atomic snapshots");
    return 0;
}
