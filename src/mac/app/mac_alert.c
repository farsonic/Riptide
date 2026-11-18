/*
 * Riptide - the classic Mac OS application kit: an alert box as the Macintosh Human Interface
 * Guidelines (Apple, 1992) draw it, built at run time, so programs without resources (everything
 * here is packed from ELF files, tools/elf2app.py) still get the standard one.
 *
 *   - the alert icon at the top left: stop, note or caution (Guidelines p. 192-196, the
 *     System's 'ICON' 0, 1 and 2);
 *   - the message to its right in the system font, wrapped (p. 197, Figure 6-21);
 *   - the buttons at the bottom right, in the order the caller gives, the action rightmost and
 *     Cancel to its left (p. 196); 20 pixels high, 59 wide at least, wider for longer names
 *     (p. 204-205);
 *   - the default button with the heavy rounded outline: three black pixels, one white, around it
 *     (p. 206), answering Return and Enter; Cancel answering Escape and Command-period, each
 *     button flashing for eight ticks when its key is used (p. 205-206);
 *   - placed a third of the way down the main screen, centred across it (the alert position);
 *   - the program's own windows behind it redrawn while it is up (mac_alert_update, which a
 *     program may supply: the Mac MUI layer does, src/mac/mui/mm_app.c).
 *
 * MUI_RequestA (src/mac/mui/mm_os.c) and the Riptide Terminal's About box are built on it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <string.h>
#include "toolbox.h"

void pas_useritem(void), pas_dlgfilter(void);

/* the program's windows behind the alert: nothing to do unless the program says otherwise */
__attribute__((weak)) void mac_alert_update(void *event) { (void)event; }

static int al_def = -1, al_cancel = -1, al_outline;

void c_useritem(void *dlg, int item)
{
	short kind;
	void *h;
	Rect box;
	GetDItem(dlg, item, &kind, &h, &box);
	PenSize(3, 3);
	FrameRoundRect(&box, 16, 16);
	PenNormal();
}

static void flash(void *dlg, int item)
{
	short kind;
	void *h;
	Rect box;
	unsigned long t;
	GetDItem(dlg, item, &kind, &h, &box);
	HiliteControl(h, 1);
	t = LM_TICKS + 8;
	while (LM_TICKS < t)
		;
	HiliteControl(h, 0);
}

int c_dlgfilter(void *dlg, EventRecord *ev, short *item)
{
	if (ev->what == updateEvt && (void *)ev->message != dlg) {
		mac_alert_update(ev);
		return 0;
	}
	if (ev->what == keyDown || ev->what == autoKey) {
		int c = (int)(ev->message & 0xFF);
		if ((c == '\r' || c == 3) && al_def > 0) {
			flash(dlg, al_def);
			*item = (short)al_def;
			return 1;
		}
		if ((c == 0x1B || ((ev->modifiers & cmdKey) && c == '.')) && al_cancel > 0) {
			flash(dlg, al_cancel);
			*item = (short)al_cancel;
			return 1;
		}
		if (c == '\r' || c == 3 || c == 0x1B) {
			ev->what = nullEvent;
			return 0;
		}
	}
	return 0;
}

/* one DITL item: placeholder, rectangle, kind, data */
static unsigned char *item(unsigned char *q, int t, int l, int b, int r, int kind, const void *data, int len)
{
	short v[4];
	int i;
	v[0] = (short)t; v[1] = (short)l; v[2] = (short)b; v[3] = (short)r;
	for (i = 0; i < 4; i++)
		*q++ = 0;
	for (i = 0; i < 4; i++) {
		*q++ = (unsigned char)(v[i] >> 8);
		*q++ = (unsigned char)v[i];
	}
	*q++ = (unsigned char)kind;
	*q++ = (unsigned char)len;
	for (i = 0; i < len; i++)
		*q++ = ((const unsigned char *)data)[i];
	if (len & 1)
		*q++ = 0;
	return q;
}

/* the lines TextBox will make of text in width pixels (the current port's font) */
static int lines_of(const char *t, int n, int width)
{
	int lines = 1, x = 0, i = 0, space = TextWidth(" ", 0, 1);
	while (i < n) {
		int j = i, w;
		if (t[i] == '\r') {
			lines++;
			x = 0;
			i++;
			continue;
		}
		while (j < n && t[j] != ' ' && t[j] != '\r')
			j++;
		w = TextWidth(t, (short)i, (short)(j - i));
		if (x && x + w > width) {
			lines++;
			x = 0;
		}
		while (w > width) {             /* a word longer than the line */
			lines++;
			w -= width;
		}
		x += w;
		if (j < n && t[j] == ' ') {
			x += space;
			j++;
		}
		i = j;
	}
	return lines;
}

int mac_alert(int icon, const char *text, const char *const *btn, int n, int def, int cancel)
{
	static char msg[1020];
	unsigned char pb[4][256], title[2] = { 0, 0 };
	void *wm, *save, *dlg, **items;
	unsigned char *q;
	int bw[6], i, len = 0, W = 360, textw, lines, top, H, x, nitems;
	short kind, hit = 0;
	void *h;
	Rect r, box;
	if (n < 1)
		return -1;
	if (n > 6)
		n = 6;
	while (text && text[len] && len < (int)sizeof msg - 1) {
		msg[len] = text[len] == '\n' ? '\r' : text[len];
		len++;
	}
	while (len && msg[len - 1] == '\r')
		len--;
	/* the sizes, in the system font (the Window Manager's port has it) */
	GetPort(&save);
	GetWMgrPort(&wm);
	SetPort(wm);
	{
		/* the dialog's font: the system font, 12 point (the port's own settings put back after) */
		short f = *(short *)((char *)wm + 68), z = *(short *)((char *)wm + 74);
		TextFont(0);
		TextSize(12);
		textw = W - 74 - 13;
		lines = lines_of(msg, len, textw) + 1;     /* TextBox breaks a little sooner than this count */
		for (i = 0; i < n; i++) {
			unsigned char s[256];
			mac_pstr(s, btn[i]);
			bw[i] = StringWidth(s) + 20;
			if (bw[i] < 59)
				bw[i] = 59;
		}
		TextFont(f);
		TextSize(z);
	}
	SetPort(save);
	top = 13 + (lines * 16 > 32 ? lines * 16 : 32) + 13;
	H = top + 20 + 13;
	/* the items: the buttons (1..n), the message, the icon, the default button's outline */
	items = mac_newhandle(64 + n * 24 + 40 + 24 + 24);
	if (!items)
		return -1;
	q = (unsigned char *)*items;
	nitems = n + 2 + (def >= 0 ? 1 : 0);
	*q++ = (unsigned char)((nitems - 1) >> 8);
	*q++ = (unsigned char)(nitems - 1);
	x = W - 13;
	{
		int right[6];
		for (i = n - 1; i >= 0; i--) {
			right[i] = x;
			x -= bw[i] + 12;
		}
		for (i = 0; i < n; i++) {
			unsigned char s[256];
			mac_pstr(s, btn[i]);
			q = item(q, top, right[i] - bw[i], top + 20, right[i], 4, s + 1, s[0]);
		}
		q = item(q, 13, 74, top - 13 + 3, W - 13, 8 | 0x80, "^0^1^2^3", 8);
		{
			unsigned char id[2] = { 0, (unsigned char)(icon < 0 ? 0 : icon > 2 ? 2 : icon) };
			q = item(q, 13, 23, 45, 55, 32 | 0x80, id, 2);
		}
		if (def >= 0)
			q = item(q, top - 4, right[def] - bw[def] - 4, top + 24, right[def] + 4, 0 | 0x80, "", 0);
	}
	/* the message in four pieces of up to 255 characters (ParamText's ^0 to ^3) */
	for (i = 0; i < 4; i++) {
		int from = i * 255, k = len - from;
		if (k < 0)
			k = 0;
		if (k > 255)
			k = 255;
		pb[i][0] = (unsigned char)k;
		memcpy(pb[i] + 1, msg + from, (unsigned long)k);
	}
	ParamText(pb[0], pb[1], pb[2], pb[3]);
	/* a third of the way down the main screen, under the menu bar */
	{
		Rect *pr = PORT_RECT(wm);
		int avail = pr->bottom - LM_MBARHEIGHT - H;
		r.left = (short)((pr->right - pr->left - W) / 2);
		r.right = (short)(r.left + W);
		r.top = (short)(LM_MBARHEIGHT + (avail > 0 ? avail / 3 : 0));
		r.bottom = (short)(r.top + H);
	}
	dlg = NewDialog(NULL, &r, title, 1, 1, (void *)-1L, 0, 0, items);
	if (!dlg)
		return -1;
	al_def = def >= 0 ? def + 1 : -1;
	al_cancel = cancel >= 0 ? cancel + 1 : -1;
	al_outline = def >= 0 ? n + 3 : -1;
	if (al_outline > 0) {
		GetDItem(dlg, al_outline, &kind, &h, &box);
		SetDItem(dlg, al_outline, kind, (void *)pas_useritem, &box);
	}
	InitCursor();
	do
		ModalDialog((void *)pas_dlgfilter, &hit);
	while (hit < 1 || hit > n);
	DisposDialog(dlg);
	SetPort(save);
	al_def = al_cancel = al_outline = -1;
	return hit - 1;
}
