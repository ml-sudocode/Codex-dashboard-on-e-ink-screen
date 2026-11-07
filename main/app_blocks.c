// Blocks app. Placeholder until its app agent lands it (see AGENTS.md).
#include <string.h>
#include "badge.h"

void app_blocks(void)
{
    memset(fb, 0, sizeof fb);
    text(48, TOP + 48, 6, "Blocks", true);
    text(48, TOP + 200, 4, "Coming soon.", true);
    text(48, TOP + 260, 3, "Any button goes home.", true);
    ui_frame(1);
    ui_close();
    btn_get(-1);
}
