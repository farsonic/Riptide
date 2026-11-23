/* src/mac/ipp.h - Riptide on the classic Mac OS: what ipp.c (the MacTCP interface) and drvr.S give
 * mactcp.c and each other, beyond mac.h.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_IPP_H
#define RT_MAC_IPP_H
#include "rt_types.h"
#include "rt_socket.h"

/* ipp.c */
int   ipp_control_a5(void *pb, void *dce, void *a5);    /* drvr.S: the Control entry, with the caller's A5 */
int   ipp_close(void *pb, void *dce);                    /* drvr.S: the Close entry (refused, as MacTCP does) */
void  ipp_opened(void);                                  /* drvr.S: after an Open that succeeded */
void  ipp_result(void *pb, int r);                       /* complete a call; errors logged once per kind */
void *ipp_creator_a5(void *pb);                          /* the A5 of the program that made this create call */
void  ipp_keep_a5(void *pb, void *a5);                   /* kept for ipp_creator_a5 (a create call's Control) */
int   ipp_in_poll(void);                                 /* the stack's task is running .IPP's and .IPP6's calls */
void  ipp_dm_dequeue(void *pb, void *dce);               /* a call that waits, off the Device Manager's queue (irq off) */

/* ipp6.c: .IPP6, the IPv6 interface (docs/IPP6.md), a second driver of the same stack */
int   ipp6_control_a5(void *pb, void *dce, void *a5);   /* drvr.S: .IPP6's Control entry */
int   ipp6_open(void *pb, void *dce);                    /* drvr.S: .IPP6's Open */
int   ipp6_close(void *pb, void *dce);                   /* drvr.S: .IPP6's Close (refused) */
void  ipp6_poll(void);                                   /* ipp_poll: .IPP6's calls, streams and lookups */
void  ipp6_install(void);                                /* ipp_opened, once: the unit table entry, Gestalt 'ipp6' */
void  ipp6_reap(const u32 *lo, const u32 *hi, int n);    /* reap: what programs that quit left on .IPP6 */
int   ipp6_config(int argc, char **argv, char *err, u32 errlen);   /* riptide.conf `ipp6 order ...` */

/* mactcp.c: the streams and lookups of programs that have quit (lo[i]..hi[i]: the partitions of the
   processes alive), released; called in a program's context with the core taken */
void  mactcp_reap(struct rt_sockctx *c, const u32 *lo, const u32 *hi, int n);

/* drvr.S */
short mac_next_process(void *psn);
short mac_process_info(const void *psn, void *info);
void  mac_call_asr5(void *proc, void *stream, u32 event, void *udata, u32 reason, void *icmp, int tcp, void *a5);
void  mac_call_notify(void *proc, void *pb, void *a5);  /* proc(pb), pascal or C, A0 = pb, A5 set */
short mac_newgestalt(u32 selector, void *proc);
void  mac_gestalt_mtcp(void);                            /* the 'mtcp' selector's function: 3 (MacTCP 2.0) */
void  mac_gestalt_ipp6(void);                            /* the 'ipp6' selector's function: the interface version */
short mac_drvrinstall(void *driver, short refnum);       /* _DrvrInstall */
extern char _drvr6_start[];                              /* .IPP6's driver header */

#endif
