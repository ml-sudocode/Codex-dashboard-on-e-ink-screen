// Hackaday: hackaday.com's latest posts (feed.c gets them), one per screen: its featured image (the post's first
// picture) and title; A opens the full post with its pictures, which come in the background.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "badge/badge.h"
#include "hackaday.h"

#define MAXL 4000    // laid-out lines of the open post
#define WIPE_EVERY 6 // ponytail: scene changes (posts, page turns) between ghost wipes (2.8 s); tune on the badge
#define X0 32        // text column
#define X1 (W - 24)  // right edge of the text; the scroll bar sits right of it

// How much of s (up to its end or a '\n') goes on a line of cols characters; *next: where the next line starts.
static int cut(const char *s, int cols, const char **next)
{
    int n = strcspn(s, "\n");
    if (n > cols) {
        int k = cols;
        while (k > 0 && s[k] != ' ') k--;
        n = k ? k : cols; // one long word: hard break
    }
    const char *t = s + n;
    while (*t == ' ') t++;
    *next = t;
    return n;
}

static void line(int x, int y, int scale, const char *s, int n)
{
    char b[128];
    snprintf(b, sizeof b, "%.*s", n, s);
    text(x, y, scale, b, true);
}

// Word-wrapped s at x, at most maxl lines of lh px, "..." if cut; returns the y below it.
static int wrap(int x, int y, int scale, int lh, int maxl, const char *s)
{
    int cols = (X1 - x) / (8 * scale);
    for (const char *t; *s && maxl--; y += lh, s = t) {
        int n = cut(s, cols, &t);
        if (!maxl && *t) {
            char b[128];
            snprintf(b, sizeof b, "%.*s...", n < cols - 3 ? n : cols - 3, s);
            text(x, y, scale, b, true);
        } else line(x, y, scale, s, n);
        if (*t == '\n') t++;
    }
    return y;
}

static void meta(char *o, int n, const post_t *p)
{
    snprintf(o, n, "%s%s%s", p->author, *p->author && *p->date ? ", " : "", p->date);
}

// The open post laid out as lines: text (s, n at scale), an image (img >= 0) or a gap (neither); h px each.
typedef struct {
    const char *s;
    int16_t n, h;
    int8_t scale, img;
} line_t;
static line_t *lines;
static int nl;
static char head[80];

static void add(const char *s, int n, int h, int scale, int img)
{
    if (nl < MAXL) lines[nl++] = (line_t){s, n, h, scale, img};
}

static void layout(int i)
{
    const char *t;
    nl = 0;
    for (const char *s = posts[i].title; *s; s = t) add(s, cut(s, (X1 - X0) / 24, &t), 30, 3, -1);
    meta(head, sizeof head - 12, &posts[i]);
    int hn = strlen(head);
    snprintf(head + hn, sizeof head - hn, "  %d/%d", i + 1, nposts);
    add(head, strlen(head), 34, 2, -1);
    for (const char *s = posts[i].body ? posts[i].body : ""; *s; s = t) {
        unsigned char c = *s;
        if (c >= 0x80 && (s[1] == '\n' || !s[1])) { // an image
            add(NULL, 0, imgs[c - 0x80].h + 12, 0, c - 0x80);
            t = s + 1 + (s[1] == '\n');
            continue;
        }
        add(s, cut(s, (X1 - X0) / 16, &t), 22, 2, -1);
        if (*t == '\n') t++, add(NULL, 0, 12, 0, -1); // paragraph gap
    }
}

static int page_end(int top) // the first line below the screen when top is the first line on it
{
    int y = TOP + 8;
    while (top < nl && y + lines[top].h <= H) y += lines[top++].h;
    return top;
}

static int page_back(int top) // the top of the page before
{
    for (int y = TOP + 8; top > 0 && y + lines[top - 1].h <= H; y += lines[--top].h) {}
    return top;
}

// Image i at y, centred (featured images are wider than the text column), at most maxh px high.
static void picture(int i, int y, int maxh)
{
    img_t *m = &imgs[i];
    int x = m->w > X1 - X0 ? (W - m->w) / 2 : X0 + (X1 - X0 - m->w) / 2, st = (m->w + 7) / 8;
    if (m->state <= 0) {
        outline(x, y, m->w, maxh, 2);
        text(x + 16, y + 16, 2, m->state ? "(no image)" : "(image loading)", true);
        return;
    }
    for (int r = 0; r < m->h && r < maxh; r++)
        if (!(x & 7)) memcpy(fb + (y + r) * (W / 8) + x / 8, m->bits + r * st, st); // byte-aligned: whole rows
        else for (int c = 0; c < m->w; c++)
            if (m->bits[r * st + c / 8] & 0x80 >> (c & 7)) fb[((y + r) * W + x + c) / 8] |= 0x80 >> ((x + c) & 7);
}

// The browser: post i's featured image, its title and author under it.
static void draw_card(int i)
{
    char s[80];
    post_t *p = &posts[i];
    memset(fb, 0, sizeof fb);
    int y;
    if (p->cover >= 0) {
        picture(p->cover, TOP, imgs[p->cover].h);
        y = wrap(X0, TOP + imgs[p->cover].h + 8, 2, 20, 2, p->title);
    } else y = wrap(X0, TOP + 48, 4, 40, 6, p->title) + 16;
    meta(s, sizeof s - 12, p);
    int n = strlen(s);
    snprintf(s + n, sizeof s - n, "  %d/%d", i + 1, nposts);
    text(X0, y, 2, s, true);
    ui_frame(1);
}

static void draw_post(int top)
{
    memset(fb, 0, sizeof fb);
    int y = TOP + 8, end = page_end(top);
    for (int i = top; i < end; y += lines[i++].h) {
        line_t *l = &lines[i];
        if (l->s) line(X0, y, l->scale, l->s, l->n);
        else if (l->img >= 0) picture(l->img, y, l->h - 12); // decoded size can differ from the box by a pixel
    }
    if (top > 0 || end < nl) { // scroll bar
        int y0 = TOP + 8, sh = H - 8 - y0;
        box(W - 14, y0 + sh * top / nl, 6, sh * (end - top) / nl, true);
    }
    ui_frame(1);
}

static void message(int scale, const char *a, const char *b)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, scale, a, true);
    if (b) wrap(48, TOP + 48 + 8 * scale + 32, 3, 32, 6, b);
    ui_frame(1);
}

// The next press; -1 when an image came in (redraw). The panel sleeps after 3 s without a press.
static int wait_btn(void)
{
    int seen = imgs_done;
    for (int idle = 0;; idle++) {
        int b = btn_get(500);
        if (b >= 0) return b;
        if (imgs_done != seen) return -1;
        if (idle == 6) ui_close();
    }
}

static void loading(void)
{
    char s[64];
    int shown = -1;
    while (stage < 2) {
        int now = stage * 100000 + bytes / 1024;
        if (now != shown) {
            shown = now;
            memset(fb, 0, sizeof fb);
            text(48, TOP + 48, 5, "Loading...", true);
            text(48, TOP + 128, 3, "hackaday.com", true);
            text(48, TOP + 200, 3, "Joining Wi-Fi...", true);
            if (stage >= 1) {
                snprintf(s, sizeof s, "Wi-Fi joined in %.1f s", join_ms / 1000.0);
                text(48, TOP + 240, 3, s, true);
                snprintf(s, sizeof s, "Downloading the feed: %d KB", bytes / 1024);
                text(48, TOP + 280, 3, s, true);
            }
            ui_frame(1);
        }
        ms(100);
    }
    ESP_LOGI("rss", "list up %lld ms after start", (esp_timer_get_time() - t0) / 1000);
}

static void task(void *arg)
{
    for (int switches = 0;;) { // load
        feed_stop();
        feed_start();
        ui_keys(NULL, NULL, NULL, NULL);
        loading();
        if (stage == 3) {
            ui_keys(NULL, NULL, NULL, "Retry");
            message(5, "No feed.", err);
            while (wait_btn() != BTN_B) {}
            continue;
        }
        for (int sel = 0, post = -1, top = 0;;) { // post < 0: the browser
            bool scene = false;
            cur_post = post < 0 ? sel : post, reading = post >= 0; // its images are fetched first
            if (post < 0) ui_keys("Prev", "Next", "Open", "Refresh"), draw_card(sel);
            else ui_keys("Up", "Down", post + 1 < nposts ? "Next post" : NULL, "Back"), draw_post(top);
            int b = wait_btn();
            if (post < 0) {
                if (b == BTN_PLUS && sel > 0) sel--, scene = true;
                else if (b == BTN_MINUS && sel < nposts - 1) sel++, scene = true;
                else if (b == BTN_A) post = sel, top = 0, layout(post), scene = true;
                else if (b == BTN_B) break; // refresh
            } else if (b == BTN_PLUS && top > 0) top = page_back(top), scene = true;
            else if (b == BTN_MINUS && page_end(top) < nl) top = page_end(top), scene = true;
            else if (b == BTN_A && post + 1 < nposts) sel = ++post, top = 0, layout(post), scene = true;
            else if (b == BTN_B) post = -1, scene = true;
            if (scene && ++switches % WIPE_EVERY == 0) ui_wipe();
        }
    }
}

// Takes over the badge for good (home is MENU + and MENU -).
static void run(void)
{
    feed_alloc();
    lines = heap_caps_calloc(MAXL, sizeof *lines, MALLOC_CAP_SPIRAM);
    xTaskCreate(task, "rss", 16384, NULL, 4, NULL);
    vTaskSuspend(NULL);
}

static void icon(int x, int y) // news feed: a dot and two arcs from the bottom left corner
{
    icon_base(x, y);
    int cx = x + 44, cy = y + ICON - 44;
    for (int r = 0; r < ICON; r++)
        for (int k = 0; k < ICON; k++) {
            int dx = x + k - cx, dy = cy - (y + r), d2 = dx * dx + dy * dy;
            if (dx >= -12 && dy >= -12 && (d2 < 14 * 14 || (d2 > 50 * 50 && d2 < 68 * 68) || (d2 > 86 * 86 && d2 < 104 * 104)))
                box(x + k, y + r, 1, 1, false);
        }
}

const app_t app_hackaday = {"Hackaday", icon, run};

