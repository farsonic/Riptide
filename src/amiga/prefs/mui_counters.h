/*
 * Riptide - RiptidePrefs (MUI): the Counters page (mui_counters.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_COUNTERS_H
#define RT_PREFS_MUI_COUNTERS_H

#include "prefs/mui.h"

int  counters_classes_make(void);
void counters_classes_free(void);
/* the page's contents (it reads and draws by itself while shown) */
Object *counters_page_build(void);
/* help bubbles on or off */
void counters_tips(int on);
/* after the window and its objects are gone */
void counters_page_free(void);

#endif
