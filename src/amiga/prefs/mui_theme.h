/*
 * Riptide - RiptidePrefs (MUI): the theme. Every class of the MUI editor
 * draws through these functions and never picks a colour or font itself.
 *
 * Colours are tokens (T_*) with one RGB value in a light and one in a
 * dark table; theme_setup() obtains a pen for each on the window's screen
 * (ObtainBestPen, so palette screens share or approximate them) and the
 * last theme_cleanup() gives them back. Fonts: the screen's font through
 * theme_font() and friends, so an own interface font can be added later
 * without touching the classes. Icons are drawn in code from short vector
 * descriptions on a 16 x 16 grid (theme_icon()).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_THEME_H
#define RT_PREFS_MUI_THEME_H

#include "prefs/mui.h"

enum {
	T_WINDOW,       /* the window's background */
	T_PANEL,        /* the page list and the button bar */
	T_HEAD,         /* the page head */
	T_BORDER,       /* separator lines, button outlines */
	T_TEXT,
	T_DIM,          /* secondary text */
	T_DISABLED,
	T_ICON,         /* the ink of icons */
	T_HOVER,        /* under the pointer */
	T_PRESSED,      /* chosen, or held down */
	T_ACCENT,       /* the one thing that is on */
	T_WHITE,        /* paper inside icons */
	T_LIGHT,        /* light grey parts of icons */
	T_RED, T_GREEN, T_ORANGE, T_YELLOW,
	T_BUTTON,       /* button face */
	T_NPENS
};

/* the light or dark table; theme_set_dark() before the window opens, or
 * between closing and reopening it */
void theme_set_dark(int dark);
int  theme_dark(void);
/* the compact layout for short screens (under 400 lines) */
void theme_set_compact(int on);
int  theme_compact(void);
/* help bubbles on or off (theme_bubble() returns NULL when off) */
void theme_set_tips(int on);
int  theme_tips(void);

/* pens for obj's screen: call in MUIM_Setup / MUIM_Cleanup of every class
 * (counts objects; MUI may send either twice) */
void theme_setup(Object *obj);
void theme_cleanup(Object *obj);
LONG tpen(int token);
/* "2:rrrrrrrr,gggggggg,bbbbbbbb", for MUIA_Background */
const char *theme_spec(int token);

struct TextFont *theme_font(void);
struct TextFont *theme_bold(void);     /* drawn with the bold soft style */
struct TextFont *theme_big(void);
int  theme_fh(void);                   /* the font's height */

/* drawing; rectangles are inclusive, text is placed by its top edge and
 * passes through tr() */
LONG theme_textlen(struct RastPort *rp, const char *s);
LONG theme_textlen_bold(struct RastPort *rp, const char *s);
void theme_text(struct RastPort *rp, LONG x, LONG y, const char *s, LONG pen, int bold);
/* text with the letter after '_' underlined (the '_' is not drawn) */
void theme_keytext(struct RastPort *rp, LONG x, LONG y, const char *s, LONG pen);
LONG theme_keytextlen(struct RastPort *rp, const char *s);
void theme_rect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen);
void theme_fill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen);
void theme_line(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen);

/* icons (one per page, IC_ + page number, and a few more) */
enum {
	IC_IFACE, IC_ADDR, IC_ALIAS, IC_VLAN, IC_NAMES, IC_TIME, IC_TCP, IC_CONN, IC_PORTS, IC_KA, IC_BG,
	IC_MEM, IC_HE, IC_QOS, IC_QRULE, IC_OTHER, IC_COUNTERS, IC_STATUS,
	IC_NICONS
};
/* icon at x,y, size pixels square; dim: everything but white in T_DISABLED */
void theme_icon(struct RastPort *rp, int icon, LONG x, LONG y, LONG size, int dim);

/* "\33bName\33n\nbody wrapped at 46" in a static buffer (one bubble at a
 * time), or NULL with bubbles off */
const char *theme_bubble(const char *name, const char *body);
/* the same into buf, for texts that stay with an object */
void theme_bubble_to(char *buf, int len, const char *name, const char *body);

/* translation (English only so far; see mui_lang.c) */
const char *tr(const char *s);

#endif
