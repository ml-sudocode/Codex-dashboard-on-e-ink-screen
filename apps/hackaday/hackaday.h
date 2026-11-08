// Hackaday app internals: feed.c downloads the feed and its images on a task of its own, hackaday.c shows them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char title[200], author[48], date[20];
    int cover;  // its featured image (index in imgs), -1: none
    char *body; // cleaned text: '\n' between paragraphs, a byte 0x80 + image index alone on a line for an image
} post_t;
typedef struct {
    char url[400];
    int post, w, h; // w, h: the box it gets (from the tag's width/height), then what was decoded
    uint8_t *bits;  // 1 bit per pixel, rows of (w + 7) / 8 bytes, 1 = ink
    volatile int8_t state; // 0 to fetch, 1 ready, -1 failed
} img_t;

// Written by the feed task, read by the screen. PSRAM, like everything big here.
extern post_t *posts;
extern img_t *imgs;
extern int nposts, nimg;
extern char err[64];                                      // why there is no feed
extern volatile int stage, bytes, imgs_done, join_ms;     // stage: 0 joining, 1 downloading, 2 feed ready, 3 failed
extern int64_t t0;                                        // when the download started
// Set by the screen: the post on it and whether it is open, so its images come first.
extern volatile int cur_post;
extern volatile bool reading;

void feed_alloc(void); // the buffers, once
void feed_start(void); // a fresh download: Wi-Fi on, the feed, the images, Wi-Fi off (and ranging back on)
void feed_stop(void);  // stops the download, frees the posts and images
