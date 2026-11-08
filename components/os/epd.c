// Panel driver: Pervasive Displays E2969JS0B1, 9.7" black/white/red, 960x672, two CoGs ("B1").
// The left CoG (master) drives columns 0-479, the right CoG (slave) columns 480-959. Pins, timings and the rules that
// keep the panel alive: docs/DEVICE_INFO.md.
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "badge/badge.h"
#include "epd.h"

static const char *TAG = "os";

#define HALF (PLANE / 2)  // what one CoG gets of one plane
#define REVERSE_BITS 1    // stock driver: reverse unless /data/hwconf display_pn == "84"

enum { CS_L = 13, CS_R = 3, BUSY_L = 12, BUSY_R = 8, DC = 10, RST = 11, SCK = 21, MOSI = 47 };
enum { LEFT = 1, RIGHT = 2, BOTH = 3 };

static spi_device_handle_t spi;

void ms(int t) { vTaskDelay(pdMS_TO_TICKS(t) + 1); } // badge.h; at least t ms (1 kHz tick)

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
static void wait_ready(const char *what, int timeout_ms)
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
}

static uint8_t rev(uint8_t b)
{
    b = b >> 4 | b << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

// One plane into one CoG's RAM: its 60 bytes of every row, bit-reversed, the RAM pointer counting down from x 0x3B
// (RAM_RW {x1, y0 lo, 0x14 | y0 >> 8}: panel_lab experiment 02). plane NULL = all white.
static void send_plane(int cog, uint8_t index, const uint8_t *plane)
{
    static DMA_ATTR uint8_t buf[HALF];
    const uint8_t *src = plane ? plane + (cog == RIGHT ? W / 16 : 0) : NULL;
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W / 16; x++) {
            uint8_t b = src ? src[y * (W / 8) + x] : 0;
            buf[n++] = REVERSE_BITS ? rev(b) : b;
        }
    CMD(cog, 0x12, 0x3B, 0x00, 0x14); // RAM_RW
    cmd(cog, index, buf, n);
}

// The whole panel as the RAM window (DUW) and the refresh window (DRFW). DRFW is never shorter: a shorter y1 copies
// the top rows down the panel.
static void duw(void) { CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); } // 60 bytes x 672 lines
static void drfw(void) { CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9); }

// A panel session: epd_reset, both RAM planes and epd_start (in either order), pulls, epd_close. Same sequence as
// PDi EPD_Driver_GU_large and the stock epd.py, plus what the badge team documented for this B1 CoG
// (2026-Billboard_Badge/documentation/itc97-driver-reference.pdf): the 0x70 booster setting and the 0xB0 waveform.

// Short waveform for the B1 CoG: data/rendered_luts.py "lutb_base" from the badge team.
// 5 tables (paper, ink, red, unused, VCOM) x 10 groups of {level, on, off, ph2, on, off, ph2, repeat};
// a level byte is 2 bits per phase: 0 GND, 1 VDH, 2 VDL, 3 VDHR. A pull runs a fixed ~1070 ms
// whatever the table asks for, so one pull is a light dose and three make a clean image.
static const uint8_t lut_fast[400] = {
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

// Calm transition table (panel_lab experiments 05 and 11-14): old frame in 0x11, new frame in 0x10, table =
// (old << 1) | new, so only pixels that changed are driven. Table 1 (white->black) VDH and table 2 (black->white) VDL
// for the first CALM_DRIVE frames, then GND; tables 0, 3 and VCOM at GND. Every group is one frame, phase c only
// (SRab 0, TPc 1, TPd 0): 10 groups = 10 frames, 237 ms. Zero in a whole half, RP 0, SRab = SRcd = 0 and SRcd 0 alone
// all hang the CoG. The last frame is GND.
#define CALM_DRIVE 9 // frames of VDH/VDL per change: 9 fills the 10-frame pull
_Static_assert(CALM_DRIVE >= 1 && CALM_DRIVE <= 9, "the pull has 10 frames and the last one must be GND");

static void calm_table(uint8_t *t)
{
    for (int k = 0; k < 50; k++) memcpy(t + 8 * k, (const uint8_t[]){0, 1, 1, 0, 1, 0, 1, 1}, 8);
    for (int f = 0; f < CALM_DRIVE; f++) {
        t[(10 + f) * 8] = 1 << 2; // VDH in phase c (level bits 3-2)
        t[(20 + f) * 8] = 2 << 2; // VDL
    }
}

// PDi Wide_Large's waits for this CoG family: the session opens in 348 ms and 0xB0 still latches.
void epd_reset(void)
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

void epd_load(const uint8_t *p10, const uint8_t *p11)
{
    duw();
    drfw();
    for (int cog = LEFT; cog <= RIGHT; cog++) send_plane(cog, 0x10, p10), send_plane(cog, 0x11, p11);
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

// After reset: the register sequence, a waveform table into 0xB0 instead of the panel's own 33 s one, DC-DC up.
void epd_start(int table)
{
    static uint8_t calm[400];
    const uint8_t *lut = lut_fast;
    if (table == EPD_CALM) calm_table(calm), lut = calm;
    int hi = 2, lo = 10; // load strobe waits
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
    cmd(BOTH, 0xB0, lut, 400); // one chance per session: rewriting it re-latches the slow one
    dcdc_on();
}

// Run the latched waveform once on what is in RAM. Returns ms spent.
int epd_pull(int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    CMD(BOTH, 0x15, 0x3C);
    ms(5);
    wait_ready("pull", timeout_ms);
    return (esp_timer_get_time() - t0) / 1000;
}

// In a calm session 0x11 holds what the film shows: after every pull the CoG swaps its planes, so the frame just shown
// is there. The next frame goes into 0x10, and the pull drives only the pixels where the two differ.
static int64_t triggered; // when epd_frame started its pull

void epd_frame(const uint8_t *next)
{
    duw();
    for (int cog = LEFT; cog <= RIGHT; cog++) send_plane(cog, 0x10, next);
    drfw();
    CMD(BOTH, 0x15, 0x3C);
    triggered = esp_timer_get_time();
}

// BUSY drops a few ms after the trigger, so a wait right after epd_frame (a ui_close straight after ui_frame) must not
// read the still-high line as done: give it 5 ms, as epd_pull does.
void epd_wait(void)
{
    while (esp_timer_get_time() - triggered < 5000) vTaskDelay(1);
    int64_t t0 = esp_timer_get_time();
    while (!(gpio_get_level(BUSY_L) && gpio_get_level(BUSY_R)) && esp_timer_get_time() - t0 < 3000000) vTaskDelay(1);
}

void epd_close(void)
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

void epd_init(void)
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
