// Noisy Cricket SAO: speaker registers over I2C, and an SWD programmer for its MSPM0C1104 (see cricket.h).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cricket.h"

static const char *TAG = "cricket";

// ---- I2C: register map in the Cricket's firmware/README.md
enum { ADDR = 0x42, R_ID = 0x00, ID = 0xC7, R_VERSION = 0x36, R_MODE = 0x37, R_FIFO_FREE = 0x3D, R_FIFO = 0x3E };
enum { HOST_OFF, HOST_TONE, HOST_STREAM };
#define OUT_RATE_X4 46875 // 4 x 11718.75 Hz, the Cricket's PCM rate

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static int (*gpio1_mv)(int port, bool pullup);
static int64_t next_probe;
static int version = -1;

void cricket_init(i2c_master_bus_handle_t b, int (*mv)(int port, bool pullup))
{
    bus = b;
    gpio1_mv = mv;
    if (bus) i2c_master_bus_add_device(bus, &(i2c_device_config_t){.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                                                   .device_address = ADDR, .scl_speed_hz = 400000},
                                       &dev);
}

static bool rd(uint8_t reg, uint8_t *d, int n) { return dev && i2c_master_transmit_receive(dev, &reg, 1, d, n, 20) == ESP_OK; }
static bool wr(const uint8_t *d, int n) { return dev && i2c_master_transmit(dev, d, n, 20) == ESP_OK; }

int cricket_version(void)
{
    if (esp_timer_get_time() < next_probe) return version;
    next_probe = esp_timer_get_time() + 2000000;
    uint8_t id, v;
    if (!bus || i2c_master_probe(bus, ADDR, 20) != ESP_OK || !rd(R_ID, &id, 1) || id != ID) return version = -1;
    if (!rd(R_VERSION, &v, 1)) return version = -1;
    return version = v == 0xFF ? 0 : v; // stock firmware reads 0xFF past its last register
}

// ---- Sound
void cricket_tone(int hz, int ms, int vol)
{
    uint8_t d[] = {R_MODE, HOST_TONE, hz, hz >> 8, ms, ms >> 8, vol};
    wr(d, sizeof d);
}

void cricket_quiet(void)
{
    uint8_t d[] = {R_MODE, HOST_OFF};
    wr(d, sizeof d);
}

static int snd_rate, snd_acc, snd_n;
static uint8_t snd_buf[1 + 64] = {R_FIFO}; // 64 bytes = 5.5 ms; the Cricket's FIFO holds 128

static void snd_flush(void)
{
    uint8_t free;
    while (snd_n && rd(R_FIFO_FREE, &free, 1) && free < snd_n) vTaskDelay(1);
    if (snd_n) wr(snd_buf, 1 + snd_n);
    snd_n = 0;
}

bool cricket_snd_start(int rate)
{
    if (rate <= 0 || cricket_version() < 1) return false;
    snd_rate = rate, snd_acc = 0, snd_n = 0;
    uint8_t d[] = {R_MODE, HOST_STREAM};
    return wr(d, sizeof d);
}

void cricket_snd_write(const int16_t *pcm, int n)
{
    if (!snd_rate) return;
    for (int i = 0; i < n; i++) // ponytail: nearest-sample resampling, add a low-pass if it aliases audibly
        for (snd_acc += OUT_RATE_X4; snd_acc >= 4 * snd_rate; snd_acc -= 4 * snd_rate) {
            snd_buf[1 + snd_n++] = (uint8_t)((pcm[i] >> 8) + 128);
            if (snd_n == sizeof snd_buf - 1) snd_flush();
        }
}

void cricket_snd_stop(void) // what is queued plays out; the Cricket goes back to chirping 1 s later
{
    if (snd_rate) snd_flush();
    snd_rate = 0;
}

// ---- Which port: the Cricket's GPIO1 is SWCLK with the MSPM0's 40k pull-down. Against the badge's ~45k pull-up that
// reads about half the supply, and 0 with the pull-down. An empty port follows the pulls (3.3 V / 0), the knob's
// analog out ignores them.
static const int sao_gpio[3][2] = {{2, 42}, {4, 5}, {7, 6}}; // GPIO1, GPIO2 of SAO1..3

int cricket_port(void)
{
    int found = -1;
    for (int p = 0; p < 3 && gpio1_mv; p++) {
        int up = gpio1_mv(p, true), down = gpio1_mv(p, false);
        ESP_LOGI(TAG, "SAO%d GPIO1: %d mV pulled up, %d mV pulled down", p + 1, up, down);
        if (up > 800 && up < 2500 && down >= 0 && down < 300) {
            if (found >= 0) return -1;
            found = p;
        }
    }
    return found;
}

// ---- SWD, bit-banged (ARM ADIv5). Clock idles low; the target samples SWDIO on the rising edge and changes it after.
static int clk, dio;

static void half(void) { esp_rom_delay_us(1); }
static void pulse(void) { gpio_set_level(clk, 1), half(), gpio_set_level(clk, 0), half(); }
static void wbit(int b) { gpio_set_level(dio, b), pulse(); }
static int rbit(void) { int b = gpio_get_level(dio); pulse(); return b; }
static void drive(bool on) { gpio_set_direction(dio, on ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT); }

static void wbits(uint32_t v, int n)
{
    for (int i = 0; i < n; i++) wbit(v >> i & 1);
}

// One transfer; returns the ACK (1 = OK, 2 = WAIT, 4 = FAULT, 0/7 = nobody), 8 on a read parity error.
static int xfer(int ap, int rnw, int a, uint32_t *v)
{
    for (int tries = 0;; tries++) {
        int par = ap ^ rnw ^ (a >> 2 & 1) ^ (a >> 3 & 1);
        wbits(1 | ap << 1 | rnw << 2 | (a & 0xC) << 1 | par << 5 | 1 << 7, 8);
        drive(false);
        pulse(); // turnaround
        int ack = rbit() | rbit() << 1 | rbit() << 2;
        if (ack != 1) {
            pulse();
            drive(true);
            wbits(0, 8);
            if (ack == 2 && tries < 100) continue;
            return ack;
        }
        uint32_t d = 0;
        int p = 0;
        if (rnw) {
            for (int i = 0; i < 32; i++) {
                int b = rbit();
                d |= (uint32_t)b << i, p ^= b;
            }
            p ^= rbit(); // 0 if the parity bit matches
            pulse();
            drive(true);
            *v = d;
        } else {
            pulse();
            drive(true);
            d = *v;
            for (int i = 0; i < 32; i++) p ^= d >> i & 1;
            wbits(d, 32);
            wbit(p);
            p = 0;
        }
        wbits(0, 8); // idle cycles: let posted writes go through
        return p ? 8 : 1;
    }
}

enum { DP_IDR = 0x0, DP_ABORT = 0x0, DP_CTRL = 0x4, DP_SELECT = 0x8, DP_RDBUFF = 0xC };
enum { AP_CSW = 0x0, AP_TAR = 0x4, AP_DRW = 0xC };
static bool swd_ok;

static uint32_t dp_rd(int a)
{
    uint32_t v = 0;
    if (xfer(0, 1, a, &v) != 1) swd_ok = false;
    return v;
}
static void dp_wr(int a, uint32_t v)
{
    if (xfer(0, 0, a, &v) != 1) swd_ok = false;
}
static void ap_wr(int a, uint32_t v)
{
    if (xfer(1, 0, a, &v) != 1) swd_ok = false;
}
static uint32_t ap_rd(int a)
{
    uint32_t v;
    if (xfer(1, 1, a, &v) != 1) swd_ok = false; // posted: the value comes with the next read
    return dp_rd(DP_RDBUFF);
}
static void mem_wr(uint32_t addr, uint32_t v) { ap_wr(AP_TAR, addr), ap_wr(AP_DRW, v); }
static uint32_t mem_rd(uint32_t addr) { return ap_wr(AP_TAR, addr), ap_rd(AP_DRW); }

static bool swd_connect(void)
{
    swd_ok = true;
    drive(true);
    gpio_set_level(dio, 1);
    for (int i = 0; i < 60; i++) pulse(); // line reset
    wbits(0xE79E, 16);                     // JTAG-to-SWD
    gpio_set_level(dio, 1);
    for (int i = 0; i < 60; i++) pulse();
    wbits(0, 8);
    uint32_t idr = dp_rd(DP_IDR);
    ESP_LOGI(TAG, "DPIDR %08lx", (unsigned long)idr);
    if (!swd_ok || (idr & 0xFFF) != 0x477) return false; // designer Arm (0x23B), bit 0 set
    dp_wr(DP_ABORT, 0x1E);                               // clear sticky errors
    dp_wr(DP_CTRL, 0x50000000);                          // CSYSPWRUPREQ | CDBGPWRUPREQ
    for (int i = 0; i < 100 && (dp_rd(DP_CTRL) & 0xA0000000) != 0xA0000000; i++) esp_rom_delay_us(100);
    dp_wr(DP_SELECT, 0); // AP 0 = AHB-AP (MSPM0 DEBUGSS), bank 0
    uint32_t csw = ap_rd(AP_CSW);
    ap_wr(AP_CSW, (csw & ~0x3Fu) | 0x2); // 32-bit, no address increment
    return swd_ok;
}

// ---- MSPM0C1104 flash controller (SLAU893 chapter 6; addresses from TI's hw_flashctl.h)
#define DHCSR 0xE000EDF0
#define FL_CMDEXEC 0x400CE100
#define FL_CMDTYPE 0x400CE104
#define FL_CMDCTL 0x400CE108
#define FL_CMDADDR 0x400CE120
#define FL_CMDBYTEN 0x400CE124
#define FL_CMDDATA0 0x400CE130
#define FL_CMDDATA1 0x400CE134
#define FL_WEPROTA 0x400CE1D0 // 1 bit per 1 KB sector of MAIN
#define FL_WEPROTB 0x400CE1D4
#define FL_STATCMD 0x400CE3D0
#define GPIOA_DOUTSET 0x400A1290
#define SYS_RESETLEVEL 0x400B0300
#define SYS_RESETCMD 0x400B0304
enum { CMD_PROGRAM = 0x01, CMD_ERASE_SECTOR = 0x42, CMD_CLEARSTATUS = 0x05, FLASH_SIZE = 16384, SECTOR = 1024 };

static bool fl_run(uint32_t type, uint32_t addr) // write protection re-arms after every command
{
    mem_wr(FL_CMDTYPE, CMD_CLEARSTATUS);
    mem_wr(FL_CMDEXEC, 1);
    for (int i = 0; i < 100 && !(mem_rd(FL_STATCMD) & 1); i++) {}
    mem_wr(FL_WEPROTA, 0);
    mem_wr(FL_WEPROTB, 0);
    mem_wr(FL_CMDCTL, 0);
    mem_wr(FL_CMDTYPE, type);
    mem_wr(FL_CMDADDR, addr);
    mem_wr(FL_CMDEXEC, 1);
    uint32_t st = 0;
    for (int i = 0; i < 1000 && !((st = mem_rd(FL_STATCMD)) & 1); i++) esp_rom_delay_us(50); // sector erase ~4 ms
    return swd_ok && (st & 3) == 3; // CMDDONE | CMDPASS
}

extern const uint8_t fw_start[] asm("_binary_cricket_fw_bin_start");
extern const uint8_t fw_end[] asm("_binary_cricket_fw_bin_end");

static const char *flash(void (*progress)(int, const char *))
{
    int size = fw_end - fw_start;
    if (size <= 0 || size > FLASH_SIZE || size % 8) return "bad firmware image";
    if (!swd_connect()) return "no SWD answer";
    mem_wr(DHCSR, 0xA05F0003); // DBGKEY | C_HALT | C_DEBUGEN
    if (!(mem_rd(DHCSR) & (1 << 17))) return "core did not halt";
    mem_wr(GPIOA_DOUTSET, 1 << 26); // amp shutdown (PA26, active high): no DC into the speaker while halted
    for (int a = 0; a < FLASH_SIZE; a += SECTOR) {
        if (progress) progress(5 + 15 * a / FLASH_SIZE, "Erasing");
        if (!fl_run(CMD_ERASE_SECTOR, a)) return "erase failed";
    }
    for (int a = 0; a < size; a += 8) {
        uint32_t w[2];
        memcpy(w, fw_start + a, 8);
        if (w[0] == 0xFFFFFFFF && w[1] == 0xFFFFFFFF) continue;
        if (progress && a % 512 == 0) progress(20 + 60 * a / size, "Writing");
        mem_wr(FL_CMDBYTEN, 0xFF);
        mem_wr(FL_CMDDATA0, w[0]);
        mem_wr(FL_CMDDATA1, w[1]);
        if (!fl_run(CMD_PROGRAM, a)) return "write failed";
    }
    for (int a = 0; a < size; a += 4) {
        if (progress && a % 1024 == 0) progress(80 + 15 * a / size, "Verifying");
        uint32_t w;
        memcpy(&w, fw_start + a, 4);
        if (mem_rd(a) != w || !swd_ok) return "verify failed";
    }
    mem_wr(DHCSR, 0xA05F0000);         // debug off, run
    mem_wr(SYS_RESETLEVEL, 1);         // BOOTRST: CPU and peripherals, like a power cycle minus the POR
    mem_wr(SYS_RESETCMD, 0xE4000001); // KEY | GO; whether this one is ACKed doesn't matter
    return NULL;
}

const char *cricket_update(int port, void (*progress)(int pct, const char *what))
{
    if (port < 0 || port > 2) return "no port";
    clk = sao_gpio[port][0], dio = sao_gpio[port][1];
    gpio_reset_pin(clk), gpio_reset_pin(dio);
    gpio_set_level(clk, 0);
    gpio_set_direction(clk, GPIO_MODE_OUTPUT);
    if (progress) progress(0, "Connecting");
    const char *err = flash(progress);
    gpio_reset_pin(clk), gpio_reset_pin(dio); // inputs again (reset_pin leaves a pull-up on)
    gpio_set_pull_mode(clk, GPIO_FLOATING), gpio_set_pull_mode(dio, GPIO_FLOATING);
    if (err) {
        ESP_LOGE(TAG, "update: %s", err);
        return err;
    }
    if (progress) progress(95, "Starting");
    for (int i = 0; i < 50; i++) { // boot + self-test chirps take ~3 s, I2C answers right away
        vTaskDelay(pdMS_TO_TICKS(100));
        next_probe = 0;
        if (cricket_version() == CRICKET_FW) {
            if (progress) progress(100, "Done");
            return NULL;
        }
    }
    return "new firmware not answering";
}
