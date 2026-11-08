// The home screen (home.c) and its apps (apps.c).
#pragma once
#include "badge/app.h"

extern const app_t *const apps[];
extern const int napps;
void home(void); // never returns
