/*
 * Riptide - RiptidePrefs (MUI): the theme (see mui_theme.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_theme.h"

/* the light and the dark look; same order as the T_* tokens */
static const ULONG light[T_NPENS] = {
	0xF3F3F3, /* WINDOW */  0xE6E6E6, /* PANEL */   0xFFFFFF, /* HEAD */    0xC4C4C4, /* BORDER */
	0x1F1F1F, /* TEXT */    0x5F5F5F, /* DIM */     0xA0A0A0, /* DISABLED */ 0x3A3A3A, /* ICON */
	0xD8E6F2, /* HOVER */   0xC2D8EE, /* PRESSED */ 0x1F6FBF, /* ACCENT */  0xFFFFFF, /* WHITE */
	0xC8C8C8, /* LIGHT */   0xC8312B, /* RED */     0x2E8B3A, /* GREEN */   0xD9822B, /* ORANGE */
	0xE8C21E, /* YELLOW */  0xFAFAFA, /* BUTTON */
};
static const ULONG dark[T_NPENS] = {
	0x2B2B2B, /* WINDOW */  0x202020, /* PANEL */   0x333333, /* HEAD */    0x4A4A4A, /* BORDER */
	0xE8E8E8, /* TEXT */    0xA8A8A8, /* DIM */     0x6A6A6A, /* DISABLED */ 0xD0D0D0, /* ICON */
	0x3A3F45, /* HOVER */   0x2F4B66, /* PRESSED */ 0x4C9BE8, /* ACCENT */  0xFFFFFF, /* WHITE */
	0x6E6E6E, /* LIGHT */   0xE5534B, /* RED */     0x57B65F, /* GREEN */   0xE69B4C, /* ORANGE */
	0xF0D04A, /* YELLOW */  0x3C3C3C, /* BUTTON */
};

static int dark_on, tips_on = 1, compact_on;
static LONG pens[T_NPENS];
static LONG got[T_NPENS];       /* the pens obtained (released as they were), -1: none */
static struct ColorMap *pen_cm;
static struct TextFont *font;
#define MAXOBJ 48
static Object *users[MAXOBJ];
static int nusers;
static char specbuf[T_NPENS][32];

void theme_set_dark(int d)
{
	dark_on = d != 0;
}

int theme_dark(void)
{
	return dark_on;
}

void theme_set_compact(int on)
{
	compact_on = on != 0;
}

int theme_compact(void)
{
	return compact_on;
}

void theme_set_tips(int on)
{
	tips_on = on != 0;
}

int theme_tips(void)
{
	return tips_on;
}

static ULONG rgb(int t)
{
	return dark_on ? dark[t] : light[t];
}

/* 8 bit colour component c as the 32 bit value graphics.library wants */
static ULONG c32(ULONG c)
{
	c &= 0xff;
	return c << 24 | c << 16 | c << 8 | c;
}

/* nearest of MUI's own pens, for graphics.library before V39 */
static LONG mui_pen(Object *obj, int t)
{
	static const UBYTE map_light[T_NPENS] = {
		MPEN_BACKGROUND, MPEN_HALFSHINE, MPEN_SHINE, MPEN_SHADOW, MPEN_TEXT, MPEN_SHADOW, MPEN_HALFSHADOW,
		MPEN_TEXT, MPEN_HALFSHINE, MPEN_FILL, MPEN_FILL, MPEN_SHINE, MPEN_HALFSHADOW, MPEN_MARK, MPEN_FILL,
		MPEN_MARK, MPEN_MARK, MPEN_SHINE,
	};
	return (LONG)_pens(obj)[map_light[t]];
}

static void obtain_pens(Object *obj)
{
	struct Screen *s = _screen(obj);
	int i;

	pen_cm = s->ViewPort.ColorMap;
	for (i = 0; i < T_NPENS; i++) {
		ULONG c = rgb(i);
		got[i] = -1;
		pens[i] = -1;
		if (pen_cm && ((struct Library *)GfxBase)->lib_Version >= 39) {
			struct TagItem t[2];
			t[0].ti_Tag = OBP_Precision;
			t[0].ti_Data = PRECISION_GUI;
			t[1].ti_Tag = TAG_DONE;
			t[1].ti_Data = 0;
			pens[i] = got[i] = ObtainBestPenA(pen_cm, c32(c >> 16), c32(c >> 8), c32(c), t);
		}
		if (pens[i] < 0)
			pens[i] = mui_pen(obj, i);
	}
	/* few colours: lines, chosen and hovered parts must still differ from
	 * the background they are drawn on */
	if (pens[T_BORDER] == pens[T_WINDOW] || pens[T_BORDER] == pens[T_PANEL])
		pens[T_BORDER] = pens[T_DIM];
	if (pens[T_PRESSED] == pens[T_PANEL] || pens[T_PRESSED] == pens[T_BUTTON])
		pens[T_PRESSED] = pens[T_WHITE] != pens[T_PANEL] ? pens[T_WHITE] : pens[T_ACCENT];
	if (pens[T_HOVER] == pens[T_PANEL] || pens[T_HOVER] == pens[T_BUTTON])
		pens[T_HOVER] = pens[T_PRESSED];
}

static void release_pens(void)
{
	int i;

	for (i = 0; i < T_NPENS; i++) {
		if (got[i] >= 0)
			ReleasePen(pen_cm, (ULONG)got[i]);
		got[i] = -1;
	}
	pen_cm = NULL;
}

void theme_setup(Object *obj)
{
	int i;

	for (i = 0; i < nusers; i++)
		if (users[i] == obj)
			return;
	if (nusers >= MAXOBJ)
		return;
	if (nusers == 0) {
		obtain_pens(obj);
		font = _font(obj);
	}
	users[nusers++] = obj;
}

void theme_cleanup(Object *obj)
{
	int i;

	for (i = 0; i < nusers; i++)
		if (users[i] == obj)
			break;
	if (i == nusers)
		return;
	users[i] = users[--nusers];
	if (nusers == 0)
		release_pens();
}

LONG tpen(int t)
{
	return pens[t];
}

const char *theme_spec(int t)
{
	ULONG c = rgb(t);
	rt_snprintf(specbuf[t], sizeof(specbuf[t]), "2:%08lx,%08lx,%08lx", (unsigned long)c32(c >> 16),
	            (unsigned long)c32(c >> 8), (unsigned long)c32(c));
	return specbuf[t];
}

struct TextFont *theme_font(void)
{
	return font;
}

struct TextFont *theme_bold(void)
{
	return font;
}

struct TextFont *theme_big(void)
{
	return font;
}

int theme_fh(void)
{
	return font ? font->tf_YSize : 8;
}

/* ---- drawing ---- */
LONG theme_textlen(struct RastPort *rp, const char *s)
{
	s = tr(s);
	if (font)
		SetFont(rp, font);
	return TextLength(rp, (CONST_STRPTR)s, (ULONG)strlen(s));
}

LONG theme_textlen_bold(struct RastPort *rp, const char *s)
{
	return theme_textlen(rp, s) + 1;
}

void theme_text(struct RastPort *rp, LONG x, LONG y, const char *s, LONG pen, int bold)
{
	s = tr(s);
	if (font)
		SetFont(rp, font);
	SetSoftStyle(rp, bold ? FSF_BOLD : 0, FSF_BOLD);
	SetAPen(rp, (ULONG)pen);
	SetDrMd(rp, JAM1);
	Move(rp, x, y + (font ? font->tf_Baseline : 6));
	Text(rp, (CONST_STRPTR)s, (ULONG)strlen(s));
	if (bold)
		SetSoftStyle(rp, 0, FSF_BOLD);
}

LONG theme_keytextlen(struct RastPort *rp, const char *s)
{
	char t[80];
	int i = 0;

	for (s = tr(s); *s && i < (int)sizeof(t) - 1; s++)
		if (*s != '_')
			t[i++] = *s;
	t[i] = 0;
	if (font)
		SetFont(rp, font);
	return TextLength(rp, (CONST_STRPTR)t, (ULONG)i);
}

void theme_keytext(struct RastPort *rp, LONG x, LONG y, const char *s, LONG pen)
{
	char t[80];
	int i = 0, key = -1;
	LONG base = font ? font->tf_Baseline : 6;

	for (s = tr(s); *s && i < (int)sizeof(t) - 1; s++) {
		if (*s == '_' && key < 0) {
			key = i;
			continue;
		}
		t[i++] = *s;
	}
	t[i] = 0;
	if (font)
		SetFont(rp, font);
	SetSoftStyle(rp, 0, FSF_BOLD);
	SetAPen(rp, (ULONG)pen);
	SetDrMd(rp, JAM1);
	Move(rp, x, y + base);
	Text(rp, (CONST_STRPTR)t, (ULONG)i);
	if (key >= 0 && key < i) {
		/* underline the shortcut letter, one pixel under the baseline */
		LONG ux = x + TextLength(rp, (CONST_STRPTR)t, (ULONG)key);
		LONG uw = TextLength(rp, (CONST_STRPTR)t + key, 1);
		Move(rp, ux, y + base + 1);
		Draw(rp, ux + uw - 2, y + base + 1);
	}
}

void theme_rect(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen)
{
	SetAPen(rp, (ULONG)pen);
	Move(rp, x0, y0);
	Draw(rp, x1, y0);
	Draw(rp, x1, y1);
	Draw(rp, x0, y1);
	Draw(rp, x0, y0);
}

void theme_fill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen)
{
	if (x1 < x0 || y1 < y0)
		return;
	SetAPen(rp, (ULONG)pen);
	RectFill(rp, x0, y0, x1, y1);
}

void theme_line(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, LONG pen)
{
	SetAPen(rp, (ULONG)pen);
	Move(rp, x0, y0);
	Draw(rp, x1, y1);
}

/* ---- icons ----
 * Each icon is a string of drawing commands on a 16 x 16 grid (0..15,
 * left to right, top to bottom), drawn at any size:
 *   cX              colour: i icon ink, a accent, r red, g green, o orange,
 *                   y yellow, d dim, w white, l light grey, t text
 *   L x0 y0 x1 y1   line             R x0 y0 x1 y1   rectangle outline
 *   F x0 y0 x1 y1   filled box       O cx cy r       circle outline
 *   E cx cy r       filled circle    P n x0 y0 ...   polyline through n points
 * Same order as the IC_* enum (one per page); all drawn for Riptide. */
static const char *const icons[IC_NICONS] = {
	/* IFACE: a network card: the board, a chip, the socket, the contacts */
	"cl F 1 3 14 11 ci R 1 3 14 11 F 3 5 6 8 ca R 9 5 13 9 F 10 6 12 8 ci L 3 12 3 13 L 5 12 5 13 L 7 12 7 13 "
	"L 9 12 9 13 L 11 12 11 13",
	/* ADDR: a house with a front door */
	"cw F 3 8 12 14 ci P 4 1 8 8 2 14 8 1 8 R 3 8 12 14 ca F 7 10 8 14",
	/* ALIAS: a smaller house and a plus */
	"cw F 2 7 9 12 ci P 4 1 7 6 1 11 7 1 7 R 2 7 9 12 ca F 10 12 14 12 F 12 10 12 14",
	/* VLAN: one cable splitting into three coloured networks */
	"ci L 1 8 6 8 ca L 6 8 14 3 cg L 6 8 14 8 co L 6 8 14 13 ci E 6 8 1",
	/* ROUTE: a signpost: the post and two arms pointing left and right */
	"ci L 7 4 7 14 L 5 14 9 14 ca P 5 2 6 7 4 7 8 2 8 2 6 ci P 5 8 9 12 9 14 11 12 13 8 13",
	/* IP6: a "6" in a box, the box in the accent */
	"ca R 1 1 14 14 ci P 5 10 4 6 4 5 8 5 11 7 12 L 7 12 10 12 L 10 12 11 11 L 11 11 11 9 L 11 9 10 8 L 10 8 6 8",
	/* ADDR6: a smaller "6" and a plus */
	"ci P 5 7 2 4 2 3 5 3 8 5 9 L 5 9 7 9 L 7 9 8 8 L 8 8 8 6 L 8 6 7 5 L 7 5 4 5 ca F 10 12 14 12 F 12 10 12 14",
	/* NAMES: a name card: a head and lines of text */
	"cw F 1 3 14 12 ci R 1 3 14 12 O 5 7 2 L 3 11 7 11 ca L 9 6 12 6 ci L 9 8 12 8 L 9 10 11 10",
	/* HOST: a name tag on a string: the tag, its hole and a line of text */
	"cw F 3 5 14 12 ci P 5 3 5 14 5 14 12 3 12 3 5 L 1 2 3 5 O 5 8 1 ca L 8 8 12 8",
	/* TIME: a clock face with the minute hand in the accent */
	"cw E 8 8 6 ci O 8 8 6 L 8 8 8 4 ca L 8 8 11 10",
	/* TCP: data out and the acknowledgement back */
	"ci L 2 5 13 5 L 10 2 13 5 L 10 8 13 5 ca L 2 11 13 11 L 2 11 5 8 L 2 11 5 14",
	/* CONN: a plug going into a socket */
	"ci L 1 8 2 8 R 3 6 7 10 L 8 7 9 7 L 8 9 9 9 ca R 10 5 13 11 L 14 8 14 8",
	/* PORTS: a box with a row of sockets, one in use */
	"cl F 1 5 14 11 ci R 1 5 14 11 F 3 7 4 9 F 6 7 7 9 F 9 7 10 9 ca F 12 7 13 9",
	/* KA: a heartbeat line */
	"cr P 6 1 8 5 8 7 3 9 13 11 8 14 8",
	/* BG: a quiet download: a dim arrow onto a tray */
	"cd L 8 2 8 10 L 5 7 8 10 L 11 7 8 10 ci L 2 11 2 14 L 2 14 13 14 L 13 14 13 11",
	/* MEM: a memory module with three chips and its pins */
	"cl F 1 4 14 10 ci R 1 4 14 10 F 3 6 5 8 F 7 6 9 8 F 11 6 12 8 L 3 11 3 12 L 5 11 5 12 L 7 11 7 12 "
	"L 9 11 9 12 L 11 11 11 12 L 13 11 13 12",
	/* HE: a pair of eyes looking side by side */
	"cw E 5 8 3 E 11 8 3 ci O 5 8 3 O 11 8 3 ca E 6 8 1 E 12 8 1",
	/* QOS: three bands of decreasing priority */
	"ca F 2 4 5 14 ci F 7 7 9 14 cd F 11 10 13 14",
	/* QRULE: a list of rules */
	"ca F 2 3 3 4 ci F 2 7 3 8 F 2 11 3 12 L 6 4 14 4 L 6 8 14 8 L 6 12 14 12",
	/* NEIGH: two computers on one cable, the near one in the accent */
	"ci L 1 13 14 13 L 4 10 4 13 L 11 10 11 13 R 1 4 7 10 cw F 2 5 6 9 ca R 8 6 14 10 cw F 9 7 13 9",
	/* OTHER: a page of text with a folded corner */
	"cw F 3 1 12 14 ci P 6 3 1 10 1 13 4 13 14 3 14 3 1 L 10 1 10 4 L 10 4 13 4 cd L 5 7 11 7 L 5 9 11 9 "
	"L 5 11 9 11",
	/* COUNTERS: a tally: four strokes and one across them */
	"ci L 3 3 3 13 L 6 3 6 13 L 9 3 9 13 L 12 3 12 13 ca L 1 11 14 5",
	/* STATUS: a screen with a line graph going up and down */
	"cw F 2 3 13 10 ci R 1 2 14 11 L 5 14 10 14 L 7 12 7 13 ca P 5 3 9 6 6 8 8 10 4 12 6",
};

/* the pen of an icon colour letter */
static LONG icon_pen(char c, int dim)
{
	int t;

	switch (c) {
	case 'a': t = T_ACCENT; break;
	case 'r': t = T_RED; break;
	case 'g': t = T_GREEN; break;
	case 'o': t = T_ORANGE; break;
	case 'y': t = T_YELLOW; break;
	case 'd': t = T_DIM; break;
	case 'w': t = T_WHITE; break;
	case 'l': t = T_LIGHT; break;
	case 't': t = T_TEXT; break;
	default:  t = T_ICON; break;
	}
	if (dim && t != T_WHITE)
		t = T_DISABLED;
	return pens[t];
}

/* the next number of an icon string */
static int inum(const char **p)
{
	int v = 0;

	while (**p == ' ')
		(*p)++;
	while (**p >= '0' && **p <= '9')
		v = v * 10 + *(*p)++ - '0';
	return v;
}

/* grid coordinate v as a pixel: the middle of its cell */
#define GX(v) (x + ((v) * size + size / 2) / 16)
#define GY(v) (y + ((v) * size + size / 2) / 16)

/* a line w pixels thick */
static void wline(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, int w)
{
	LONG dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int k;

	for (k = 0; k < w; k++) {
		LONG ox = dx >= dy ? 0 : k, oy = dx >= dy ? k : 0;
		Move(rp, x0 + ox, y0 + oy);
		Draw(rp, x1 + ox, y1 + oy);
	}
}

/* half the width of a circle of radius r at dy rows from its centre */
static LONG half_width(LONG r, LONG dy)
{
	LONG xe = 0;

	while ((xe + 1) * (xe + 1) + dy * dy <= r * r + r)
		xe++;
	return xe;
}

/* a filled circle, or (fill = 0) a ring w pixels thick */
static void circle(struct RastPort *rp, LONG cx, LONG cy, LONG r, int fill, int w)
{
	LONG d, ri = r - w;

	for (d = -r; d <= r; d++) {
		LONG xe = half_width(r, d), xi;
		if (fill || ri < 0 || d < -ri || d > ri) {
			RectFill(rp, cx - xe, cy + d, cx + xe, cy + d);
			continue;
		}
		xi = half_width(ri, d);
		if (xe > xi) {
			RectFill(rp, cx - xe, cy + d, cx - xi - 1, cy + d);
			RectFill(rp, cx + xi + 1, cy + d, cx + xe, cy + d);
		} else {
			WritePixel(rp, cx - xe, cy + d);
			WritePixel(rp, cx + xe, cy + d);
		}
	}
}

void theme_icon(struct RastPort *rp, int icon, LONG x, LONG y, LONG size, int dim)
{
	const char *p;
	int w = size >= 24 ? 2 : 1;

	if (icon < 0 || icon >= IC_NICONS)
		return;
	SetDrMd(rp, JAM1);
	SetAPen(rp, (ULONG)icon_pen('i', dim));
	for (p = icons[icon]; *p;) {
		char c = *p++;
		int a, b, cc, d, n, i;
		LONG px = 0, py = 0;
		switch (c) {
		case 'c':
			SetAPen(rp, (ULONG)icon_pen(*p, dim));
			if (*p)
				p++;
			break;
		case 'L':
			a = inum(&p), b = inum(&p), cc = inum(&p), d = inum(&p);
			wline(rp, GX(a), GY(b), GX(cc), GY(d), w);
			break;
		case 'R':
			a = inum(&p), b = inum(&p), cc = inum(&p), d = inum(&p);
			wline(rp, GX(a), GY(b), GX(cc), GY(b), w);
			wline(rp, GX(a), GY(d) - (w - 1), GX(cc), GY(d) - (w - 1), w);
			wline(rp, GX(a), GY(b), GX(a), GY(d), w);
			wline(rp, GX(cc) - (w - 1), GY(b), GX(cc) - (w - 1), GY(d), w);
			break;
		case 'F':
			a = inum(&p), b = inum(&p), cc = inum(&p), d = inum(&p);
			RectFill(rp, x + a * size / 16, y + b * size / 16, x + (cc + 1) * size / 16 - 1,
			         y + (d + 1) * size / 16 - 1);
			break;
		case 'O':
		case 'E':
			a = inum(&p), b = inum(&p), cc = inum(&p);
			circle(rp, GX(a), GY(b), cc * size / 16, c == 'E', w);
			break;
		case 'P':
			n = inum(&p);
			for (i = 0; i < n; i++) {
				a = inum(&p), b = inum(&p);
				if (i)
					wline(rp, px, py, GX(a), GY(b), w);
				px = GX(a), py = GY(b);
			}
			break;
		default:
			break;
		}
	}
}

/* ---- help bubbles ---- */
void theme_bubble_to(char *buf, int len, const char *name, const char *body)
{
	int o, col = 0;
	const char *s;

	o = rt_snprintf(buf, (size_t)len, "\33b%s\33n\n", tr(name));
	if (o >= len)
		o = len - 1;
	for (s = tr(body); *s && o < len - 1;) {
		/* the next word: start a new line when it would pass column 46 */
		const char *e = s;
		while (*e && *e != ' ')
			e++;
		if (col && col + (e - s) >= 46) {
			buf[o++] = '\n';
			col = 0;
		} else if (col) {
			buf[o++] = ' ';
			col++;
		}
		while (s < e && o < len - 1) {
			buf[o++] = *s++;
			col++;
		}
		while (*s == ' ')
			s++;
	}
	buf[o] = 0;
}

const char *theme_bubble(const char *name, const char *body)
{
	static char buf[400];

	if (!tips_on)
		return NULL;
	theme_bubble_to(buf, sizeof(buf), name, body);
	return buf;
}
