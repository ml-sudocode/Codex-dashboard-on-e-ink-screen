// RSS reader, shared by Hackaday and News: rss_reader(feed URL, name). Joins Wi-Fi, reads the feed into PSRAM, shows
// the list at once, then fetches the posts' images in the background through wsrv.nl (downscaled to a small JPEG,
// decoded by the ROM's TJpgDec, dithered to 1 bit). Wi-Fi goes off (and ranging back on) when the images are in.
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
#include "rom/tjpgd.h"
#include "badge.h"

#define CAP (1024 * 1024) // feed buffer in PSRAM; Hackaday's feed carries full post bodies (~100-300 KB)
#define JCAP (256 * 1024) // one downscaled JPEG
#define MAXP 10           // posts
#define MAXI 64           // images in all, at most PERP per post (~32 KB each once dithered)
#define PERP 8
#define MAXL 4000         // laid-out lines of the open post
#define IW 640            // images fit inside IW x IH (wsrv.nl scales them)
#define IH 400
#define PROXY "https://wsrv.nl/?w=640&h=400&fit=inside&output=jpg&q=40&url="
#define WIPE_EVERY 6 // ponytail: scene changes (list<->post, page turns) between ghost wipes (2.8 s); tune on the badge
#define X0 32        // text column
#define X1 (W - 24)  // right edge of the text; the scroll bar sits right of it

typedef struct {
    char title[200], author[48], date[20];
    char *body; // cleaned text: '\n' between paragraphs, a byte 0x80 + image index alone on a line for an image
} post_t;
typedef struct {
    char url[400];  // the proxy URL
    int post, w, h; // w, h: the box it gets (from the tag's width/height), then what was decoded
    uint8_t *bits;  // 1 bit per pixel, rows of (w + 7) / 8 bytes, 1 = ink
    volatile int8_t state; // 0 to fetch, 1 ready, -1 failed
} img_t;

static const char *feed_url;
static post_t *posts; // PSRAM, like everything big here
static int nposts, nimg;
static img_t *imgs;
static char *feed, *jpg;
static char err[64];
static volatile int stage, bytes, imgs_done, cur_post; // stage: 0 joining, 1 downloading, 2 feed ready, 3 failed
static volatile int join_ms, feed_ms;
static volatile bool net_stop, net_running;

// ---- Text: HTML/XML/UTF-8 to the 7-bit font.
static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"; // U+00C0..U+00FF

static int put(char *o, int n, int max, unsigned cp)
{
    const char *s = NULL;
    char c[2] = {0};
    if (cp >= 0x20 && cp < 0x7F) c[0] = cp;
    else if (cp == '\n') c[0] = '\n';
    else if (cp == '\t' || cp == '\r' || cp == 0xA0) c[0] = ' ';
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) c[0] = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) c[0] = '"';
    else if (cp >= 0x2010 && cp <= 0x2015) c[0] = '-';
    else if (cp == 0x2026) s = "...";
    else if (cp == 0xD7) c[0] = 'x';
    else if (cp >= 0xC0 && cp <= 0xFF) c[0] = latin1[cp - 0xC0];
    if (!s) s = c; // anything else is dropped
    for (; *s && n < max - 1; s++) {
        if (*s == ' ' && (n == 0 || o[n - 1] == ' ' || o[n - 1] == '\n')) continue; // collapse whitespace
        if (*s == '\n') {
            while (n && o[n - 1] == ' ') n--;
            if (n == 0 || o[n - 1] == '\n') continue; // no empty paragraphs
        }
        o[n++] = *s;
    }
    return n;
}

// The value of name="..." inside the tag [s, e), into o; false if it is not there.
static bool attr(const char *s, const char *e, const char *name, char *o, int max)
{
    char k[16];
    int kl = snprintf(k, sizeof k, " %s=\"", name);
    for (; s + kl < e; s++)
        if (!strncmp(s, k, kl)) {
            const char *v = s + kl, *q = memchr(v, '"', e - v);
            if (!q) return false;
            snprintf(o, max, "%.*s", (int)(q - v), v);
            return true;
        }
    return false;
}

// An image of post p from the tag [s, e) (its URL in attribute u): returns its marker byte, 0 if not taken.
static int add_img(int p, const char *s, const char *e, const char *u)
{
    char src[300], v[12];
    int w = 0, h = 0, k = 0;
    for (int i = 0; i < nimg; i++) k += imgs[i].post == p;
    if (nimg == MAXI || k == PERP || !attr(s, e, u, src, sizeof src)) return 0;
    if (attr(s, e, "width", v, sizeof v)) w = atoi(v);
    if (attr(s, e, "height", v, sizeof v)) h = atoi(v);
    if (w && w < 64) return 0; // smileys and icons
    img_t *m = &imgs[nimg];
    if (w <= 0 || h <= 0) w = 16, h = 9; // unknown: assume a wide picture
    if (w * IH > h * IW) m->w = IW, m->h = IW * h / w; // the box wsrv.nl fits it into
    else m->h = IH, m->w = IH * w / h;
    int n = snprintf(m->url, sizeof m->url, PROXY);
    for (const char *c = src; *c && n < sizeof m->url - 4; c++) { // percent-encode the image URL
        if (!strncmp(c, "&amp;", 5)) c += 4;
        if (strchr("-._~/:", *c) || (*c >= '0' && *c <= '9') || ((*c | 32) >= 'a' && (*c | 32) <= 'z'))
            m->url[n++] = *c;
        else n += sprintf(m->url + n, "%%%02X", (unsigned char)*c);
    }
    m->url[n] = 0;
    m->post = p, m->bits = NULL, m->state = 0;
    return 0x80 + nimg++;
}

// Copies [s, e) into o as plain ASCII: no CDATA markers, entities and UTF-8 punctuation decoded. Tags: dropped, or
// for post p >= 0 (a body) block tags become paragraph breaks and <img> an image marker on its own line.
static void clean(char *o, int max, const char *s, const char *e, int p)
{
    int n = 0;
    while (s < e) {
        if (!strncmp(s, "<![CDATA[", 9)) s += 9;
        else if (!strncmp(s, "]]>", 3)) s += 3;
        else if (*s == '<') {
            const char *t = s;
            bool closing = s[1] == '/';
            while (s < e && *s != '>') s++;
            s++;
            if (p < 0) continue;
            char tag[16] = " ";
            for (int k = 1, i = 1 + closing; k < 14 && (((t[i] | 32) >= 'a' && (t[i] | 32) <= 'z') || (t[i] >= '0' && t[i] <= '9')); i++)
                tag[k++] = t[i] | 32;
            strcat(tag, " ");
            if (!strcmp(tag, " img ")) {
                int m = add_img(p, t, s, "src");
                if (m && n < max - 3) n = put(o, n, max, '\n'), o[n++] = m, o[n++] = '\n';
            } else if (!strcmp(tag, " iframe ") && !closing) {
                for (const char *c = "\n[video]\n"; *c; c++) n = put(o, n, max, *c);
            } else if (!strcmp(tag, " li ") && !closing) {
                n = put(o, n, max, '\n'), n = put(o, n, max, '-'), n = put(o, n, max, ' ');
            } else if (strstr(" p br div h1 h2 h3 h4 h5 h6 ul ol li figure figcaption blockquote pre tr table ", tag)) {
                n = put(o, n, max, '\n');
            }
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
    while (n && (o[n - 1] == ' ' || o[n - 1] == '\n')) n--;
    o[n] = 0;
}

// <tag>...</tag> inside [s, e): its content's start, its end in *end; NULL if not there.
static const char *find(const char *s, const char *e, const char *tag, const char **end)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(s, open), *b = a ? strstr(a, close) : NULL;
    if (!b || b > e) return NULL;
    *end = b;
    return a + strlen(open);
}

static void field(char *o, int max, const char *s, const char *e, const char *tag)
{
    const char *b, *a = find(s, e, tag, &b);
    if (a) clean(o, max, a, b, -1);
    else *o = 0;
}

static void parse(void)
{
    nposts = nimg = 0;
    for (const char *s = strstr(feed, "<item>"), *e; s && nposts < MAXP; s = strstr(e, "<item>")) {
        if (!(e = strstr(s, "</item>"))) break;
        int i = nposts++;
        post_t *p = &posts[i];
        field(p->title, sizeof p->title, s, e, "title");
        field(p->author, sizeof p->author, s, e, "dc:creator");
        field(p->date, 17, s, e, "pubDate"); // "Sat, 07 Nov 2026"
        const char *b, *a = find(s, e, "content:encoded", &b); // the full post; short feeds only have a description
        if (!a) a = find(s, e, "description", &b);
        if (!a) a = b = e;
        int max = b - a + 8, n = 0;
        if (!(p->body = heap_caps_malloc(max, MALLOC_CAP_SPIRAM))) continue;
        const char *th = strstr(s, "<media:thumbnail"), *te = th ? strchr(th, '>') : NULL; // BBC: a thumbnail only
        int m = te && te < e && !memmem(a, b - a, "<img", 4) ? add_img(i, th, te, "url") : 0;
        if (m) p->body[n++] = m, p->body[n++] = '\n';
        clean(p->body + n, max - n, a, b, i);
        char *rm = strstr(p->body, "...read more"); // Hackaday's descriptions end in a link
        if (rm) strcpy(rm, "...");
    }
}

// ---- Network task: join, the feed, then the images (the open post's first), leave.
static int64_t t0;

typedef struct {
    char *buf;
    int n, cap;
} sink_t;

static esp_err_t on_data(esp_http_client_event_t *e)
{
    sink_t *k = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !k) return ESP_OK;
    int len = e->data_len < k->cap - 1 - k->n ? e->data_len : k->cap - 1 - k->n;
    memcpy(k->buf + k->n, e->data, len);
    k->n += len;
    if (k->buf == feed) bytes = k->n;
    return ESP_OK;
}

// GET url into buf (cap bytes): bytes read, or -1 (reason in err).
static int get(esp_http_client_handle_t c, const char *url, char *buf, int cap)
{
    sink_t k = {buf, 0, cap};
    esp_http_client_set_url(c, url);
    esp_http_client_set_user_data(c, &k);
    esp_err_t e = esp_http_client_perform(c);
    esp_http_client_set_user_data(c, NULL);
    int r = esp_http_client_get_status_code(c);
    buf[k.n] = 0;
    if (e != ESP_OK) snprintf(err, sizeof err, "%s", esp_err_to_name(e));
    else if (r != 200) snprintf(err, sizeof err, "HTTP %d", r);
    else return k.n;
    ESP_LOGW("rss", "%s: %s", url, err);
    return -1;
}

static esp_http_client_handle_t client(void)
{
    return esp_http_client_init(&(esp_http_client_config_t){
        .url = feed_url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .user_agent = "badge-os",
        .buffer_size = 16384, .keep_alive_enable = true, .event_handler = on_data});
}

typedef struct {
    int n, pos, w, h;
    int16_t *g; // grey, w x h
} jdev_t;

static UINT jin(JDEC *jd, BYTE *buf, UINT len)
{
    jdev_t *d = jd->device;
    if (len > d->n - d->pos) len = d->n - d->pos;
    if (buf) memcpy(buf, jpg + d->pos, len);
    d->pos += len;
    return len;
}

static UINT jout(JDEC *jd, void *bm, JRECT *r)
{
    jdev_t *d = jd->device;
    BYTE *p = bm;
    for (int y = r->top; y <= r->bottom; y++)
        for (int x = r->left; x <= r->right; x++, p += 3)
            if (x < d->w && y < d->h) d->g[y * d->w + x] = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
    return 1;
}

// The JPEG in jpg (n bytes): decode, dither (Atkinson: crisp on e-paper), keep the bits in m.
static bool decode(img_t *m, int n)
{
    static uint8_t pool[4096];
    JDEC jd;
    jdev_t d = {.n = n};
    if (jd_prepare(&jd, jin, pool, sizeof pool, &d) != JDR_OK) return false;
    d.w = jd.width < IW ? jd.width : IW;
    d.h = jd.height < IH ? jd.height : IH;
    d.g = heap_caps_malloc(d.w * d.h * 2, MALLOC_CAP_SPIRAM);
    int st = (d.w + 7) / 8;
    uint8_t *bits = heap_caps_calloc(st * d.h, 1, MALLOC_CAP_SPIRAM);
    if (!d.g || !bits || jd_decomp(&jd, jout, 0) != JDR_OK) {
        free(d.g), free(bits);
        return false;
    }
    static const int8_t dx[] = {1, 2, -1, 0, 1, 0}, dy[] = {0, 0, 1, 1, 1, 2};
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            int16_t *g = &d.g[y * d.w + x];
            int v = *g < 128 ? 0 : 255, er = (*g - v) / 8;
            if (!v) bits[y * st + x / 8] |= 0x80 >> (x & 7);
            for (int k = 0; k < 6; k++)
                if (x + dx[k] >= 0 && x + dx[k] < d.w && y + dy[k] < d.h) g[dy[k] * d.w + dx[k]] += er;
        }
    free(d.g);
    m->w = d.w, m->h = d.h, m->bits = bits;
    return true;
}

static void net(void *arg)
{
    esp_http_client_handle_t c = NULL;
    t0 = esp_timer_get_time();
    if (!wifi_connect(20000)) {
        snprintf(err, sizeof err, "Could not join the badge Wi-Fi.");
        stage = 3;
    } else {
        join_ms = (esp_timer_get_time() - t0) / 1000;
        stage = 1;
        c = client();
        int n = get(c, feed_url, feed, CAP);
        esp_http_client_cleanup(c);
        feed_ms = (esp_timer_get_time() - t0) / 1000 - join_ms;
        ESP_LOGI("rss", "join %d ms, feed %d bytes in %d ms", join_ms, n, feed_ms);
        if (n >= 0 && (parse(), !nposts)) snprintf(err, sizeof err, "No posts in the feed (%d bytes).", n);
        stage = n < 0 || !nposts ? 3 : 2;
        c = stage == 2 ? client() : NULL;
    }
    while (c && !net_stop) {
        int i = -1, cp = cur_post;
        for (int k = 0; k < nimg && i < 0; k++)
            if (!imgs[k].state && imgs[k].post == cp) i = k;
        for (int k = 0; k < nimg && i < 0; k++)
            if (!imgs[k].state) i = k;
        if (i < 0) break;
        int64_t t = esp_timer_get_time();
        int len = get(c, imgs[i].url, jpg, JCAP);
        imgs[i].state = len > 0 && decode(&imgs[i], len) ? 1 : -1;
        if (len < 0) esp_http_client_cleanup(c), c = client(); // a fresh connection after an error
        ESP_LOGI("rss", "image %d: %d bytes, %s, %lld ms", i, len, imgs[i].state > 0 ? "ok" : "failed",
                 (esp_timer_get_time() - t) / 1000);
        imgs_done++;
    }
    if (c) esp_http_client_cleanup(c);
    wifi_off();
    ranging_start();
    net_running = false;
    vTaskDelete(NULL);
}

// ---- Screen.
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

static void draw_list(int sel)
{
    char s[80];
    memset(fb, 0, sizeof fb);
    for (int i = 0, y = TOP + 8; i < nposts; i++, y += 60) {
        if (i == sel) box(8, y, 12, 52, true);
        int y2 = wrap(X0, y, 2, 18, 2, posts[i].title);
        meta(s, sizeof s, &posts[i]);
        text(64, y2, 2, s, true);
    }
    ui_frame(1);
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

static void blit(const img_t *m, int x, int y, int maxh)
{
    int st = (m->w + 7) / 8;
    for (int r = 0; r < m->h && r < maxh; r++)
        for (int c = 0; c < m->w; c++)
            if (m->bits[r * st + c / 8] & 0x80 >> (c & 7)) fb[((y + r) * W + x + c) / 8] |= 0x80 >> ((x + c) & 7);
}

static void draw_post(int top)
{
    memset(fb, 0, sizeof fb);
    int y = TOP + 8, end = page_end(top);
    for (int i = top; i < end; y += lines[i++].h) {
        line_t *l = &lines[i];
        if (l->s) line(X0, y, l->scale, l->s, l->n);
        else if (l->img >= 0) {
            img_t *m = &imgs[l->img];
            int x = X0 + (X1 - X0 - m->w) / 2;
            if (m->state > 0) blit(m, x, y, l->h - 12); // decoded size can differ from the box by a pixel
            else {
                outline(x, y, m->w, l->h - 12, 2);
                text(x + 16, y + 16, 2, m->state ? "(no image)" : "(image loading)", true);
            }
        }
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

static void loading(const char *name)
{
    char s[64];
    int shown = -1;
    while (stage < 2) {
        int now = stage * 100000 + bytes / 1024;
        if (now != shown) {
            shown = now;
            memset(fb, 0, sizeof fb);
            text(48, TOP + 48, 5, "Loading...", true);
            text(48, TOP + 128, 3, name, true);
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

static void stop_net(void)
{
    net_stop = true;
    while (net_running) ms(50);
    for (int i = 0; i < nposts; i++) free(posts[i].body), posts[i].body = NULL;
    for (int i = 0; i < nimg; i++) free(imgs[i].bits), imgs[i].bits = NULL;
    nposts = nimg = 0;
}

static void run(const char *name)
{
    for (int switches = 0;;) { // load
        stop_net();
        *err = 0;
        stage = bytes = cur_post = 0, net_stop = false, net_running = true;
        xTaskCreate(net, "rss net", 16384, NULL, 4, NULL);
        ui_keys(NULL, NULL, NULL, NULL);
        loading(name);
        if (stage == 3) {
            ui_keys(NULL, NULL, NULL, "Retry");
            message(5, "No feed.", err);
            while (wait_btn() != BTN_B) {}
            continue;
        }
        for (int sel = 0, post = -1, top = 0;;) { // post < 0: the list
            bool scene = false;
            cur_post = post < 0 ? sel : post; // its images are fetched first
            if (post < 0) ui_keys("Up", "Down", "Open", "Refresh"), draw_list(sel);
            else ui_keys("Up", "Down", post + 1 < nposts ? "Next post" : NULL, "List"), draw_post(top);
            int b = wait_btn();
            if (post < 0) {
                if (b == BTN_PLUS && sel > 0) sel--;
                else if (b == BTN_MINUS && sel < nposts - 1) sel++;
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

static const char *run_name;

static void task(void *arg)
{
    run(run_name);
}

// Takes over the badge for good (home is MENU + and MENU -). The app task's stack is too small for TLS: own tasks.
void rss_reader(const char *url, const char *name)
{
    feed_url = url, run_name = name;
    feed = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    jpg = heap_caps_malloc(JCAP, MALLOC_CAP_SPIRAM);
    posts = heap_caps_calloc(MAXP, sizeof *posts, MALLOC_CAP_SPIRAM);
    imgs = heap_caps_calloc(MAXI, sizeof *imgs, MALLOC_CAP_SPIRAM);
    lines = heap_caps_calloc(MAXL, sizeof *lines, MALLOC_CAP_SPIRAM);
    xTaskCreate(task, "rss", 16384, NULL, 4, NULL);
    vTaskSuspend(NULL);
}

void app_hackaday(void)
{
    rss_reader("https://hackaday.com/blog/feed/", "hackaday.com");
}
