// The home screen's apps, in this order, 8 to a page. Each one lives in apps/<name>/ (how to write one: apps/README.md).
#include "home.h"

extern const app_t app_badge, app_clock, app_update, app_pong, app_snake, app_life, app_aliens, app_blocks, app_rocks,
    app_radar, app_source, app_hackaday, app_video, app_weather, app_settings, app_doom, app_codex_board;

const app_t *const apps[] = {
    &app_badge, // the name tag first: what a badge is for
    &app_codex_board,
    &app_clock,
    &app_update,
    &app_pong,
    &app_snake,
    &app_life,
    &app_aliens,
    &app_blocks,
    &app_rocks,
    &app_radar,
    &app_source,
    &app_hackaday,
    &app_video,
    &app_weather,
    &app_settings,
    &app_doom,
};
const int napps = sizeof apps / sizeof *apps;
