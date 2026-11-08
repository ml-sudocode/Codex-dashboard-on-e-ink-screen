// Doom: id Software's shareware episode (DOOM1.WAD v1.9) on doomgeneric (components/doomgeneric, GPL-2).
// The WAD does not fit the 3 MB app slot: on first open the app downloads it once, zlib-compressed (doom1.wad.z from
// tools/make_wad.py, 1.75 MB), into free flash at WAD_ADDR (outside the partition table, like the Badge
// picture), and every start inflates it into PSRAM (4.2 MB) where the engine reads its lumps in place.
// The game runs at Doom's 35 tics/s on this task; a second task shows the latest frame whenever the panel is free
// (~4 fps): 320x200 scaled x3 to 960x600, palette -> luminance -> 4x4 Bayer dither (still areas stay still).
// Buttons: MENU +/- turn, A walks, B fires, A and B pressed together = use (doors, switches, respawn). The AS5600
// knob steers too. Sound effects (no music) go mixed to 11 kHz mono through snd_write, shaped for a piezo.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_flash_mmap.h"
#include "badge/badge.h"
#include "stb_image.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "g_game.h"
#include "i_sound.h"
#include "i_video.h"
#include "w_file.h"
#include "w_wad.h"
#include "z_zone.h"

#define TAG "doom"
#define WAD_URL "https://github.com/yigitabi5444/supercon26-badge-os/releases/download/doom-wad/doom1.wad.z"
#define WAD_ADDR 0x650000 // free flash up to the end (0x800000): 1.69 MB
#define WAD_MAX (0x800000 - WAD_ADDR)
#define WAD_MAGIC 0x315A5744 // "DWZ1": then the WAD's length, the zlib stream's length, the zlib stream
// luminance curve, picked from host renders of E1M1: LUM_LO..LUM_HI stretched to black..white, then the gamma
#define LUM_LO 0.05f
#define LUM_HI 0.45f
#define LUM_GAMMA 0.8f
#define CHORD_MS 120  // A and B pressed within this of each other = use
#define KNOB_DIR 1    // flip to -1 if turning the knob clockwise turns the view the wrong way
#define SND_RATE 11025
#define SND_GAIN 3    // piezo: loud; the mix clips softly above this
#define SND_HP 0.93f  // one-pole high-pass (~800 Hz at 11 kHz): the piezo can't play the lows anyway

// ---- Game data: download once, inflate into PSRAM every start.

static bool wad_ok(uint32_t *h) { return h[0] == WAD_MAGIC && h[2] + 12 <= WAD_MAX; }

static void say(const char *a, const char *b)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, 6, "Doom", true);
    text(48, TOP + 200, 3, a, true);
    if (b) text(48, TOP + 260, 3, b, true);
    ui_frame(1);
}

static bool download(void)
{
    say("Getting the game data (1.7 MB),", "once. Connecting to Wi-Fi...");
    if (!wifi_connect(20000)) return say("No Wi-Fi.", "Settings has the network."), false;
    esp_http_client_handle_t c = esp_http_client_init(&(esp_http_client_config_t){
        .url = WAD_URL, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 16384, .buffer_size_tx = 4096,
        .timeout_ms = 15000});
    static EXT_RAM_BSS_ATTR uint8_t buf[4096];
    bool ok = false;
    int len = -1, got = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        for (int hops = 0; (status == 301 || status == 302) && hops < 5; hops++) { // GitHub hands on to its CDN
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            if (esp_http_client_open(c, 0) != ESP_OK) break;
            len = esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
        }
        ESP_LOGI(TAG, "download: HTTP %d, %d bytes", status, len);
        if (status == 200 && len > 12 && len <= WAD_MAX &&
            esp_flash_erase_region(NULL, WAD_ADDR, (len + 4095) & ~4095) == ESP_OK) {
            uint32_t magic = 0;
            for (int n, step = 0; got < len && (n = esp_http_client_read(c, (char *)buf, sizeof buf)) > 0; got += n) {
                if (!got) memcpy(&magic, buf, 4), memset(buf, 0xFF, 4); // the magic goes in last
                if (esp_flash_write(NULL, buf, WAD_ADDR + got, n) != ESP_OK) break;
                if (got * 10 / len != step) { // a progress line every 10 %
                    char s[24];
                    snprintf(s, sizeof s, "%d %%", (step = got * 10 / len) * 10);
                    say("Downloading the game data...", s);
                }
            }
            ok = got == len && magic == WAD_MAGIC && esp_flash_write(NULL, &magic, WAD_ADDR, 4) == ESP_OK;
        }
    }
    esp_http_client_cleanup(c);
    wifi_off();
    ranging_start();
    if (!ok) say("Download failed.", "A: try again, B: home");
    return ok;
}

static byte *wad;
static unsigned wad_len;

static bool load_wad(void) // -> wad, wad_len
{
    uint32_t h[3];
    if (esp_flash_read(NULL, h, WAD_ADDR, sizeof h) != ESP_OK || !wad_ok(h)) return false;
    const void *z;
    spi_flash_mmap_handle_t m;
    if (spi_flash_mmap(WAD_ADDR, (h[2] + 12 + 0xFFFF) & ~0xFFFF, SPI_FLASH_MMAP_DATA, &z, &m) != ESP_OK) return false;
    say("Loading...", NULL);
    wad = heap_caps_malloc(h[1], MALLOC_CAP_SPIRAM);
    int n = wad ? stbi_zlib_decode_buffer((char *)wad, h[1], (const char *)z + 12, h[2]) : -1;
    spi_flash_munmap(m);
    ESP_LOGI(TAG, "WAD: %d of %u bytes inflated", n, (unsigned)h[1]);
    if (n != (int)h[1] || memcmp(wad, "IWAD", 4)) return free(wad), false;
    wad_len = n;
    return true;
}

// The engine's WAD "file": the one in PSRAM, mapped, so lumps are used in place (W_CacheLumpNum copies nothing).
static wad_file_t *wad_open(char *path)
{
    static wad_file_t f;
    extern wad_file_class_t stdc_wad_file;
    f = (wad_file_t){&stdc_wad_file, wad, wad_len};
    return &f;
}
static void wad_close(wad_file_t *f) {}
static size_t wad_read(wad_file_t *f, unsigned int off, void *buf, size_t n)
{
    n = off >= wad_len ? 0 : n < wad_len - off ? n : wad_len - off;
    memcpy(buf, wad + off, n);
    return n;
}
wad_file_class_t stdc_wad_file = {wad_open, wad_close, wad_read};

// ---- Display: the latest frame goes to the draw task whenever it is idle.

static uint8_t lum[256]; // palette index -> 0..255, after the luminance curve
static EXT_RAM_BSS_ATTR uint8_t shot[DOOMGENERIC_RESX * DOOMGENERIC_RESY]; // the frame being drawn
static TaskHandle_t drawer;
static volatile bool drawing;

static void draw_task(void *arg)
{
    static const uint8_t bayer[4][4] = {{8, 136, 40, 168}, {200, 72, 232, 104}, {56, 184, 24, 152}, {248, 120, 216, 88}};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int y = 0; y < 3 * DOOMGENERIC_RESY; y++) {
            const uint8_t *s = shot + y / 3 * DOOMGENERIC_RESX, *ty = bayer[y & 3];
            uint8_t *d = fb + (TOP + y) * (W / 8);
            for (int x = 0; x < W; x += 8) {
                uint8_t b = 0;
                for (int k = x; k < x + 8; k++) b = b << 1 | (lum[s[k / 3]] < ty[k & 3]);
                *d++ = b;
            }
        }
        ui_frame(1);
        drawing = false; // ui_frame returned: the panel took fb, the next frame may come
    }
}

void DG_Init(void) {}
void DG_SetWindowTitle(const char *title) {}
void DG_SleepMs(uint32_t t) { ms(t); }
uint32_t DG_GetTicksMs(void) { return esp_timer_get_time() / 1000; }

void DG_DrawFrame(void)
{
    if (drawing) return; // the panel is busy: this frame is skipped, the next one after it shows
    if (palette_changed) {
        palette_changed = false;
        for (int i = 0; i < 256; i++) {
            float l = ((0.299f * colors[i].r + 0.587f * colors[i].g + 0.114f * colors[i].b) / 255 - LUM_LO) /
                      (LUM_HI - LUM_LO);
            lum[i] = 255 * powf(l < 0 ? 0 : l > 1 ? 1 : l, LUM_GAMMA);
        }
    }
    memcpy(shot, DG_ScreenBuffer, sizeof shot); // the engine draws on meanwhile
    drawing = true;
    xTaskNotifyGive(drawer);
}

// ---- Buttons -> Doom keys. MENU +/- turn while held. A and B wait CHORD_MS: if the other comes too, it is a use
// (a tap), else A walks / B fires for as long as it is held. Nothing needs MENU + and MENU - together (OS: home).

static int keyq[16], kn, ki; // queued key events: key | 0x100 if pressed

static void key(int k, bool down)
{
    if (kn < 16) keyq[kn++] = k | down << 8;
}

static void read_buttons(void)
{
    static int was, use, knob_ref = -1;
    static int sent[2] = {-1, -1}; // A, B: 0 = undecided, KEY_x = held down, -1 = idle, -2 - KEY_x = up next poll
    static int64_t down[2];
    int h = btn_held(), now = h & ~was;
    int64_t t = esp_timer_get_time() / 1000;
    // keys go up a poll (a tic) after they went down at the earliest, or Doom would not see a quick tap
    if (use) key(KEY_USE, false), use = 0;
    for (int i = 0; i < 2; i++)
        if (sent[i] < -1) key(-2 - sent[i], false), sent[i] = -1;
    if ((demoplayback || gamestate == GS_DEMOSCREEN || gamestate == GS_FINALE) && now) {
        G_DeferedInitNew(sk_medium, 1, 1); // title or the end of the episode: no menus here, a press starts anew
        was = h;
        return;
    }
    if ((h ^ was) & 1 << BTN_PLUS) key(KEY_LEFTARROW, h & 1 << BTN_PLUS);
    if ((h ^ was) & 1 << BTN_MINUS) key(KEY_RIGHTARROW, h & 1 << BTN_MINUS);
    for (int i = 0; i < 2; i++)
        if (now & 1 << (i ? BTN_B : BTN_A)) down[i] = t, sent[i] = 0;
    if (!sent[0] && !sent[1]) key(KEY_USE, true), use = 1, sent[0] = sent[1] = -1; // both within the window
    for (int i = 0; i < 2; i++) {
        bool held = h & 1 << (i ? BTN_B : BTN_A);
        if (!sent[i] && (!held || t - down[i] >= CHORD_MS)) key(sent[i] = i ? KEY_FIRE : KEY_UPARROW, true);
        if (sent[i] > 0 && !held) sent[i] = -2 - sent[i];
    }
    was = h;

    int a = knob_angle(); // the knob turns the view 1:1: a mouse x of 2 per AS5600 step (4096 per turn)
    if (a >= 0 && knob_ref >= 0) {
        int d = ((a - knob_ref) % 4096 + 6144) % 4096 - 2048;
        if (d) D_PostEvent(&(event_t){.type = ev_mouse, .data2 = 2 * KNOB_DIR * d});
    }
    knob_ref = a;
}

// The engine drains this until it returns 0, once per tic: read the buttons at most once per drain, so a release queued
// for the next poll lands in the next tic (a press and its release in the same tic never reach the game: use, taps).
int DG_GetKey(int *pressed, unsigned char *k)
{
    static bool polled;
    if (ki == kn) {
        ki = kn = 0;
        if (polled) return polled = false, 0;
        read_buttons();
        if (!kn) return 0;
        polled = true;
    }
    *pressed = keyq[ki] >> 8;
    *k = keyq[ki++];
    return 1;
}

// ---- Sound: 8 channels of Doom's 8-bit sound lumps mixed to SND_RATE mono on their own task.

static struct {
    const uint8_t *p;
    volatile uint32_t n; // samples left to play in 16.16 below: n = end, pos runs up to it; 0 = idle
    uint32_t pos, step;
    int vol;
} ch[8];

static void mix_task(void *arg)
{
    static int16_t out[256];
    float prev = 0, hp = 0;
    for (;;) {
        for (int i = 0; i < 256; i++) {
            int s = 0;
            for (int c = 0; c < 8; c++) {
                if (ch[c].pos >= ch[c].n) continue;
                s += (ch[c].p[ch[c].pos >> 16] - 128) * ch[c].vol;
                ch[c].pos += ch[c].step;
            }
            float x = s * SND_GAIN; // up to 8 x 127 x 127 x gain: soft clip into int16
            hp = SND_HP * (hp + x - prev); // the piezo plays the highs; the lows only waste its swing
            prev = x;
            out[i] = hp > 0 ? hp / (1 + hp / 32767) : hp / (1 - hp / 32767);
        }
        snd_write(out, 256); // blocks while the queue is full: this paces the task
    }
}

static snddevice_t snd_devices[] = {SNDDEVICE_SB};

static boolean snd_init(boolean prefix)
{
    if (!snd_start(SND_RATE)) return false; // no sound add-on set up: Doom runs silent
    xTaskCreate(mix_task, "doom snd", 4096, NULL, 6, NULL);
    return true;
}
static void snd_none(void) {}
static int snd_lump(sfxinfo_t *s)
{
    char name[16];
    snprintf(name, sizeof name, "ds%s", s->link ? s->link->name : s->name);
    return W_GetNumForName(name);
}
static void snd_params(int c, int vol, int sep) { ch[c].vol = vol; }
static int snd_play(sfxinfo_t *s, int c, int vol, int sep)
{
    // DMX lump: format (3), rate, sample count, then the samples with 16 pad bytes at each end
    const uint8_t *d = W_CacheLumpNum(s->lumpnum, PU_STATIC);
    int rate = d[2] | d[3] << 8, n = d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24;
    if (d[0] != 3 || n <= 32 || n + 8 > W_LumpLength(s->lumpnum)) return -1;
    ch[c].n = 0; // the mixer leaves the channel alone while it is set up
    ch[c].p = d + 8 + 16;
    ch[c].pos = 0;
    ch[c].step = ((uint32_t)rate << 16) / SND_RATE;
    ch[c].vol = vol;
    ch[c].n = (uint32_t)(n - 32) << 16;
    return c;
}
static void snd_halt(int c) { ch[c].n = 0; }
static boolean snd_playing(int c) { return ch[c].pos < ch[c].n; }
static void snd_cache(sfxinfo_t *s, int n) {}

sound_module_t DG_sound_module = {snd_devices, 1, snd_init, snd_none, snd_lump, snd_none,
                                  snd_params, snd_play, snd_halt, snd_playing, snd_cache};
music_module_t DG_music_module; // never used: Doom starts with -nomusic

static void run(void)
{
    uint32_t h[3];
    while (esp_flash_read(NULL, h, WAD_ADDR, sizeof h) != ESP_OK || !wad_ok(h)) {
        ui_keys(NULL, NULL, "Try again", "Home");
        if (download()) break;
        ui_close();
        if (btn_get(-1) == BTN_B) return;
    }
    if (!load_wad()) {
        ui_keys(NULL, NULL, "Download", "Home");
        say("The game data is damaged.", "A: download it again, B: home");
        ui_close();
        if (btn_get(-1) == BTN_A) esp_flash_erase_region(NULL, WAD_ADDR, 4096), run();
        return;
    }
    ui_keys("Turn left", "Turn right", "Walk  (+B: use)", "Fire  (+A: use)");
    ui_wipe();
    xTaskCreate(draw_task, "doom draw", 6144, NULL, 5, &drawer);
    // zone: what PSRAM has left after the WAD, keeping 1 MB for the OS and the engine's own mallocs
    static char mb[12];
    snprintf(mb, sizeof mb, "%d", (int)((heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) - 1024 * 1024) >> 20));
    ESP_LOGI(TAG, "zone %s MB, internal free %u", mb, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    char *argv[] = {"doom", "-iwad", "doom1.wad", "-mb", mb, "-nomusic", "-warp", "1", "1", "-skill", "3", NULL};
    doomgeneric_Create(11, argv);
    for (;;) doomgeneric_Tick(), ms(1); // ms(1): the idle task gets its turn even when rendering lags
}

static void icon(int x, int y) // a crosshair down a corridor
{
    icon_base(x, y);
    for (int k = 0; k < 40; k++) box(x + 24 + k, y + 24 + k, 3, 3, false), box(x + 149 - k, y + 24 + k, 3, 3, false),
                                 box(x + 24 + k, y + 149 - k, 3, 3, false), box(x + 149 - k, y + 149 - k, 3, 3, false);
    outline(x + 64, y + 64, 48, 48, 3);
    box(x + 86, y + 76, 4, 24, false), box(x + 76, y + 86, 24, 4, false);
}

const app_t app_doom = {"Doom", icon, run};
