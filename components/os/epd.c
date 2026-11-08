// Panel driver: Pervasive Displays E2969JS0B1, 9.7" black/white/red, 960x672, two CoGs ("B1").
// The left CoG (master) drives columns 0-479, the right CoG (slave) columns 480-959. Pins, timings and the rules that
// keep the panel alive: docs/DEVICE_INFO.md.
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
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

// A panel session: epd_open (reset, frame data, register sequence, DC-DC up), pulls, epd_close. Same sequence as
// PDi EPD_Driver_GU_large and the stock epd.py, plus what the badge team documented for this B1 CoG
// (2026-Billboard_Badge/documentation/itc97-driver-reference.pdf): the 0x70 booster setting and the 0xB0 waveform.

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

// PDi Wide_Large's waits for this CoG family: the session opens in 348 ms and 0xB0 still latches.
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

// Frame into CoG RAM: black into 0x10, 0x11 (red) all white. black NULL = all white.
void epd_load(const uint8_t *black)
{
    CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02); // DUW: 60 bytes x 672 lines
    CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9);             // DRFW: whole panel
    for (int cog = LEFT; cog <= RIGHT; cog++) {
        send_rows(cog, 0x10, black, 0, H - 1);
        send_rows(cog, 0x11, NULL, 0, H - 1);
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

// After reset (and the frame data for a full session): the register sequence, a waveform table into 0xB0 instead
// of the panel's own 33 s one, DC-DC up.
static void epd_start(const uint8_t *lut)
{
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

// A full session on the badge team's black/white table.
void epd_open(const uint8_t *black)
{
    epd_reset();
    epd_load(black);
    epd_start(lutb);
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

// ---- Calm partial updates (panel_lab's driver, experiments 05 and 11-14)
// part_start(black)  black/white frame, no flash: only pixels that differ from what the panel shows are driven. One
//                    pull of 10 frames over the rows that changed: ~0.24 s if they reach the top of the panel, down
//                    to ~0.13 s if they all lie in the lower half. Returns as soon as the pull runs; part_wait()
//                    before the next panel access (the CoG drops every command and RAM write while BUSY is low).
//                    Ink comes out dark red; erased pixels leave a light ghost. Keeps the session (DC-DC) open.
// epd_sleep()        ends the partial session with the normal DC-DC soft shutdown.
// part_shown must match the film before the first part_start: ui_wipe leaves both all white.

#define PART_DRIVE 9 // frames of VDH/VDL per change: 9 fills the 10-frame pull
_Static_assert(PART_DRIVE >= 1 && PART_DRIVE <= 9, "the pull has 10 frames and the last one must be GND");
#define PART_MIN_ROWS 336 // fewest rows a pull scans. A frame lasts as long as its scan, so this sets the least drive a
                          // change gets: 9 frames x 336 rows x 35 us = ~105 ms, like 4.4 full-height frames (readable)

EXT_RAM_BSS_ATTR uint8_t part_shown[PLANE]; // what the film shows; after every pull the CoG's 0x11 holds the same
static int part_prev[2][4];       // per CoG, the byte box {x0, x1, y0, y1} the last pull changed; x1 < x0: none
int part_n = -1;                  // pulls in the open partial session, -1 = no session
static volatile int64_t part_t0;  // when the running pull's frame started, 0 = no pull running
static int part_a, part_b;        // its rows

// Transition table: old frame in 0x11, new frame in 0x10, table = (old << 1) | new. Table 1 (white->black) VDH and
// table 2 (black->white) VDL for the first PART_DRIVE frames, then GND; tables 0, 3 and VCOM at GND. Every group is
// one frame, phase c only (SRab 0, TPc 1, TPd 0): 10 groups = 10 frames, 237 ms at full height. Zero in a whole half,
// RP 0, SRab = SRcd = 0 and SRcd 0 alone all hang the CoG. The last frame is GND.
static void part_table(uint8_t *t)
{
    for (int k = 0; k < 50; k++) memcpy(t + 8 * k, (const uint8_t[]){0, 1, 1, 0, 1, 0, 1, 1}, 8);
    for (int f = 0; f < PART_DRIVE; f++) {
        t[(10 + f) * 8] = 1 << 2; // VDH in phase c (level bits 3-2)
        t[(20 + f) * 8] = 2 << 2; // VDL
    }
}

// Session open on the transition table, then both RAMs = what the film shows.
void part_open(void)
{
    static uint8_t lut[400];
    part_table(lut);
    epd_reset();
    epd_start(lut);
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

// Until the running pull is done: BUSY polled every 1 ms tick.
void part_wait(void)
{
    if (!part_t0) return;
    int64_t t;
    while (!(gpio_get_level(BUSY_L) && gpio_get_level(BUSY_R)) && (t = esp_timer_get_time() - part_t0) < 3000000)
        vTaskDelay(1);
    ESP_LOGI(TAG, "partial: rows %d-%d, %d ms", part_a, part_b, (int)((esp_timer_get_time() - part_t0) / 1000));
    part_t0 = 0;
}

void epd_sleep(void)
{
    part_wait();
    if (part_n >= 0) epd_close();
    part_n = -1;
}

void part_start(const uint8_t *black)
{
    int64_t t0 = esp_timer_get_time();
    int now[2][4];
    bool any = false;
    for (int c = 0; c < 2; c++) part_box(black, part_shown, c + 1, now[c]), any |= now[c][1] >= now[c][0];
    if (!any) return;
    part_wait();
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
    // Scan only rows a..b: DUW at the trigger moves the scan start, DRFW the end, and nothing outside is driven.
    // Widened to PART_MIN_ROWS for the drive, and until a + b + 1 >= 680: the gate chain then carries the last frame's
    // pulse past the panel's 680 gate lines before the next frame, otherwise the band reappears b + 1 rows lower.
    // Rows that cannot meet that get the whole panel.
    int a = H, b = -1;
    for (int c = 0; c < 2; c++)
        if (now[c][1] >= now[c][0]) a = MIN(a, now[c][2]), b = MAX(b, now[c][3]);
    a &= ~3, b = MIN(H - 1, MAX(b, MAX(679 - a, a + PART_MIN_ROWS - 1)) | 3);
    a = MIN(a, (b - PART_MIN_ROWS + 1) & ~3);
    if (a + b + 1 < 680) a = 0, b = H - 1;
    if (a == 0) {
        CMD(BOTH, 0x13, 0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02);
        CMD(BOTH, 0x90, 0x00, 0x3B, 0x00, 0xA9);
    } else {
        CMD(BOTH, 0x13, 0x00, 0x3B, a & 0xFF, a >> 8, b & 0xFF, b >> 8);
        CMD(BOTH, 0x90, 0x00, 0x3B, a >> 2, MIN(0xA9, (b >> 2) + 1));
    }
    CMD(BOTH, 0x15, 0x3C);
    part_t0 = t0, part_a = a, part_b = b;
    memcpy(part_shown, black, PLANE);
    part_n++;
}
