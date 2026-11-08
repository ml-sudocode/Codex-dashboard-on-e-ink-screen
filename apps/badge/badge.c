// Badge app: the name tag. Settings' picture if saved, else its text, else "HELLO my name is Badge XXXX".
#include <string.h>
#include "badge/badge.h"
#include "badge/font8x8_basic.h" // same font as text(); a second static copy is 1 KB

static bool px(int ch, int r, int c) // glyph pixel, false outside the 8x8 cell
{
    return r >= 0 && r < 8 && c >= 0 && c < 8 && (font8x8_basic[ch & 0x7F][r] >> c & 1);
}

// text() with smoothed edges: each font pixel is an s x s square whose corners are cut where the outline turns
// outward and filled where it turns inward, so diagonals come out as 45 degree strokes instead of stairs.
static void big(int x, int y, int s, const char *str, bool ink)
{
    for (; *str; str++, x += 8 * s)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                bool on = px(*str, r, c);
                for (int v = 0; v < s; v++)
                    for (int u = 0; u < s; u++) {
                        int dy = v < s / 2 ? -1 : 1, dx = u < s / 2 ? -1 : 1; // the corner this pixel is near
                        int du = dx < 0 ? u : s - 1 - u, dv = dy < 0 ? v : s - 1 - v; // distance to it
                        bool a = px(*str, r, c + dx), b = px(*str, r + dy, c), corner = du + dv < s / 2;
                        bool set = on ? !(corner && !a && !b && !px(*str, r + dy, c + dx)) : corner && a && b;
                        int X = x + c * s + u, Y = y + r * s + v;
                        if (set && X >= 0 && X < W && Y >= TOP && Y < H) {
                            if (ink) fb[Y * (W / 8) + X / 8] |= 0x80 >> (X & 7);
                            else fb[Y * (W / 8) + X / 8] &= ~(0x80 >> (X & 7));
                        }
                    }
            }
}

static void centred(int y, int s, const char *str, bool ink) { big((W - (int)strlen(str) * 8 * s) / 2, y, s, str, ink); }

static void draw(void)
{
    memset(fb, 0, sizeof fb);
    if (cfg_get_image(fb + TOP * (W / 8))) return;
    char t[256];
    int n = cfg_get("badge_text", t, sizeof t);
    if (n > 0) { // whole text, every line centred, as large as fits
        t[n < (int)sizeof t ? n : (int)sizeof t - 1] = 0;
        int lines = 1, len = 0, longest = 1;
        for (char *p = t; *p; p++)
            if (*p == '\n') lines++, len = 0;
            else if (++len > longest) longest = len;
        int s = (W - 48) / (longest * 8), fit = (H - TOP - 48) / (lines * 10 - 2); // lines 8 s tall, 2 s apart
        if (s > fit) s = fit;
        if (s < 1) s = 1;
        int y = TOP + (H - TOP - s * (lines * 10 - 2)) / 2;
        for (char *line = t, *end; line; line = end ? end + 1 : NULL, y += 10 * s) {
            if ((end = strchr(line, '\n'))) *end = 0;
            centred(y, s, line, true);
        }
        return;
    }
    char id[5], name[11] = "Badge ";
    badge_id(id);
    strcat(name, id);
    box(24, TOP + 16, W - 48, 228, true); // the red band: HELLO / my name is
    centred(TOP + 40, 13, "HELLO", false);
    centred(TOP + 160, 5, "my name is", false);
    centred(TOP + 358, 11, name, true);
    box(24, H - 64, W - 48, 48, true);
}

static void run(void)
{
    ui_keys(NULL, NULL, "Redraw", "Home");
    for (;;) {
        ui_wipe(); // a still screen: start it without ghosts
        draw();
        ui_frame(1);
        ui_close();
        int b;
        while ((b = btn_get(-1)) != BTN_A && b != BTN_B) {}
        if (b == BTN_B) return;
    }
}

static void icon(int x, int y) // a name tag: a white card with a black band and a name line
{
    icon_base(x, y);
    box(x + 24, y + 40, 128, 96, false);
    box(x + 24, y + 40, 128, 28, true);
    box(x + 40, y + 96, 96, 10, true);
}

const app_t app_badge = {"Badge", icon, run};
