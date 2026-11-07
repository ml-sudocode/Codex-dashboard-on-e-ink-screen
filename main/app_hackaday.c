// Hackaday app. Placeholder until its app agent lands it (see AGENTS.md).
#include <string.h>
#include "badge.h"

void app_hackaday(void)
{
    ui_keys(NULL, NULL, "Home", "Home");
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, 6, "Hackaday", true);
    text(48, TOP + 200, 4, "Coming soon.", true);
    ui_frame(1);
    ui_close();
    btn_get(-1);
}
