/*
 * Riptide - RiptidePrefs (MUI): the custom classes the window is built
 * from, each drawn in the theme (mui_theme.c):
 *
 *   nav     the page list on the left: an icon and a name per page, under
 *           "Settings" and "Advanced"; P, Shift-P and the cursor keys
 *           change the page; a help bubble per page
 *   head    the top of a page: its icon, name and what it is about
 *   qbutton a flat button; "_Save" makes S its key
 *   qlabel  a label of a setting (right-aligned, its key underlined), a
 *           note (dim), a section title (bold) or the text of a message
 *   status  the line at the bottom
 *   line    a one-pixel separator
 *
 * Tags of our own are in one private range (RTAG); "(init)" is given at
 * creation, "(set)", "(get)", "(notify)" may be watched.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_CLASSES_H
#define RT_PREFS_MUI_CLASSES_H

#include "prefs/mui.h"

#define RTAG (TAG_USER | 0x52500000UL)
#define MUIA_Nav_Active     (RTAG + 0x101)  /* (init, set, get, notify) the page shown */
#define MUIA_Head_Page      (RTAG + 0x201)  /* (init, set) the page described */
#define MUIA_QButton_Label  (RTAG + 0x301)  /* (init) "_Save" */
#define MUIA_QButton_Key    (RTAG + 0x302)  /* (init) a raw key code (Return, Esc) that presses it too */
#define MUIA_QButton_ID     (RTAG + 0x303)  /* (init) the return ID that key gives */
#define MUIA_QLabel_Text    (RTAG + 0x401)  /* (init, set) the text (not copied) */
#define MUIA_QLabel_Kind    (RTAG + 0x402)  /* (init) QL_* */
#define MUIA_Status_Text    (RTAG + 0x501)  /* (set) the text (not copied) */
#define MUIA_Line_Vert      (RTAG + 0x601)  /* (init) vertical */

enum { QL_LABEL, QL_NOTE, QL_TITLE, QL_TEXT };   /* QL_TEXT: several lines (\n) */

/* make all classes; 0 or -1 */
int  rp_classes_make(void);
void rp_classes_free(void);
extern struct MUI_CustomClass *nav_mcc, *head_mcc, *qbutton_mcc, *qlabel_mcc, *status_mcc, *line_mcc;

/* the page list's help bubbles follow theme_tips() */

#endif
