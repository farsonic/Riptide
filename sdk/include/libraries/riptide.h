#ifndef LIBRARIES_RIPTIDE_H
#define LIBRARIES_RIPTIDE_H
/*
 * riptide.library - definitions for programs that use Riptide's own API.
 *
 *   #include <proto/riptide.h>
 *   RiptideBase = OpenLibrary(RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
 *
 * Open the library from the task that uses it: like bsdsocket.library,
 * every opener task gets its own base, and both share the task's socket
 * descriptor table and errno. See docs/AMIGA.md, "riptide.library".
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define RIPTIDE_LIB_NAME    "riptide.library"
#define RIPTIDE_LIB_VERSION 1    /* RiptideConnectName() needs version 2,
                                    RiptideSendMMsg()/RecvMMsg() version 3 */

/* RiptideConnectName(host, port, tags) (version 2): a TCP (or UDP)
 * socket connected to host:port, IPv6 and IPv4 raced as RFC 8305 "Happy
 * Eyeballs" describes; returns the socket descriptor (blocking mode) or
 * -1 with Errno() set. The tags are optional. */
#define RTCN_Dummy    (TAG_USER + 0x52430000)
#define RTCN_Family   (RTCN_Dummy + 1)   /* AF_UNSPEC (default), AF_INET, AF_INET6 */
#define RTCN_Type     (RTCN_Dummy + 2)   /* SOCK_STREAM (default) or SOCK_DGRAM */
#define RTCN_Timeout  (RTCN_Dummy + 3)   /* milliseconds, default 75000 */
#define RTCN_Result   (RTCN_Dummy + 4)   /* struct RiptideConnectResult *, filled on success */

struct RiptideConnectResult {
	UBYTE rcr_Addr[16];      /* the address that answered (IPv4: ::ffff:a.b.c.d) */
	ULONG rcr_Millis;        /* time to connect, name lookups included */
	UWORD rcr_Family;        /* AF_INET or AF_INET6 */
	UWORD rcr_NumAddrs;      /* addresses the name had */
	UWORD rcr_Attempts;      /* connection attempts started */
	UWORD rcr_Index;         /* the one that won, in the order tried */
};

/* SOL_SOCKET option: socket priority 0..7 (Linux SO_PRIORITY semantics,
 * Riptide 0.15): the transmit priority band, and the DSCP of packets of a
 * socket without IP_TOS. Linux's number 12 is accepted too. */
#define SO_RIPTIDE_PRIORITY 0x1100

/* SOL_SOCKET option, datagram sockets (Riptide 0.17): int, 1 = send this
 * socket's IPv4 UDP datagrams without a checksum (Linux SO_NO_CHECK; Linux's
 * number 11 is accepted too). IPv6 datagrams always carry one (RFC 8200). */
#define SO_RIPTIDE_NO_CHECK 0x1101

/* RiptideSendMMsg(fd, vec, vlen, flags) / RiptideRecvMMsg(fd, vec, vlen,
 * flags) (version 3): Linux sendmmsg()/recvmmsg(). vec is an array of
 * vlen of these; returns how many messages were sent / received (rmm_Len
 * holds each one's byte count) or -1 with Errno() set when the first one
 * failed. Sending stops at the first message that fails (for a
 * non-blocking socket: that would block, EWOULDBLOCK); receiving takes the
 * first as flags say and the rest only if already queued. */
struct RiptideMMsgHdr {
	/* a struct msghdr, as for sendmsg()/recvmsg() */
	APTR  rmm_Name;          /* msg_name: struct sockaddr * or NULL */
	ULONG rmm_NameLen;       /* msg_namelen */
	APTR  rmm_Iov;           /* msg_iov: struct iovec * */
	ULONG rmm_IovLen;        /* msg_iovlen */
	APTR  rmm_Control;       /* msg_control (not used) */
	ULONG rmm_ControlLen;    /* msg_controllen */
	LONG  rmm_Flags;         /* msg_flags (receive: MSG_TRUNC ...) */
	ULONG rmm_Len;           /* out: bytes sent / received */
};

/* RiptideSupportedCPUs() / RiptideSetFdCallback() CPU types */
#define RIPTIDECPU_M68KREG 1     /* 68k code, arguments in registers */
#define RIPTIDECPU_PPCV4   2     /* PowerPC SysV ABI (not supported) */

/* RiptideSysCtl() MIB of Riptide's tables, {CTL_NET, RIPTIDE_CTL, n}.
 * Each is a header (version, record size, count) followed by fixed-layout
 * records, as defined in include/rt/rt_ctl.h of the Riptide sources. */
#define RIPTIDE_CTL_NET       4      /* CTL_NET */
#define RIPTIDE_CTL           255
#define RIPTIDE_CTL_IFACES    1
#define RIPTIDE_CTL_ROUTES    2
#define RIPTIDE_CTL_NEIGH     3
#define RIPTIDE_CTL_SOCKETS   4
#define RIPTIDE_CTL_DNS       5
#define RIPTIDE_CTL_IPSTAT    6
#define RIPTIDE_CTL_ICMPSTAT  7
#define RIPTIDE_CTL_UDPSTAT   8
#define RIPTIDE_CTL_TCPSTAT   9
/* IPv6 tables (new record types; the IPv4 tables above are unchanged) */
#define RIPTIDE_CTL_ADDRS6    10
#define RIPTIDE_CTL_ROUTES6   11
#define RIPTIDE_CTL_NEIGH6    12
#define RIPTIDE_CTL_SOCKETS6  13
#define RIPTIDE_CTL_DNS6      14
#define RIPTIDE_CTL_IP6STAT   15
#define RIPTIDE_CTL_ICMP6STAT 16
#define RIPTIDE_CTL_IFACES6   17
#define RIPTIDE_CTL_ADDRS4    40     /* every IPv4 address (Riptide 0.15) */
#define RIPTIDE_CTL_QOS       41     /* transmit priority bands (Riptide 0.15) */

/* IPv6 (RiptideSupportsIPV6() returns TRUE). BSD/KAME numbers, as in the
 * Miami SDK; sockaddr_in6 is the 28-byte RFC 3493 layout (24-byte RFC 2133
 * callers are accepted). Defined here only if the network headers in use
 * do not have them. */
#ifndef AF_INET6
#define AF_INET6 28
#endif
#ifndef PF_INET6
#define PF_INET6 AF_INET6
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6 58
#endif
#ifndef IPV6_V6ONLY
#define IPV6_UNICAST_HOPS   4
#define IPV6_MULTICAST_IF   9
#define IPV6_MULTICAST_HOPS 10
#define IPV6_MULTICAST_LOOP 11
#define IPV6_JOIN_GROUP     12
#define IPV6_LEAVE_GROUP    13
#define IPV6_CHECKSUM       26
#define IPV6_V6ONLY         27
#define IPV6_TCLASS         61
#define IPV6_DONTFRAG       62
#endif
#ifndef AI_V4MAPPED
#define AI_NUMERICSERV  0x0008
#define AI_ALL          0x0100
#define AI_V4MAPPED_CFG 0x0200
#define AI_ADDRCONFIG   0x0400
#define AI_V4MAPPED     0x0800
#endif

#endif /* LIBRARIES_RIPTIDE_H */
