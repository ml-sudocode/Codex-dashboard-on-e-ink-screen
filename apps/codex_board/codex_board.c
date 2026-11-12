#include "board_alert.h"
#include "board_usb.h"
#include "board_view.h"
#include "badge/badge.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include <string.h>

#define POLL_US 3000000LL
#define STALE_US 30000000LL
#define FLASH_US 5000000LL
#define FLASH_HALF_PERIOD_US 250000LL

// The home task has a 16 KB stack. Keep HTTP buffers and snapshots out of it.
static char body[CB_BODY_MAX + 2], saved[CB_SEEN_TEXT];
static cb_feed_t current, received;
static cb_seen_t seen;
static struct { int64_t started; bool active, lit; } flash;

static void flash_set(bool lit)
{
    static const uint8_t red[LEDS][3] = {{10}, {10}, {10}, {10}, {10}, {10}, {10}, {10}};
    static const uint8_t off[LEDS][3] = {0};
    if (flash.lit == lit) return;
    leds(lit ? red : off);
    flash.lit = lit;
}

static void flash_stop(void)
{
    flash.active = false;
    flash_set(false);
}

static void red_flash(void)
{
    flash.started = esp_timer_get_time();
    flash.active = true;
    flash_set(true);
}

static void flash_tick(void)
{
    if (!flash.active) return;
    int64_t elapsed = esp_timer_get_time() - flash.started;
    if (elapsed >= FLASH_US) flash_stop();
    else flash_set((elapsed / FLASH_HALF_PERIOD_US) % 2 == 0);
}

static bool url_allowed(const char *url)
{
    if (strncmp(url, "http://", 7) || !url[7] || url[7] == '/') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p > 126 || *p == '@' || *p == '#') return false;
    return true;
}

static bool budget(esp_http_client_handle_t client, int64_t deadline)
{
    int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) return false;
    return esp_http_client_set_timeout_ms(client, (int)((left + 999) / 1000)) == ESP_OK;
}

static const char *fetch(const char *url)
{
    esp_http_client_handle_t client = esp_http_client_init(&(esp_http_client_config_t){
        .url = url, .timeout_ms = 5000, .buffer_size = 1024,
        .user_agent = "codex-board", .disable_auto_redirect = true,
    });
    if (!client) return "HTTP initialization failed";
    const char *error = "OFFLINE: retaining last snapshot";
    int64_t deadline = esp_timer_get_time() + 5000000LL;
    size_t count = 0;
    if (esp_http_client_open(client, 0) != ESP_OK || !budget(client, deadline)) goto done;
    int64_t declared = esp_http_client_fetch_headers(client);
    if (declared < 0 || !budget(client, deadline)) goto done;
    if (esp_http_client_get_status_code(client) != 200) {
        error = "HTTP error: retaining last snapshot";
        goto done;
    }
    if (declared > CB_BODY_MAX) {
        error = "Feed exceeds 8192 bytes";
        goto done;
    }
    // Header fetching can already buffer the complete body; always drain read()
    // before checking completion, or a small response would appear empty.
    while (count <= CB_BODY_MAX) {
        if (!budget(client, deadline)) goto done;
        int n = esp_http_client_read(client, body + count, (int)(CB_BODY_MAX + 1 - count));
        if (n < 0) goto done;
        if (!n) break;
        count += (size_t)n;
        if (esp_http_client_is_complete_data_received(client)) break;
    }
    if (!esp_http_client_is_complete_data_received(client) || count > CB_BODY_MAX ||
        esp_timer_get_time() > deadline) {
        error = "Incomplete feed: retaining last snapshot";
        goto done;
    }
    error = cb_feed_parse(body, count, &received) ? "" : "Invalid feed: retaining last snapshot";
done:
    esp_http_client_cleanup(client);
    return error;
}

typedef struct {
    bool have_feed, memory_ok, stale;
    unsigned page;
    char last_sample[41];
    int64_t sample_changed, last_received;
} board_state_t;

static bool accept_received(board_state_t *state, int64_t now)
{
    if (!state->have_feed || strcmp(state->last_sample, received.sampled)) {
        memcpy(state->last_sample, received.sampled, sizeof state->last_sample);
        state->sample_changed = now;
    }
    state->last_received = now;
    state->stale = now - state->sample_changed > STALE_US;
    bool draw = !state->have_feed || strcmp(current.revision, received.revision) ||
                current.source != received.source || current.quota != received.quota;
    if (state->have_feed) cb_feed_retain_terminals(&received, &current);
    current = received;
    state->have_feed = true;
    state->page %= cb_pages(&current);
    if (!state->stale && cb_seen_observe(&seen, &current)) {
        cb_seen_save(&seen, saved);
        // Save before flashing, so leaving/rebooting cannot replay this event.
        state->memory_ok = cfg_set("cb_seen", saved);
        red_flash();
    }
    return draw;
}

static const char *healthy_notice(const board_state_t *state)
{
    return state->stale ? "STALE FEED: collector has stopped updating" :
           state->memory_ok ? "" : "Alert history could not be saved";
}

static void paint(const board_state_t *state, const char *notice, bool usb)
{
    if (state->have_feed) cb_view(&current, state->page, notice, usb);
    else cb_setup_view(notice[0] ? notice : "Connecting to the feed...", usb);
}

static void run(void)
{
    flash_stop();
    char url[241] = {0}, previous_notice[80] = {0};
    board_state_t state = {.memory_ok = true};
    bool joined = false, usb_ready = false;
    int64_t next_poll = 0;
    const char *notice = "Connecting to the feed...";
    memset(&seen, 0, sizeof seen);
    if (cfg_get("cb_seen", saved, sizeof saved) >= 0) cb_seen_load(&seen, saved);
    cfg_get("cb_url", url, sizeof url);
    bool usb = !url[0];
    if (usb) {
        usb_ready = cb_usb_start();
        notice = usb_ready ? "USB waiting: send a Codex Board feed" : "USB receiver could not start; press A";
    } else if (!url_allowed(url)) notice = "Set a valid http:// feed URL in Settings";
    paint(&state, notice, usb);
    for (;;) {
        int key = btn_get(usb ? 20 : 100);
        flash_tick();
        bool draw = false;
        if (key == BTN_B) break;
        if (key == BTN_A) {
            if (state.have_feed && current.message[0]) red_flash();
            else if (usb) {
                if (!state.have_feed) {
                    usb_ready = cb_usb_start();
                    notice = usb_ready ? "USB waiting: send a Codex Board feed" : "USB receiver could not start; press A";
                }
                draw = true;
            } else next_poll = 0;
        }
        if (state.have_feed && !current.message[0] && (key == BTN_PLUS || key == BTN_MINUS)) {
            unsigned total = cb_pages(&current);
            state.page = (state.page + total + (key == BTN_PLUS ? 1 : -1)) % total;
            draw = true;
        }
        int64_t now = esp_timer_get_time();
        if (usb && usb_ready) {
            // A bounded amount of work per button loop, independent of HTTP polling.
            for (unsigned frames = 0; frames < 4; frames++) {
                cb_stream_result_t result = cb_usb_poll(&received);
                if (result == CB_STREAM_NONE) break;
                if (result == CB_STREAM_VALID) {
                    now = esp_timer_get_time();
                    draw |= accept_received(&state, now);
                    cb_usb_ack(current.revision);
                    notice = healthy_notice(&state);
                } else notice = "Invalid USB feed: retaining last snapshot";
            }
            if (state.have_feed && now - state.last_received > STALE_US)
                notice = "USB STALE: no complete feed for 30 seconds";
            else if (state.have_feed && now - state.sample_changed > STALE_US)
                notice = "STALE FEED: collector has stopped updating";
        } else if (!usb && !flash.active && now >= next_poll && url_allowed(url)) {
            // HTTP can block for five seconds. Delay the next poll until the
            // LED sequence finishes; buttons and USB remain responsive.
            if (!joined) joined = wifi_connect(5000);
            const char *error = joined ? fetch(url) : "Wi-Fi unavailable: check Settings";
            now = esp_timer_get_time();
            if (error[0]) {
                notice = error;
                // Wi-Fi also retries by itself; a later join verifies we have an IP.
                joined = false;
            } else {
                draw |= accept_received(&state, now);
                notice = healthy_notice(&state);
            }
            next_poll = now + POLL_US;
        }
        if (strcmp(previous_notice, notice)) {
            // All notices are fixed application strings shorter than this buffer.
            strcpy(previous_notice, notice);
            draw = true;
        }
        if (draw) paint(&state, notice, usb);
        flash_tick();
    }
    flash_stop();
    ui_close();
    if (!usb) wifi_off();
}

static void icon(int x, int y)
{
    icon_base(x, y);
    box(x + 24, y + 35, 128, 106, false);
    box(x + 30, y + 41, 116, 94, true);
    text(x + 41, y + 51, 3, ">_", false);
    for (int i = 0; i < 3; i++) box(x + 41, y + 87 + i * 14, 84 - i * 12, 5, false);
}

const app_t app_codex_board = {"Codex Board", icon, run};
