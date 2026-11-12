#include "board_alert.h"
#include <string.h>

static bool contains(const cb_seen_t *seen, const char *id)
{
    for (unsigned i = 0; i < seen->count; i++)
        if (!strcmp(id, seen->ids[i])) return true;
    return false;
}

bool cb_seen_load(cb_seen_t *seen, const char *saved)
{
    cb_seen_t next = {0};
    size_t length = strlen(saved);
    if (length % 33 || length / 33 > CB_SEEN_MAX) return false;
    for (size_t offset = 0; offset < length; offset += 33) {
        char id[33] = {0};
        for (unsigned j = 0; j < 32; j++) {
            char c = saved[offset + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
            id[j] = c;
        }
        if (saved[offset + 32] != '\n' || contains(&next, id)) return false;
        memcpy(next.ids[next.count++], id, sizeof id);
    }
    *seen = next;
    return true;
}

void cb_seen_save(const cb_seen_t *seen, char out[CB_SEEN_TEXT])
{
    for (unsigned i = 0; i < seen->count; i++) {
        memcpy(out + i * 33, seen->ids[i], 32);
        out[i * 33 + 32] = '\n';
    }
    out[seen->count * 33] = '\0';
}

static bool flashes(const cb_pane_t *pane)
{
    return cb_attention(pane->status) || pane->status == CB_REPLY_READY;
}

static bool pending(const cb_feed_t *feed, const char *id)
{
    for (unsigned i = 0; i < feed->pane_count; i++)
        if (flashes(&feed->panes[i]) && !strcmp(id, feed->panes[i].event)) return true;
    return false;
}

unsigned cb_seen_observe(cb_seen_t *seen, const cb_feed_t *feed)
{
    if (feed->source != CB_OK) return 0;
    unsigned added = 0;
    for (unsigned i = 0; i < feed->pane_count; i++) {
        const cb_pane_t *pane = &feed->panes[i];
        if (!flashes(pane) || !pane->event[0] || contains(seen, pane->event)) continue;
        if (seen->count == CB_SEEN_MAX) {
            unsigned victim = 0;
            while (victim < seen->count && pending(feed, seen->ids[victim])) victim++;
            // At most 16 pending panes, so a 32-slot history always has a victim.
            if (victim == seen->count) continue;
            memmove(seen->ids[victim], seen->ids[victim + 1],
                    (seen->count - victim - 1) * sizeof seen->ids[0]);
            seen->count--;
        }
        memcpy(seen->ids[seen->count++], pane->event, sizeof pane->event);
        added++;
    }
    return added;
}
