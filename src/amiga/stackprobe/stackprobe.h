/*
 * stackprobe - which TCP/IP features an Amiga stack offers, and how they
 * behave, through the public APIs only (bsdsocket.library, the Roadshow
 * extensions, miami.library, usergroup.library), so that the same binary
 * runs on AmiTCP, Miami, Genesis, Roadshow and Riptide and the reports can
 * be compared (tools/stackprobe_compare.py).
 *
 * Plain 68000 code, no Riptide headers: the constants below are the BSD 4.4
 * values every Amiga stack uses.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef STACKPROBE_H
#define STACKPROBE_H

#include <exec/types.h>
#include <exec/libraries.h>

#include "sp/bsdsocket_calls.h"
#include "sp/miami_calls.h"
#include "sp/usergroup_calls.h"

#define SP_VERSION "1.0"

/* does library l have the vector of function f (LVO_lib_f, negative)? */
#define SP_HAVE(l, lvo) ((l) && (LONG)(l)->lib_NegSize >= -(lvo))
#define SP_HAVE_BSD(f)   SP_HAVE(SocketBase, LVO_bsdsocket_##f)
#define SP_HAVE_MIAMI(f) SP_HAVE(MiamiBase, LVO_miami_##f)
#define SP_HAVE_UG(f)    SP_HAVE(UserGroupBase, LVO_usergroup_##f)

/* ---- BSD constants (the AmiTCP SDK values) ---- */
#define AF_UNSPEC   0
#define AF_INET     2
#define AF_INET6    28
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define SOL_SOCKET  0xffff
#define SO_DEBUG       0x0001
#define SO_ACCEPTCONN  0x0002
#define SO_REUSEADDR   0x0004
#define SO_KEEPALIVE   0x0008
#define SO_DONTROUTE   0x0010
#define SO_BROADCAST   0x0020
#define SO_USELOOPBACK 0x0040
#define SO_LINGER      0x0080
#define SO_OOBINLINE   0x0100
#define SO_REUSEPORT   0x0200
#define SO_SNDBUF      0x1001
#define SO_RCVBUF      0x1002
#define SO_SNDLOWAT    0x1003
#define SO_RCVLOWAT    0x1004
#define SO_SNDTIMEO    0x1005
#define SO_RCVTIMEO    0x1006
#define SO_ERROR       0x1007
#define SO_TYPE        0x1008
#define SO_EVENTMASK   0x2001
#define TCP_NODELAY 1
#define TCP_MAXSEG  2
#define IP_HDRINCL          2
#define IP_TOS              3
#define IP_TTL              4
#define IP_MULTICAST_IF     9
#define IP_MULTICAST_TTL    10
#define IP_MULTICAST_LOOP   11
#define IP_ADD_MEMBERSHIP   12
#define IP_DROP_MEMBERSHIP  13
#define IPV6_V6ONLY         27
#define MSG_OOB      0x01
#define MSG_PEEK     0x02
#define MSG_WAITALL  0x40
#define MSG_DONTWAIT 0x80
#define FIOASYNC   0x8004667dUL
#define FIONBIO    0x8004667eUL
#define FIONREAD   0x4004667fUL
#define SIOCATMARK 0x40047307UL
#define INADDR_ANY       0x00000000UL
#define INADDR_LOOPBACK  0x7f000001UL
#define INADDR_BROADCAST 0xffffffffUL

#define EINTR        4
#define EBADF        9
#define EACCES       13
#define EINVAL       22
#define EMFILE       24
#define EPIPE        32
#define EWOULDBLOCK  35
#define EINPROGRESS  36
#define EALREADY     37
#define ENOTSOCK     38
#define EMSGSIZE     40
#define EPROTOTYPE   41
#define ENOPROTOOPT  42
#define EPROTONOSUPPORT 43
#define ESOCKTNOSUPPORT 44
#define EOPNOTSUPP   45
#define EAFNOSUPPORT 47
#define EADDRINUSE   48
#define EADDRNOTAVAIL 49
#define ENETUNREACH  51
#define ECONNABORTED 53
#define ECONNRESET   54
#define ENOBUFS      55
#define EISCONN      56
#define ENOTCONN     57
#define ETIMEDOUT    60
#define ECONNREFUSED 61
#define EHOSTUNREACH 65
#define ENOSYS       78

/* SocketBaseTagList() tags (amitcp/socketbasetags.h) */
#define SBTF_REF 0x8000
#define SBTS_CODE 0x3fff
#define SBTM_GETREF(c) (TAG_USER | SBTF_REF | (((c) & SBTS_CODE) << 1))
#define SBTM_GETVAL(c) (TAG_USER | (((c) & SBTS_CODE) << 1))
#define SBTM_SETREF(c) (TAG_USER | SBTF_REF | (((c) & SBTS_CODE) << 1) | 1)
#define SBTM_SETVAL(c) (TAG_USER | (((c) & SBTS_CODE) << 1) | 1)
#define SBTC_BREAKMASK      1
#define SBTC_SIGIOMASK      2
#define SBTC_SIGURGMASK     3
#define SBTC_SIGEVENTMASK   4
#define SBTC_ERRNO          6
#define SBTC_HERRNO         7
#define SBTC_DTABLESIZE     8
#define SBTC_FDCALLBACK     9
#define SBTC_LOGSTAT        10
#define SBTC_LOGTAGPTR      11
#define SBTC_LOGFACILITY    12
#define SBTC_LOGMASK        13
#define SBTC_ERRNOSTRPTR    14
#define SBTC_HERRNOSTRPTR   15
#define SBTC_IOERRNOSTRPTR  16
#define SBTC_S2ERRNOSTRPTR  17
#define SBTC_S2WERRNOSTRPTR 18
#define SBTC_ERRNOBYTEPTR   21
#define SBTC_ERRNOWORDPTR   22
#define SBTC_ERRNOLONGPTR   24
#define SBTC_HERRNOLONGPTR  25
#define SBTC_RELEASESTRPTR  29
#define SBTC_COMPAT43       30

struct sp_in_addr { ULONG s_addr; };
struct sp_sockaddr_in {
	UBYTE sin_len, sin_family;
	UWORD sin_port;
	struct sp_in_addr sin_addr;
	UBYTE sin_zero[8];
};
struct sp_sockaddr_in6 {
	UBYTE sin6_len, sin6_family;
	UWORD sin6_port;
	ULONG sin6_flowinfo;
	UBYTE sin6_addr[16];
	ULONG sin6_scope_id;
};
struct sp_timeval { LONG tv_sec, tv_usec; };
struct sp_linger { LONG l_onoff, l_linger; };
struct sp_ip_mreq { struct sp_in_addr imr_multiaddr, imr_interface; };
struct sp_iovec { APTR iov_base; LONG iov_len; };
struct sp_msghdr {
	APTR msg_name; LONG msg_namelen;
	struct sp_iovec *msg_iov; LONG msg_iovlen;
	APTR msg_control; LONG msg_controllen; LONG msg_flags;
};
struct sp_hostent { STRPTR h_name; STRPTR *h_aliases; LONG h_addrtype, h_length; STRPTR *h_addr_list; };
struct sp_servent { STRPTR s_name; STRPTR *s_aliases; LONG s_port; STRPTR s_proto; };
struct sp_protoent { STRPTR p_name; STRPTR *p_aliases; LONG p_proto; };
struct sp_netent { STRPTR n_name; STRPTR *n_aliases; LONG n_addrtype; ULONG n_net; };
struct sp_passwd { STRPTR pw_name, pw_passwd; LONG pw_uid, pw_gid; STRPTR pw_gecos, pw_dir, pw_shell; };
struct sp_group { STRPTR gr_name, gr_passwd; LONG gr_gid; STRPTR *gr_mem; };
struct sp_addrinfo {
	LONG ai_flags, ai_family, ai_socktype, ai_protocol, ai_addrlen;
	STRPTR ai_canonname; APTR ai_addr; struct sp_addrinfo *ai_next;
};

/* fd_set for WaitSelect(): bit n of long n/32, as AmiTCP defines it */
#define SP_FDWORDS 16 /* up to 512 descriptors */
typedef struct { ULONG bits[SP_FDWORDS]; } sp_fdset;
#define SP_FD_ZERO(s)  memset((s), 0, sizeof(sp_fdset))
#define SP_FD_SET(n, s)   ((s)->bits[(n) >> 5] |= 1UL << ((n) & 31))
#define SP_FD_ISSET(n, s) (((s)->bits[(n) >> 5] >> ((n) & 31)) & 1)

/* ---- results ---- */
enum { R_PASS, R_FAIL, R_UNSUP, R_SKIP, R_INFO };

struct sp_result {
	char id[36];      /* area.name, stable across versions */
	char desc[60];
	UBYTE res;
	char value[64];   /* a number or short text, "" when none */
	LONG err;         /* errno (or other code) for FAIL/UNSUP, else 0 */
	LONG ms;          /* time taken, -1 when not measured */
};

extern struct Library *SocketBase, *MiamiBase, *UserGroupBase;
extern int sp_verbose, sp_quick, sp_perf;
extern const char *sp_peer;
extern ULONG sp_peer_addr;

/* report */
void sp_area(const char *area);
void sp_result(const char *id, const char *desc, int res, LONG err, const char *fmt, ...);
void sp_result_ms(const char *id, const char *desc, int res, LONG err, LONG ms, const char *fmt, ...);
void sp_unsup_call(const char *id, const char *desc);
void sp_note(const char *fmt, ...);

/* clock */
void sp_clock_init(void);
ULONG sp_now_ms(void);           /* milliseconds, wraps after 49 days */
ULONG sp_now_us(void);           /* microseconds, wraps after 71 minutes */

/* helpers (stackprobe.c) */
LONG sp_errno(void);
void sp_sin(struct sp_sockaddr_in *sa, ULONG addr, UWORD port);
LONG sp_tcp_listener(UWORD *port);
LONG sp_tcp_pair(LONG *client, LONG *server, LONG lst, UWORD port);
LONG sp_nonblock(LONG s, LONG on);
LONG sp_wait(LONG s, int write, LONG ms);
LONG sp_recv_all(LONG s, char *buf, LONG len, LONG ms);
void sp_close(LONG *s);
const char *sp_errname(LONG e);
int sp_strnicmp(const char *a, const char *b, unsigned long n);
void sp_watchdog(LONG ms);       /* Ctrl-C to us after ms (0: off, clears Ctrl-C) */
int sp_watchdog_fired(void);

/* process entry points must find their data (vbcc small data: a4) */
#ifdef __VBCC__
#define SP_ENTRY __saveds
#else
#define SP_ENTRY
#endif

/* the tests */
void sp_test_libraries(void);
void sp_test_api(void);
void sp_test_tags(void);
void sp_test_netdb(void);
void sp_test_tcp(void);
void sp_test_udp(void);
void sp_test_options(void);
void sp_test_select(void);
void sp_test_misc(void);
void sp_test_ipv6(void);
void sp_test_ext(void);
void sp_test_perf(void);
void sp_test_peer(void);

#endif
