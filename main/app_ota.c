// Online update app. Placeholder until its app agent lands it (see AGENTS.md).
#include <string.h>
#include "badge.h"

void app_ota(void)
{
    memset(fb, 0, sizeof fb);
    text(48, 48, 6, "Online update", true);
    text(48, 200, 4, "Coming soon.", true);
    text(48, 260, 3, "Any button goes home.", true);
    ui_frame(3);
    ui_close();
    btn_get(-1);
}
