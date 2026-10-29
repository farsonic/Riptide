/*
 * Riptide - RiptidePrefs (MUI): the Status page (mui_status.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_STATUS_H
#define RT_PREFS_MUI_STATUS_H

#include "prefs/mui.h"

int  status_classes_make(void);
void status_classes_free(void);
/* the page's contents; a click on a connection makes app return
 * chosen_id, a change of the Update setting update_id */
Object *status_page_build(Object *app, ULONG chosen_id, ULONG update_id);
/* call when app returned update_id */
void status_update_chosen(void);
/* call when app returned chosen_id */
void status_table_chosen(void);
/* show tab t (0 interfaces, 1 connections, 2 names and time, 3 counters) */
void status_page_tab(int t);
/* after the window and its objects are gone */
void status_page_free(void);

#endif
