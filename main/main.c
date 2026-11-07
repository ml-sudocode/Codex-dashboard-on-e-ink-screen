// badge os: boots to the home screen, the only way back to it is a reboot (or an app that returns).
// Panel driver, drawing, buttons and the home screen live here; the apps are app_*.c behind badge.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "usb/usb_host.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "badge.h"

static const char *TAG = "os";

// ---- Panel: Pervasive Displays 9.7" black/white/red, 960x672, two CoGs.
// The left CoG (master) drives columns 0-479, the right CoG (slave) columns 480-959.
#define IMG (2 * PLANE)   // an image is the black plane, then the red plane
#define HALF (PLANE / 2)  // what one CoG gets of one plane
#define REVERSE_BITS 1    // stock driver: reverse unless /data/hwconf display_pn == "84"

enum { CS_L = 13, CS_R = 3, BUSY_L = 12, BUSY_R = 8, DC = 10, RST = 11, SCK = 21, MOSI = 47 };
enum { LEFT = 1, RIGHT = 2, BOTH = 3 };

static spi_device_handle_t spi;

void ms(int t) { vTaskDelay(pdMS_TO_TICKS(t) + 1); } // at least t ms (1 kHz tick)

// One S3 SPI transaction carries at most 32 KB, so long data goes out in 4 KB pieces (CS stays low).
static void tx(const uint8_t *d, size_t n)
{
    for (size_t k; n; d += k, n -= k) {
        k = MIN(n, 4096);
        spi_transaction_t t = {.length = 8 * k, .tx_buffer = d};
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
    }
}

// Index byte with DC low, then data with DC high, to the CoGs in `cog`.
static void cmd(int cog, uint8_t index, const uint8_t *data, size_t n)
{
    gpio_set_level(DC, 0);
    if (cog & LEFT) gpio_set_level(CS_L, 0);
    if (cog & RIGHT) gpio_set_level(CS_R, 0);
    tx(&index, 1);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    gpio_set_level(DC, 1);
    if (cog & LEFT) gpio_set_level(CS_L, 0);
    if (cog & RIGHT) gpio_set_level(CS_R, 0);
    tx(data, n);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
}
#define CMD(cog, index, ...) \
    cmd(cog, index, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

// Each CoG pulls its BUSY low while it works. Logs how long each side stayed busy (-1: timeout).
// Returns false on timeout.
static bool wait_ready(const char *what, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time(), t = 0;
    int l = -1, r = -1;
    while ((l < 0 || r < 0) && t < timeout_ms * 1000LL) {
        t = esp_timer_get_time() - t0;
        if (l < 0 && gpio_get_level(BUSY_L)) l = t / 1000;
        if (r < 0 && gpio_get_level(BUSY_R)) r = t / 1000;
        if (l < 0 || r < 0) ms(10);
    }
    ESP_LOGI(TAG, "%s: BUSY left %d ms, right %d ms", what, l, r);
    return l >= 0 && r >= 0;
}

static uint8_t rev(uint8_t b)
{
    b = b >> 4 | b << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

// Rows y0..y1 of one plane into one CoG's RAM: its 60 bytes of every row, bit-reversed, the RAM pointer counting
// down from x 0x3B (RAM_RW {x1, y0 lo, 0x14 | y0 >> 8}: panel_lab experiment 02). plane NULL = all white.
static void send_rows(int cog, uint8_t index, const uint8_t *plane, int y0, int y1)
{
    static DMA_ATTR uint8_t buf[HALF];
    const uint8_t *src = plane ? plane + (cog == RIGHT ? W / 16 : 0) : NULL;
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < W / 16; x++) {
            uint8_t b = src ? src[y * (W / 8) + x] : 0;
            buf[n++] = REVERSE_BITS ? rev(b) : b;
        }
    CMD(cog, 0x12, 0x3B, y0 & 0xFF, 0x14 | y0 >> 8); // RAM_RW
    cmd(cog, index, buf, n);
}

// A panel session: epd_open (reset, frame data, register sequence, DC-DC up), any number of
// epd_pull, epd_close. Same sequence as PDi EPD_Driver_GU_large and the stock epd.py, plus what the
// badge team documented for this B1 CoG (2026-Billboard_Badge/documentation/itc97-driver-reference.pdf):
// the 0x70 booster setting and the 0xB0 short waveform.

// Short waveform for the B1 CoG: data/rendered_luts.py "lutb_base" from the badge team.
// 5 tables (paper, ink, red, unused, VCOM) x 10 groups of {level, on, off, ph2, on, off, ph2, repeat};
// a level byte is 2 bits per phase: 0 GND, 1 VDH, 2 VDL, 3 VDHR. A pull runs a fixed ~1070 ms
// whatever the table asks for, so one pull is a light dose and three make a clean image.
static const uint8_t lutb[400] = {
    // table 0: paper
    0x99, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0xAA, 0x2D, 0x03, 0x01, 0x2D, 0x03, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 1: ink
    0x66, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x44, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 2: red
    0xEE, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0xAA, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0xFF, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    // table 3: unused
    0x00, 0x0A, 0x0A, 0x01, 0x0A, 0x0A, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x2D, 0x01, 0x01, 0x2D, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // table 4: VCOM
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

// PDi Wide_Large's waits for this CoG family (panel_lab experiment 02: session open 348 ms instead of 2443 ms,
// 0xB0 still latches). badge os never runs the slow OTP refresh (~35 s), so only these waits are here.
static void epd_reset(void)
{
    gpio_set_level(RST, 1);
    ms(2);
    gpio_set_level(RST, 0);
    ms(4);
    gpio_set_level(RST, 1);
    ms(20);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    ms(5);
    CMD(BOTH, 0x70, 0x71, 0x36); // B1 booster setting. Without it a refresh draws ~2 A (badge team errata 9)
}

// Frame into CoG RAM. red NULL = no red; with_red false leaves the red RAM as it is (saves 80 ms).
static void epd_load(const uint8_t *black, const uint8_t *red, bool with_red)
{
    CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); // DUW: 60 bytes x 672 lines
    CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9);             // DRFW: whole panel
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        send_rows(cog, 0x10, black, 0, H - 1);
        if (with_red) send_rows(cog, 0x11, red, 0, H - 1);
    }
}

static void dcdc_on(void)
{
    CMD(BOTH, 0x51, 0x50, 0x01); // DC-DC soft start
    for (int v = 1; v <= 4; v++) {
        CMD(BOTH, 0x09, 0x1F);
        CMD(BOTH, 0x51, 0x50, v);
        CMD(BOTH, 0x09, 0x9F);
        ms(2);
    }
    for (int v = 1; v <= 10; v++) {
        CMD(BOTH, 0x09, 0x1F);
        CMD(BOTH, 0x51, 0x0A, v);
        CMD(BOTH, 0x09, 0x9F);
        ms(2);
    }
    for (int v = 3; v <= 10; v++) {
        CMD(BOTH, 0x09, 0x7F);
        CMD(BOTH, 0x51, 0x0A, v);
        CMD(BOTH, 0x09, 0xFF);
        ms(2);
    }
    for (int v = 9; v >= 2; v--) {
        CMD(BOTH, 0x09, 0x7F);
        CMD(BOTH, 0x51, v, 0x0A);
        CMD(BOTH, 0x09, 0xFF);
        ms(2);
    }
    CMD(BOTH, 0x09, 0xFF);
    ms(100);
    wait_ready("dc-dc on", 10000); // never trigger into a booster that is still ramping
}

// Loads the short waveform instead of the panel's own 33 s one (black/white only, red goes pale).
static void epd_open(const uint8_t *black, const uint8_t *red)
{
    int hi = 2, lo = 10; // load strobe waits
    epd_reset();
    epd_load(black, red, true);
    CMD(BOTH, 0x05, 0x7D);
    ms(50);
    CMD(BOTH, 0x05, 0x00);
    ms(1);
    CMD(BOTH, 0xD8, 0x80); // MS_SYNC
    CMD(BOTH, 0xD6, 0x00); // BVSS
    CMD(BOTH, 0xA7, 0x10);
    ms(hi);
    CMD(BOTH, 0xA7, 0x00);
    ms(lo);
    CMD(BOTH, 0x03, 0x00, 0x11); // OSC
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        CMD(cog, 0x44, 0x00);
        CMD(cog, 0x45, 0x80);
        CMD(cog, 0xA7, 0x10);
        ms(hi);
        CMD(cog, 0xA7, 0x00);
        ms(lo);
        CMD(cog, 0x44, 0x06);
        CMD(cog, 0x45, 0x84); // temperature 0x50 + 2 * C: 26 C as the badge team uses
        CMD(cog, 0xA7, 0x10);
        ms(hi);
        CMD(cog, 0xA7, 0x00);
        ms(lo); // this strobe commits the OTP waveform; 0xB0 latches 10 ms after it (experiment 02)
    }
    CMD(BOTH, 0x60, 0x25); // TCON
    CMD(LEFT, 0x61, 0x01); // STV_DIR, master only
    CMD(BOTH, 0x02, 0x00); // VCOM
    cmd(BOTH, 0xB0, lutb, sizeof lutb); // one chance per session: rewriting it re-latches the slow one
    dcdc_on();
}

// Run the latched waveform n times on what is in RAM. Returns ms spent.
static int epd_pull(int n, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (n--) {
        CMD(BOTH, 0x15, 0x3C);
        ms(5);
        wait_ready("pull", timeout_ms);
    }
    return (esp_timer_get_time() - t0) / 1000;
}

static void epd_close(void)
{
    CMD(BOTH, 0x09, 0x7F); // DC-DC soft shutdown
    CMD(BOTH, 0x05, 0x7D);
    CMD(BOTH, 0x09, 0x00);
    ms(200);
    wait_ready("dc-dc off", 2000);
    gpio_set_level(DC, 0);
    gpio_set_level(CS_L, 0);
    gpio_set_level(RST, 0);
    gpio_set_level(CS_L, 1);
}

static void epd_init(void)
{
    gpio_set_level(DC, 1);
    gpio_set_level(RST, 1);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(DC) | BIT64(RST) | BIT64(CS_L) | BIT64(CS_R),
                                 .mode = GPIO_MODE_OUTPUT});
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(BUSY_L) | BIT64(BUSY_R), .mode = GPIO_MODE_INPUT});
    spi_bus_config_t bus = {.mosi_io_num = MOSI, .sclk_io_num = SCK, .miso_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {.clock_speed_hz = 20000000, .mode = 0, .spics_io_num = -1, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));
}


// ==== panel_lab: calm partial updates for the 9.7" B1 panel (mechanism proven in experiment 05) ====
// From panel_lab/driver/epd_partial.c (INTEGRATION.md, FINDINGS.md experiments 05-10); 1185 calls in 10 min on the badge.
//
// part_shown must match the film before the first epd_partial: badge os gets there with ui_wipe (all white).
// epd_partial(black)    black/white frame, no flash: only pixels that differ from what the panel shows are driven.
//                       One pull, ~0.48 s, however much changed. Ink comes out dark red; erased pixels leave a light
//                       ghost. Red pixels count as white. Keeps the panel session (DC-DC) open for the next call.
// epd_sleep()           ends the partial session with the normal DC-DC soft shutdown. Call it when updates stop.
// Planes: 960x672 MONO_HLSB, 1 = ink, as everywhere in main.c.

#define PART_DRIVE 10 // frames of VDH/VDL per change (of 20 per pull): 10 for the film's health (owner); 6 is the least that reads clearly
_Static_assert(PART_DRIVE >= 1 && PART_DRIVE <= 19, "the pull has 20 frames and the last one must be GND");

static EXT_RAM_BSS_ATTR uint8_t part_shown[PLANE]; // what the film shows; after every pull the CoG's 0x11 holds the same
static int part_prev[2][4];       // per CoG, the byte box {x0, x1, y0, y1} the last pull changed; x1 < x0: none
static int part_n = -1;           // pulls in the open partial session, -1 = no session

// Transition table: old frame in 0x11, new frame in 0x10, table = (old << 1) | new. Table 1 (white->black) VDH and
// table 2 (black->white) VDL for the first PART_DRIVE frames, then GND; tables 0, 3 and VCOM at GND. Every group runs
// only its phases c and d (SRab 0): 10 groups x 2 frames = 20 frames = 475 ms, the shortest pull this CoG runs
// (a zero TP, RP 0 and SRab = SRcd = 0 all hang it). The last frame is GND, so no pixel keeps a charge.
static void part_table(uint8_t *t)
{
    for (int k = 0; k < 50; k++) memcpy(t + 8 * k, (const uint8_t[]){0, 1, 1, 0, 1, 1, 1, 1}, 8);
    for (int f = 0; f < PART_DRIVE; f++) {
        int g = f / 2, shift = f % 2 ? 0 : 2; // phase c: bits 3-2, phase d: bits 1-0
        t[(10 + g) * 8] |= 1 << shift;        // VDH
        t[(20 + g) * 8] |= 2 << shift;        // VDL
    }
}

// Session open with PDi Wide_Large's waits (experiment 02: 348 ms, the table latches 10 ms after the last strobe),
// then both RAMs = what the film shows.
static void part_open(void)
{
    static uint8_t lut[400];
    part_table(lut);
    gpio_set_level(RST, 1);
    ms(2);
    gpio_set_level(RST, 0);
    ms(4);
    gpio_set_level(RST, 1);
    ms(20);
    gpio_set_level(CS_L, 1);
    gpio_set_level(CS_R, 1);
    ms(5);
    CMD(BOTH, 0x70, 0x71, 0x36);
    CMD(BOTH, 0x05, 0x7D);
    ms(50);
    CMD(BOTH, 0x05, 0x00);
    ms(1);
    CMD(BOTH, 0xD8, 0x80);
    CMD(BOTH, 0xD6, 0x00);
    CMD(BOTH, 0xA7, 0x10);
    ms(2);
    CMD(BOTH, 0xA7, 0x00);
    ms(10);
    CMD(BOTH, 0x03, 0x00, 0x11);
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        CMD(cog, 0x44, 0x00);
        CMD(cog, 0x45, 0x80);
        CMD(cog, 0xA7, 0x10);
        ms(2);
        CMD(cog, 0xA7, 0x00);
        ms(10);
        CMD(cog, 0x44, 0x06);
        CMD(cog, 0x45, 0x84);
        CMD(cog, 0xA7, 0x10);
        ms(2);
        CMD(cog, 0xA7, 0x00);
        ms(10);
    }
    CMD(BOTH, 0x60, 0x25);
    CMD(LEFT, 0x61, 0x01);
    CMD(BOTH, 0x02, 0x00);
    cmd(BOTH, 0xB0, lut, sizeof lut);
    dcdc_on();
    CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); // DUW whole panel
    CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9);             // DRFW whole panel; a shorter y1 copies the top rows down
    for (int cog = LEFT; cog <= RIGHT; cog++) send_rows(cog, 0x10, part_shown, 0, H - 1), send_rows(cog, 0x11, part_shown, 0, H - 1);
    for (int c = 0; c < 2; c++) part_prev[c][0] = 1, part_prev[c][1] = 0;
    part_n = 0;
}

// Byte box {x0, x1, y0, y1} of one CoG's half where frames a and b differ; x1 < x0 if they do not.
static void part_box(const uint8_t *a, const uint8_t *b, int cog, int *box)
{
    int off = cog == RIGHT ? W / 16 : 0;
    box[0] = W / 16, box[1] = -1, box[2] = H, box[3] = -1;
    for (int y = 0; y < H; y++) {
        const uint8_t *p = a + y * (W / 8) + off, *q = b + y * (W / 8) + off;
        if (!memcmp(p, q, W / 16)) continue;
        for (int x = 0; x < W / 16; x++)
            if (p[x] != q[x]) box[0] = MIN(box[0], x), box[1] = MAX(box[1], x);
        box[2] = MIN(box[2], y), box[3] = y;
    }
}

// Box b of one CoG's half of src into its 0x10 RAM: DUW + RAM_RW window (RAM x = 59 - screen byte, experiment 02),
// the rows in screen order, bit-reversed.
static void part_window(int cog, const uint8_t *src, const int *b)
{
    static DMA_ATTR uint8_t row[W / 16];
    int x0 = b[0], x1 = b[1], y0 = b[2], y1 = b[3], cs = cog == LEFT ? CS_L : CS_R;
    uint8_t index = 0x10;
    CMD(cog, 0x13, 59 - x1, 59 - x0, y0 & 0xFF, y0 >> 8, y1 & 0xFF, y1 >> 8);
    CMD(cog, 0x12, 59 - x0, y0 & 0xFF, 0x14 | y0 >> 8);
    gpio_set_level(DC, 0);
    gpio_set_level(cs, 0);
    tx(&index, 1);
    gpio_set_level(cs, 1);
    gpio_set_level(DC, 1);
    gpio_set_level(cs, 0);
    for (int y = y0; y <= y1; y++) {
        const uint8_t *s = src + y * (W / 8) + (cog == RIGHT ? W / 16 : 0);
        for (int x = x0; x <= x1; x++) row[x - x0] = REVERSE_BITS ? rev(s[x]) : s[x];
        tx(row, x1 - x0 + 1);
    }
    gpio_set_level(cs, 1);
}

static void epd_sleep(void)
{
    if (part_n >= 0) epd_close();
    part_n = -1;
}

static void epd_partial(const uint8_t *black)
{
    int64_t t0 = esp_timer_get_time();
    int now[2][4];
    bool any = false;
    for (int c = 0; c < 2; c++) part_box(black, part_shown, c + 1, now[c]), any |= now[c][1] >= now[c][0];
    if (!any) return ms(1); // nothing changed; still yield, apps call this in tight loops
    if (part_n == 60) epd_sleep(); // a fresh session every 60 pulls: sessions of 60 are proven, longer ones are not
    if (part_n < 0) part_open();
    // The CoG swaps 0x10 and 0x11 after every pull, so 0x11 already holds what the film shows and 0x10 holds the frame
    // before it. Rewrite 0x10 where that frame or the new one differs from the film: this pull's box and the last one.
    for (int c = 0; c < 2; c++) {
        int w[4], *p = part_prev[c];
        memcpy(w, now[c], sizeof w);
        if (p[1] >= p[0]) {
            if (w[1] < w[0]) memcpy(w, p, sizeof w);
            else w[0] = MIN(w[0], p[0]), w[1] = MAX(w[1], p[1]), w[2] = MIN(w[2], p[2]), w[3] = MAX(w[3], p[3]);
        }
        if (w[1] >= w[0]) part_window(c + 1, black, w);
        memcpy(p, now[c], sizeof now[c]);
    }
    CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); // DUW whole panel again, as in experiment 05
    int pull = epd_pull(1, 3000);
    memcpy(part_shown, black, PLANE);
    part_n++;
    ESP_LOGI(TAG, "partial: pull %d ms, total %d ms", pull, (int)((esp_timer_get_time() - t0) / 1000));
}

// ---- Drawing into fb
EXT_RAM_BSS_ATTR uint8_t fb[PLANE]; // in PSRAM: internal RAM is needed for Wi-Fi and TLS (Online update)

void box(int x, int y, int w, int h, bool ink)
{
    for (int r = MAX(y, 0); r < MIN(y + h, H); r++)
        for (int c = MAX(x, 0); c < MIN(x + w, W); c++)
            if (ink) fb[r * (W / 8) + c / 8] |= 0x80 >> (c & 7);
            else fb[r * (W / 8) + c / 8] &= ~(0x80 >> (c & 7));
}

void outline(int x, int y, int w, int h, int t)
{
    box(x, y, w, t, true), box(x, y + h - t, w, t, true), box(x, y, t, h, true), box(x + w - t, y, t, h, true);
}

#include "font8x8_basic.h" // public domain IBM VGA 8x8 font: one byte per row, bit 0 = leftmost pixel

void text(int x, int y, int scale, const char *s, bool ink)
{
    for (; *s; s++, x += 8 * scale)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (font8x8_basic[*s & 0x7F][r] >> c & 1) box(x + c * scale, y + r * scale, scale, scale, ink);
}

// ---- Panel frames: apps draw into fb, ui_frame shows it with one calm partial update (only changed pixels move).
static const char *bar_app; // the running app's name for the top bar, NULL on the home screen
static void draw_bar(void);

void ui_frame(int pulls)
{
    (void)pulls; // ponytail: kept so apps build; a partial update is one pull whatever was asked
    if (bar_app) draw_bar();
    epd_partial(fb);
}

// Partial updates leave light ghosts of erased ink; a scene change shakes the film with the short black/white
// waveform (all black, then all white, ~2.8 s) and leaves the panel and the driver agreeing on white.
void ui_wipe(void)
{
    epd_sleep();
    memset(fb, 0xFF, sizeof fb);
    epd_open(fb, NULL);
    epd_pull(1, 5000);
    epd_load(NULL, NULL, true); // both planes: after the pull 0x11 holds the black frame, which would select red
    epd_pull(1, 5000);
    epd_close();
    memset(fb, 0, sizeof fb);
    memset(part_shown, 0, sizeof part_shown);
}

void ui_close(void) { epd_sleep(); }

// ---- Buttons, active low with pull-ups, polled every 20 ms; each new press goes into a queue.
static QueueHandle_t btnq;
static volatile int held;

static void buttons(void *arg)
{
    static const int pin[4] = {40, 15, 39, 16}; // BTN_PLUS, BTN_MINUS, BTN_A, BTN_B (GPIOs of the printed names)
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(40) | BIT64(15) | BIT64(39) | BIT64(16), .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE});
    for (int was = 0;; ms(20)) {
        int now = 0;
        for (int k = 0; k < 4; k++) now |= (!gpio_get_level(pin[k])) << k;
        for (int k = 0; k < 4; k++)
            if (now & ~was & 1 << k) xQueueSend(btnq, &k, 0);
        held = was = now;
    }
}

int btn_get(int timeout_ms)
{
    int k;
    return xQueueReceive(btnq, &k, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) ? k : -1;
}

void btn_flush(void) { xQueueReset(btnq); }

int btn_held(void) { return held; }

// ---- Wi-Fi: off unless an app turns it on (no radio overhead at home); off again when the app returns.
#if __has_include("wifi_secret.h")
#include "wifi_secret.h" // WIFI_SSID / WIFI_PASS, gitignored; the release workflow writes it from repo secrets
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif
static EventGroupHandle_t wifi_ev; // bit 0: got an IP
static volatile bool wifi_join;    // keep (re)joining the badge Wi-Fi

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT) xEventGroupSetBits(wifi_ev, 1);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(wifi_ev, 1);
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) && wifi_join)
        esp_wifi_connect();
}

void wifi_on(void)
{
    if (!wifi_ev) { // the driver is set up once and kept; on/off only starts and stops the radio
        wifi_ev = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    }
    esp_wifi_start(); // already running: harmless error
}

bool wifi_connect(int timeout_ms)
{
    if (!*WIFI_SSID) return false;
    wifi_on();
    wifi_config_t c = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS}};
    esp_wifi_set_config(WIFI_IF_STA, &c);
    wifi_join = true;
    esp_wifi_connect(); // not started yet: harmless error, the STA_START event connects
    return xEventGroupWaitBits(wifi_ev, 1, false, true, pdMS_TO_TICKS(timeout_ms)) & 1;
}

void wifi_off(void)
{
    if (!wifi_ev) return;
    wifi_join = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    xEventGroupClearBits(wifi_ev, 1);
}

// ---- SAO add-ons share one I2C bus: GPIO17 SDA / GPIO18 SCL, 5.1k pull-ups on the badge (DEVICE_INFO.md).
// The knob add-on is an AS5600 magnetic encoder at 0x36 (its analog out is on the port's GPIO1, a NeoPixel on GPIO2).
static i2c_master_bus_handle_t sao_bus;
static i2c_master_dev_handle_t knob;

static void sao_init(void) // boot: open the bus and log what answers on it
{
    if (i2c_new_master_bus(&(i2c_master_bus_config_t){.i2c_port = -1, .sda_io_num = 17, .scl_io_num = 18,
                                                      .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7},
                           &sao_bus) != ESP_OK) {
        ESP_LOGE(TAG, "SAO I2C bus failed");
        return;
    }
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(sao_bus, a, 20) == ESP_OK) ESP_LOGI(TAG, "SAO I2C device at 0x%02x", a);
}

// The AS5600 also drives its angle as a voltage (0-3.3 V over a turn) on its port's GPIO1. The knob in SAO1 does not
// answer on I2C, so read that: SAO1's GPIO1 is GPIO2 (ADC1). A floating pin follows the pull resistors, a driven one
// does not: that tells a plugged knob from an empty slot.
static int knob_analog(int *up_mv, int *down_mv)
{
    enum { PIN = 2 };
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static adc_channel_t ch;
    if (!adc) {
        adc_unit_t unit;
        adc_oneshot_io_to_channel(PIN, &unit, &ch);
        adc_oneshot_new_unit(&(adc_oneshot_unit_init_cfg_t){.unit_id = unit}, &adc);
        adc_oneshot_config_channel(adc, ch, &(adc_oneshot_chan_cfg_t){.atten = ADC_ATTEN_DB_12});
        adc_cali_create_scheme_curve_fitting(&(adc_cali_curve_fitting_config_t){.unit_id = unit, .chan = ch,
                                                                                .atten = ADC_ATTEN_DB_12}, &cali);
    }
    if (!cali) return -1;
    int mv[2];
    for (int k = 0; k < 2; k++) { // with the pull-up, then with the pull-down
        gpio_set_pull_mode(PIN, k ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
        int sum = 0, v;
        for (int i = 0; i < 4; i++) sum += adc_oneshot_get_calibrated_result(adc, cali, ch, &v) == ESP_OK ? v : 0;
        mv[k] = sum / 4;
    }
    gpio_set_pull_mode(PIN, GPIO_FLOATING);
    if (up_mv) *up_mv = mv[0], *down_mv = mv[1];
    if (abs(mv[0] - mv[1]) > 300) return -1; // floating: no knob
    return MIN((mv[0] + mv[1]) / 2 * 4096 / 3300, 4095);
}

int knob_angle(void)
{
    static int64_t next_probe;
    if (!sao_bus) return knob_analog(NULL, NULL);
    if (!knob) { // not seen on I2C yet: look again at most every 2 s, use the analog out meanwhile
        if (esp_timer_get_time() < next_probe) return knob_analog(NULL, NULL);
        next_probe = esp_timer_get_time() + 2000000;
        if (i2c_master_probe(sao_bus, 0x36, 20) != ESP_OK) return knob_analog(NULL, NULL);
        i2c_master_bus_add_device(sao_bus, &(i2c_device_config_t){.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                                                   .device_address = 0x36, .scl_speed_hz = 100000},
                                  &knob);
        ESP_LOGI(TAG, "AS5600 knob found");
    }
    uint8_t reg = 0x0B, d[2]; // STATUS: without a magnet (MD, bit 5) RAW ANGLE is noise, so there is no knob
    if (i2c_master_transmit_receive(knob, &reg, 1, d, 1, 20) != ESP_OK || !(d[0] & 0x20)) return -1;
    reg = 0x0C; // RAW ANGLE, 12 bits, high byte first
    if (i2c_master_transmit_receive(knob, &reg, 1, d, 2, 20) != ESP_OK) return -1;
    return (d[0] & 0x0F) << 8 | d[1];
}

// ---- Battery: GPIO14 (BAT_SENS) reads half of the LDO input through a 10k/10k divider. On battery that is the
// LiPo voltage; on USB the battery is cut off from it (power path) and it reads about VBUS - 0.3 V.
int battery_pct(void)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static adc_channel_t ch;
    if (!adc) {
        adc_unit_t unit;
        adc_oneshot_io_to_channel(14, &unit, &ch);
        adc_oneshot_new_unit(&(adc_oneshot_unit_init_cfg_t){.unit_id = unit}, &adc);
        adc_oneshot_config_channel(adc, ch, &(adc_oneshot_chan_cfg_t){.atten = ADC_ATTEN_DB_12});
        adc_cali_create_scheme_curve_fitting(&(adc_cali_curve_fitting_config_t){.unit_id = unit, .chan = ch,
                                                                                .atten = ADC_ATTEN_DB_12}, &cali);
    }
    int mv, sum = 0;
    for (int k = 0; k < 8; k++) {
        if (!cali || adc_oneshot_get_calibrated_result(adc, cali, ch, &mv) != ESP_OK) return -2;
        sum += mv;
    }
    int vin = sum / 8 * 2;
    if (vin > 4400) return -1;
    // ponytail: a typical LiPo rest-voltage curve, not this badge's cell; good to ~10 %
    static const int v[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const int p[] = {0, 3, 10, 25, 45, 62, 77, 90, 100};
    if (vin <= v[0]) return 0;
    for (int i = 1; i < 9; i++)
        if (vin < v[i]) return p[i - 1] + (p[i] - p[i - 1]) * (vin - v[i - 1]) / (v[i] - v[i - 1]);
    return 100;
}

static void draw_battery(int x, int y, int pct) // a battery outline, filled to the charge, then "87%" or "USB"
{
    if (pct == -2) return;
    char s[16] = "USB";
    if (pct >= 0) snprintf(s, sizeof s, "%d%%", pct);
    outline(x, y, 48, 24, 3);
    box(x + 48, y + 7, 4, 10, true);
    box(x + 5, y + 5, pct < 0 ? 38 : 38 * pct / 100, 14, true);
    text(x + 64, y, 3, s, true);
}

// ---- Top bar in apps (rows 0..TOP-1): app name left, "badge os vX.X" and battery right, a rule under it.
// ui_frame draws it over fb, so whatever an app puts in those rows is covered.
static void draw_bar(void)
{
    static int pct = -2;
    static int64_t next;
    if (esp_timer_get_time() >= next) { // every 30 s, so the bar sits still; a failed read (Wi-Fi busy) keeps the last
        int p = battery_pct();
        if (p != -2) pct = p;
        next = esp_timer_get_time() + 30000000;
    }
    char ver[48];
    snprintf(ver, sizeof ver, "badge os v%s", esp_app_get_description()->version);
    int bx = W - 24 - 64 - 4 * 24; // the battery, as on the home screen
    box(0, 0, W, TOP, false);
    text(24, 12, 3, bar_app, true);
    text(bx - 32 - (int)strlen(ver) * 16, 16, 2, ver, true);
    draw_battery(bx, 12, pct);
    box(0, TOP - 4, W, 2, true);
}

// ---- Home screen: iOS-style grid of icons. MENU + / MENU - move left / right, A or B opens the app.
#define ICON 176 // icon square, px
#define PITCH 224 // icon to icon
#define X0 56     // (960 - 4 * 176 - 3 * 48) / 2: four icons per row
#define Y0 72
#define ROW 300

static void icon_base(int x, int y) // black square with rounded corners
{
    enum { R = 28 };
    box(x, y, ICON, ICON, true);
    for (int r = 0; r < R; r++)
        for (int c = 0; c < R; c++)
            if ((R - c) * (R - c) + (R - r) * (R - r) > R * R)
                box(x + c, y + r, 1, 1, false), box(x + ICON - 1 - c, y + r, 1, 1, false),
                    box(x + c, y + ICON - 1 - r, 1, 1, false), box(x + ICON - 1 - c, y + ICON - 1 - r, 1, 1, false);
}

static void icon_update(int x, int y) // download arrow into a tray
{
    icon_base(x, y);
    box(x + 78, y + 32, 20, 60, false);
    for (int i = 0; i < 36; i++) box(x + 88 - (36 - i), y + 92 + i, 2 * (36 - i), 1, false);
    box(x + 36, y + 136, 104, 14, false);
}

static void icon_pong(int x, int y) // two paddles and a ball
{
    icon_base(x, y);
    box(x + 28, y + 40, 16, 64, false);
    box(x + 132, y + 76, 16, 64, false);
    box(x + 80, y + 68, 20, 20, false);
    for (int k = 0; k < 6; k++) box(x + 86, y + 18 + k * 26, 6, 12, false);
}

static void icon_snake(int x, int y) // a snake of white squares heading for a dot
{
    icon_base(x, y);
    static const int cells[][2] = {{1, 4}, {2, 4}, {3, 4}, {3, 3}, {3, 2}, {4, 2}, {5, 2}};
    for (int i = 0; i < 7; i++) box(x + cells[i][0] * 24 + 2, y + cells[i][1] * 24 + 2, 20, 20, false);
    box(x + 5 * 24 + 8, y + 5 * 24 + 8, 8, 8, false); // food
}

static void icon_life(int x, int y) // a glider
{
    icon_base(x, y);
    static const int cells[][2] = {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
    for (int i = 0; i < 5; i++) box(x + 28 + cells[i][0] * 40 + 2, y + 28 + cells[i][1] * 40 + 2, 36, 36, false);
}

typedef struct {
    const char *name;
    void (*icon)(int x, int y);
    void (*run)(void);
} app_t;

static const app_t apps[] = {
    {"Update", icon_update, app_update},
    {"Pong", icon_pong, app_pong},
    {"Snake", icon_snake, app_snake},
    {"Life", icon_life, app_life},
};
#define NAPPS ((int)(sizeof apps / sizeof *apps))

static void draw_home(int sel)
{
    char ver[48];
    snprintf(ver, sizeof ver, "badge os v%s", esp_app_get_description()->version);
    memset(fb, 0, sizeof fb);
    for (int i = 0; i < NAPPS; i++) {
        int x = X0 + i % 4 * PITCH, y = Y0 + i / 4 * ROW;
        apps[i].icon(x, y);
        text(x + ICON / 2 - (int)strlen(apps[i].name) * 12, y + ICON + 24, 3, apps[i].name, true);
        if (i == sel) outline(x - 16, y - 16, ICON + 32, ICON + 32, 8);
    }
    text(W - (int)strlen(ver) * 24 - 24, H - 48, 3, ver, true);
    draw_battery(W - 24 - 64 - 4 * 24, 24, battery_pct());
}

static void home(void)
{
    btn_flush();
    for (int sel = 0, pulls = 3;;) {
        draw_home(sel);
        ui_frame(pulls);
        if (pulls == 3) esp_ota_mark_app_valid_cancel_rollback(); // this version boots: keep it (rollback)
        pulls = 1;
        int b = btn_get(10000);
        if (b < 0) { // the screen sits still: end the panel session, then wait as long as it takes
            ui_close();
            b = btn_get(-1);
        }
        if (b == BTN_PLUS) sel = (sel + NAPPS - 1) % NAPPS; // MENU + is the left one: move left
        else if (b == BTN_MINUS) sel = (sel + 1) % NAPPS; // MENU - is the right one: move right
        else {
            ESP_LOGI(TAG, "opening %s", apps[sel].name);
            ui_wipe(); // the home screen would ghost through the app's first frame
            ui_close();
            btn_flush();
            bar_app = apps[sel].name;
            apps[sel].run();
            bar_app = NULL;
            usb_host_lib_info_t usb;
            if (usb_host_lib_info(&usb) == ESP_OK) esp_restart(); // the app made USB a host: home must stay flashable
            wifi_off(); // whatever the app turned on is off again at home
            ui_wipe(); // and the app through the home screen
            btn_flush();
            pulls = 3;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init(); // Wi-Fi (ESP-NOW) keeps its calibration here
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    epd_init();
    btnq = xQueueCreate(8, sizeof(int));
    sao_init();
    int up, down;
    ESP_LOGI(TAG, "knob: %d (analog on SAO1 GPIO1: %d %d)", knob_angle(), (knob_analog(&up, &down), up), down);
    xTaskCreate(buttons, "buttons", 3072, NULL, 3, NULL);

    ui_wipe(); // the film still shows whatever ran before the reset; partial updates need a known start
    home();
}
