/*
 * Riptide - BSD socket ABI as seen by AmigaOS applications (AmiTCP 4 /
 * 4.4BSD values). The core uses these RT_ prefixed names so it can
 * be compiled on a host next to the host's own <sys/socket.h>.
 *
 * Structures here are exchanged with Amiga applications verbatim: all
 * fields are naturally sized and there is no padding on any ABI we target.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_BSD_H
#define RT_BSD_H

#include "rt_types.h"

#define RT_AF_UNSPEC 0
#define RT_AF_INET   2
#define RT_PF_INET   RT_AF_INET
/* 4.4BSD-Lite2/KAME value, also in the Miami SDK's <sys/socket.h> */
#define RT_AF_INET6  28
#define RT_PF_INET6  RT_AF_INET6

#define RT_SOCK_STREAM 1
#define RT_SOCK_DGRAM  2
#define RT_SOCK_RAW    3

#define RT_SOMAXCONN   128

/* sockaddr (4.4BSD: with length byte) */
struct rt_sockaddr {
	u8   sa_len;
	u8   sa_family;
	char sa_data[14];
};

struct rt_sockaddr_in {
	u8   sin_len;         /* sizeof(struct rt_sockaddr_in) == 16 */
	u8   sin_family;      /* RT_AF_INET */
	u16  sin_port;        /* network order */
	u32  sin_addr;        /* network order */
	u8   sin_zero[8];
};

/* IPv6 socket address, RFC 3493 layout (KAME/BSD, 28 bytes). Programs
 * built with RFC 2133 headers (the Miami SDK) use the first 24 bytes
 * only (no sin6_scope_id): Riptide accepts 24 byte addresses and returns
 * 24 bytes when the caller's buffer is smaller than 28. */
struct rt_sockaddr_in6 {
	u8   sin6_len;
	u8   sin6_family;     /* RT_AF_INET6 */
	u16  sin6_port;       /* network order */
	u32  sin6_flowinfo;   /* network order */
	rt_in6_addr sin6_addr;
	u32  sin6_scope_id;   /* interface index for link-local addresses */
};
#define RT_SIN6_LEN         28
#define RT_SIN6_LEN_RFC2133 24

struct rt_linger {
	s32 l_onoff;
	s32 l_linger;         /* seconds */
};

/* Amiga struct timeval (devices/timer.h): seconds + microseconds */
struct rt_timeval {
	u32 tv_secs;
	u32 tv_micro;
};

/* ---- SOL_SOCKET options ---- */
#define RT_SOL_SOCKET      0xffff
#define RT_SO_DEBUG        0x0001
#define RT_SO_ACCEPTCONN   0x0002
#define RT_SO_REUSEADDR    0x0004
#define RT_SO_KEEPALIVE    0x0008
#define RT_SO_DONTROUTE    0x0010
#define RT_SO_BROADCAST    0x0020
#define RT_SO_USELOOPBACK  0x0040
#define RT_SO_LINGER       0x0080
#define RT_SO_OOBINLINE    0x0100
#define RT_SO_REUSEPORT    0x0200
#define RT_SO_SNDBUF       0x1001
#define RT_SO_RCVBUF       0x1002
#define RT_SO_SNDLOWAT     0x1003
#define RT_SO_RCVLOWAT     0x1004
#define RT_SO_SNDTIMEO     0x1005   /* struct rt_timeval */
#define RT_SO_RCVTIMEO     0x1006   /* struct rt_timeval */
#define RT_SO_ERROR        0x1007
#define RT_SO_TYPE         0x1008
#define RT_SO_EVENTMASK    0x2001   /* AmiTCP: RT_SEV_* mask */

/* ---- IPPROTO_IP options ---- */
#define RT_IP_OPTIONS          1
#define RT_IP_HDRINCL          2
#define RT_IP_TOS              3
#define RT_IP_TTL              4
#define RT_IP_RECVOPTS         5
#define RT_IP_RECVRETOPTS      6
#define RT_IP_RECVDSTADDR      7
#define RT_IP_RETOPTS          8
#define RT_IP_MULTICAST_IF     9
#define RT_IP_MULTICAST_TTL    10
#define RT_IP_MULTICAST_LOOP   11
#define RT_IP_ADD_MEMBERSHIP   12
#define RT_IP_DROP_MEMBERSHIP  13
#define RT_IP_DEFAULT_MULTICAST_TTL  1
#define RT_IP_DEFAULT_MULTICAST_LOOP 1
#define RT_IP_MAX_MEMBERSHIPS        20   /* per socket (4.4BSD) */

/* IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP (4.4BSD, AmiTCP SDK): addresses
 * in network order; imr_interface 0 = the interface the group routes to.
 * The 12 byte Linux struct ip_mreqn (third field: interface index, which
 * wins when non-zero) is accepted too, also for IP_MULTICAST_IF. */
struct rt_ip_mreq {
	u32 imr_multiaddr;
	u32 imr_interface;
};
struct rt_ip_mreqn {
	u32 imr_multiaddr;
	u32 imr_address;
	s32 imr_ifindex;
};

/* ---- IPPROTO_IPV6 (41) options: KAME/RFC 3542 numbers. The Miami SDK
 * defines the first six with the same values (as aliases of the IPv4
 * options). ---- */
#define RT_IPV6_UNICAST_HOPS   4
#define RT_IPV6_MULTICAST_IF   9    /* u32 interface index */
#define RT_IPV6_MULTICAST_HOPS 10
#define RT_IPV6_MULTICAST_LOOP 11
#define RT_IPV6_JOIN_GROUP     12   /* struct rt_ipv6_mreq (IPV6_ADD_MEMBERSHIP) */
#define RT_IPV6_LEAVE_GROUP    13   /* struct rt_ipv6_mreq (IPV6_DROP_MEMBERSHIP) */
#define RT_IPV6_CHECKSUM       26   /* raw sockets: checksum offset, -1 off */
#define RT_IPV6_V6ONLY         27
#define RT_IPV6_TCLASS         61
#define RT_IPV6_DONTFRAG       62
struct rt_ipv6_mreq {
	rt_in6_addr ipv6mr_multiaddr;
	u32 ipv6mr_interface;
};
/* IPPROTO_ICMPV6 (58) level: KAME ICMP6_FILTER, struct of 8 u32; a set
 * bit lets the type pass (KAME semantics, unlike Linux). */
#define RT_ICMP6_FILTER        18

/* ---- IPPROTO_TCP options ----
 * 1..8 are the 4.4BSD values. The rest use FreeBSD's numbers for the
 * Linux-style knobs Riptide adds. */
#define RT_TCP_NODELAY     0x01
#define RT_TCP_MAXSEG      0x02
#define RT_TCP_CORK        0x03   /* Linux TCP_CORK: like NOPUSH, at most 200 ms */
#define RT_TCP_NOPUSH      0x04   /* cork: send only full segments */
#define RT_TCP_NOOPT       0x08
#define RT_TCP_INFO        0x20   /* struct rt_tcp_info (read only) */
#define RT_TCP_CONGESTION  0x40   /* char[16] algorithm name */
#define RT_TCP_KEEPINIT    0x80
#define RT_TCP_KEEPIDLE    0x100  /* seconds */
#define RT_TCP_KEEPINTVL   0x200  /* seconds */
#define RT_TCP_KEEPCNT     0x400
#define RT_TCP_QUICKACK    0x1000 /* Linux semantics */
#define RT_TCP_USER_TIMEOUT 0x2000 /* ms, RFC 5482 / Linux semantics */
/* TCP Fast Open client (RFC 7413). TCP_FASTOPEN_CONNECT has Linux's number
 * (30, unused in BSD): connect() returns at once and the first send()
 * goes in the SYN. TCP_FASTOPEN is FreeBSD's (1025): on a socket that is
 * not listening it does the same. */
#define RT_TCP_FASTOPEN_CONNECT 30
#define RT_TCP_FASTOPEN    0x401

/* ---- message flags ---- */
#define RT_MSG_OOB       0x01
#define RT_MSG_PEEK      0x02
#define RT_MSG_DONTROUTE 0x04
#define RT_MSG_EOR       0x08
#define RT_MSG_TRUNC     0x10
#define RT_MSG_CTRUNC    0x20
#define RT_MSG_WAITALL   0x40
#define RT_MSG_DONTWAIT  0x80
#define RT_MSG_EOF       0x100
/* Linux values, unused in AmiTCP and Roadshow: MSG_MORE holds a partial
 * segment for more data (at most 200 ms, as TCP_CORK); sendto() with
 * MSG_FASTOPEN on an unconnected TCP socket connects with the data in the
 * SYN (TCP Fast Open). */
#define RT_MSG_MORE      0x8000
#define RT_MSG_FASTOPEN  0x20000000

/* ---- ioctls (4.4BSD encoding) ---- */
#define RT_IOCPARM_MASK 0x1fff
#define RT_IOC_VOID  0x20000000u
#define RT_IOC_OUT   0x40000000u
#define RT_IOC_IN    0x80000000u
#define RT_IOC_INOUT (RT_IOC_IN | RT_IOC_OUT)
#define RT_IOC(inout, g, n, len) \
	((u32)(inout) | (((u32)(len) & RT_IOCPARM_MASK) << 16) | ((u32)(g) << 8) | (u32)(n))
#define RT_IOR(g, n, len)  RT_IOC(RT_IOC_OUT, g, n, len)
#define RT_IOW(g, n, len)  RT_IOC(RT_IOC_IN, g, n, len)
#define RT_IOWR(g, n, len) RT_IOC(RT_IOC_INOUT, g, n, len)

#define RT_FIONREAD   RT_IOR('f', 127, 4)
#define RT_FIONBIO    RT_IOW('f', 126, 4)
#define RT_FIOASYNC   RT_IOW('f', 125, 4)
#define RT_FIOSETOWN  RT_IOW('f', 124, 4)
#define RT_FIOGETOWN  RT_IOR('f', 123, 4)
#define RT_SIOCATMARK RT_IOR('s', 7, 4)
#define RT_SIOCSPGRP  RT_IOW('s', 8, 4)
#define RT_SIOCGPGRP  RT_IOR('s', 9, 4)

/* struct ifreq: 16 byte name + 16 byte union */
struct rt_ifreq {
	char ifr_name[16];
	union {
		struct rt_sockaddr ifru_addr;
		s16  ifru_flags;
		s32  ifru_metric;
		s32  ifru_mtu;
		u8   ifru_data[16];
	} ifr_ifru;
};
struct rt_ifconf {
	s32  ifc_len;
	void *ifc_buf;        /* Amiga: 32-bit pointer (host tests: native) */
};

#define RT_SIOCSIFADDR     RT_IOW('i', 12, 32)
#define RT_SIOCSIFDSTADDR  RT_IOW('i', 14, 32)
#define RT_SIOCSIFFLAGS    RT_IOW('i', 16, 32)
#define RT_SIOCGIFFLAGS    RT_IOWR('i', 17, 32)
#define RT_SIOCSIFBRDADDR  RT_IOW('i', 19, 32)
#define RT_SIOCSIFNETMASK  RT_IOW('i', 22, 32)
#define RT_SIOCGIFMETRIC   RT_IOWR('i', 23, 32)
#define RT_SIOCSIFMETRIC   RT_IOW('i', 24, 32)
#define RT_SIOCDIFADDR     RT_IOW('i', 25, 32)
#define RT_SIOCGIFADDR     RT_IOWR('i', 33, 32)
#define RT_SIOCGIFDSTADDR  RT_IOWR('i', 34, 32)
#define RT_SIOCGIFBRDADDR  RT_IOWR('i', 35, 32)
#define RT_SIOCGIFCONF     RT_IOWR('i', 36, 8)
#define RT_SIOCGIFNETMASK  RT_IOWR('i', 37, 32)
#define RT_SIOCGIFMTU      RT_IOWR('i', 51, 32)
#define RT_SIOCSIFMTU      RT_IOW('i', 52, 32)

/* SIOCAIFADDR: set address, broadcast (or peer) and mask in one call */
struct rt_ifaliasreq {
	char ifra_name[16];
	struct rt_sockaddr ifra_addr;
	struct rt_sockaddr ifra_broadaddr;
	struct rt_sockaddr ifra_mask;
};
#define RT_SIOCAIFADDR     RT_IOW('i', 26, 64)

/* ---- routing ioctls (4.3BSD struct ortentry, 48 bytes) ----
 * rt_flags take RT_RTF_UP/GATEWAY/HOST/REJECT (rt_ip.h). Riptide
 * extensions, both harmless for programs that leave the fields zero:
 *  - with RT_RTF_MASK (BSD's "subnet mask present", 0x80) in rt_flags,
 *    rt_hash holds the netmask in network order; otherwise a host route
 *    is /32, "default" (0.0.0.0) is /0 and a network route gets its
 *    classful mask, widened until it covers every set bit of rt_dst;
 *  - rt_ifp, a kernel pointer in BSD, may hold an interface index (1..255)
 *    to bind the route to that interface.
 * Without RT_RTF_GATEWAY, rt_gateway is the address of the local
 * interface (BSD convention) or 0 together with an rt_ifp index. */
struct rt_ortentry {
	u32  rt_hash;
	struct rt_sockaddr rt_dst;
	struct rt_sockaddr rt_gateway;
	s16  rt_flags;
	s16  rt_refcnt;
	u32  rt_use;
	u32  rt_ifp;          /* a 32-bit pointer on AmigaOS */
};
#define RT_SIOCADDRT       RT_IOW('r', 10, 48)
#define RT_SIOCDELRT       RT_IOW('r', 11, 48)

/* ---- ARP ioctls (AmiTCP 4 struct arpreq, 38 bytes) ----
 * arp_ha is 2+16 bytes (MAXADDRARP) and the int arp_flags sits at offset
 * 34 (m68k aligns int to 2), so it is declared as bytes here: access it
 * with memcpy() of an s32. The same requests with the 4.4BSD/Roadshow
 * layout (arp_ha a plain sockaddr, 36 bytes, group 'i') are accepted too. */
struct rt_arpreq {
	struct rt_sockaddr arp_pa;
	struct {
		u8 sa_len;
		u8 sa_family;
		u8 sa_data[16];
	} arp_ha;
	u8   arp_flags[4];
};
#define RT_SIOCSARP        RT_IOW('I', 30, 38)
#define RT_SIOCGARP        RT_IOWR('I', 38, 38)
#define RT_SIOCDARP        RT_IOW('I', 32, 38)
#define RT_SIOCSARP_BSD    RT_IOW('i', 30, 36)
#define RT_SIOCGARP_BSD    RT_IOWR('i', 38, 36)
#define RT_SIOCDARP_BSD    RT_IOW('i', 32, 36)
#define RT_ATF_INUSE       0x01
#define RT_ATF_COM         0x02
#define RT_ATF_PERM        0x04
#define RT_ATF_PUBL        0x08

/* ---- TCP_INFO (subset of Linux struct tcp_info, fixed 32-bit fields) ---- */
struct rt_tcp_info {
	u8  tcpi_state;          /* RT_TCPS_* */
	u8  tcpi_ca_state;       /* 0 open 1 disorder 2 cwr 3 recovery 4 loss */
	u8  tcpi_retransmits;
	u8  tcpi_options;        /* 1 TS, 2 SACK, 4 WSCALE, 8 ECN, 32 SYN data acknowledged (TFO) */
	u8  tcpi_snd_wscale;
	u8  tcpi_rcv_wscale;
	u8  tcpi_backoff;
	u8  tcpi_pad;
	u32 tcpi_rto_us;
	u32 tcpi_snd_mss;
	u32 tcpi_rcv_mss;
	u32 tcpi_unacked;        /* packets_out */
	u32 tcpi_sacked;
	u32 tcpi_lost;
	u32 tcpi_retrans;        /* currently retransmitted, unacked */
	u32 tcpi_rtt_us;         /* smoothed */
	u32 tcpi_rttvar_us;
	u32 tcpi_min_rtt_us;
	u32 tcpi_snd_ssthresh;
	u32 tcpi_snd_cwnd;       /* packets */
	u32 tcpi_rcv_space;
	u32 tcpi_snd_wnd;        /* peer's advertised window, bytes */
	u32 tcpi_rcv_wnd;        /* our advertised window, bytes */
	u32 tcpi_total_retrans;
	u32 tcpi_bytes_acked;    /* low 32 bits */
	u32 tcpi_bytes_received; /* low 32 bits */
	u32 tcpi_segs_out;
	u32 tcpi_segs_in;
	u32 tcpi_reordering;
	char tcpi_ca_name[16];
	/* Riptide 0.12 (a shorter optlen gets the fields above only): the
	 * counters of net.riptide.tcpinfo, see rt_ctl.h */
	u32 tcpi_busy_ms, tcpi_rwnd_limited_ms, tcpi_sndbuf_limited_ms;
	u32 tcpi_unread_ms, tcpi_zwin_ms;
	u32 tcpi_fast_recoveries, tcpi_rto_timeouts, tcpi_tlp_probes;
	u32 tcpi_dupacks_in, tcpi_dupacks_out, tcpi_sack_blocks_out;
	u32 tcpi_acks_out, tcpi_delayed_acks_out;
	u32 tcpi_zwin_out, tcpi_zwin_in;
	u32 tcpi_data_segs_in, tcpi_data_segs_out;
	u32 tcpi_ofo_bytes, tcpi_rcvbuf, tcpi_sndbuf;
};

/* TCP states (numbering follows 4.4BSD TCPS_* so netstat tools work) */
#define RT_TCPS_CLOSED       0
#define RT_TCPS_LISTEN       1
#define RT_TCPS_SYN_SENT     2
#define RT_TCPS_SYN_RECEIVED 3
#define RT_TCPS_ESTABLISHED  4
#define RT_TCPS_CLOSE_WAIT   5
#define RT_TCPS_FIN_WAIT_1   6
#define RT_TCPS_CLOSING      7
#define RT_TCPS_LAST_ACK     8
#define RT_TCPS_FIN_WAIT_2   9
#define RT_TCPS_TIME_WAIT    10

#endif
