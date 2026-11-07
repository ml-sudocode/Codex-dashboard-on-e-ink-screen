// Weather: now and the next days at the saved location from Open-Meteo. Settings: wx_lat + wx_lon, or wx_city (looked
// up once with Open-Meteo geocoding; the result goes to wx_lat, wx_lon and wx_place), and wx_unit ("F", else Celsius).
// Joins Wi-Fi only to fetch, then gives the radio back to ranging. Refreshes every 30 min and on B.
#include <ctype.h>
#include <math.h>
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

#define EVERY_US (30 * 60 * 1000000LL)
#define CAP 8192 // the reply is ~1.5 KB
#define DAYS 6   // today + 5

typedef struct {
    int gen;                 // fetches that brought data; 0 = none yet
    float temp, wind;
    int code, wdir, hh, mm;  // hh:mm = fetch time, the location's local time
    int dcode[DAYS], hi[DAYS], lo[DAYS], wday[DAYS];
    char status[64];         // "Updating...", an error, or ""
} wx_t;
static wx_t wx;              // the main task writes, the draw task copies under lock
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t drawer;
static char lat[16], lon[16], place[64], city[48];
static bool fahr;
static int date_hm = -1;     // minutes since UTC midnight, from the reply's Date header

// ---- Weather codes (WMO) to words and an icon.
enum { SUN, FEW, CLOUD, FOG, RAIN, SNOW, STORM };

static int kind(int c)
{
    return c == 0 ? SUN : c == 1 || c == 2 ? FEW : c == 3 ? CLOUD : c < 50 ? FOG
        : (c >= 71 && c <= 77) || c == 85 || c == 86 ? SNOW : c >= 95 ? STORM : RAIN;
}

static const char *words(int c)
{
    static const struct { int c; const char *s; } w[] = {
        {0, "Clear"}, {1, "Mainly clear"}, {2, "Partly cloudy"}, {3, "Overcast"}, {48, "Fog"}, {55, "Drizzle"},
        {57, "Freezing drizzle"}, {65, "Rain"}, {67, "Freezing rain"}, {77, "Snow"}, {82, "Showers"},
        {86, "Snow showers"}, {99, "Thunderstorm"}};
    for (int i = 0; i < sizeof w / sizeof *w; i++)
        if (c <= w[i].c) return w[i].s;
    return "?";
}

// ---- Parsing the JSON by hand: the reply is small and fixed (no cJSON in IDF 6).
static const char *after(const char *s, const char *key)
{
    const char *p = s ? strstr(s, key) : NULL;
    return p ? p + strlen(key) : NULL;
}

static bool num(const char *sec, const char *key, float *v)
{
    const char *p = after(sec, key);
    if (p) *v = strtof(p, NULL);
    return p;
}

static bool arr(const char *sec, const char *key, int *v)
{
    const char *p = after(sec, key);
    for (int i = 0; p && i < DAYS; i++) {
        char *e;
        v[i] = lroundf(strtof(p, &e));
        p = *e == ',' ? e + 1 : e;
    }
    return p;
}

static int weekday(const char *d) // "YYYY-MM-DD", 0 = Sunday (Sakamoto)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = atoi(d), m = atoi(d + 5), dd = atoi(d + 8);
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + dd) % 7;
}

static bool parse(const char *js, wx_t *w)
{
    const char *cur = after(js, "\"current\":{"), *day = after(js, "\"daily\":{"), *t;
    float code = 0, dir = 0, off = 0;
    if (!num(cur, "\"temperature_2m\":", &w->temp) || !num(cur, "\"weather_code\":", &code)
        || !num(cur, "\"wind_speed_10m\":", &w->wind) || !num(cur, "\"wind_direction_10m\":", &dir)
        || !arr(day, "\"weather_code\":[", w->dcode) || !arr(day, "\"temperature_2m_max\":[", w->hi)
        || !arr(day, "\"temperature_2m_min\":[", w->lo) || !(t = after(day, "\"time\":[")))
        return false;
    w->code = code, w->wdir = dir;
    for (int i = 0; i < DAYS && (t = strchr(t, '"')); i++, t += 12) w->wday[i] = weekday(t + 1);
    num(js, "\"utc_offset_seconds\":", &off);
    int m = date_hm < 0 ? 0 : ((date_hm + (int)off / 60) % 1440 + 1440) % 1440;
    w->hh = m / 60, w->mm = m % 60;
    return true;
}

// ---- Network.
static esp_err_t on_http(esp_http_client_event_t *e)
{
    int h, m;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Date")) // "Fri, 06 Nov 2026 22:15:03 GMT"
        if (sscanf(e->header_value, "%*s %*d %*s %*d %d:%d", &h, &m) == 2) date_hm = h * 60 + m;
    return ESP_OK;
}

// GET url into buf (Wi-Fi already up); returns "" or what went wrong.
static const char *get(const char *url, char *buf)
{
    static char err[64];
    esp_http_client_handle_t c = esp_http_client_init(&(esp_http_client_config_t){
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        .user_agent = "badge-os", .event_handler = on_http});
    int n = 0, r = -1;
    date_hm = -1;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        r = esp_http_client_get_status_code(c);
        for (int got; n < CAP - 1 && (got = esp_http_client_read(c, buf + n, CAP - 1 - n)) > 0;) n += got;
    }
    esp_http_client_cleanup(c);
    buf[n] = 0;
    ESP_LOGI("weather", "%s, HTTP %d, %d bytes", esp_err_to_name(e), r, n);
    *err = 0;
    if (e != ESP_OK) snprintf(err, sizeof err, "Fetch failed: %s", esp_err_to_name(e));
    else if (r != 200) snprintf(err, sizeof err, "Open-Meteo said HTTP %d.", r);
    return err;
}

static void str(const char *js, const char *key, char *o, int n) // "key":"value" -> o (no escapes in place names)
{
    const char *p = after(js, key), *e = p ? strchr(p, '"') : NULL;
    snprintf(o, n, "%.*s", e ? (int)(e - p) : 0, p ? p : "");
}

// wx_city set, no coordinates yet: look it up once and save the result.
static const char *geocode(char *buf, const char *city)
{
    char url[200], *u = url + sprintf(url, "https://geocoding-api.open-meteo.com/v1/search?count=1&name=");
    for (const unsigned char *c = (const unsigned char *)city; *c && u < url + sizeof url - 4; c++)
        u += isalnum(*c) || strchr("-_.~", *c) ? sprintf(u, "%c", *c) : sprintf(u, "%%%02X", *c);
    const char *err = get(url, buf);
    if (*err) return err;
    char name[48], country[48];
    float la, lo;
    const char *r = after(buf, "\"results\":[");
    if (!num(r, "\"latitude\":", &la) || !num(r, "\"longitude\":", &lo)) {
        static char nf[64];
        snprintf(nf, sizeof nf, "No place called \"%.20s\" found: check Settings.", city);
        return nf;
    }
    str(r, "\"name\":\"", name, sizeof name);
    str(r, "\"country\":\"", country, sizeof country);
    snprintf(lat, sizeof lat, "%.4f", la);
    snprintf(lon, sizeof lon, "%.4f", lo);
    snprintf(place, sizeof place, "%.30s%s%.30s", name, *country ? ", " : "", country);
    cfg_set("wx_lat", lat), cfg_set("wx_lon", lon), cfg_set("wx_place", place);
    return "";
}

static void set_status(const char *s)
{
    taskENTER_CRITICAL(&lock);
    snprintf(wx.status, sizeof wx.status, "%s", s);
    taskEXIT_CRITICAL(&lock);
    xTaskNotifyGive(drawer);
}

static void fetch(char *buf)
{
    char url[400], err[64] = "";
    wx_t w;
    set_status("Updating...");
    if (!wifi_connect(20000)) snprintf(err, sizeof err, "Could not join the Wi-Fi.");
    else if (!*lat) snprintf(err, sizeof err, "%s", geocode(buf, city));
    if (!*err) {
        snprintf(url, sizeof url,
            "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&forecast_days=%d"
            "&current=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min%s",
            lat, lon, DAYS, fahr ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
        snprintf(err, sizeof err, "%s", get(url, buf));
        if (!*err && !parse(buf, &w)) snprintf(err, sizeof err, "Could not read the reply.");
    }
    wifi_off();
    ranging_start();
    taskENTER_CRITICAL(&lock);
    if (!*err) w.gen = wx.gen + 1, wx = w;
    snprintf(wx.status, sizeof wx.status, "%s", err);
    taskEXIT_CRITICAL(&lock);
    xTaskNotifyGive(drawer);
}

// ---- Drawing.
static void disc(int cx, int cy, int r, bool ink)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = sqrtf(r * r - dy * dy);
        box(cx - dx, cy + dy, 2 * dx + 1, 1, ink);
    }
}

static void ring(int x, int y, int r) // the font has no degree sign
{
    disc(x + r, y + r, r, true);
    disc(x + r, y + r, r * 2 / 5, false);
}

// "21" + ring (+ "C"): returns the x after it.
static int deg(int x, int y, int scale, int v, const char *unit)
{
    char s[8];
    snprintf(s, sizeof s, "%d", v);
    text(x, y, scale, s, true);
    x += 8 * scale * strlen(s) + scale;
    ring(x, y, scale * 3 / 2 + 1);
    x += 3 * scale + 2 + scale;
    if (unit) text(x, y, scale, unit, true), x += 8 * scale;
    return x;
}

static void cloud(int x, int y, int s, int t) // in an s x s/2 box: outlined, white inside (hides what is behind)
{
    for (int pass = 0, e = t; pass < 2; pass++, e = 0) {
        bool ink = !pass;
        disc(x + s * 3 / 10, y + s * 3 / 10, s / 5 + e, ink);
        disc(x + s * 11 / 20, y + s / 5, s / 4 + e, ink);
        disc(x + s * 3 / 4, y + s * 7 / 20, s / 6 + e, ink);
        box(x + s * 3 / 10 - e, y + s * 3 / 10 - e, s * 9 / 20 + 2 * e, s / 5 + 2 * e, ink);
        disc(x + s * 3 / 10, y + s * 3 / 10, s / 5 - (pass ? t : 0), ink); // round the bottom corners
    }
    // ponytail: the second pass leaves the ring of thickness t; good enough at these sizes
}

static void sun(int cx, int cy, int r)
{
    disc(cx, cy, r, true);
    int a = r * 3 / 2, b = r * 2, w = r / 4 + 1;
    box(cx - w / 2, cy - b, w, b - a, true), box(cx - w / 2, cy + a, w, b - a, true);
    box(cx - b, cy - w / 2, b - a, w, true), box(cx + a, cy - w / 2, b - a, w, true);
    for (int i = 0; i < 4; i++) { // diagonals: little squares
        int d = (a + b) * 35 / 100;
        box(cx + (i & 1 ? d : -d) - w / 2, cy + (i & 2 ? d : -d) - w / 2, w, w, true);
    }
}

static void icon(int x, int y, int s, int k) // in an s x s box
{
    int t = s / 25 + 1;
    if (k == SUN) return sun(x + s / 2, y + s / 2, s / 5);
    if (k == FEW) sun(x + s * 3 / 10, y + s * 3 / 10, s / 7);
    if (k == FOG) {
        for (int i = 0; i < 4; i++) box(x + s / 10 + (i & 1) * s / 10, y + s / 4 + i * s / 7, s * 7 / 10, t * 2, true);
        return;
    }
    int cy = k == FEW || k == CLOUD ? y + s * 3 / 10 : y + s / 8;
    cloud(x, cy, s, t);
    int top = cy + s / 2 + t + s / 20;
    for (int i = 0; i < 3; i++) {
        int dx = x + s / 4 + i * s / 4;
        if (k == RAIN) // slanted drops
            for (int j = 0; j < s / 5; j++) box(dx - j / 3, top + j, t + 1, 1, true);
        if (k == SNOW) disc(dx, top + s / 10 + (i & 1) * s / 12, s / 22 + 1, true);
    }
    if (k == STORM) { // a bolt: two slants joined by a step
        int h = s / 5, bx = x + s / 2;
        for (int j = 0; j < h; j++) {
            box(bx - j / 2, top + j, t * 3, 1, true);
            box(bx + h / 4 - j / 2, top + h + j, t * 3, 1, true);
        }
        box(bx - h / 2, top + h - t, h / 4 + h / 2 + t * 3, t * 2, true);
    }
}

static void draw(const wx_t *w)
{
    static const char *dn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *dir[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const char *u = fahr ? "F" : "C";
    char s[80];
    memset(fb, 0, sizeof fb);
    if (!w->gen) { // nothing to show yet
        text(48, TOP + 48, 6, "Weather", true);
        const char *m = *w->status ? w->status : "Updating...";
        text(48, TOP + 160, strlen(m) > 36 ? 2 : 3, m, true);
        if (*w->status && strcmp(w->status, "Updating...")) text(48, TOP + 210, 3, "B: try again", true);
        return;
    }
    icon(40, TOP + 24, 220, kind(w->code));
    deg(290, TOP + 30, 12, lroundf(w->temp), u);
    text(290, TOP + 146, 4, words(w->code), true);
    snprintf(s, sizeof s, "Wind %d %s %s", (int)lroundf(w->wind), fahr ? "mph" : "km/h", dir[(w->wdir + 22) / 45 % 8]);
    text(290, TOP + 196, 3, s, true);
    text(290, TOP + 236, 3, "High", true);
    int x = deg(290 + 5 * 24, TOP + 236, 3, w->hi[0], NULL);
    text(x + 24, TOP + 236, 3, "Low", true);
    deg(x + 24 + 4 * 24, TOP + 236, 3, w->lo[0], NULL);
    if (*place) snprintf(s, sizeof s, "%.36s", place);
    else snprintf(s, sizeof s, "%s, %s", lat, lon);
    text(48, TOP + 292, 2, s, true);
    snprintf(s, sizeof s, "Updated %02d:%02d", w->hh, w->mm);
    text(W - 48 - 16 * strlen(s), TOP + 292, 2, s, true);
    box(32, TOP + 320, W - 64, 3, true);
    for (int i = 1; i < DAYS; i++) {
        int cx = (i - 1) * W / (DAYS - 1) + W / (DAYS - 1) / 2;
        text(cx - 36, TOP + 340, 3, dn[w->wday[i]], true);
        icon(cx - 55, TOP + 380, 110, kind(w->dcode[i]));
        deg(cx - 60, TOP + 512, 3, w->hi[i], NULL);
        deg(cx - 60, TOP + 552, 3, w->lo[i], NULL);
    }
    if (*w->status) text(48, H - 28, 2, w->status, true);
}

static void draw_task(void *arg)
{
    int gen = 0;
    for (;;) {
        wx_t w;
        taskENTER_CRITICAL(&lock);
        w = wx;
        taskEXIT_CRITICAL(&lock);
        if (w.gen != gen && gen) ui_wipe(); // new numbers: shake the old ones' ghosts off
        gen = w.gen;
        draw(&w);
        ui_frame(1);
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000))) ui_close(), ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void run(void *arg)
{
    char *buf = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM), u[4] = "";
    if (cfg_get("wx_lat", lat, sizeof lat) < 0 || cfg_get("wx_lon", lon, sizeof lon) < 0) *lat = *lon = 0;
    if (cfg_get("wx_city", city, sizeof city) > 0) cfg_get("wx_place", place, sizeof place);
    else if (!*lat) strcpy(lat, "34.1478"), strcpy(lon, "-118.1445"), strcpy(place, "Pasadena, US"); // Supercon
    cfg_get("wx_unit", u, sizeof u);
    fahr = *u == 'F' || *u == 'f';
    ui_keys(NULL, NULL, NULL, "Refresh");
    xTaskCreate(draw_task, "wx draw", 6144, NULL, 4, &drawer);
    for (;;) {
        fetch(buf);
        int64_t due = esp_timer_get_time() + EVERY_US;
        btn_flush();
        while (esp_timer_get_time() < due)
            if (btn_get((due - esp_timer_get_time()) / 1000 + 1) == BTN_B) break;
    }
}

// Never returns: home is MENU + and MENU -.
void app_weather(void)
{
    xTaskCreate(run, "weather", 16384, NULL, 4, NULL);
    vTaskSuspend(NULL);
}
