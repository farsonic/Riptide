/*
 * Riptide - the Mac's MUI (see mm.h): the graphics.library calls of the MUI editor on QuickDraw.
 *
 * A struct RastPort (opaque, 100 bytes, in src/amiga/os/intuition.h) holds the Mac port it draws
 * in, the pens, the drawing mode, the pen position and the font. A pen is an index into a table
 * of RGB colours: MUI's eight pens first, then whatever ObtainBestPenA asks for (the theme's
 * tokens), so the editor's theme picks its colours exactly as on an Amiga screen with a palette.
 *
 * Colour screens get RGBForeColor. A one-bit screen (the SE's) gets black, white and three dither
 * patterns for the greys in between; text is black or white, and text in a middle grey (the
 * theme's disabled and dim inks) is drawn black and then dimmed with a grey pattern, which is
 * how the Mac shows disabled text.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"
#include "prefs/mui_theme.h"

/* ---- the rastport ---- */
struct mm_rpd {
	ULONG magic;
	void *port;
	LONG apen, bpen;
	UBYTE drmd, style;
	WORD cpx, cpy;
	struct TextFont *font;
};
#define RPD(rp) ((struct mm_rpd *)(rp))
#define RP_MAGIC 0x52504D4DUL
_Static_assert(sizeof(struct mm_rpd) <= sizeof(struct RastPort), "RastPort");

void InitRastPort(struct RastPort *rp)
{
	memset(rp, 0, sizeof(*rp));
	RPD(rp)->magic = RP_MAGIC;
	RPD(rp)->apen = 1;
	RPD(rp)->font = &mm_font;
}

struct RastPort *mm_rp_new(void *port)
{
	struct RastPort *rp = mm_alloc(sizeof(*rp));
	if (rp) {
		InitRastPort(rp);
		RPD(rp)->port = port;
	}
	return rp;
}

void mm_rp_free(struct RastPort *rp)
{
	mm_free(rp);
}

void *mm_rp_port(struct RastPort *rp)
{
	return rp ? RPD(rp)->port : NULL;
}

/* ---- pens ---- */
#define NPENS 96
static ULONG pen_rgb[NPENS];
static int npens;
UWORD mm_mui_pens[8];

ULONG mm_pen_rgb(LONG pen)
{
	return pen >= 0 && pen < npens ? pen_rgb[pen] : 0;
}

static LONG pen_for(ULONG rgb)
{
	int i;
	for (i = 8; i < npens; i++)
		if (pen_rgb[i] == rgb)
			return i;
	if (npens >= NPENS)
		return 5;
	pen_rgb[npens] = rgb;
	return npens++;
}

LONG ObtainBestPenA(struct ColorMap *cm, ULONG r, ULONG g, ULONG b, const struct TagItem *tags)
{
	(void)cm;
	(void)tags;
	return pen_for(MM_RGB(r >> 24, g >> 24, b >> 24));
}

void ReleasePen(struct ColorMap *cm, ULONG pen)
{
	(void)cm;
	(void)pen;
}

void SetAPen(struct RastPort *rp, ULONG pen)
{
	RPD(rp)->apen = (LONG)pen;
}

void SetBPen(struct RastPort *rp, ULONG pen)
{
	RPD(rp)->bpen = (LONG)pen;
}

void SetDrMd(struct RastPort *rp, ULONG mode)
{
	RPD(rp)->drmd = (UBYTE)mode;
}

/* ---- fonts ---- */
struct TextFont mm_font;
static short font_num = geneva, font_size = 9;

void mm_use_font(int b)
{
	TextFont(font_num);
	TextSize(font_size);
	TextFace(b ? MAC_BOLD : 0);
}

LONG SetFont(struct RastPort *rp, const struct TextFont *f)
{
	RPD(rp)->font = f ? (struct TextFont *)f : &mm_font;
	return 0;
}

ULONG SetSoftStyle(struct RastPort *rp, ULONG style, ULONG enable)
{
	RPD(rp)->style = (UBYTE)((RPD(rp)->style & ~enable) | (style & enable));
	return RPD(rp)->style;
}

static int mono_screen = 1;
int mm_mono(void)
{
	return mono_screen;
}

void mm_gfx_init(int compact)
{
	FontInfo fi;
	void *mdev;
	int i;
	static const ULONG mui_rgb[8] = {
		0xFFFFFF, /* SHINE */ 0xE0E0E0, /* HALFSHINE */ 0xC8C8C8, /* BACKGROUND */ 0x8C8C8C, /* HALFSHADOW */
		0x000000, /* SHADOW */ 0x000000, /* TEXT */ 0x1F6FBF, /* FILL */ 0x000000, /* MARK */
	};
	for (i = 0; i < 8; i++) {
		pen_rgb[i] = mui_rgb[i];
		mm_mui_pens[i] = (UWORD)i;
	}
	npens = 8;
	font_size = compact ? 9 : 10;
	mm_use_font(0);
	GetFontInfo(&fi);
	mm_font.tf_YSize = (UWORD)(fi.ascent + fi.descent + fi.leading);
	mm_font.tf_Baseline = (UWORD)fi.ascent;
	mm_font.tf_XSize = (UWORD)fi.widMax;
	/* the main screen's depth: MainDevice -> gdPMap -> pixelSize */
	mdev = *(void **)0x8A4;
	mono_screen = 1;
	if (mdev && *(void **)mdev) {
		char *gd = *(char **)mdev;
		void **pm = *(void ***)(gd + 22);
		if (pm && *pm)
			mono_screen = *(short *)((char *)*pm + 32) <= 1;
	}
	mm_log("font Geneva %d: height %d, baseline %d; %s screen", font_size, mm_font.tf_YSize, mm_font.tf_Baseline,
	       mono_screen ? "one-bit" : "colour");
}

/* ---- drawing in an RGB colour ---- */
static const UBYTE pat_white[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const UBYTE pat_tint[8] = { 0x80, 0x00, 0x08, 0x00, 0x80, 0x00, 0x08, 0x00 };
static const UBYTE pat_ltgray[8] = { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 };
static const UBYTE pat_gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
static const UBYTE pat_dkgray[8] = { 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD };
static const UBYTE pat_black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int lum(ULONG rgb)
{
	return (int)((((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 + (rgb & 0xFF) * 11) / 100);
}

static void color(ULONG rgb)
{
	RGBColor c;
	c.red = (unsigned short)(((rgb >> 16) & 0xFF) * 257);
	c.green = (unsigned short)(((rgb >> 8) & 0xFF) * 257);
	c.blue = (unsigned short)((rgb & 0xFF) * 257);
	RGBForeColor(&c);
}

static void mono_ink(void)
{
	ForeColor(33);      /* blackColor */
	BackColor(30);      /* whiteColor */
}

void mm_fill(void *port, const Rect *r, ULONG rgb)
{
	if (port)
		SetPort(port);
	if (r->right <= r->left || r->bottom <= r->top)
		return;
	if (mono_screen) {
		int l = lum(rgb);
		mono_ink();
		/* a line drawn as a box one pixel thin (the classes' separators): solid, as mm_line draws */
		if (r->bottom - r->top == 1 || r->right - r->left == 1) {
			FillRect(r, l >= 215 ? pat_white : pat_black);
			return;
		}
		/* light tints (the hover and chosen colours) only faintly, so text on them stays clear */
		FillRect(r, l >= 229 ? pat_white : l >= 195 ? pat_tint : l >= 150 ? pat_ltgray : l >= 100 ? pat_gray :
		         l >= 40 ? pat_dkgray : pat_black);
		return;
	}
	color(rgb);
	PaintRect(r);
	ForeColor(33);
}

void mm_frame(void *port, const Rect *r, ULONG rgb)
{
	if (port)
		SetPort(port);
	if (mono_screen) {
		mono_ink();
		PenPat(lum(rgb) >= 215 ? pat_white : pat_black);
		FrameRect(r);
		PenNormal();
		return;
	}
	color(rgb);
	FrameRect(r);
	ForeColor(33);
}

void mm_line(void *port, int x0, int y0, int x1, int y1, ULONG rgb)
{
	if (port)
		SetPort(port);
	if (mono_screen) {
		mono_ink();
		PenPat(lum(rgb) >= 215 ? pat_white : pat_black);
	} else {
		color(rgb);
	}
	MoveTo(x0, y0);
	LineTo(x1, y1);
	PenNormal();
	ForeColor(33);
}

int mm_textw(const char *s, int n, int b)
{
	mm_use_font(b);
	return TextWidth(s, 0, n);
}

/* text with its baseline at y */
void mm_text(void *port, int x, int y, const char *s, int n, ULONG rgb, int b)
{
	if (port)
		SetPort(port);
	mm_use_font(b);
	if (mono_screen) {
		int l = lum(rgb);
		mono_ink();
		if (l >= 215) {
			TextMode(3);            /* srcBic: white on a dark ground */
			MoveTo(x, y);
			DrawText(s, 0, n);
		} else {
			TextMode(srcOr);
			MoveTo(x, y);
			DrawText(s, 0, n);
			if (l >= 110) {
				/* a middle grey: dimmed with a grey pattern, as the Mac shows disabled text */
				Rect d;
				d.left = (short)x;
				d.right = (short)(x + TextWidth(s, 0, n));
				d.top = (short)(y - mm_font.tf_Baseline);
				d.bottom = (short)(y + mm_font.tf_YSize - mm_font.tf_Baseline);
				/* a quarter of the ink taken away: half (the Mac's own grey) leaves Geneva 9 unreadable */
				PenPat(pat_ltgray);
				PenMode(14);    /* patBic */
				PaintRect(&d);
				PenNormal();
			}
		}
		TextMode(srcOr);
		return;
	}
	color(rgb);
	TextMode(srcOr);
	MoveTo(x, y);
	DrawText(s, 0, n);
	ForeColor(33);
}

/* ---- graphics.library ---- */
static struct Library gfx_lib;
struct GfxBase *GfxBase = (struct GfxBase *)&gfx_lib;

void mm_gfx_lib_init(void)
{
	gfx_lib.lib_Version = 40;
}

void Move(struct RastPort *rp, LONG x, LONG y)
{
	RPD(rp)->cpx = (WORD)x;
	RPD(rp)->cpy = (WORD)y;
}

void Draw(struct RastPort *rp, LONG x, LONG y)
{
	struct mm_rpd *d = RPD(rp);
	if (d->port)
		mm_line(d->port, d->cpx, d->cpy, (int)x, (int)y, mm_pen_rgb(d->apen));
	d->cpx = (WORD)x;
	d->cpy = (WORD)y;
}

WORD TextLength(struct RastPort *rp, CONST_STRPTR s, ULONG n)
{
	return (WORD)mm_textw((const char *)s, (int)n, (RPD(rp)->style & FSF_BOLD) != 0);
}

LONG Text(struct RastPort *rp, CONST_STRPTR s, ULONG n)
{
	struct mm_rpd *d = RPD(rp);
	int b = (d->style & FSF_BOLD) != 0;
	int w = mm_textw((const char *)s, (int)n, b);
	if (d->port) {
		if (d->drmd & 1) {      /* JAM2: the background pen behind the text */
			Rect r;
			r.left = d->cpx;
			r.right = (short)(d->cpx + w);
			r.top = (short)(d->cpy - mm_font.tf_Baseline);
			r.bottom = (short)(r.top + mm_font.tf_YSize);
			mm_fill(d->port, &r, mm_pen_rgb(d->bpen));
		}
		mm_text(d->port, d->cpx, d->cpy, (const char *)s, (int)n, mm_pen_rgb(d->apen), b);
	}
	d->cpx = (WORD)(d->cpx + w);
	return 0;
}

void RectFill(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1)
{
	Rect r;
	if (!RPD(rp)->port || x1 < x0 || y1 < y0)
		return;
	r.left = (short)x0;
	r.top = (short)y0;
	r.right = (short)(x1 + 1);
	r.bottom = (short)(y1 + 1);
	mm_fill(RPD(rp)->port, &r, mm_pen_rgb(RPD(rp)->apen));
}

LONG WritePixel(struct RastPort *rp, LONG x, LONG y)
{
	RectFill(rp, x, y, x, y);
	return 0;
}

/* ScrollRaster: the box moved by -dx,-dy; what it leaves is filled with the background pen */
void ScrollRaster(struct RastPort *rp, LONG dx, LONG dy, LONG x0, LONG y0, LONG x1, LONG y1)
{
	struct mm_rpd *d = RPD(rp);
	Rect r, v;
	void *rgn;
	if (!d->port)
		return;
	SetPort(d->port);
	r.left = (short)x0;
	r.top = (short)y0;
	r.right = (short)(x1 + 1);
	r.bottom = (short)(y1 + 1);
	rgn = NewRgn();
	ScrollRect(&r, (int)-dx, (int)-dy, rgn);
	DisposeRgn(rgn);
	v = r;
	if (dx > 0)
		v.left = (short)(r.right - dx);
	else if (dx < 0)
		v.right = (short)(r.left - dx);
	if (dy > 0)
		v.top = (short)(r.bottom - dy);
	else if (dy < 0)
		v.bottom = (short)(r.top - dy);
	if (dx || dy)
		mm_fill(d->port, &v, mm_pen_rgb(d->bpen));
}

/* the theme's colour of token t (for the built-in classes) */
ULONG mm_theme_rgb(int t)
{
	return mm_pen_rgb(tpen(t));
}
