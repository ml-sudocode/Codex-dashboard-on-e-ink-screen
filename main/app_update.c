// Wired update app. Placeholder until its app agent lands it (see AGENTS.md).
#include <string.h>
#include "badge.h"

void app_update(void)
{
    memset(fb, 0, sizeof fb);
    text(48, 48, 6, "Wired update", true);
    text(48, 200, 4, "Coming soon.", true);
    text(48, 260, 3, "Any button goes home.", true);
    ui_frame(1);
    ui_close();
    btn_get(-1);
}
