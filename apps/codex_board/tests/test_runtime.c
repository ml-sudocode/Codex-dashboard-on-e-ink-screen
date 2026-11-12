// Include the app so its transport and loop can run against deterministic doubles.
#include "../codex_board.c"
#include <assert.h>
#include <stdio.h>
#include "driver/usb_serial_jtag.h"

static const char valid[] =
    "CODEXBOARD\t1\nMETA\tabc\tLIVE\tAmerica/Los_Angeles\t2026-11-10 15:00:00 PST\n"
    "SOURCE\tOK\nQUOTA\tUNAVAILABLE\t\n"
    "PANE\t%1\tReview changes\tNEEDS_YOU\t00000000000000000000000000000001\n"
    "FOOTER\tquote\tReady when you are.\nEND\n";
uint8_t fb[PLANE];
static const char *response;
static size_t response_length, offset;
static int chunk, status, read_calls, cleanups, opened, flash_pulses, draws, button_calls, stop_after, press_a;
static int64_t clock_us, read_delay, open_delay, declared_length;
static bool complete, complete_on_headers, init_fail, connect_ok;
static char nvs[CB_SEEN_TEXT], last_notice[80];
static const char *configured_url, *usb_input;
static size_t usb_length, usb_offset;
static bool usb_installed, usb_start_fail, usb_repeat;
static unsigned usb_installs, usb_reads, usb_acks, usb_readies, usb_chunk;
static int64_t usb_sent_at;

static void reset_http(const char *data)
{
    response = data;
    response_length = strlen(data);
    offset = 0; chunk = 1024; status = 200; read_calls = 0; cleanups = 0;
    opened = 0; clock_us = 1; read_delay = 1000; open_delay = 1000;
    declared_length = (int64_t)response_length;
    complete = true; complete_on_headers = false; init_fail = false; connect_ok = true;
    button_calls = 0; stop_after = 2; press_a = 0;
    last_notice[0] = '\0';
    configured_url = "http://192.168.1.2:8765/feed.tsv";
    usb_input = ""; usb_length = usb_offset = 0; usb_chunk = 256;
    usb_start_fail = usb_repeat = false; usb_reads = 0; usb_sent_at = 0;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    assert(c->disable_auto_redirect && c->timeout_ms == 5000);
    offset = 0;
    return init_fail ? NULL : (void *)response;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h, int timeout)
{ (void)h; assert(timeout > 0 && timeout <= 5000); return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int len)
{ (void)h; assert(!len); opened++; clock_us += open_delay; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{ (void)h; return declared_length; }
int esp_http_client_get_status_code(esp_http_client_handle_t h)
{ (void)h; return status; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h)
{ (void)h; return complete && (complete_on_headers || offset == response_length); }
int esp_http_client_read(esp_http_client_handle_t h, char *out, int length)
{
    (void)h;
    read_calls++; clock_us += read_delay;
    size_t n = response_length - offset;
    if (n > (size_t)length) n = (size_t)length;
    if (n > (size_t)chunk) n = (size_t)chunk;
    memcpy(out, response + offset, n);
    offset += n;
    return (int)n;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h)
{ (void)h; cleanups++; return ESP_OK; }
int64_t esp_timer_get_time(void) { return clock_us; }

void box(int x, int y, int w, int h, bool ink)
{ (void)x; (void)y; (void)w; (void)h; (void)ink; }
void text(int x, int y, int s, const char *str, bool ink)
{ (void)x; (void)y; (void)s; (void)str; (void)ink; }
void icon_base(int x, int y) { (void)x; (void)y; }
void ui_keys(const char *p, const char *m, const char *a, const char *b)
{ (void)p; (void)m; (void)a; (void)b; }
void ui_frame(int pulls) { (void)pulls; }
void ui_close(void) {}
void leds(const uint8_t rgb[LEDS][3])
{
    for (unsigned i = 0; i < LEDS; i++) {
        assert(rgb[i][0] <= 10 && !rgb[i][1] && !rgb[i][2]);
        assert(rgb[i][0] == rgb[0][0]);
    }
    if (rgb[0][0]) flash_pulses++;
}
void ms(int t) { clock_us += t * 1000; }
int btn_get(int timeout)
{
    clock_us += timeout * 1000;
    button_calls++;
    if (button_calls >= stop_after) return BTN_B;
    return button_calls == press_a ? BTN_A : -1;
}
int cfg_get(const char *key, char *out, int n)
{
    const char *value = !strcmp(key, "cb_seen") ? nvs : configured_url;
    snprintf(out, (size_t)n, "%s", value);
    return (int)strlen(out);
}
bool cfg_set(const char *key, const char *value)
{ assert(!strcmp(key, "cb_seen")); strcpy(nvs, value); return true; }
bool wifi_connect(int timeout) { assert(timeout == 5000); return connect_ok; }
void wifi_off(void) {}
unsigned cb_pages(const cb_feed_t *f) { (void)f; return 1; }
void cb_view(const cb_feed_t *f, unsigned page, const char *notice, bool usb)
{ (void)f; assert(usb == !configured_url[0]); assert(page == 0); draws++; strcpy(last_notice, notice); }
void cb_setup_view(const char *notice, bool usb)
{ assert(usb == !configured_url[0]); strcpy(last_notice, notice); }

bool usb_serial_jtag_is_driver_installed(void) { return usb_installed; }
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    assert(!usb_installed && config->rx_buffer_size >= CB_BODY_MAX && config->tx_buffer_size >= 64);
    usb_installs++;
    if (usb_start_fail) return -1;
    usb_installed = true;
    return ESP_OK;
}
void usb_serial_jtag_vfs_use_driver(void) { assert(usb_installed); }
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, uint32_t ticks)
{
    assert(usb_installed && !ticks);
    usb_reads++;
    if (usb_repeat && usb_offset == usb_length && clock_us - usb_sent_at >= 3000000)
        usb_offset = 0;
    size_t n = usb_length - usb_offset;
    if (n > length) n = length;
    if (n > usb_chunk) n = usb_chunk;
    memcpy(buf, usb_input + usb_offset, n);
    usb_offset += n;
    if (n && usb_offset == usb_length) usb_sent_at = clock_us;
    return (int)n;
}
int usb_serial_jtag_write_bytes(const void *buf, size_t length, uint32_t ticks)
{
    assert(usb_installed && !ticks);
    char line[80] = {0};
    assert(length < sizeof line);
    memcpy(line, buf, length);
    if (!strcmp(line, "\nCODEXBOARD READY\n")) usb_readies++;
    else {
        assert(!strncmp(line, "CODEXBOARD ACK ", 15) && line[length - 1] == '\n');
        usb_acks++;
    }
    return (int)length;
}

static void http_tests(void)
{
    const char *url = "http://192.168.1.2/feed.tsv";
    assert(url_allowed(url));
    assert(!url_allowed("https://example.com/feed"));
    assert(!url_allowed("http://user:pass@192.168.1.2/feed"));
    assert(!url_allowed("http:///path"));
    assert(!url_allowed("http://host/#fragment"));
    reset_http(valid);
    complete_on_headers = true;
    assert(!fetch(url)[0]); // buffered small body must still be read
    assert(read_calls == 1 && cleanups == 1 && received.pane_count == 1);
    reset_http(valid);
    declared_length = 0; chunk = 7; // chunked response
    assert(!fetch(url)[0] && read_calls > 1 && cleanups == 1);
    cb_feed_t previous = received;
    reset_http(valid); complete = false;
    assert(fetch(url)[0] && cleanups == 1);
    assert(!memcmp(&previous, &received, sizeof previous));
    reset_http(valid); status = 302;
    assert(fetch(url)[0] && !read_calls && cleanups == 1);
    reset_http(valid); declared_length = CB_BODY_MAX + 1;
    assert(fetch(url)[0] && !read_calls && cleanups == 1);
    char giant[CB_BODY_MAX + 3];
    memset(giant, 'x', sizeof giant - 1); giant[sizeof giant - 1] = '\0';
    reset_http(giant); declared_length = 0;
    assert(fetch(url)[0] && offset == CB_BODY_MAX + 1 && cleanups == 1);
    reset_http(valid); open_delay = 5000000;
    assert(fetch(url)[0] && !read_calls && cleanups == 1);
    reset_http(valid); chunk = 1; read_delay = 1000000;
    assert(fetch(url)[0] && read_calls <= 5 && cleanups == 1);
    reset_http("CODEXBOARD\t1\nEND\n");
    assert(fetch(url)[0] && cleanups == 1);
    assert(!memcmp(&previous, &received, sizeof previous));
    reset_http(valid); init_fail = true;
    assert(fetch(url)[0] && !cleanups);
}

static void loop_tests(void)
{
    static const char ready[] =
        "CODEXBOARD\t1\nMETA\tdef\tLIVE\tUTC\t2026-11-10 15:00:00 UTC\n"
        "SOURCE\tOK\nQUOTA\tOK\t2026-11-10 15:00:00 UTC\n"
        "WINDOW\tWeekly\t77\t2026-11-16 18:00:00 PST\n"
        "PANE\t%2\tA finished reply\tREPLY_READY\t\n"
        "FOOTER\tquote\tReady for review\nEND\n";
    reset_http(ready); flash_pulses = 0; draws = 0; nvs[0] = '\0'; stop_after = 60;
    run();
    assert(flash_pulses == 0 && draws == 1 && cb_pending(&current) == 0);
    static const char ready_event[] =
        "CODEXBOARD\t1\nMETA\tdef\tLIVE\tUTC\t2026-11-10 15:00:00 UTC\n"
        "SOURCE\tOK\nQUOTA\tOK\t2026-11-10 15:00:00 UTC\n"
        "PANE\t%2\tA finished reply\tREPLY_READY\t00000000000000000000000000000002\n"
        "FOOTER\tquote\tReady for review\nEND\n";
    reset_http(ready_event); flash_pulses = 0; draws = 0; nvs[0] = '\0'; stop_after = 60;
    run();
    assert(flash_pulses == 10 && nvs[0] && cb_pending(&current) == 0);
    reset_http(ready_event);
    run();
    assert(flash_pulses == 10); // A relaunch must not replay the ready event.
    reset_http(valid); flash_pulses = 0; draws = 0; nvs[0] = '\0';
    stop_after = 60;
    run();
    assert(flash_pulses == 10 && nvs[0] && draws == 1 && !flash.active && !flash.lit);
    reset_http(valid); draws = 0;
    run();
    assert(flash_pulses == 10 && draws == 1); // relaunch must not flash again
    reset_http(valid); stop_after = 400; draws = 0;
    run();
    assert(strstr(last_notice, "STALE FEED"));
    assert(flash_pulses == 10 && draws == 2); // no redraw on every poll
    static const char smoke[] =
        "CODEXBOARD\t1\nMETA\tdef\tDEMO\tUTC\t2026-11-10 15:00:00 UTC\n"
        "SOURCE\tOK\nQUOTA\tUNAVAILABLE\t\nMESSAGE\tHello from the Linux VM\n"
        "FOOTER\tquote\tTesting\nEND\n";
    reset_http(smoke); stop_after = 70; press_a = 2;
    run();
    assert(flash_pulses == 20); // explicit LED test only, no pending event needed
}

static void usb_tests(void)
{
    reset_http(valid); configured_url = ""; usb_start_fail = true;
    run();
    assert(strstr(last_notice, "could not start") && !opened && !usb_reads);
    reset_http(valid); configured_url = ""; usb_input = valid; usb_length = strlen(valid);
    usb_installs = usb_acks = usb_readies = 0; nvs[0] = '\0'; flash_pulses = draws = 0;
    usb_chunk = 1; // driver may split each byte, including header and END
    usb_repeat = true; // Same request arrives again during its LED sequence.
    stop_after = 300; // Six seconds of responsive button/USB polling.
    run();
    assert(usb_installs == 1 && usb_readies == 1 && usb_acks == 2);
    assert(flash_pulses == 10 && draws == 1 && !opened && button_calls == 300);
    reset_http(valid); configured_url = ""; usb_input = valid; usb_length = strlen(valid);
    stop_after = 2000; draws = 0;
    run();
    assert(usb_installs == 1 && usb_acks == 3 && usb_readies == 2); // installed once, even on relaunch
    assert(strstr(last_notice, "USB STALE") && cb_pending(&current) == 1);
    assert(flash_pulses == 10 && draws == 2 && !opened);
    reset_http(valid); configured_url = ""; usb_input = valid; usb_length = strlen(valid);
    stop_after = 2000; usb_repeat = true;
    run();
    assert(strstr(last_notice, "STALE FEED") && flash_pulses == 10 && usb_acks > 10);
    // Invalid frames retain the active attention banner and are not acknowledged.
    static const char bad[] = "CODEXBOARD\t1\nSOURCE\tBAD\nEND\n";
    reset_http(valid); configured_url = ""; usb_input = bad; usb_length = strlen(bad);
    unsigned old_acks = usb_acks;
    assert(cb_usb_start());
    cb_feed_t before = current;
    assert(cb_usb_poll(&current) == CB_STREAM_INVALID);
    assert(!memcmp(&before, &current, sizeof current) && usb_acks == old_acks);
    // A single poll consumes at most 1024 bytes even for a huge uninterrupted stream.
    char huge[4097]; memset(huge, 'x', sizeof huge - 1); huge[sizeof huge - 1] = '\0';
    reset_http(valid); usb_input = huge; usb_length = strlen(huge);
    assert(cb_usb_start());
    assert(cb_usb_poll(&current) == CB_STREAM_NONE && usb_offset == 1024);
}

static void flash_timing_tests(void)
{
    flash_stop(); clock_us = 1000000; flash_pulses = 0;
    red_flash();
    assert(clock_us == 1000000 && flash.active && flash.lit); // No blocking sleep.
    for (int64_t elapsed = 0; elapsed < 5000000; elapsed += 50000) {
        clock_us = 1000000 + elapsed;
        flash_tick();
        assert(flash.active);
        assert(flash.lit == ((elapsed / 250000) % 2 == 0));
    }
    clock_us = 6000000;
    flash_tick();
    assert(!flash.active && !flash.lit && flash_pulses == 10);
    clock_us += 5000000;
    flash_tick();
    assert(flash_pulses == 10); // No automatic replay after completion.
    red_flash();
    assert(flash.lit);
    flash_stop();
    assert(!flash.lit && !flash.active);
}

int main(void)
{
    http_tests();
    flash_timing_tests();
    loop_tests();
    usb_tests();
    puts("PASS: HTTP/USB framing, limits, deadlines, stale watchdogs, persistent alerts, LED button, bounded serial reads");
    return 0;
}
