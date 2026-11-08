// Ranging: badges running badge os find each other with ESP-NOW beacons on channel 1 and measure distances with
// Wi-Fi FTM (802.11mc round-trip time). Each beacon carries the sender's own ranges, so every badge also learns the
// distances between the others. The OS starts it at boot; it needs Wi-Fi in APSTA mode, the hidden SoftAP being this
// badge's FTM responder, so wifi_on/wifi_connect/wifi_off stop it. Background: apps/radar/RADAR_RESEARCH.md.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "badge/badge.h"
#include "badge/os.h"

static const char *TAG = "os";
#define RNG_CH 1
#define RNG_FRAMES 16       // FTM frames per session (API: 0, 16, 24, 32, 64): two bursts of 8
#define RNG_PERIOD 2        // FTM burst period, 100 ms units
#define RNG_WAIT_MS 1000    // for the FTM report: a (16, 2) session took ~0.3 s on our badges
#define RNG_OFFSET_CM 0     // what FTM reads at 0 m, subtracted: 0 on our badges (checked side by side)
#define RNG_KEEP 128        // pairs kept, own and shared: n badges share up to n * 8 (about 80 with 10 badges)
#define RNG_SHARE 8         // own ranges per BOSR beacon (v0.8 badges reject more)
#define RNG_FAST 16         // results per BOSF packet
#define RNG_STALE_US 30000000LL // a pair not heard of for 30 s is dropped

// Two ESP-NOW packets on channel 1. BOSR, ~1 s: the sender's SoftAP (where to aim FTM), its latest ranges and its
// username; v0.8 badges know only this one, so it stays byte for byte as it was. BOSF, ~250 ms, badges after v0.8:
// the raw results of the sessions the sender finished since its last BOSF, so the badge it measured needs not
// measure back (half the FTM traffic between updated badges).
typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    int16_t cm;
} rng_pair_t;

typedef struct __attribute__((packed)) {
    char magic[4]; // "BOSR"
    uint8_t ap[6]; // the sender's SoftAP MAC: where to aim FTM
    uint8_t n;
    rng_pair_t r[RNG_SHARE];
} rng_beacon_t;

typedef struct __attribute__((packed)) {
    char magic[4]; // "BOSF"
    uint8_t n;
    rng_pair_t r[RNG_FAST];
} rng_fast_t;

// a -> b. Own entries (a = me) also keep b's SoftAP MAC and the last 4 FTM results (the minimum is the best guess:
// multipath only ever makes the path longer).
static struct {
    uint8_t a[6], b[6], ap[6];
    char name[13]; // own entries: b's username from its beacon, "" if none
    int16_t cm, hist[4];
    int64_t seen, at;  // last beacon about this pair, last distance
    int64_t fast, tried; // own entries: b's last BOSF (none: a v0.8 badge), our last FTM attempt on b
} rng[RNG_KEEP];
static uint8_t rng_me[6];
static rng_pair_t rng_pend[RNG_FAST]; // my results for the next BOSF
static int rng_npend;
static SemaphoreHandle_t rng_lock, rng_done;
static volatile bool rng_run;
static EventGroupHandle_t ftm_ev; // bit 0: an FTM report
static volatile int ftm_cm;       // last FTM result in cm, -1 failed
static uint8_t ftm_peer[6];       // ... and whose SoftAP it was for

static void on_ftm(void *arg, esp_event_base_t base, int32_t id, void *data) // one FTM session done
{
    wifi_event_ftm_report_t *r = data;
    ftm_cm = r->status == FTM_STATUS_SUCCESS ? (int)r->dist_est : -1;
    memcpy(ftm_peer, r->peer_mac, 6);
    ESP_LOGD(TAG, "ftm %02x%02x: status %d, rtt %u ns, %u cm", r->peer_mac[4], r->peer_mac[5], r->status,
             (unsigned)r->rtt_est, (unsigned)r->dist_est);
    xEventGroupSetBits(ftm_ev, 1);
}

static int rng_find(const uint8_t *a, const uint8_t *b) // under rng_lock: the entry for a -> b, or -1
{
    for (int i = 0; i < RNG_KEEP; i++)
        if (rng[i].seen && !memcmp(rng[i].a, a, 6) && !memcmp(rng[i].b, b, 6)) return i;
    return -1;
}

static int rng_slot(const uint8_t *a, const uint8_t *b) // under rng_lock: the entry for a -> b, else the oldest, reset
{
    int old = 0;
    for (int i = 0; i < RNG_KEEP; i++) {
        if (!memcmp(rng[i].a, a, 6) && !memcmp(rng[i].b, b, 6)) return i;
        if (rng[i].seen < rng[old].seen) old = i;
    }
    memset(&rng[old], 0, sizeof rng[old]);
    memcpy(rng[old].a, a, 6), memcpy(rng[old].b, b, 6);
    rng[old].cm = rng[old].hist[0] = rng[old].hist[1] = rng[old].hist[2] = rng[old].hist[3] = -1;
    return old;
}

static void rng_result(int i, int cm, int64_t now) // under rng_lock: a measurement on own entry i
{
    memmove(rng[i].hist + 1, rng[i].hist, 3 * sizeof rng[i].hist[0]);
    rng[i].hist[0] = cm;
    for (int k = 1; k < 4; k++)
        if (rng[i].hist[k] >= 0) cm = MIN(cm, rng[i].hist[k]);
    rng[i].cm = cm, rng[i].at = now;
    if (!rng[i].seen) rng[i].seen = now;
}

static void rng_recv(const esp_now_recv_info_t *info, const uint8_t *d, int len)
{
    int64_t now = esp_timer_get_time();
    if (len >= 5 && !memcmp(d, "BOSF", 4)) {
        const rng_fast_t *f = (const void *)d;
        if (f->n > RNG_FAST || len < 5 + f->n * (int)sizeof f->r[0]) return;
        xSemaphoreTake(rng_lock, portMAX_DELAY);
        int x = rng_find(rng_me, info->src_addr); // only badges whose BOSR we already have
        if (x >= 0) {
            rng[x].fast = now;
            for (int k = 0; k < f->n; k++) {
                if (!memcmp(f->r[k].mac, rng_me, 6)) { // it measured me: as good as my own measurement of it
                    rng_result(x, f->r[k].cm, now);
                    continue;
                }
                int j = rng_slot(info->src_addr, f->r[k].mac);
                rng[j].cm = f->r[k].cm, rng[j].seen = rng[j].at = now;
            }
        }
        xSemaphoreGive(rng_lock);
        return;
    }
    const rng_beacon_t *m = (const void *)d;
    int head = offsetof(rng_beacon_t, r);
    if (len < head || memcmp(m->magic, "BOSR", 4) || m->n > RNG_SHARE || len < head + m->n * (int)sizeof m->r[0]) return;
    xSemaphoreTake(rng_lock, portMAX_DELAY);
    int i = rng_slot(rng_me, info->src_addr); // a badge nearby: range it
    if (!rng[i].seen) ESP_LOGI(TAG, "ranging: new badge %02x%02x", info->src_addr[4], info->src_addr[5]);
    memcpy(rng[i].ap, m->ap, 6);
    rng[i].seen = now;
    int tail = head + m->n * (int)sizeof m->r[0]; // the username follows the ranges (badges before v0.7 send none)
    if (len >= tail + 12) memcpy(rng[i].name, d + tail, 12), rng[i].name[12] = 0;
    for (int k = 0; k < m->n; k++) { // what it measured
        int j = rng_slot(info->src_addr, m->r[k].mac);
        rng[j].cm = m->r[k].cm, rng[j].seen = rng[j].at = now;
    }
    xSemaphoreGive(rng_lock);
}

static void rng_task(void *arg)
{
    static const uint8_t all[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int64_t beacon = 0, fast = 0;
    for (int next = 0; rng_run;) {
        int64_t now = esp_timer_get_time();
        if (now >= beacon) {
            static uint8_t buf[sizeof(rng_beacon_t) + 12];
            rng_beacon_t m = {.magic = {'B', 'O', 'S', 'R'}};
            esp_wifi_get_mac(WIFI_IF_AP, m.ap);
            xSemaphoreTake(rng_lock, portMAX_DELAY);
            for (int i = 0; i < RNG_KEEP && m.n < RNG_SHARE; i++)
                if (!memcmp(rng[i].a, rng_me, 6) && rng[i].cm >= 0 && now - rng[i].at < RNG_STALE_US)
                    memcpy(m.r[m.n].mac, rng[i].b, 6), m.r[m.n++].cm = rng[i].cm;
            xSemaphoreGive(rng_lock);
            int len = offsetof(rng_beacon_t, r) + m.n * sizeof m.r[0];
            char name[13] = "";
            cfg_get("name", name, sizeof name);
            memcpy(buf, &m, len);
            memcpy(buf + len, name, 12); // username, zero padded
            esp_now_send(all, buf, len + 12);
            beacon = now + 800000 + esp_random() % 400000; // ~1 s, jittered so two badges do not keep colliding
        }
        if (now >= fast) { // also when empty: it tells the others this badge sends BOSF
            rng_fast_t f = {.magic = {'B', 'O', 'S', 'F'}};
            xSemaphoreTake(rng_lock, portMAX_DELAY);
            memcpy(f.r, rng_pend, rng_npend * sizeof rng_pend[0]);
            f.n = rng_npend, rng_npend = 0;
            xSemaphoreGive(rng_lock);
            esp_now_send(all, (const uint8_t *)&f, 5 + f.n * sizeof f.r[0]);
            fast = now + 200000 + esp_random() % 100000;
        }
        // FTM to the next badge heard from in the last 10 s, round robin. Skip one with a result in the last 400 ms
        // (mine or from its BOSF); a badge that sends no BOSF (v0.8) gets at most one attempt per 5 s, as v0.8 does.
        uint8_t b[6], ap[6];
        int found = -1;
        xSemaphoreTake(rng_lock, portMAX_DELAY);
        for (int k = 0; k < RNG_KEEP && found < 0; k++) {
            int i = (next + k) % RNG_KEEP;
            if (memcmp(rng[i].a, rng_me, 6) || now - rng[i].seen >= 10000000 || now - rng[i].at < 400000) continue;
            bool updated = rng[i].fast && now - rng[i].fast < 5000000;
            if (!updated && now - rng[i].tried < 5000000) continue;
            found = i, rng[i].tried = now, memcpy(b, rng[i].b, 6), memcpy(ap, rng[i].ap, 6);
        }
        xSemaphoreGive(rng_lock);
        if (found < 0) {
            ms(20);
            continue;
        }
        next = found + 1;
        wifi_ftm_initiator_cfg_t cfg = {.channel = RNG_CH, .frm_count = RNG_FRAMES, .burst_period = RNG_PERIOD};
        memcpy(cfg.resp_mac, ap, 6);
        xEventGroupClearBits(ftm_ev, 1);
        esp_err_t e = esp_wifi_ftm_initiate_session(&cfg);
        if (e != ESP_OK) ESP_LOGW(TAG, "ftm %02x%02x: start failed: %s", ap[4], ap[5], esp_err_to_name(e));
        bool ok = false;
        for (int64_t end = esp_timer_get_time() + RNG_WAIT_MS * 1000LL; e == ESP_OK;) { // a late report of an earlier
            int64_t left = end - esp_timer_get_time();                                   // session: keep waiting
            if (left <= 0 || !(xEventGroupWaitBits(ftm_ev, 1, true, true, pdMS_TO_TICKS(left / 1000 + 1)) & 1)) break;
            if (!memcmp(ftm_peer, ap, 6)) {
                ok = ftm_cm >= 0;
                break;
            }
        }
        if (ok) {
            int cm = MAX(0, ftm_cm - RNG_OFFSET_CM);
            xSemaphoreTake(rng_lock, portMAX_DELAY);
            rng_result(rng_slot(rng_me, b), cm, esp_timer_get_time());
            if (rng_npend < RNG_FAST) memcpy(rng_pend[rng_npend].mac, b, 6), rng_pend[rng_npend++].cm = cm;
            xSemaphoreGive(rng_lock);
        } else {
            esp_wifi_ftm_end_session();
        }
        ms(5 + esp_random() % 16); // air for the other badges' bursts
    }
    xSemaphoreGive(rng_done);
    vTaskDelete(NULL);
}

void ranging_start(void)
{
    if (rng_run) return;
    wifi_on();
    if (!rng_lock) { // once, after wifi_on has made the event loop
        rng_lock = xSemaphoreCreateMutex(), rng_done = xSemaphoreCreateBinary(), ftm_ev = xEventGroupCreate();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm, NULL);
    }
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = {.ap = {.ssid = "badge-os", .ssid_len = 8, .channel = RNG_CH, .authmode = WIFI_AUTH_OPEN,
                               .ssid_hidden = 1, .max_connection = 1, .beacon_interval = 1000, .ftm_responder = true}};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); // modem sleep would drop beacons
    esp_wifi_set_channel(RNG_CH, WIFI_SECOND_CHAN_NONE);
    esp_read_mac(rng_me, ESP_MAC_WIFI_STA);
    memset(rng, 0, sizeof rng);
    esp_log_level_set("wifi", ESP_LOG_ERROR); // the driver warns about every FTM session
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_recv_cb(rng_recv);
    esp_now_add_peer(&(esp_now_peer_info_t){.peer_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, .channel = RNG_CH,
                                            .ifidx = WIFI_IF_STA});
    rng_run = true;
    xTaskCreate(rng_task, "ranging", 4096, NULL, 4, NULL);
}

void ranging_stop(void)
{
    if (!rng_run) return;
    rng_run = false;
    xSemaphoreTake(rng_done, pdMS_TO_TICKS(5000));
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA); // as wifi_on() left it
    esp_log_level_set("wifi", ESP_LOG_WARN);
}

int ranging_get(range_t *out, int max)
{
    if (!rng_lock) return 0;
    int n = 0;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(rng_lock, portMAX_DELAY);
    for (int i = 0; i < RNG_KEEP && n < max; i++) {
        if (!rng[i].seen || now - rng[i].seen > RNG_STALE_US) continue;
        memcpy(out[n].a, rng[i].a, 6), memcpy(out[n].b, rng[i].b, 6);
        out[n].cm = rng[i].cm;
        out[n++].age_ms = (now - (rng[i].cm >= 0 ? rng[i].at : rng[i].seen)) / 1000;
    }
    xSemaphoreGive(rng_lock);
    return n;
}

bool ranging_running(void) { return rng_run; }

void ranging_me(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

bool ranging_name(const uint8_t mac[6], char name[13])
{
    bool ok = false;
    *name = 0;
    if (!rng_lock) return false;
    xSemaphoreTake(rng_lock, portMAX_DELAY);
    for (int i = 0; i < RNG_KEEP && !ok; i++)
        if (!memcmp(rng[i].a, rng_me, 6) && !memcmp(rng[i].b, mac, 6) && rng[i].name[0]) strcpy(name, rng[i].name), ok = true;
    xSemaphoreGive(rng_lock);
    return ok;
}

void badge_id(char id[5])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(id, "%02X%02X", mac[4], mac[5]); // as Radar shows it
}
