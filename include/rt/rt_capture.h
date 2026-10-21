/*
 * Riptide - packet capture into a RAM ring, saved as a pcap file
 * (src/core/capture.c).
 *
 * Frames are taken where they meet the driver: rt_netif_input() for
 * received frames and rt_netif_xmit() for sent ones (loopback only on
 * delivery, so every lo0 packet appears once). The file uses the Linux
 * "cooked" link type (LINKTYPE_LINUX_SLL, 113): one file can mix
 * Ethernet, point-to-point and loopback interfaces, keeps the direction
 * and the sender's link address, and carries ARP as well as IPv4/IPv6.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_CAPTURE_H
#define RT_CAPTURE_H

#include "rt_types.h"

struct rt_netif;
struct rt_buf;

#define RT_CAP_IN  0
#define RT_CAP_OUT 1

/* Nonzero while frames are being recorded: the only cost when off. */
extern u8 rt_capture_on;

/* Record one frame (b->data at the L3 header). Lock held. srchw: the
 * sender's link address or NULL; flags: RT_BUFF_BCAST/MCAST. */
void rt_capture_frame(struct rt_netif *nif, const struct rt_buf *b, int dir,
                      u16 ethertype, const u8 *srchw, u16 flags);

/* "capture start|stop|status|save|clear ..." (argv[0] == "capture").
 * Lock held; "save" drops it while writing the file. Text for the user
 * (status, or the error) goes to out. Returns 0 or -errno. */
int rt_capture_command(int argc, char **argv, char *out, u32 outlen);

/* Stack shutdown: save to the file given at start (if any) and free the
 * ring. Lock held or no other users. */
void rt_capture_shutdown(void);

#endif
