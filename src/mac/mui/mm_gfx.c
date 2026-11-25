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
/* A font of the program's own (OpenFont, OpenDiskFont: Quill's ribbon opens its sizes and its bold
   face that way): a Mac family, size and face behind an Amiga TextFont, known by its reply port.
   Text and TextLength draw in the rastport's font; everything else in the interface font. */
static char macfont_tag;
struct mm_macfont { struct TextFont tf; short num, size, face; };
static const struct TextFont *cur_tf;
static int tf_is_mac(const struct TextFont *f)
{
	return f && f->tf_Message.mn_ReplyPort == (struct MsgPort *)&macfont_tag;
}
static const struct TextFont *cur_metrics(void)
{
	return tf_is_mac(cur_tf) ? cur_tf : &mm_font;
}

/* b: the Mac's face bits (1 bold, 2 italic, 4 underline; the classes pass 0 or 1) */
void mm_use_font(int b)
{
	if (tf_is_mac(cur_tf)) {
		const struct mm_macfont *m = (const struct mm_macfont *)cur_tf;
		TextFont(m->num);
		TextSize(m->size);
		TextFace((m->face | b) & 0x7F);
		return;
	}
	TextFont(font_num);
	TextSize(font_size);
	TextFace(b & 0x7F);
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

/* ---- colour or one bit, per window ----
 * A port draws in colour when it is a colour port (NewCWindow) on a screen of two bits or more:
 * the deepest screen the window touches decides (GetMaxDevice), as QuickDraw then maps the RGB
 * colours on each screen itself. A one-bit screen gets the dither patterns. Each window's answer
 * is kept and asked again when it opens, moves, changes size or is redrawn (a change of depth in
 * the Monitors control panel redraws every window). */
#define NPORTS 16
static struct { void *port; UBYTE mono; } ports[NPORTS];

static int gd_depth(void **gd)
{
	void **pm;
	if (!gd || !*gd)
		return 1;
	pm = *(void ***)((char *)*gd + 22);             /* gdPMap */
	return pm && *pm ? *(short *)((char *)*pm + 32) : 1;     /* pixelSize */
}

/* The deepest active screen the global rectangle r touches: the device list (DeviceList, 0x8A8)
 * walked through gdNextGD, each device's gdRect, gdFlags bit 15 for an active screen (GDevice
 * record: gdFlags 20, gdPMap 22, gdNextGD 30, gdRect 34). Read from low memory rather than through
 * GetMaxDevice, whose trap word is easy to get wrong. */
static int max_depth(const Rect *r)
{
	void **gd = *(void ***)0x8A8;
	int best = 0, guard = 0;
	while (gd && *gd && guard++ < 16) {
		char *d = (char *)*gd;
		Rect *g = (Rect *)(d + 34);
		if ((*(unsigned short *)(d + 20) & 0x8000) && g->left < r->right && g->right > r->left &&
		    g->top < r->bottom && g->bottom > r->top) {
			int k = gd_depth(gd);
			if (k > best)
				best = k;
		}
		gd = *(void ***)(d + 30);
	}
	return best ? best : gd_depth(*(void ***)0x8A4);
}

static int has_cqd = -1;
static int color_qd(void)
{
	if (has_cqd < 0) {
		long r = 0;
		has_cqd = mac_gestalt(MAC_TYPE('q', 'd', ' ', ' '), &r) == 0 && r >= 0x100;
	}
	return has_cqd;
}

static int compute_mono(void *port)
{
	void *old;
	Rect r;
	Point a, b;
	if (!color_qd() || !port || !PORT_IS_COLOR(port))
		return 1;
	GetPort(&old);
	SetPort(port);
	a.h = PORT_RECT(port)->left;
	a.v = PORT_RECT(port)->top;
	b.h = PORT_RECT(port)->right;
	b.v = PORT_RECT(port)->bottom;
	LocalToGlobal(&a);
	LocalToGlobal(&b);
	SetPort(old);
	r.left = a.h;
	r.top = a.v;
	r.right = b.h;
	r.bottom = b.v;
	return max_depth(&r) <= 1;
}

void mm_gfx_port_moved(void *port)
{
	int i, free = -1;
	for (i = 0; i < NPORTS; i++) {
		if (ports[i].port == port) {
			ports[i].mono = (UBYTE)compute_mono(port);
			return;
		}
		if (!ports[i].port && free < 0)
			free = i;
	}
	if (free >= 0) {
		ports[free].port = port;
		ports[free].mono = (UBYTE)compute_mono(port);
	}
}

void mm_gfx_port_gone(void *port)
{
	int i;
	for (i = 0; i < NPORTS; i++)
		if (ports[i].port == port)
			ports[i].port = NULL;
}

int mm_mono_port(void *port)
{
	int i;
	if (!port)
		GetPort(&port);
	for (i = 0; i < NPORTS; i++)
		if (ports[i].port == port)
			return ports[i].mono;
	return compute_mono(port);
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
	mdev = color_qd() ? *(void **)0x8A4 : NULL;      /* MainDevice */
	mm_log("font Geneva %d: height %d, baseline %d; main screen %d bit%s%s", font_size, mm_font.tf_YSize,
	       mm_font.tf_Baseline, gd_depth(mdev), gd_depth(mdev) == 1 ? "" : "s", color_qd() ? "" : " (no Color QuickDraw)");
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
	if (mm_mono_port(port)) {
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
	if (mm_mono_port(port)) {
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
	if (mm_mono_port(port)) {
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
	if (mm_mono_port(port)) {
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
				d.top = (short)(y - cur_metrics()->tf_Baseline);
				d.bottom = (short)(y + cur_metrics()->tf_YSize - cur_metrics()->tf_Baseline);
				/* a quarter of the ink taken away: half (the Mac's own grey) leaves Geneva 9 unreadable */
				PenPat(pat_ltgray);
				PenMode(11);    /* patBic (11; 14 was notPatXor, which turned disabled labels into blocks) */
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

/* the soft style as the Mac's face bits (FSF_UNDERLINED 1, FSF_BOLD 2, FSF_ITALIC 4) */
static int face_of(const struct mm_rpd *d)
{
	return ((d->style & 2) ? MAC_BOLD : 0) | ((d->style & 4) ? 2 : 0) | ((d->style & 1) ? 4 : 0);
}

WORD TextLength(struct RastPort *rp, CONST_STRPTR s, ULONG n)
{
	WORD w;
	cur_tf = RPD(rp)->font;
	w = (WORD)mm_textw((const char *)s, (int)n, face_of(RPD(rp)));
	cur_tf = NULL;
	return w;
}

LONG Text(struct RastPort *rp, CONST_STRPTR s, ULONG n)
{
	struct mm_rpd *d = RPD(rp);
	int b = face_of(d);
	int w;
	cur_tf = d->font;
	w = mm_textw((const char *)s, (int)n, b);
	if (d->port) {
		if (d->drmd & 1) {      /* JAM2: the background pen behind the text */
			Rect r;
			r.left = d->cpx;
			r.right = (short)(d->cpx + w);
			r.top = (short)(d->cpy - cur_metrics()->tf_Baseline);
			r.bottom = (short)(r.top + cur_metrics()->tf_YSize);
			mm_fill(d->port, &r, mm_pen_rgb(d->bpen));
		}
		mm_text(d->port, d->cpx, d->cpy, (const char *)s, (int)n, mm_pen_rgb(d->apen), b);
	}
	cur_tf = NULL;
	d->cpx = (WORD)(d->cpx + w);
	return 0;
}

/* ---- graphics.library beyond the MUI editor's (Quill's ribbon, panes and File page) ---- */
/* a font by name and pixel height: a Mac family (Geneva for sans serif, Times for serif, Monaco
   for fixed widths), the bitmap size whose line is nearest the height asked (not taller, where one
   is), bold when the style asks or the name ends in B ("QuillSansB.font") */
static int has(const char *s, const char *w)
{
	int n = (int)strlen(w);
	for (; *s; s++)
		if (!strncmp(s, w, (size_t)n))
			return 1;
	return 0;
}

struct TextFont *OpenDiskFont(const struct TextAttr *ta)
{
	static const short sizes[] = { 9, 10, 12, 14, 18, 20, 24, 36 };
	struct mm_macfont *m;
	char low[64];
	const char *n = ta && ta->ta_Name ? (const char *)ta->ta_Name : "";
	const char *base = n, *dot;
	FontInfo fi;
	int i, best = 0, bestd = 1000, len;
	if (!ta || !(m = mm_alloc(sizeof(*m))))
		return NULL;
	for (dot = n; *dot; dot++)
		if (*dot == '/' || *dot == ':')
			base = dot + 1;
	dot = strchr(base, '.');
	len = dot ? (int)(dot - base) : (int)strlen(base);
	for (i = 0; i < len && i < (int)sizeof(low) - 1; i++)
		low[i] = (char)(base[i] >= 'A' && base[i] <= 'Z' ? base[i] + 32 : base[i]);
	low[i] = 0;
	m->num = has(low, "mono") || has(low, "courier") || has(low, "topaz") ? 4 /* monaco */ :
	         (has(low, "serif") && !has(low, "sans")) || has(low, "times") ? 20 /* times */ : geneva;
	m->face = (ta->ta_Style & FSF_BOLD) || (len > 1 && base[len - 1] == 'B') ? MAC_BOLD : 0;
	for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
		int h, dd;
		TextFont(m->num);
		TextSize(sizes[i]);
		TextFace(m->face);
		GetFontInfo(&fi);
		h = fi.ascent + fi.descent;
		dd = h <= (int)ta->ta_YSize ? (int)ta->ta_YSize - h : 2 * (h - (int)ta->ta_YSize) + 1;
		if (dd < bestd) {
			bestd = dd;
			best = i;
		}
	}
	m->size = sizes[best];
	TextSize(m->size);
	GetFontInfo(&fi);
	mm_use_font(0);
	m->tf.tf_Message.mn_ReplyPort = (struct MsgPort *)&macfont_tag;
	m->tf.tf_YSize = (UWORD)(fi.ascent + fi.descent + fi.leading);
	m->tf.tf_Baseline = (UWORD)fi.ascent;
	m->tf.tf_XSize = (UWORD)fi.widMax;
	m->tf.tf_Style = m->face ? FSF_BOLD : 0;
	m->tf.tf_LoChar = 32;
	m->tf.tf_HiChar = 255;
	return &m->tf;
}

struct TextFont *OpenFont(const struct TextAttr *ta)
{
	return OpenDiskFont(ta);
}

void CloseFont(struct TextFont *tf)
{
	if (tf_is_mac(tf))
		mm_free(tf);
}

void TextExtent(struct RastPort *rp, CONST_STRPTR s, ULONG n, struct TextExtent *te)
{
	const struct TextFont *f = RPD(rp)->font ? RPD(rp)->font : &mm_font;
	te->te_Width = (UWORD)TextLength(rp, s, n);
	te->te_Height = f->tf_YSize;
	te->te_Extent.MinX = 0;
	te->te_Extent.MinY = (WORD)-f->tf_Baseline;
	te->te_Extent.MaxX = (WORD)(te->te_Width ? te->te_Width - 1 : 0);
	te->te_Extent.MaxY = (WORD)(f->tf_YSize - f->tf_Baseline - 1);
}

/* how many of the n characters fit in width (and height) pixels; their extent in te */
ULONG TextFit(struct RastPort *rp, CONST_STRPTR s, ULONG n, struct TextExtent *te, const struct TextExtent *cte,
              LONG dir, ULONG width, ULONG height)
{
	const struct TextFont *f = RPD(rp)->font ? RPD(rp)->font : &mm_font;
	ULONG k = n;
	(void)cte;
	(void)dir;
	if (height < f->tf_YSize)
		k = 0;
	while (k > 0 && (ULONG)TextLength(rp, s, k) > width)
		k--;
	TextExtent(rp, s, k, te);
	return k;
}

/* an ellipse's outline around cx, cy, a pixels wide and b high either side */
void DrawEllipse(struct RastPort *rp, LONG cx, LONG cy, LONG a, LONG b)
{
	struct mm_rpd *d = RPD(rp);
	Rect r;
	ULONG rgb = mm_pen_rgb(d->apen);
	if (!d->port)
		return;
	SetPort(d->port);
	r.left = (short)(cx - a);
	r.top = (short)(cy - b);
	r.right = (short)(cx + a + 1);
	r.bottom = (short)(cy + b + 1);
	if (mm_mono_port(d->port)) {
		mono_ink();
		PenPat(lum(rgb) >= 215 ? pat_white : pat_black);
	} else {
		color(rgb);
	}
	FrameOval(&r);
	PenNormal();
	ForeColor(33);
}

/* a one-bit template (each row mod bytes, from bit xsrc) drawn in the A pen where it has ones */
void BltTemplate(CONST_PLANEPTR src, LONG xsrc, LONG mod, struct RastPort *rp, LONG x, LONG y, LONG w, LONG h)
{
	struct mm_rpd *d = RPD(rp);
	struct { const void *base; short rowbytes; Rect bounds; } bm;
	Rect sr, dr;
	ULONG rgb = mm_pen_rgb(d->apen);
	if (!d->port || w <= 0 || h <= 0 || (mod & 1))
		return;
	SetPort(d->port);
	bm.base = src;
	bm.rowbytes = (short)mod;
	bm.bounds.left = 0;
	bm.bounds.top = 0;
	bm.bounds.right = (short)(mod * 8);
	bm.bounds.bottom = (short)h;
	sr.left = (short)xsrc;
	sr.top = 0;
	sr.right = (short)(xsrc + w);
	sr.bottom = (short)h;
	dr.left = (short)x;
	dr.top = (short)y;
	dr.right = (short)(x + w);
	dr.bottom = (short)(y + h);
	if (mm_mono_port(d->port))
		mono_ink();
	else
		color(rgb);
	CopyBits(&bm, (char *)d->port + 2, &sr, &dr, srcOr, NULL);
	ForeColor(33);
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
