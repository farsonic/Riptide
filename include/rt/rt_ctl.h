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
#define RT_CTL_TCPINFO   18       /* net.riptide.tcpinfo   struct rt_ctl_tcpinfo[] (TCP, both families) */
#define RT_CTL_DRIVERS   20       /* net.riptide.drivers   struct rt_ctl_driver[] */

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

/* ---- TCP connection details (netstat -T) ----
 * One record per TCP socket of either family (listeners included, time-wait
 * records not). Times are in ms; "_ago" fields count back from now (0 =
 * never). Sender chronographs follow Linux: busy_ms counts all time with
 * data unacknowledged or unsent, rwnd_limited_ms / sndbuf_limited_ms the
 * part of it held back by the peer's window / by our send buffer. Receive
 * side: unread_ms is time with received data waiting for the application,
 * zwin_ms time with our advertised window at zero. 64-bit byte counts are
 * split into _hi/_lo words. */
#define RT_CTL_TCPI_TS    1       /* options: timestamps */
#define RT_CTL_TCPI_SACK  2
#define RT_CTL_TCPI_WSCALE 4
#define RT_CTL_TCPI_ECN   8
struct rt_ctl_tcpinfo {
	u8   family;              /* RT_AF_INET / RT_AF_INET6 (socket domain) */
	u8   state;               /* RT_TCPS_* */
	u8   ca_state;            /* 0 open 1 disorder 2 cwr 3 recovery 4 loss */
	u8   options;             /* RT_CTL_TCPI_* */
	u8   snd_wscale, rcv_wscale;
	u8   backoff;             /* RTO backoff exponent */
	u8   pad;
	u16  lport, fport;        /* host order */
	u8   laddr[16], faddr[16];/* IPv4 as ::ffff:a.b.c.d */
	u32  srtt_us, rttvar_us, min_rtt_us, rto_us;
	u32  snd_mss, rcv_mss, advmss;
	u32  cwnd, ssthresh;      /* packets */
	u32  snd_wnd;             /* the peer's window, bytes */
	u32  rcv_wnd;             /* the window we advertised, bytes */
	u32  rcv_ssthresh;        /* window growth limit, bytes */
	u32  sndbuf, rcvbuf;      /* current (auto-tuned) buffer sizes, bytes */
	u32  sendq, recvq;        /* bytes unacknowledged+unsent / unread */
	u32  ofo_bytes, ofo_segs; /* out-of-order queue */
	u32  packets_out, retrans_out, sacked_out, lost_out;
	u32  total_retrans;       /* segments retransmitted */
	u32  fast_recoveries;     /* entries into fast recovery */
	u32  rto_timeouts;        /* retransmission timeouts */
	u32  tlp_probes;          /* tail loss probes */
	u32  dupacks_in;          /* duplicate ACKs received */
	u32  dupacks_out;         /* ACKs sent with out-of-order data held (or D-SACK) */
	u32  sack_blocks_out;     /* SACK blocks sent */
	u32  acks_out;            /* pure ACKs sent */
	u32  delayed_acks_out;    /* of those, sent by the delayed ACK timer */
	u32  zwin_out, zwin_in;   /* zero windows advertised / received */
	u32  segs_in, segs_out, data_segs_in, data_segs_out;
	u32  bytes_received_hi, bytes_received_lo;
	u32  bytes_acked_hi, bytes_acked_lo;
	u32  busy_ms, rwnd_limited_ms, sndbuf_limited_ms;
	u32  unread_ms, zwin_ms;
	u32  age_ms;              /* since the connection was established (0 never) */
	u32  last_data_recv_ago, last_data_sent_ago, last_ack_recv_ago;
	char cc[16];              /* congestion control */
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

/* ---- driver counters (net.riptide.drivers) ----
 * One record per interface whose driver keeps them (SANA-II, MNI, host
 * TAP). Counters run since the last "interface NAME resetstats"; the
 * dq_*, gs_* and special[] values come from the driver and are not reset. */
#define RT_CTL_DRV_RXQ      3     /* rxq[]: 0 IPv4, 1 ARP, 2 IPv6 */
#define RT_CTL_DRV_BATCH    6     /* batch[]: 1, 2-3, 4-7, 8-15, 16-31, 32+ frames */
#define RT_CTL_DRV_NERR     8     /* rxerr[], txerr[] */
#define RT_CTL_DRV_NSPECIAL 8     /* special[] */
#define RT_CTL_DRV_ERR_OTHER  0x7fffffff /* rt_ctl_drv_err.code: all codes that did not fit */
#define RT_CTL_DRV_ERR_LENGTH 0x7ffffffe /* rt_ctl_drv_err.code: frame too short or too long */

/* rt_ctl_driver.flags */
#define RT_CTL_DRVF_DQ      0x01  /* dq_* valid (S2_DEVICEQUERY) */
#define RT_CTL_DRVF_GS      0x02  /* gs_* valid (S2_GETGLOBALSTATS) */
#define RT_CTL_DRVF_SS      0x04  /* special[] valid (S2_GETSPECIALSTATS) */
#define RT_CTL_DRVF_DROPS   0x08  /* drv_drops valid (MNI: frames refused, no free buffer) */
#define RT_CTL_DRVF_SHARED  0x10  /* rxq[0] is one buffer pool for every packet type (MNI) */
#define RT_CTL_DRVF_NOQ     0x20  /* no receive queue to report (host TAP) */

struct rt_ctl_drv_rxq {
	u32  cfg;                 /* reads (receive buffers) configured */
	u32  queued;              /* handed to the driver now */
	u32  low;                 /* fewest the driver held at a wakeup (0: frames may be lost) */
	u32  empty;               /* wakeups at which the driver held none */
};

struct rt_ctl_drv_err {
	s32  code;                /* SANA-II io_Error (IOERR_* < 0), MNI status, -errno */
	u32  count;
};

struct rt_ctl_drv_special {
	u32  type;                /* S2SS_* */
	u32  count;
	char name[24];            /* the driver's name for it, may be empty */
};

struct rt_ctl_driver {
	char name[16];            /* interface */
	u32  index;
	char kind[8];             /* "sana2", "mni", "tap" */
	char driver[48];          /* device or driver file */
	u32  unit;
	u32  flags;               /* RT_CTL_DRVF_* */
	u32  since_s;             /* seconds since the counters were reset */
	u32  rx_frames, rx_bytes, tx_frames, tx_bytes;
	struct rt_ctl_drv_rxq rxq[RT_CTL_DRV_RXQ];
	u32  batch[RT_CTL_DRV_BATCH]; /* wakeups, by number of frames received in them */
	u32  batch_max;           /* most frames received in one wakeup */
	u32  rx_nobuf;            /* receive buffer allocation failures */
	u32  rx_errors, tx_errors;
	struct rt_ctl_drv_err rxerr[RT_CTL_DRV_NERR];
	struct rt_ctl_drv_err txerr[RT_CTL_DRV_NERR];
	u32  tx_slots;            /* transmit requests available, 0 = no limit */
	u32  tx_inflight;         /* handed to the driver, not completed */
	u32  tx_inflight_max;
	u32  tx_full;             /* packets dropped: no transmit request free */
	u32  drv_drops;           /* RT_CTL_DRVF_DROPS */
	/* S2_DEVICEQUERY */
	u32  dq_hwtype, dq_level, dq_mtu, dq_rawmtu, dq_bps;
	/* S2_GETGLOBALSTATS */
	u32  gs_rx, gs_tx, gs_baddata, gs_overruns, gs_unknown, gs_reconfig;
	u32  gs_laststart_s;      /* LastStart.tv_secs */
	u32  gs_age_ms;           /* age of gs_* and special[] */
	u32  nspecial;
	struct rt_ctl_drv_special special[RT_CTL_DRV_NSPECIAL];
};

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
