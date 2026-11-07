// Gallery: the featured image of each post in hackaday.com's RSS feed, one at a time, 15 s each, title underneath.
// Wi-Fi stays joined for the session (ranging is off until the badge goes home). The feed is read once into PSRAM,
// only titles and image URLs are kept; each image is downloaded (the WordPress resizer fits it to the screen) and
// decoded with stb_image (JPEG, PNG) (the resizer only serves progressive JPEGs: the ROM TJpgDec can't read them), then dithered
// to 1 bit. The next image loads while the current one shows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "badge.h"

#include "stb_image.h" // implementation: main/stb_image.c

#define FEED "https://hackaday.com/blog/feed/"
#define CAP (1536 * 1024) // download buffer in PSRAM: the feed (~100-300 KB) and each image (JPEG 10-110 KB, PNG/GIF up to ~600 KB)
#define MAXP 20
#define SHOW_MS 15000
#define IH 576            // image box: W x IH below the top bar, the title in the 48 px under it
#define WIPE_EVERY 6      // ponytail: images between ghost wipes (2.8 s each); tune on the badge
#define LIFT 0            // ponytail: added to grey (0-255) before dithering; raise if pictures come out too dark

static const char *TAG = "gallery";
static struct { char title[160], url[200]; } *posts; // PSRAM
static int nposts;
static uint8_t *buf;  // PSRAM, CAP
static uint8_t *next; // PSRAM, the next picture: PLANE bytes, fb layout
static int next_h;    // its height: the title goes under it
static char err[64];

// ---- Text: the title's entities and UTF-8 punctuation to the 7-bit font, everything else dropped.
static void clean(char *o, int max, const char *s, const char *e)
{
    int n = 0;
    while (s < e && n < max - 1) {
        unsigned cp = (unsigned char)*s++;
        if (cp == '<' && !strncmp(s, "![CDATA[", 8)) { s += 8; continue; }
        if (cp == ']' && !strncmp(s, "]>", 2)) { s += 2; continue; }
        if (cp == '&') {
            const char *semi = memchr(s, ';', e - s < 9 ? e - s : 9);
            if (semi) {
                cp = *s == '#' ? strtoul(s + 1 + (s[1] == 'x'), NULL, s[1] == 'x' ? 16 : 10)
                   : !strncmp(s, "amp;", 4) ? '&' : !strncmp(s, "lt;", 3) ? '<' : !strncmp(s, "gt;", 3) ? '>'
                   : !strncmp(s, "quot;", 5) ? '"' : '\'';
                s = semi + 1;
            }
        } else if (cp >= 0xC0) { // UTF-8
            int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> more;
            for (; more && s < e; more--) cp = cp << 6 | (*s++ & 0x3F);
        }
        char c = cp >= 0x20 && cp < 0x7F ? cp : cp == 0x2018 || cp == 0x2019 ? '\'' : cp == 0x201C || cp == 0x201D ? '"'
               : cp >= 0x2010 && cp <= 0x2015 ? '-' : cp == 0xA0 || cp == '\n' || cp == '\t' ? ' ' : 0;
        if (c && !(c == ' ' && (!n || o[n - 1] == ' '))) o[n++] = c;
    }
    while (n && o[n - 1] == ' ') n--;
    o[n] = 0;
}

// Titles and featured-image URLs out of the feed in buf (each post's first <media:content>).
static void parse(void)
{
    nposts = 0;
    for (const char *s = strstr((char *)buf, "<item>"), *e; s && nposts < MAXP; s = strstr(e, "<item>")) {
        if (!(e = strstr(s, "</item>"))) break;
        const char *t = strstr(s, "<title>"), *te = t ? strstr(t, "</title>") : NULL;
        const char *u = strstr(s, "<media:content url=\""), *ue = u ? strpbrk(u + 20, "?\"") : NULL;
        if (!te || !ue || ue > e || !strncmp(ue - 4, ".gif", 4)) continue; // ponytail: no GIFs (animations, ~600 KB, and stb's GIF loader needs ~34 KB of stack)
        clean(posts[nposts].title, sizeof posts->title, t + 7, te);
        snprintf(posts[nposts].url, sizeof posts->url, "%.*s?fit=%d,%d&quality=60", (int)(ue - u - 20), u + 20, W, IH);
        nposts++;
    }
}

// ---- Network.
static int get(const char *url) // into buf, 0-terminated; returns the length, -1 on error (err says why)
{
    esp_http_client_handle_t c = esp_http_client_init(&(esp_http_client_config_t){
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .user_agent = "badge-os",
        .buffer_size = 4096});
    int n = 0, r = -1;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if ((r = esp_http_client_get_status_code(c)) == 200)
            while (n < CAP - 1) {
                int got = esp_http_client_read(c, (char *)buf + n, CAP - 1 - n);
                if (got <= 0) break;
                n += got;
            }
    }
    bool more = n == CAP - 1 && !esp_http_client_is_complete_data_received(c);
    esp_http_client_cleanup(c);
    buf[n] = 0;
    ESP_LOGI(TAG, "%s: %s, HTTP %d, %d bytes, %d ms", url, esp_err_to_name(e), r, n,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (e != ESP_OK) snprintf(err, sizeof err, "%s", esp_err_to_name(e));
    else if (r != 200) snprintf(err, sizeof err, "HTTP %d", r);
    else if (more) snprintf(err, sizeof err, "Too big.");
    else return n;
    return -1;
}

// ---- Picture: decode buf (n bytes) to grey, fit it into W x IH (centred), Floyd-Steinberg dither into out (fb layout).
static bool render(int n, uint8_t *out)
{
    int w, h, ch;
    int64_t t0 = esp_timer_get_time();
    uint8_t *g = stbi_load_from_memory(buf, n, &w, &h, &ch, 1);
    if (!g) return snprintf(err, sizeof err, "Can't decode: %s", stbi_failure_reason()), false;
    // ponytail: nearest-neighbour scaling; the resizer already sends it at screen size, so this rarely does much
    int ow = W, oh = h * W / w;
    if (oh > IH) oh = IH, ow = w * IH / h;
    int x0 = (W - ow) / 2;
    int16_t *e0 = calloc(2 * (W + 2), sizeof *e0), *e1 = e0 + W + 2; // this row's and the next row's error, 1 px margins
    memset(out, 0, PLANE);
    for (int y = 0; y < oh && e0; y++) {
        const uint8_t *src = g + (y * h / oh) * w;
        uint8_t *row = out + (TOP + y) * (W / 8);
        for (int x = 0; x < ow; x++) {
            int v = src[x * w / ow] + LIFT + e0[x + 1], q = v < 128 ? 0 : 255, d = v - q;
            if (!q) row[(x0 + x) / 8] |= 0x80 >> ((x0 + x) % 8);
            e0[x + 2] += d * 7 / 16, e1[x] += d * 3 / 16, e1[x + 1] += d * 5 / 16, e1[x + 2] += d / 16;
        }
        int16_t *t = e0; e0 = e1, e1 = t;
        memset(e1, 0, (W + 2) * sizeof *e1);
    }
    free(e0 < e1 ? e0 : e1);
    stbi_image_free(g);
    next_h = oh;
    ESP_LOGI(TAG, "%dx%d -> %dx%d in %d ms", w, h, ow, oh, (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

// The picture of post i into next, with its title under it; false (err says why) if it couldn't be had.
static bool load(int i)
{
    int n = get(posts[i].url);
    return n >= 0 && render(n, next);
}

// ---- Screen.
static void wrap(int y, int maxl, const char *s) // the title, word-wrapped at scale 2 from x = 16
{
    int cols = (W - 32) / 16;
    char line[64];
    for (; *s && maxl--; y += 20) {
        int n = strlen(s);
        if (n > cols) {
            for (n = cols; n > 0 && s[n] != ' '; n--) {}
            if (!n) n = cols;
        }
        snprintf(line, sizeof line, "%.*s", n, s);
        text(16, y, 2, line, true);
        for (s += n; *s == ' '; s++) {}
    }
}

static void message(const char *a, const char *b)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, 5, a, true);
    if (b) text(48, TOP + 120, 3, b, true);
    ui_frame(1);
}

static void run(void)
{
    for (bool joined = false;;) { // each pass: (join,) fetch the feed, show every post once
        *err = 0;
        int n = -1;
        if (!joined) {
            ui_keys(NULL, NULL, NULL, NULL);
            message("Loading...", "Joining Wi-Fi");
            if (!(joined = wifi_connect(20000))) snprintf(err, sizeof err, "Could not join the badge Wi-Fi.");
            else message("Loading...", "Fetching the feed");
        }
        if (joined) n = get(FEED); // a re-fetch keeps the last picture up meanwhile
        if (n >= 0 && (parse(), !nposts)) snprintf(err, sizeof err, "No images in the feed.");
        if (*err) {
            wifi_off(), joined = false;
            ui_keys(NULL, NULL, NULL, "Retry");
            message("No gallery.", err);
            ui_close();
            while (btn_get(-1) != BTN_B) {}
            continue;
        }
        // Slideshow: cur is on screen, staged is in next (-1: nothing). Errors skip the post.
        int cur = -1, staged = -1, want = 0, shown = 0, fails = 0;
        bool paused = false;
        int64_t since = 0;
        for (;;) {
            if (want != staged) {
                if (!load(want)) {
                    ESP_LOGW(TAG, "post %d: %s", want, err);
                    if (++fails >= nposts) break; // nothing loads: start over (re-join, re-fetch)
                    want = (want + 1) % nposts;
                    continue;
                }
                staged = want, fails = 0;
            }
            if (cur != staged) { // show it
                if (++shown % WIPE_EVERY == 0) ui_wipe();
                memcpy(fb, next, PLANE);
                wrap(TOP + next_h + 8, 2, posts[staged].title);
                ui_keys("Prev", "Next", NULL, paused ? "Play" : "Pause");
                ui_frame(1);
                ui_close();
                cur = staged, since = esp_timer_get_time();
                if (cur < nposts - 1) want = cur + 1; // preload the next one while this one shows
                continue;
            }
            int left = paused ? -1 : SHOW_MS - (int)((esp_timer_get_time() - since) / 1000);
            int b = left < 0 && !paused ? -1 : btn_get(left);
            if (b == BTN_PLUS) want = (cur + nposts - 1) % nposts, cur = -1;
            else if (b == BTN_MINUS || (b < 0 && !paused)) {
                if (cur == nposts - 1) break; // past the last post: re-fetch the feed for new posts
                cur = -1;                     // want is already cur + 1
            }
            else if (b == BTN_B) {
                paused = !paused;
                ui_keys("Prev", "Next", NULL, paused ? "Play" : "Pause");
                ui_frame(1); // the label changes
                ui_close();
                since = esp_timer_get_time();
            }
        }
        if (fails) wifi_off(), joined = false; // nothing loaded: re-join too
    }
}

// The app task's stack is too small for TLS and stb_image: run on our own task (home is MENU + and MENU -, which
// restarts the badge, so ranging comes back then).
static void task(void *arg)
{
    run();
}

void app_gallery(void)
{
    buf = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    next = heap_caps_malloc(PLANE, MALLOC_CAP_SPIRAM);
    posts = heap_caps_calloc(MAXP, sizeof *posts, MALLOC_CAP_SPIRAM);
    xTaskCreate(task, "gallery", 20480, NULL, 4, NULL);
    vTaskSuspend(NULL);
}
