// Drawing into fb (badge/display.h): everything clips to the screen. ink true = black, false = white.
#pragma once
#include <stdbool.h>

void box(int x, int y, int w, int h, bool ink);              // filled rectangle
void outline(int x, int y, int w, int h, int t);             // black frame, t px thick
void text(int x, int y, int scale, const char *s, bool ink); // 8x8 font (font8x8_basic.h), 8 * scale px per character
