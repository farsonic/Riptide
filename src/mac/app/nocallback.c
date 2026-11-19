/* src/mac/app/nocallback.c - ostraps.S's callbacks (pas_ctl_action, pas_useritem, pas_dlgfilter)
 * for small programs without scroll bars or dialogs (the test programs in tests/mac).
 * SPDX-License-Identifier: GPL-2.0-only */
void c_ctl_action(void *ctl, int part);
void c_useritem(void *dlg, int item);
int c_dlgfilter(void *dlg, void *ev, short *item);
void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	(void)part;
}
void c_useritem(void *dlg, int item)
{
	(void)dlg;
	(void)item;
}
int c_dlgfilter(void *dlg, void *ev, short *item)
{
	(void)dlg;
	(void)ev;
	(void)item;
	return 0;
}
