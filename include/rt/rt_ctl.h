/*
 * Riptide - control and monitoring interface for network tools (ip,
 * ifconfig, route, arp, netstat, ...).
 *
 * Table dumps are read-only OPAQUE sysctls. Each one is reachable by name
 * ("net.riptide.ifaces") and by the numeric MIB {CTL_NET, RT_CTL_MIB_RIPTIDE,
 * RT_CTL_*}, so AmigaOS programs can fetch them with RiptideSysCtl()
 * (riptide.library, LVO -30) using BSD sysctl(3) semantics:
 *
 *   1. call with old == NULL: *oldlen is set to the current size;
 *   2. allocate a little more (tables can grow meanwhile) and call again;
 *   3. -1 with errno ENOMEM means the table grew past the buffer: the
 *      buffer holds a valid, shorter dump (whole records only, header
 *      count adjusted) and *oldlen the size needed; retry with more.
 *
 * Every dump starts with struct rt_ctl_hdr followed by hdr.count records
 * of hdr.recsize bytes each. Readers must step by hdr.recsize (records may
 * grow at the end in later versions) and check hdr.version.
 *
 * Records contain only u8/u16/u32 fields and char arrays, no pointers and
 * no padding. Byte order: IPv4 addresses and netmasks are in NETWORK
 * order, everything else (counters, flags, ports, times) in HOST order.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_CTL_H
#define RT_CTL_H

#include "rt_types.h"

#define RT_CTL_MIB_RIPTIDE 255    /* {4, 255, n}: Riptide private subtree */
#define RT_CTL_VERSION     1

/* third MIB component and sysctl names */
#define RT_CTL_IFACES    1        /* net.riptide.ifaces    struct rt_ctl_iface[]  */
#define RT_CTL_ROUTES    2        /* net.riptide.routes    struct rt_ctl_route[]  */
#define RT_CTL_NEIGH     3        /* net.riptide.neigh     struct rt_ctl_neigh[]  */
#define RT_CTL_SOCKETS   4        /* net.riptide.sockets   struct rt_ctl_sock[]   */
#define RT_CTL_DNS       5        /* net.riptide.dns       one struct rt_ctl_dns  */
#define RT_CTL_IPSTAT    6        /* net.riptide.ipstat    one struct rt_ip_stats   */
#define RT_CTL_ICMPSTAT  7        /* net.riptide.icmpstat  one struct rt_icmp_stats */
#define RT_CTL_UDPSTAT   8        /* net.riptide.udpstat   one struct rt_udp_stats  */
#define RT_CTL_TCPSTAT   9        /* net.riptide.tcpstat   one struct rt_tcp_stats  */
/* IPv6 (new record types; the IPv4 tables above are unchanged and list
 * AF_INET sockets only) */
#define RT_CTL_ADDRS6    10       /* net.riptide.addrs6    struct rt_ctl_addr6[]  */
#define RT_CTL_ROUTES6   11       /* net.riptide.routes6   struct rt_ctl_route6[] */
#define RT_CTL_NEIGH6    12       /* net.riptide.neigh6    struct rt_ctl_neigh6[] */
#define RT_CTL_SOCKETS6  13       /* net.riptide.sockets6  struct rt_ctl_sock6[] (AF_INET6 sockets) */
#define RT_CTL_DNS6      14       /* net.riptide.dns6      one struct rt_ctl_dns6 */
#define RT_CTL_IP6STAT   15       /* net.riptide.ip6stat   one struct rt_ip6_stats   */
#define RT_CTL_ICMP6STAT 16       /* net.riptide.icmp6stat one struct rt_icmp6_stats */
#define RT_CTL_IFACES6   17       /* net.riptide.ifaces6   struct rt_ctl_iface6[] */
#define RT_CTL_PROFILE   19       /* net.riptide.profile   struct rt_ctl_prof[] ("profile") */

struct rt_ctl_hdr {
	u16 version;              /* RT_CTL_VERSION */
	u16 recsize;              /* size of one record */
	u32 count;                /* number of records that follow */
};

/* ---- interfaces ---- */
#define RT_CTL_DESCRLEN 48

/* DHCP client state (rt_ctl_iface.dhcp_state) */
#define RT_DHCPS_NONE        0    /* no DHCP client on this interface */
#define RT_DHCPS_INIT        1
#define RT_DHCPS_SELECTING   2
#define RT_DHCPS_REQUESTING  3
#define RT_DHCPS_BOUND       4
#define RT_DHCPS_RENEWING    5
#define RT_DHCPS_REBINDING   6

struct rt_ctl_iface {
	char name[16];
	u32  index;
	u16  flags;               /* RT_IFF_* (BSD IFF_* values) */
	u8   type;                /* RT_IFT_* */
	u8   hwlen;
	u16  mtu;
	u16  dhcp_state;          /* RT_DHCPS_* */
	u8   hwaddr[16];
	u32  addr, netmask;       /* network order, 0 = none */
	u32  broadaddr, dstaddr;  /* network order */
	u32  metric;
	/* struct rt_netif_stats */
	u32  ipackets, ierrors, opackets, oerrors, collisions;
	u32  ibytes, obytes, imcasts, omcasts, iqdrops, noproto;
	/* DHCP lease (valid when dhcp_state >= BOUND) */
	u32  dhcp_server;         /* network order */
	u32  dhcp_router;         /* network order */
	u32  dhcp_lease_s;        /* lease time, 0xffffffff = infinite */
	u32  dhcp_left_s;         /* seconds until the lease expires */
	char descr[RT_CTL_DESCRLEN]; /* driver, e.g. "uaenet.device unit 0" */
};

/* ---- routes ---- */
/* rt_ctl_route.proto (Linux RTPROT_* values) */
#define RT_PROTO_REDIRECT 1
#define RT_PROTO_KERNEL   2       /* connected (interface) routes, PMTU cache */
#define RT_PROTO_BOOT     3
#define RT_PROTO_STATIC   4
#define RT_PROTO_DHCP     16

struct rt_ctl_route {
	u32  dst, mask, gateway;  /* network order; gateway 0 = direct */
	u32  ifindex;             /* 0 = none (reject routes) */
	u32  flags;               /* RT_RTF_* (BSD RTF_* values plus RT_RTF_CONNECTED/PMTU/DHCP) */
	u32  metric;
	u32  mtu;                 /* 0 = interface MTU */
	u32  use;
	u32  expire_s;            /* seconds left, 0 = never */
	u8   proto;               /* RT_PROTO_* */
	u8   linkdown;            /* interface is down: route not used */
	u8   pad[2];
	char ifname[16];
};

/* ---- ARP / neighbour cache ---- */
#define RT_NUD_INCOMPLETE 0
#define RT_NUD_REACHABLE  1
#define RT_NUD_STALE      2
#define RT_NUD_PERMANENT  3
/* IPv6 neighbour cache only (rt_ctl_neigh6) */
#define RT_NUD_DELAY      4
#define RT_NUD_PROBE      5
#define RT_NUD_FAILED     6

struct rt_ctl_neigh {
	u32  addr;                /* network order */
	u32  ifindex;
	u32  age_s;               /* since the last state change */
	u8   hwaddr[6];
	u8   state;               /* RT_NUD_* */
	u8   pad;
	char ifname[16];
};

/* ---- sockets ---- */
#define RT_CTL_SOCK_TCP 1
#define RT_CTL_SOCK_UDP 2
#define RT_CTL_SOCK_RAW 3

struct rt_ctl_sock {
	u8   kind;                /* RT_CTL_SOCK_* */
	u8   proto;               /* IP protocol number */
	u8   state;               /* TCP: RT_TCPS_*; others 0 */
	u8   pad;
	u16  lport, fport;        /* host order */
	u32  laddr, faddr;        /* network order */
	u32  sendq, recvq;        /* bytes queued (listen: SYN queue) */
	u32  rtt_us, cwnd;        /* TCP only: smoothed RTT, cwnd in packets */
	u32  retrans;
};

/* ---- profiler ("profile start", see rt_diag.h) ----
 * Record 0 is the run itself (kind WALL): total = wall-clock time since
 * "profile start" (until "profile stop"), count = 1 while running,
 * rx_bytes = bytes received by all interfaces in that time. Then one
 * record per section (exclusive time) and per counter. */
#define RT_CTL_PROF_WALL    0
#define RT_CTL_PROF_SECTION 1
#define RT_CTL_PROF_COUNTER 2
struct rt_ctl_prof {
	char name[16];
	u32  kind;                /* RT_CTL_PROF_* */
	u32  count;               /* completed calls */
	u32  total_ms;            /* total time */
	u32  total_us;            /* the same in microseconds (saturates at 0xffffffff) */
	u32  max_us;              /* longest single call (sections: including nested sections) */
	u32  rx_bytes;            /* WALL record only */
};

/* ---- resolver configuration ---- */
#define RT_CTL_MAXNS 8
struct rt_ctl_dns {
	u32  nservers;
	u32  servers[RT_CTL_MAXNS];  /* network order */
	char domain[64];             /* search domain */
	char hostname[64];
};

/* ---- IPv6 records (addresses as 16 network order bytes) ---- */

/* rt_ctl_addr6.flags: RT_IN6_IFF_* of rt_ip6.h */
#define RT_CTL_IN6_TENTATIVE  0x0001
#define RT_CTL_IN6_DEPRECATED 0x0002
#define RT_CTL_IN6_AUTOCONF   0x0004
#define RT_CTL_IN6_DUPLICATED 0x0008
#define RT_CTL_IN6_NODAD      0x0010
#define RT_CTL_IN6_PERMANENT  0x0020
#define RT_CTL_IN6_TEMPORARY  0x0040

struct rt_ctl_addr6 {
	u8   addr[16];
	u32  ifindex;
	u32  valid_s, pref_s;     /* seconds left, 0xffffffff = forever */
	u16  flags;               /* RT_CTL_IN6_* */
	u8   plen;
	u8   scope;               /* 1 interface (::1 uses 2), 2 link, 5 site, 14 global */
	char ifname[16];
};

/* per interface IPv6 state */
struct rt_ctl_iface6 {
	char name[16];
	u32  index;
	u8   enabled;             /* IPv6 on this interface */
	u8   nd;                  /* uses neighbour discovery (Ethernet) */
	u8   cur_hlim;            /* hop limit (router advertisement or sysctl) */
	u8   ra_flags;            /* M (0x80) / O (0x40) of the last RA */
	u32  mtu;                 /* IPv6 link MTU */
	u32  reachable_ms, retrans_ms;
};

/* rt_ctl_route6.proto: RT_PROTO_* plus */
#define RT_PROTO_RA       9       /* Linux RTPROT_RA */

struct rt_ctl_route6 {
	u8   dst[16];
	u8   gateway[16];         /* all zero: on-link */
	u32  ifindex;
	u32  flags;               /* RT_RTF_* (RT_RTF_RA = 0x80000) */
	u32  metric;
	u32  mtu;
	u32  use;
	u32  expire_s;            /* seconds left, 0 = never */
	u8   plen;
	u8   proto;               /* RT_PROTO_* */
	u8   linkdown;
	s8   pref;                /* RFC 4191 router preference: -1 low, 0, 1 high */
	char ifname[16];
};

struct rt_ctl_neigh6 {
	u8   addr[16];
	u32  ifindex;
	u32  age_s;
	u8   hwaddr[6];
	u8   state;               /* RT_NUD_* including DELAY/PROBE/FAILED */
	u8   router;
	char ifname[16];
};

struct rt_ctl_sock6 {
	u8   kind;                /* RT_CTL_SOCK_* */
	u8   proto;
	u8   state;               /* TCP: RT_TCPS_* */
	u8   v6only;
	u16  lport, fport;        /* host order */
	u8   laddr[16], faddr[16];/* IPv4 peers of dual-stack sockets as ::ffff:a.b.c.d */
	u32  sendq, recvq;
	u32  rtt_us, cwnd;
	u32  retrans;
};

/* every resolver server, IPv4 ones IPv4-mapped (rt_ctl_dns keeps the IPv4 list) */
#define RT_CTL_DNS_STATIC 0
#define RT_CTL_DNS_RA     1
#define RT_CTL_DNS_DHCP6  2
struct rt_ctl_dns6 {
	u32  nservers;
	struct {
		u8  addr[16];
		u32 ifindex;      /* zone of a link-local server */
		u8  source;       /* RT_CTL_DNS_* */
		u8  pad[3];
	} servers[RT_CTL_MAXNS];
	char domain[64];
};

/* ---- core entry points (src/core/ctl.c), lock held ---- */
void rt_ctl_init(void);

/* Execute one command line in the configuration language (riptide.conf
 * grammar plus the query verbs below) and write the text reply to out
 * (always NUL terminated). Query verbs:
 *   sysctl -a | sysctl NAME [NAME...] | sysctl NAME=VALUE | sysctl -w NAME=VALUE
 * Returns 0 or -errno; on error out holds the message. Lock held.
 * 'unknown' handles keywords the core does not know (OS glue). */
int rt_ctl_command(const char *line, char *out, u32 outlen,
                   int (*unknown)(int argc, char **argv, char *err, u32 errlen));

/* Prefix length of a contiguous netmask (network order). */
static inline int rt_ctl_prefixlen(u32 mask_n)
{
	const u8 *b = (const u8 *)&mask_n;
	int n = 0, i, k;

	for (i = 0; i < 4; i++)
		for (k = 7; k >= 0; k--) {
			if (!(b[i] & (1 << k)))
				return n;
			n++;
		}
	return n;
}

/* State names shared by the core and the tools (never NULL). */
static inline const char *rt_ctl_tcp_state_name(int st)
{
	static const char *const n[] = { "CLOSE", "LISTEN", "SYN_SENT", "SYN_RECV", "ESTABLISHED",
	                                 "CLOSE_WAIT", "FIN_WAIT1", "CLOSING", "LAST_ACK",
	                                 "FIN_WAIT2", "TIME_WAIT" };
	return st >= 0 && st <= 10 ? n[st] : "UNKNOWN";
}
static inline const char *rt_ctl_nud_name(int st)
{
	static const char *const n[] = { "INCOMPLETE", "REACHABLE", "STALE", "PERMANENT",
	                                 "DELAY", "PROBE", "FAILED" };
	return st >= 0 && st <= 6 ? n[st] : "NONE";
}
static inline const char *rt_ctl_dhcp_state_name(int st)
{
	static const char *const n[] = { "none", "init", "selecting", "requesting", "bound",
	                                 "renewing", "rebinding" };
	return st >= 0 && st <= 6 ? n[st] : "unknown";
}

#endif
