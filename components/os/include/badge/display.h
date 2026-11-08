// Display: the 9.7" black/white/red e-paper, 960 x 672, behind a 1-bit frame buffer.
//
// Draw into fb (badge/draw.h has box, outline, text), then ui_frame() shows it.
// - Rows 0..TOP-1 are the OS top bar on every screen: ui_frame draws it over fb. Keep your picture below TOP.
// - ui_frame shows fb with one calm partial update: ~0.24 s if the changes reach the top of the screen, ~0.13 s if
//   they all lie in the lower half. Only pixels that changed are driven, nothing else flashes.
// - It returns while the panel works (it waits only if the previous frame has not started yet), so draw the next
//   frame meanwhile. Run game logic on its own clock (apps/snake), not one step per frame.
// - No change since the last frame = no refresh.
// - Ink comes out dark red; erased ink leaves a light ghost. ui_wipe() shakes the ghosts off at scene changes.
// - Call ui_close() when the screen will sit still for a while: it also shows a frame still on its way.
// The panel's slow full refresh (~35 s, true black and red) is never used: the owner does not want it on the badge.
#pragma once
#include <stdint.h>

#define W 960
#define H 672
#define PLANE (W * H / 8) // MONO_HLSB, MSB = leftmost pixel, 1 = black
#define TOP 48            // rows 0..TOP-1 are the OS top bar on every screen: ui_frame draws over them

extern uint8_t fb[PLANE]; // what the next ui_frame() shows

void ui_frame(int pulls); // show fb (pulls is ignored)
void ui_close(void);
// What each button does now, shown beside it in the top bar (two buttons in each top corner: MENU + above A on the
// left, MENU - above B on the right). Up to ~16 characters each; NULL or "" = no label. Set it again when it changes;
// it shows from the next ui_frame. Every app starts with no labels.
void ui_keys(const char *plus, const char *minus, const char *a, const char *b);
void ui_wipe(void); // scene change: an all-black then an all-white frame shake the ghosts off (~2.8 s), fb all white
