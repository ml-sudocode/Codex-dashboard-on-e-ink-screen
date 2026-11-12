#pragma once
#include <stdbool.h>
#include <stddef.h>

#define CB_BODY_MAX 8192
#define CB_PANES_MAX 16
#define CB_WINDOWS_MAX 8

typedef enum { CB_OK, CB_STALE, CB_UNAVAILABLE } cb_health_t;
typedef enum {
    CB_WORKING, CB_IDLE, CB_DONE, CB_NEEDS_YOU, CB_APPROVAL, CB_UNKNOWN, CB_REPLY_READY
} cb_status_t;

typedef struct {
    char id[33], task[65], event[33];
    cb_status_t status;
} cb_pane_t;

typedef struct {
    char label[33], resets[41];
    unsigned remaining;
} cb_window_t;

typedef struct {
    char revision[33], zone[49], sampled[41], fetched[41];
    char message[121], footer[161];
    bool demo, joke;
    cb_health_t source, quota;
    unsigned pane_count, window_count;
    cb_pane_t panes[CB_PANES_MAX];
    cb_window_t windows[CB_WINDOWS_MAX];
} cb_feed_t;

// Input is writable, with capacity >= length + 1. Invalid input leaves out unchanged.
bool cb_feed_parse(char *body, size_t length, cb_feed_t *out);
// An unavailable collector cannot resolve requests. Its next healthy snapshot can.
void cb_feed_retain_terminals(cb_feed_t *next, const cb_feed_t *previous);
bool cb_attention(cb_status_t status);
unsigned cb_pending(const cb_feed_t *feed);
