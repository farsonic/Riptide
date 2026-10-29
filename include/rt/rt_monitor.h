/*
 * Riptide - network monitoring hooks inside the stack (the in-stack kinds
 * of Roadshow's AddNetMonitorHookTagList(): MHT_ICMP, MHT_UDP,
 * MHT_TCP_Connect, MHT_Packet). The OS glue installs rt_mon_hook and
 * sets a bit in rt_mon_mask for each kind that has hooks; the protocols
 * ask it about every incoming ICMP message, UDP datagram and connection
 * request (a TCP SYN without ACK), and about every frame that is
 * received or sent. Called with the stack lock held.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MONITOR_H
#define RT_MONITOR_H

#include "rt_types.h"

#define RT_MON_ICMP        0
#define RT_MON_UDP         1
#define RT_MON_TCP_CONNECT 2
#define RT_MON_PACKET      5

/* verdicts (Roadshow MA_*) */
#define RT_MA_CONTINUE     0
#define RT_MA_IGNORE      (-1)   /* ICMP: no processing (raw sockets still see it) */
#define RT_MA_DROP        (-2)   /* as if the checksum was wrong */
#define RT_MA_DROPRESET   (-3)   /* TCP: drop and answer with a reset */

/* 1 << RT_MON_* for each kind with hooks */
extern u8 rt_mon_mask;
/* ICMP, UDP, TCP connection requests: src/dst point at the addresses
 * (network order), hdr at the protocol header. Returns a verdict; any
 * other value (positive: an errno) drops. */
extern int (*rt_mon_hook)(int type, const u32 *src, const u32 *dst, const void *hdr);
/* Frames: out 0 received / 1 to be sent; the link header is not
 * available (lh NULL, lhlen 0); data/len: the packet from its L3 header. */
extern int (*rt_mon_packet_hook)(int out, const void *lh, u32 lhlen, const void *data, u32 len);

static inline int rt_mon_on(int type)
{
	return (rt_mon_mask >> type) & 1;
}

#endif
