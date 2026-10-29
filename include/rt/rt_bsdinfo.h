/*
 * Riptide - the 4.4BSD binary tables of the Roadshow bsdsocket.library
 * API (src/core/bsdinfo.c): the routing table as routing socket messages
 * (GetRouteInfo()) and the protocol statistics (GetNetworkStatistics()).
 *
 * The layouts are those of the Amiga ABI (68k alignment: 2 bytes for
 * 32-bit fields); numbers are in host order, addresses and ports in
 * network order, as the Roadshow SDK documents them. Riptide counts what
 * the Linux SNMP MIB counts, so a BSD counter Riptide does not keep is 0
 * and some are derived from the nearest Riptide counter (see bsdinfo.c).
 * The stack lock must be held.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_BSDINFO_H
#define RT_BSDINFO_H

#include "rt_types.h"

/* struct rt_msghdr (net/route.h, version 3) */
#define RT_RTM_VERSION  3
#define RT_RTM_GET      4
#define RT_RTM_HDRLEN   74       /* sizeof(struct rt_msghdr) on the Amiga */
#define RT_RTM_OFS_MSGLEN  0     /* UWORD */
#define RT_RTM_OFS_VERSION 2     /* UBYTE */
#define RT_RTM_OFS_TYPE    3     /* UBYTE */
#define RT_RTM_OFS_INDEX   4     /* UWORD */
#define RT_RTM_OFS_FLAGS   6     /* LONG */
#define RT_RTM_OFS_ADDRS   10    /* LONG: RTA_* */
#define RT_RTM_OFS_PID     14
#define RT_RTM_OFS_SEQ     18
#define RT_RTM_OFS_ERRNO   22
#define RT_RTM_OFS_USE     26
#define RT_RTM_OFS_INITS   30
#define RT_RTM_OFS_RMX     34    /* struct rt_metrics: 10 ULONGs */
#define RT_RMX_MTU      1        /* index in rt_metrics */
#define RT_RMX_HOPCOUNT 2
#define RT_RMX_EXPIRE   3
#define RT_RMX_PKSENT   9
#define RT_RTA_DST      0x01
#define RT_RTA_GATEWAY  0x02
#define RT_RTA_NETMASK  0x04
#define RT_RTA_IFA      0x20

/* The IPv4 routes whose flags (BSD RTF_*) include all of 'flags', one
 * message each (header, then a struct sockaddr_in for each RTA_* bit in
 * rtm_addrs: destination, gateway if any, netmask unless a host route,
 * the interface address), then a terminating header with msglen 0.
 * Fills at most len bytes of buf (buf NULL: nothing) and returns the
 * size of the whole table. */
u32 rt_bsd_route_dump(u32 flags, u8 *buf, u32 len);

/* GetNetworkStatistics() types (NETSTATUS_*) */
#define RT_NETSTATUS_ICMP        0
#define RT_NETSTATUS_IGMP        1
#define RT_NETSTATUS_IP          2
#define RT_NETSTATUS_MB          3
#define RT_NETSTATUS_MRT         4
#define RT_NETSTATUS_RT          5
#define RT_NETSTATUS_TCP         6
#define RT_NETSTATUS_UDP         7
#define RT_NETSTATUS_TCP_SOCKETS 9
#define RT_NETSTATUS_UDP_SOCKETS 10
#define RT_NETWORKSTATUS_VERSION 1

/* sizes of the structures */
#define RT_BSD_ICMPSTAT_SIZE 184
#define RT_BSD_IGMPSTAT_SIZE 36
#define RT_BSD_IPSTAT_SIZE   96
#define RT_BSD_MBSTAT_SIZE   28
#define RT_BSD_MRTSTAT_SIZE  32
#define RT_BSD_RTSTAT_SIZE   10
#define RT_BSD_TCPSTAT_SIZE  196
#define RT_BSD_UDPSTAT_SIZE  36
#define RT_BSD_PCD_SIZE      24  /* struct protocol_connection_data */

/* Copy statistics of 'type' into buf (at most len bytes; buf NULL:
 * nothing). Returns the size of the complete data, or -RT_EINVAL for an
 * unknown type. */
int rt_bsd_netstat(int type, u8 *buf, u32 len);

#endif
