// Hackaday: the newest posts from hackaday.com's RSS feed. Joins Wi-Fi only to fetch, then gives the radio back to
// ranging. List: title, author, date; A opens a post's summary.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "badge.h"

#define URL "https://hackaday.com/blog/feed/"
#define CAP (1024 * 1024) // feed buffer in PSRAM; the feed is ~100-300 KB (full post bodies)
#define MAXP 10
#define WIPE_EVERY 4 // ponytail: list<->post switches between ghost wipes (2.8 s each); tune on the badge

typedef struct {
    char title[200], author[48], date[20], desc[1200];
} post_t;
static post_t *posts; // PSRAM
static int nposts;
static char *feed;    // PSRAM
static char err[64];

// ---- Text: HTML/XML/UTF-8 to the 7-bit font.
static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"; // U+00C0..U+00FF

static int put(char *o, int n, int max, unsigned cp)
{
    const char *s = NULL;
    char c[2] = {0};
    if (cp >= 0x20 && cp < 0x7F) c[0] = cp;
    else if (cp == '\n' || cp == '\t' || cp == '\r' || cp == 0xA0) c[0] = ' ';
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) c[0] = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) c[0] = '"';
    else if (cp >= 0x2010 && cp <= 0x2015) c[0] = '-';
    else if (cp == 0x2026) s = "...";
    else if (cp == 0xD7) c[0] = 'x';
    else if (cp >= 0xC0 && cp <= 0xFF) c[0] = latin1[cp - 0xC0];
    if (!s) s = c; // anything else is dropped
    for (; *s && n < max - 1; s++) {
        if (*s == ' ' && (n == 0 || o[n - 1] == ' ')) continue; // collapse whitespace, no leading space
        o[n++] = *s;
    }
    return n;
}

// Copies [s, e) into o as plain ASCII: no CDATA markers, no tags, entities and UTF-8 punctuation decoded.
static void clean(char *o, int max, const char *s, const char *e)
{
    int n = 0;
    while (s < e) {
        if (!strncmp(s, "<![CDATA[", 9)) s += 9;
        else if (!strncmp(s, "]]>", 3)) s += 3;
        else if (*s == '<') { // a tag: a space (block tags separate words)
            while (s < e && *s != '>') s++;
            s++;
            n = put(o, n, max, ' ');
        } else if (*s == '&') {
            const char *semi = memchr(s, ';', e - s < 10 ? e - s : 10);
            unsigned cp = '&';
            if (semi && s[1] == '#') cp = s[2] == 'x' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            else if (semi) {
                static const struct { const char *n; unsigned cp; } ent[] = {
                    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
                    {"hellip", 0x2026}, {"ndash", '-'}, {"mdash", '-'}, {"rsquo", '\''}, {"lsquo", '\''},
                    {"rdquo", '"'}, {"ldquo", '"'}};
                cp = 0;
                for (int i = 0; i < sizeof ent / sizeof *ent; i++)
                    if (strlen(ent[i].n) == semi - s - 1 && !strncmp(s + 1, ent[i].n, semi - s - 1)) cp = ent[i].cp;
            }
            n = put(o, n, max, cp);
            s = semi ? semi + 1 : s + 1;
        } else { // UTF-8
            unsigned char c = *s++;
            unsigned cp = c;
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (more) cp = c & (0x3F >> more);
            for (; more && s < e; more--) cp = cp << 6 | (*s++ & 0x3F);
            n = put(o, n, max, cp);
        }
    }
    while (n && o[n - 1] == ' ') n--;
    o[n] = 0;
}

// The text of <tag>...</tag> inside [s, e), cleaned into o.
static void field(char *o, int max, const char *s, const char *e, const char *tag)
{
    char open[24], close[24];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(s, open), *b = a ? strstr(a, close) : NULL;
    if (!a || b > e) return (void)(*o = 0);
    clean(o, max, a + strlen(open), b);
}

static void parse(void)
{
    nposts = 0;
    for (const char *s = strstr(feed, "<item>"), *e; s && nposts < MAXP; s = strstr(e, "<item>")) {
        if (!(e = strstr(s, "</item>"))) break;
        post_t *p = &posts[nposts++];
        field(p->title, sizeof p->title, s, e, "title");
        field(p->author, sizeof p->author, s, e, "dc:creator");
        field(p->date, 17, s, e, "pubDate"); // "Sat, 07 Nov 2026"
        field(p->desc, sizeof p->desc, s, e, "description");
        char *m = strstr(p->desc, "...read more");
        for (; m && m > p->desc && m[-1] == ' '; m--) {}
        if (m) strcpy(m, "..."); // the feed cuts the summary mid-sentence
    }
}

// ---- Network: join, read the whole feed into PSRAM, leave.
static bool fetch(void)
{
    if (!wifi_connect(20000)) {
        snprintf(err, sizeof err, "Could not join the badge Wi-Fi.");
        wifi_off();
        ranging_start();
        return false;
    }
    esp_http_client_handle_t c = esp_http_client_init(&(esp_http_client_config_t){
        .url = URL, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .user_agent = "badge-os",
        .buffer_size = 4096});
    int n = 0, r = -1;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if ((r = esp_http_client_get_status_code(c)) == 200)
            while (n < CAP - 1) {
                int got = esp_http_client_read(c, feed + n, CAP - 1 - n);
                if (got <= 0) break;
                n += got;
            }
    }
    esp_http_client_cleanup(c);
    feed[n] = 0;
    ESP_LOGI("hackaday", "%s, HTTP %d, %d bytes", esp_err_to_name(e), r, n);
    if (e != ESP_OK) snprintf(err, sizeof err, "%s", esp_err_to_name(e));
    else if (r != 200) snprintf(err, sizeof err, "HTTP %d", r);
    else if (parse(), !nposts) snprintf(err, sizeof err, "No posts in the feed (%d bytes).", n);
    wifi_off();
    ranging_start();
    return !*err;
}

// ---- Screen.
// Word-wrapped text from x to the right edge, at most maxl lines of lh px; returns the y below it.
static int wrap(int x, int y, int scale, int lh, int maxl, const char *s)
{
    int cols = (W - 16 - x) / (8 * scale);
    char line[128];
    for (; *s && maxl--; y += lh) {
        int n = strlen(s);
        if (n > cols) {
            n = cols;
            while (n > 0 && s[n] != ' ') n--;
            if (!n) n = cols; // one long word: hard break
        }
        if (!maxl && s[n]) n = n > cols - 3 ? cols - 3 : n; // last line and more to come: "..."
        snprintf(line, sizeof line, "%.*s%s", n, s, !maxl && s[n] ? "..." : "");
        text(x, y, scale, line, true);
        s += n;
        while (*s == ' ') s++;
    }
    return y;
}

static void meta(char *o, int n, const post_t *p)
{
    snprintf(o, n, "%s%s%s", p->author, *p->author && *p->date ? ", " : "", p->date);
}

static void draw_list(int sel)
{
    char s[80];
    memset(fb, 0, sizeof fb);
    for (int i = 0, y = TOP + 8; i < nposts; i++, y += 60) {
        if (i == sel) box(8, y, 12, 52, true);
        int y2 = wrap(32, y, 2, 18, 2, posts[i].title);
        meta(s, sizeof s, &posts[i]);
        text(64, y2, 2, s, true);
    }
    ui_frame(1);
}

static void draw_post(int i)
{
    char s[80];
    memset(fb, 0, sizeof fb);
    int y = wrap(32, TOP + 16, 3, 30, 4, posts[i].title) + 8;
    meta(s, sizeof s, &posts[i]);
    text(32, y, 2, s, true);
    snprintf(s, sizeof s, "%d/%d", i + 1, nposts);
    text(W - 16 - 16 * strlen(s), y, 2, s, true);
    wrap(32, y + 40, 2, 22, (H - 8 - y - 40) / 22, posts[i].desc);
    ui_frame(1);
}

static void message(int scale, const char *a, const char *b)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, scale, a, true);
    if (b) wrap(48, TOP + 48 + 8 * scale + 32, 3, 32, 6, b);
    ui_frame(1);
}

static int wait_btn(void) // the panel sleeps while nobody presses
{
    int b = btn_get(3000);
    if (b < 0) ui_close(), b = btn_get(-1);
    return b;
}

static void run(void)
{
    int sel = 0, switches = 0;
    for (;;) { // load
        ui_keys(NULL, NULL, NULL, NULL);
        message(5, "Loading...", "Fetching hackaday.com/blog/feed");
        *err = 0;
        if (!fetch()) {
            ui_keys(NULL, NULL, NULL, "Retry");
            message(5, "No feed.", err);
            while (wait_btn() != BTN_B) {}
            continue;
        }
        sel = 0;
        for (int post = -1;;) { // post < 0: the list
            if (post < 0) ui_keys("Up", "Down", "Open", "Refresh"), draw_list(sel);
            else ui_keys("Prev", "Next", NULL, "List"), draw_post(post);
            int b = wait_btn();
            int *cur = post < 0 ? &sel : &post;
            if (b == BTN_PLUS && *cur > 0) (*cur)--;
            else if (b == BTN_MINUS && *cur < nposts - 1) (*cur)++;
            else if ((b == BTN_A && post < 0) || (b == BTN_B && post >= 0)) {
                if (post >= 0) sel = post;
                post = post < 0 ? sel : -1;
                if (++switches % WIPE_EVERY == 0) ui_wipe();
            } else if (b == BTN_B) break; // refresh
        }
    }
}

// The app task's stack is too small for TLS: run on our own task (it never returns; home is MENU + and MENU -).
static void task(void *arg)
{
    run();
}

void app_hackaday(void)
{
    feed = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    posts = heap_caps_calloc(MAXP, sizeof *posts, MALLOC_CAP_SPIRAM);
    xTaskCreate(task, "hackaday", 16384, NULL, 4, NULL);
    vTaskSuspend(NULL);
}
