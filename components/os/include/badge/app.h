// An app: what the home screen shows and runs. Each app in apps/<name>/ defines one, e.g.
//   const app_t app_snake = {"Snake", icon, run};
// and main/apps.c lists it. run() takes over the badge on the home screen's task; returning goes back home (most
// apps never return: MENU + and MENU - held together restart the badge into the home screen).
#pragma once

typedef struct {
    const char *name;           // under the icon and in the top bar
    void (*icon)(int x, int y); // draws the ICON x ICON icon with its top left corner at (x, y)
    void (*run)(void);
} app_t;

#define ICON 176 // icon square, px
void icon_base(int x, int y); // the black square with rounded corners every icon starts from: draw on it in white
