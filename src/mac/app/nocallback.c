/* src/mac/app/nocallback.c - the control action callback (ostraps.S pas_ctl_action) for programs
 * without scroll bars. SPDX-License-Identifier: GPL-2.0-only */
void c_ctl_action(void *ctl, int part);
void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	(void)part;
}
