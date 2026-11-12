#include "board_alert.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char head[] =
    "CODEXBOARD\t1\n"
    "META\t0123456789abcdef\tDEMO\tAmerica/Los_Angeles\t2026-11-10 12:45:00 PST\n"
    "SOURCE\tOK\nQUOTA\tOK\t2026-11-10 12:40:00 PST\n";
static const char tail[] = "FOOTER\tjoke\tMy code needs more coffee.\nEND\n";
static const char attention[] =
    "PANE\t%1\tFix the parser\tNEEDS_YOU\t00000000000000000000000000000001\n";

static bool parse(const char *s, cb_feed_t *out)
{
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    assert(copy);
    memcpy(copy, s, n + 1);
    bool ok = cb_feed_parse(copy, n, out);
    free(copy);
    return ok;
}

static void reject(const char *s)
{
    cb_feed_t sentinel, result;
    memset(&sentinel, 0xa5, sizeof sentinel);
    result = sentinel;
    assert(!parse(s, &result));
    assert(!memcmp(&sentinel, &result, sizeof result));
}

static void rows(const char *row, bool valid)
{
    char body[CB_BODY_MAX + 1];
    snprintf(body, sizeof body, "%s%s%s", head, row, tail);
    cb_feed_t out;
    if (valid) assert(parse(body, &out));
    else reject(body);
}

static void parser_tests(void)
{
    char body[CB_BODY_MAX + 2];
    snprintf(body, sizeof body, "%s%s%s%s", head, attention,
             "WINDOW\t5 hours\t29\t2026-11-10 16:00:00 PST\n", tail);
    cb_feed_t f;
    assert(parse(body, &f));
    assert(f.demo && f.joke && f.pane_count == 1 && f.window_count == 1);
    assert(f.panes[0].status == CB_NEEDS_YOU && cb_pending(&f) == 1);
    assert(f.windows[0].remaining == 29 && !strcmp(f.zone, "America/Los_Angeles"));
    assert(!strcmp(f.fetched, "2026-11-10 12:40:00 PST"));
    assert(!strcmp(f.windows[0].resets, "2026-11-10 16:00:00 PST"));
    body[strlen(body) - 1] = '\0';
    assert(parse(body, &f)); // final newline is optional
    for (size_t i = 0; i < strlen(body); i++) {
        char truncated[CB_BODY_MAX + 1];
        memcpy(truncated, body, i);
        truncated[i] = '\0';
        reject(truncated); // every truncation, including END missing one byte
    }
    rows("", true);
    rows("MESSAGE\tHello from the Linux VM\n", true);
    rows("WINDOW\tPrimary\t0\t2026-11-10 12:00:00 UTC\n", true);
    rows("WINDOW\tPrimary\t100\t2026-11-10 12:00:00 UTC\n", true);
    snprintf(body, sizeof body, "%sWINDOW\tWeekly\t77\t2026-11-16 18:00:00 PST\n"
             "PANE\t%%2\tFinished reply\tREPLY_READY\t\n%s", head, tail);
    assert(parse(body, &f));
    assert(f.windows[0].remaining == 77 && f.panes[0].status == CB_REPLY_READY);
    assert(cb_pending(&f) == 0 && !cb_attention(CB_REPLY_READY));
    rows("WINDOW\tPrimary\t101\t2026-11-10 12:00:00 UTC\n", false);
    rows("WINDOW\tPrimary\t-1\t2026-11-10 12:00:00 UTC\n", false);
    rows("WINDOW\tPrimary\t1.5\t2026-11-10 12:00:00 UTC\n", false);
    rows("WINDOW\tPrimary\t30 \t2026-11-10 12:00:00 UTC\n", false);
    rows("WINDOW\tPrimary\t\t2026-11-10 12:00:00 UTC\n", false);
    rows("SOURCE\tOK\n", false);
    rows("QUOTA\tOK\t\n", false);
    rows("META\t1\tLIVE\tUTC\t2026-11-10\n", false);
    rows("CODEXBOARD\t1\n", false);
    rows("FOOTER\tquote\tx\n", false);
    rows("MESSAGE\tx\nMESSAGE\ty\n", false);
    rows("BAD\tx\n", false);
    rows("END\n", false);
    rows("\n", false);
    rows("PANE\t%1\ttask\tWORKING\t\textra\n", false);
    rows("PANE\t%1\ttask\tACTION_REQUIRED\t\n", false);
    rows("PANE\t%1\ttask\tNEEDS_YOU\tzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n", false);
    rows("PANE\t%1\ttask\tNEEDS_YOU\t0123\n", false);
    rows("PANE\t\ttask\tIDLE\t\n", false);
    rows("PANE\t%1\ttask\tIDLE\t\nPANE\t%1\tother\tDONE\t\n", false);
    rows("PANE\t%1\ttask\tIDLE\t\nPANE\t%2\tother\tAPPROVAL\t\n", true);
    rows("MESSAGE\ttwo\tfields\n", false);
    rows("MESSAGE\tbad\rcontrol\n", false);
    rows("MESSAGE\tbad\x80\n", false);
    snprintf(body, sizeof body, "%s%s", head, tail);
    strcat(body, "\n");
    reject(body);
    snprintf(body, sizeof body, "%s%s", head, tail);
    size_t length = strlen(body);
    body[50] = '\0';
    memset(&f, 0x5a, sizeof f);
    cb_feed_t saved = f;
    assert(!cb_feed_parse(body, length, &f));
    assert(!memcmp(&saved, &f, sizeof f));
    memset(body, 'x', CB_BODY_MAX + 1);
    assert(!cb_feed_parse(body, CB_BODY_MAX + 1, &f));
    assert(!cb_feed_parse(NULL, 1, &f));
    assert(!cb_feed_parse(body, 1, NULL));
}

static void boundary_tests(void)
{
    char body[CB_BODY_MAX + 1], part[256], task[66];
    memset(task, 'T', 64);
    task[64] = '\0';
    strcpy(body, head);
    for (unsigned i = 0; i < CB_PANES_MAX; i++) {
        snprintf(part, sizeof part, "PANE\t%%%u\t%s\tWORKING\t\n", i, task);
        strcat(body, part);
    }
    for (unsigned i = 0; i < CB_WINDOWS_MAX; i++) {
        snprintf(part, sizeof part, "WINDOW\tw%u\t30\t2026-11-10 14:00:00 PST\n", i);
        strcat(body, part);
    }
    size_t records = strlen(body);
    strcat(body, tail);
    cb_feed_t f;
    assert(parse(body, &f));
    assert(f.pane_count == 16 && f.window_count == 8);
    body[records] = '\0';
    strcat(body, "PANE\t%99\textra\tDONE\t\n");
    strcat(body, tail);
    reject(body);
    body[records] = '\0';
    strcat(body, "WINDOW\tninth\t50\t2026-11-10\n");
    strcat(body, tail);
    reject(body);
    task[64] = 'T'; task[65] = '\0';
    snprintf(part, sizeof part, "PANE\t%%1\t%s\tWORKING\t\n", task);
    rows(part, false);
    char long_text[162];
    memset(long_text, 'x', 161); long_text[161] = '\0';
    snprintf(body, sizeof body, "%sFOOTER\tquote\t%s\nEND\n", head, long_text);
    reject(body);
    long_text[160] = '\0';
    snprintf(body, sizeof body, "%sFOOTER\tquote\t%s\nEND\n", head, long_text);
    assert(parse(body, &f));
}

static void event(cb_pane_t *pane, unsigned id)
{
    pane->status = CB_NEEDS_YOU;
    snprintf(pane->event, sizeof pane->event, "%032x", id);
}

static void alert_tests(void)
{
    cb_seen_t seen = {0}, restored = {0};
    cb_feed_t f = {.source = CB_OK, .pane_count = 2};
    event(&f.panes[0], 1);
    f.panes[0].status = CB_REPLY_READY;
    assert(cb_pending(&f) == 0 && cb_seen_observe(&seen, &f) == 1);
    assert(cb_seen_observe(&seen, &f) == 0); // Repeated ready frames stay quiet.
    event(&f.panes[0], 1);
    event(&f.panes[1], 2);
    f.panes[1].status = CB_APPROVAL;
    assert(cb_seen_observe(&seen, &f) == 1);
    assert(cb_seen_observe(&seen, &f) == 0);
    char persisted[CB_SEEN_TEXT];
    cb_seen_save(&seen, persisted);
    assert(cb_seen_load(&restored, persisted));
    assert(cb_seen_observe(&restored, &f) == 0); // app relaunch / power cycle
    cb_seen_t old = restored;
    assert(!cb_seen_load(&restored, "broken"));
    assert(!memcmp(&old, &restored, sizeof old));
    persisted[10] = 'z';
    assert(!cb_seen_load(&restored, persisted));
    assert(!memcmp(&old, &restored, sizeof old));
    f.panes[0].status = CB_DONE;
    f.panes[1].status = CB_WORKING;
    assert(cb_pending(&f) == 0);
    assert(cb_seen_observe(&seen, &f) == 0);
    event(&f.panes[1], 3);
    f.source = CB_STALE;
    assert(cb_seen_observe(&seen, &f) == 0);
    f.source = CB_UNAVAILABLE;
    assert(cb_seen_observe(&seen, &f) == 0);
    f.source = CB_OK;
    assert(cb_seen_observe(&seen, &f) == 1);
    f.panes[1].event[0] = '\0';
    assert(cb_pending(&f) == 1 && cb_seen_observe(&seen, &f) == 0);
    // Keep one old request pending while 100 others arrive and resolve.
    event(&f.panes[0], 1);
    for (unsigned i = 4; i < 104; i++) {
        event(&f.panes[1], i);
        assert(cb_seen_observe(&seen, &f) == 1);
        assert(cb_seen_observe(&seen, &f) == 0);
    }
    assert(seen.count == CB_SEEN_MAX);
    cb_seen_save(&seen, persisted);
    assert(strlen(persisted) == CB_SEEN_MAX * 33);
    assert(cb_seen_load(&restored, persisted));
    assert(cb_seen_observe(&restored, &f) == 0);
    // Unavailable data does not clear a pending banner, but a healthy empty list does.
    cb_feed_t next = {.source = CB_UNAVAILABLE};
    cb_feed_retain_terminals(&next, &f);
    assert(cb_pending(&next) == 2);
    next = (cb_feed_t){.source = CB_OK};
    cb_feed_retain_terminals(&next, &f);
    assert(cb_pending(&next) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        char body[CB_BODY_MAX + 2];
        FILE *file = fopen(argv[1], "rb");
        assert(file);
        size_t n = fread(body, 1, sizeof body - 1, file);
        fclose(file);
        cb_feed_t f;
        if (!cb_feed_parse(body, n, &f)) return 1;
        printf("Valid feed: %u panes, %u windows, %u pending\n", f.pane_count, f.window_count, cb_pending(&f));
        return 0;
    }
    parser_tests();
    boundary_tests();
    alert_tests();
    puts("PASS: strict TSV parsing, truncation, limits, retained snapshots, persistent alert deduplication");
    return 0;
}
