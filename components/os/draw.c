// Drawing into fb: rectangles, the 8x8 font, the base of every home screen icon.
#include <sys/param.h>
#include "badge/badge.h"
#include "badge/font8x8_basic.h" // public domain IBM VGA 8x8 font: one byte per row, bit 0 = leftmost pixel

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

void text(int x, int y, int scale, const char *s, bool ink)
{
    for (; *s; s++, x += 8 * scale)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (font8x8_basic[*s & 0x7F][r] >> c & 1) box(x + c * scale, y + r * scale, scale, scale, ink);
}

void icon_base(int x, int y) // black square with rounded corners
{
    enum { R = 28 };
    box(x, y, ICON, ICON, true);
    for (int r = 0; r < R; r++)
        for (int c = 0; c < R; c++)
            if ((R - c) * (R - c) + (R - r) * (R - r) > R * R)
                box(x + c, y + r, 1, 1, false), box(x + ICON - 1 - c, y + r, 1, 1, false),
                    box(x + c, y + ICON - 1 - r, 1, 1, false), box(x + ICON - 1 - c, y + ICON - 1 - r, 1, 1, false);
}
