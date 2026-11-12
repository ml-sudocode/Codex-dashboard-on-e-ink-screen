// Settings: the badge opens a Wi-Fi access point "Badge-XXXX" (password "supercon") with a captive portal, so a
// phone that joins opens the settings page by itself. Saving stores the values (cfg_set) and restarts the badge.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "qrcode.h"
#include "badge/badge.h"

#define TAG "settings"
#define PASS "supercon"
#define ADDR "192.168.4.1" // the default SoftAP netif

static const char page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Badge settings</title><style>"
    "body{font:16px system-ui,sans-serif;margin:0 auto;max-width:34em;padding:1em}"
    "fieldset{border:1px solid #ccc;border-radius:8px;margin:0 0 1em}"
    "input,textarea,button,select{font:inherit;box-sizing:border-box;width:100%;padding:.4em;margin:.2em 0 .6em}"
    "input[type=radio],input[type=checkbox]{width:auto}canvas{width:100%;border:1px solid #ccc}"
    "button{background:#900;color:#fff;border:0;border-radius:8px;padding:.7em}small{color:#666}"
    "</style><h2>Badge settings</h2>"
    "<fieldset><legend>Username</legend><input id=name maxlength=12 placeholder='up to 12 characters'>"
    "<small>Top bar and other badges' Radar.</small></fieldset>"
    "<fieldset><legend>Wi-Fi the badge joins</legend>SSID<input id=ssid maxlength=32>"
    "Password<input id=pass maxlength=63 autocomplete=off></fieldset>"
    "<fieldset><legend>Codex Board</legend>Feed URL<input id=cb_url maxlength=240 "
    "placeholder='http://192.168.1.10:8765/feed.txt'>"
    "<small>Leave blank for USB. For Wi-Fi, enter the Linux collector or Mac relay address.</small></fieldset>"
    "<fieldset><legend>Badge app</legend>"
    "<label><input type=radio name=m id=mt checked> Text</label> <label><input type=radio name=m id=mi> Picture</label>"
    "<div id=bt><textarea id=badge_text rows=5 maxlength=1000></textarea></div>"
    "<div id=bi hidden><input type=file id=file accept='image/*'>"
    "Threshold <input type=range id=th min=0 max=255 value=128>"
    "<label><input type=checkbox id=dit checked> Dither</label>"
    "<canvas id=cv width=960 height=624></canvas><small id=ist></small></div></fieldset>"
    "<fieldset><legend>Weather location</legend>"
    "<label><input type=radio name=w id=wc checked> Place name</label> <label><input type=radio name=w id=wn> Coordinates</label>"
    "<div id=wcd><input id=wx_city maxlength=64 placeholder='Pasadena (default), or e.g. Istanbul'>"
    "<small>The Weather app looks it up online.</small></div>"
    "<div id=wnd hidden>Paste a Google Maps link<input id=link placeholder='https://www.google.com/maps/@40.4,-3.7,12z'>"
    "Latitude<input id=wx_lat inputmode=decimal>Longitude<input id=wx_lon inputmode=decimal>"
    "<small>Or in Google Maps long-press a place and copy the coordinates (short maps.app.goo.gl links do not "
    "work: the badge has no internet to follow them).</small></div>"
    "Temperature<select id=wx_unit><option value=''>Celsius<option value=F>Fahrenheit</select></fieldset>"
    "<button id=save>Save and restart the badge</button><p id=st></p><script>"
    "var K=['name','ssid','pass','badge_text','wx_city','wx_lat','wx_lon','wx_unit','cb_url'],$=function(i){return document.getElementById(i)},"
    "img=null,gray=null,hadImg=false;"
    "fetch('/cfg').then(function(r){return r.json()}).then(function(c){K.forEach(function(k){$(k).value=c[k]||''});"
    "hadImg=c.img;if(!c.wx_city&&c.wx_lat)$('wn').checked=true;if(c.img){$('mi').checked=true;$('ist').textContent='A picture is saved; choose a file to replace it.'}mode()});"
    "function mode(){$('bt').hidden=$('mi').checked;$('bi').hidden=!$('mi').checked;"
    "$('wcd').hidden=$('wn').checked;$('wnd').hidden=!$('wn').checked}"
    "$('mt').onchange=$('mi').onchange=$('wc').onchange=$('wn').onchange=mode;"
    "$('link').oninput=function(){var s=decodeURIComponent(this.value),"
    "m=s.match(/!3d(-?[\\d.]+)!4d(-?[\\d.]+)/)||s.match(/@(-?[\\d.]+),\\s*(-?[\\d.]+)/)||s.match(/(-?\\d+\\.\\d+)\\s*,\\s*(-?\\d+\\.\\d+)/);"
    "if(m){$('wx_lat').value=m[1];$('wx_lon').value=m[2]}};"
    "$('file').onchange=function(){var f=this.files[0];if(!f)return;var im=new Image();im.onload=function(){"
    "var c=document.createElement('canvas');c.width=960;c.height=624;var x=c.getContext('2d');"
    "x.fillStyle='#fff';x.fillRect(0,0,960,624);var s=Math.min(960/im.width,624/im.height),w=im.width*s,h=im.height*s;"
    "x.drawImage(im,(960-w)/2,(624-h)/2,w,h);var d=x.getImageData(0,0,960,624).data;gray=new Float32Array(960*624);"
    "for(var i=0;i<gray.length;i++)gray[i]=.299*d[4*i]+.587*d[4*i+1]+.114*d[4*i+2];draw()};"
    "im.src=URL.createObjectURL(f)};"
    "$('th').oninput=$('dit').onchange=function(){if(gray)draw()};"
    "function draw(){var g=Float32Array.from(gray),t=+$('th').value,D=$('dit').checked;img=new Uint8Array(74880);"
    "var x=$('cv').getContext('2d'),o=x.createImageData(960,624);"
    "for(var y=0;y<624;y++)for(var X=0;X<960;X++){var i=y*960+X,v=g[i],k=v<t,e=v-(k?0:255);"
    "if(k)img[i>>3]|=128>>(i&7);o.data.set(k?[140,0,0,255]:[255,255,255,255],4*i);"
    "if(D){if(X<959)g[i+1]+=e*7/16;if(y<623){if(X)g[i+959]+=e*3/16;g[i+960]+=e*5/16;if(X<959)g[i+961]+=e/16}}}"
    "x.putImageData(o,0,0)}"
    "$('save').onclick=function(){var b='',pic=$('mi').checked;"
    "var skip={badge_text:pic,wx_city:$('wn').checked,wx_lat:$('wc').checked,wx_lon:$('wc').checked};"
    "K.forEach(function(k){if(!skip[k])b+=k+'\\0'+$(k).value+'\\0'});"
    "if(pic&&!img&&!hadImg){$('st').textContent='Choose a picture first.';return}"
    "$('st').textContent='Saving...';"
    "(pic&&img?fetch('/image',{method:'POST',body:img}):Promise.resolve({ok:1})).then(function(r){"
    "if(!r.ok)throw 'picture';return fetch('/save',{method:'POST',body:b})}).then(function(r){return r.text()})"
    ".then(function(t){$('st').textContent=t}).catch(function(e){$('st').textContent='Failed: '+e})}"
    "</script>";

static volatile bool saved;
static const char *const keys[] = {"name", "ssid", "pass", "badge_text", "wx_city", "wx_lat", "wx_lon", "wx_unit", "cb_url"};

static void json_str(char **p, const char *key, const char *v) // "key":"v", (v JSON-escaped)
{
    *p += sprintf(*p, "\"%s\":\"", key);
    for (; *v; v++) {
        unsigned char c = *v;
        if (c == '"' || c == '\\') *(*p)++ = '\\', *(*p)++ = c;
        else if (c < 0x20) *p += sprintf(*p, "\\u%04x", c);
        else *(*p)++ = c;
    }
    *p += sprintf(*p, "\",");
}

static esp_err_t get_page(httpd_req_t *r) { return httpd_resp_send(r, page, sizeof page - 1); }

static esp_err_t get_cfg(httpd_req_t *r)
{
    char *out = heap_caps_malloc(16 * 1024 + IMG_BYTES, MALLOC_CAP_SPIRAM), *p = out, v[1024];
    if (!out) return httpd_resp_send_500(r);
    *p++ = '{';
    for (int i = 0; i < sizeof keys / sizeof *keys; i++) cfg_get(keys[i], v, sizeof v), json_str(&p, keys[i], v);
    p += sprintf(p, "\"img\":%s}", cfg_get_image((uint8_t *)out + 16 * 1024) ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_send(r, out, p - out);
    free(out);
    return e;
}

static int recv_all(httpd_req_t *r, char *buf, int n)
{
    for (int got = 0; got < n;) {
        int k = httpd_req_recv(r, buf + got, n - got);
        if (k <= 0) return -1;
        got += k;
    }
    return n;
}

static bool printable(const char *s)
{
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7e) return false;
    return true;
}

static bool known(const char *k)
{
    for (int i = 0; i < sizeof keys / sizeof *keys; i++)
        if (!strcmp(k, keys[i])) return true;
    return false;
}

static bool coord(const char *s, double max) // empty, or a number in -max..max
{
    char *e;
    double d = strtod(s, &e);
    return !*s || (e != s && !*e && d >= -max && d <= max);
}

static bool feed_url(const char *s)
{
    if (!*s) return true;
    if (strlen(s) > 240 || strncmp(s, "http://", 7) || !s[7] || s[7] == '/') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 32 || *p > 126 || *p == '@' || *p == '#') return false;
    return true;
}

// Body: key \0 value \0 ... (no URL decoding needed). Everything is checked before anything is stored.
static esp_err_t post_save(httpd_req_t *r)
{
    static char b[4097];
    if (r->content_len <= 0 || r->content_len >= sizeof b || recv_all(r, b, r->content_len) < 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Too long.");
    b[r->content_len] = 0;
    const char *kv[32];
    int n = 0;
    for (char *p = b; p < b + r->content_len && n < 32; p += strlen(p) + 1) kv[n++] = p;
    const char *bad = n % 2 ? "Bad request." : NULL;
    for (int i = 0; i + 1 < n && !bad; i += 2) {
        const char *k = kv[i], *v = kv[i + 1];
        int l = strlen(v);
        if (!strcmp(k, "name") && (l > 12 || !printable(v))) bad = "Username: up to 12 plain characters (A-Z, 0-9, ...).";
        else if (!strcmp(k, "ssid") && l > 32) bad = "SSID: up to 32 characters.";
        else if (!strcmp(k, "pass") && l && (l < 8 || l > 63)) bad = "Wi-Fi password: 8 to 63 characters, or empty.";
        else if (!strcmp(k, "wx_city") && (l > 64 || !printable(v))) bad = "Place name: up to 64 plain characters.";
        else if (!strcmp(k, "wx_unit") && strcmp(v, "") && strcmp(v, "C") && strcmp(v, "F")) bad = "Unit: C or F.";
        else if (!strcmp(k, "wx_lat") && !coord(v, 90)) bad = "Latitude: a number from -90 to 90.";
        else if (!strcmp(k, "wx_lon") && !coord(v, 180)) bad = "Longitude: a number from -180 to 180.";
        else if (!strcmp(k, "cb_url") && !feed_url(v)) bad = "Feed URL: http:// address, up to 240 characters, no credentials.";
        else if (!known(k)) bad = "Unknown setting.";
    }
    if (bad) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, bad);
    for (int i = 0; i + 1 < n; i += 2) {
        cfg_set(kv[i], *kv[i + 1] ? kv[i + 1] : NULL); // empty = unset (ssid/pass: back to the event Wi-Fi)
        if (!strcmp(kv[i], "badge_text")) cfg_set_image(NULL); // text replaces the picture
        if (!strcmp(kv[i], "wx_city")) cfg_set("wx_lat", NULL), cfg_set("wx_lon", NULL); // a place name replaces
        if (!strcmp(kv[i], "wx_lat")) cfg_set("wx_city", NULL);                         // coordinates, and back
        if (!strcmp(kv[i], "wx_city") || !strcmp(kv[i], "wx_lat")) cfg_set("wx_place", NULL); // Weather looks it up again
    }
    ESP_LOGI(TAG, "saved %d settings", n / 2);
    httpd_resp_sendstr(r, "Saved. The badge restarts now.");
    saved = true;
    return ESP_OK;
}

static esp_err_t post_image(httpd_req_t *r)
{
    if (r->content_len != IMG_BYTES) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Want 74880 bytes.");
    uint8_t *img = heap_caps_malloc(IMG_BYTES, MALLOC_CAP_SPIRAM);
    bool ok = img && recv_all(r, (char *)img, IMG_BYTES) > 0 && cfg_set_image(img);
    free(img);
    return ok ? httpd_resp_sendstr(r, "ok") : httpd_resp_send_500(r);
}

// Captive portal, part 1: every DNS A query gets the badge's address, so the phone's connectivity check lands here.
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(s, (struct sockaddr *)&a, sizeof a);
    static const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1}; // name ptr, A, IN, TTL 60
    for (;;) {
        uint8_t b[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, b, sizeof b - sizeof answer, 0, (struct sockaddr *)&from, &fl);
        int p = 12; // skip the header, then the first question's name
        while (p < n && b[p]) p += b[p] + 1;
        p += 5; // name end, QTYPE, QCLASS
        if (n < 12 || p > n) continue;
        bool a_query = b[p - 4] == 0 && b[p - 3] == 1;
        b[2] = 0x81, b[3] = 0x80;                     // response, no error
        b[4] = 0, b[5] = 1, b[6] = 0, b[7] = a_query; // 1 question, 1 answer for A (others: none)
        memset(b + 8, 0, 4);                          // drop authority/additional (EDNS)
        if (a_query) memcpy(b + p, answer, sizeof answer), p += sizeof answer;
        sendto(s, b, p, 0, (struct sockaddr *)&from, fl);
    }
}

// Captive portal, part 2: anything unknown (generate_204, hotspot-detect.html, ...) goes to the page.
// iOS wants a body, not only the redirect.
static esp_err_t to_portal(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "http://" ADDR "/");
    return httpd_resp_sendstr(r, "Badge settings: http://" ADDR "/");
}

static void draw_qr(esp_qrcode_handle_t qr) // right half, as big as fits in 336 px
{
    int n = esp_qrcode_get_size(qr), m = 336 / n, x0 = 576 + (336 - n * m) / 2, y0 = TOP + 120;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (esp_qrcode_get_module(qr, x, y)) box(x0 + x * m, y0 + y * m, m, m, true);
}

static void run(void)
{
    char id[5], ssid[16];
    badge_id(id);
    snprintf(ssid, sizeof ssid, "Badge-%s", id);

    wifi_on(); // stops ranging; STA mode, driver up
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap(); // once: going home restarts the badge
    wifi_config_t c = {.ap = {.password = PASS, .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK}};
    c.ap.ssid_len = snprintf((char *)c.ap.ssid, sizeof c.ap.ssid, "%s", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_page},
        {.uri = "/cfg", .method = HTTP_GET, .handler = get_cfg},
        {.uri = "/save", .method = HTTP_POST, .handler = post_save},
        {.uri = "/image", .method = HTTP_POST, .handler = post_image},
    };
    for (int i = 0; i < sizeof uris / sizeof *uris; i++) httpd_register_uri_handler(srv, &uris[i]);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, to_portal);
    ESP_LOGI(TAG, "SoftAP \"%s\", password \"" PASS "\", http://" ADDR "/", ssid);

    ui_keys(NULL, NULL, NULL, "Home");
    memset(fb, 0, sizeof fb);
    text(48, TOP + 32, 5, "Settings", true);
    text(48, TOP + 120, 3, "Join this Wi-Fi:", true);
    text(48, TOP + 160, 5, ssid, true);
    text(48, TOP + 232, 3, "Password:", true);
    text(48, TOP + 272, 5, PASS, true);
    text(48, TOP + 344, 3, "or scan the QR code.", true);
    text(48, TOP + 416, 3, "The page opens by", true);
    text(48, TOP + 448, 3, "itself, or browse to", true);
    text(48, TOP + 488, 3, "http://" ADDR, true);
    text(48, TOP + 560, 3, "Saving restarts it.", true);
    char wifi_qr[64];
    snprintf(wifi_qr, sizeof wifi_qr, "WIFI:T:WPA;S:%s;P:" PASS ";;", ssid);
    esp_qrcode_config_t q = ESP_QRCODE_CONFIG_DEFAULT();
    q.display_func = draw_qr;
    q.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_qrcode_generate(&q, wifi_qr);
    ui_frame(1);
    ui_close();

    // The web server runs on its own task: this one only waits for Home or a save, the network never waits on it.
    while (!saved)
        if (btn_get(200) == BTN_B) esp_restart();
    text(48, TOP + 600, 3, "Saved. Restarting...", true); // a fresh line: no text over erased text
    ui_frame(1);
    esp_restart();
}

static void icon(int x, int y) // a gear
{
    icon_base(x, y);
    int c = ICON / 2;
    for (int r = 0; r < ICON; r++)
        for (int k = 0; k < ICON; k++) {
            int dx = k - c, dy = r - c, d2 = dx * dx + dy * dy;
            bool tooth = d2 < 64 * 64 && (abs(dx) < 12 || abs(dy) < 12 || abs(dx - dy) < 16 || abs(dx + dy) < 16);
            if ((d2 < 46 * 46 || tooth) && d2 > 20 * 20) box(x + k, y + r, 1, 1, false);
        }
}

const app_t app_settings = {"Settings", icon, run};
