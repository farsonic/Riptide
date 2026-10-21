/*
 * Riptide - shared code of the network tools (ip, ifconfig, route, arp,
 * netstat, traceroute, ping, host, sysctl, hostname): Linux style argument
 * splitting, buffered output, library handling, the net.riptide.* table
 * dumps (RiptideSysCtl) and the RIPTIDE port command channel.
 *
 * Tools are small CLI programs: keep stack use low (the default CLI stack
 * is 4 KB) and allocate buffers with AllocVec().
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_CMDLIB_H
#define RT_CMDLIB_H

#define RT_TOOL_BUILD 1
#include "amiga_os.h"
#include "amiga_net.h"
#include "rt/rt_types.h"
#include "rt/rt_bsd.h"
#include "rt/rt_errno.h"
#include "rt/rt_ip.h"
#include "rt/rt_ip6.h"
#include "rt/rt_ctl.h"
#include "rt/rt_stats.h"
#include "rt/riptide_port.h"

extern struct Library *SocketBase, *RiptideBase;
extern struct Device *TimerBase;

/* exit codes (AmigaDOS conventions) */
#define CL_OK    0
#define CL_WARN  5
#define CL_ERROR 10
#define CL_FAIL  20

/* ---- entry point: CL_MAIN(fn, "name") defines main()/amiga_main() ---- */
typedef int (*cl_main_fn)(int argc, char **argv);
int cl_run(cl_main_fn fn, const char *name);
extern const char *cl_prog;
#if defined(__VBCC__)
#define CL_MAIN(fn, name) int main(void) { return cl_run(fn, name); }
#else
#define CL_MAIN(fn, name) long amiga_main(char *s, long l) { (void)s; (void)l; return cl_run(fn, name); }
#endif

/* ---- output (buffered, to Output()) ---- */
void cl_printf(const char *fmt, ...) RT_PRINTF(1, 2);
void cl_puts(const char *s);
void cl_flush(void);
/* "prog: message\n" */
void cl_error(const char *fmt, ...) RT_PRINTF(1, 2);
/* Ctrl-C pressed? (checks and clears the signal) */
int  cl_break(void);

/* ---- libraries; print an error and return -1 when unavailable ---- */
int  cl_need_socket(void);       /* bsdsocket.library 4 */
int  cl_need_riptide(void);      /* riptide.library (RiptideSysCtl) */
int  cl_need_timer(void);        /* timer.device for cl_now_us() */
u32  cl_now_us(void);            /* microseconds, wraps */
int  cl_socket(int type, int proto);   /* socket(AF_INET...) for ioctls */
int  cl_ioctl(int fd, u32 req, void *arg); /* 0 or errno */
/* Wait until fd is readable (wr = 0) or writable (wr = 1), at most ms
 * milliseconds (0: no limit). Ctrl-C is in the WaitSelect() signal mask.
 * Returns 1 ready, 0 timeout, -1 Ctrl-C (also EINTR), -2 error (Errno()). */
int  cl_wait_fd(int fd, int wr, u32 ms);
/* connect() a TCP socket with a timeout (non-blocking connect + WaitSelect;
 * the socket is blocking again afterwards). 0 or the errno: RT_EINTR on
 * Ctrl-C, RT_ETIMEDOUT on timeout. */
int  cl_connect(int fd, u32 addr, u16 port, u32 ms);

/* ---- argument helpers ---- */
/* Is 'arg' a (non-empty) prefix of 'word'? (iproute2 style matching) */
int  cl_match(const char *arg, const char *word);
int  cl_streq_nocase(const char *a, const char *b);
/* Minimal getopt: options in 'spec' ("nc:s:"); returns the option char,
 * -1 at the first non-option, '?' on errors (message printed). *optarg
 * gets the argument of options followed by ':'. */
struct cl_getopt {
	int ind;                 /* next argv index (start at 1) */
	int pos;                 /* position inside grouped options */
	char *arg;
};
int  cl_getopt(int argc, char **argv, const char *spec, struct cl_getopt *g);
int  cl_parse_uint(const char *s, u32 max, u32 *out);
/* "1.5" -> 1500 (milliseconds); 0 or -1 */
int  cl_parse_ms(const char *s, u32 *ms);

/* ---- addresses ---- */
int  cl_parse_ip(const char *s, u32 *addr);           /* network order */
/* "A.B.C.D[/BITS]"; *bits = -1 when no /BITS */
int  cl_parse_prefix(const char *s, u32 *addr, int *bits);
u32  cl_mask(int bits);                               /* network order */
char *cl_ip(u32 addr, char *buf);                      /* buf >= 16 */
int  cl_parse_mac(const char *s, u8 *mac);
char *cl_mac(const u8 *mac, int len, char *buf);       /* buf >= 3*len */
/* Name or dotted quad -> address. 0 or -1 (message printed). */
int  cl_resolve(const char *name, u32 *addr);
/* Reverse lookup into buf, or the dotted quad if numeric/no name. */
char *cl_hostname(u32 addr, int numeric, char *buf, u32 len);
const char *cl_strerror(int err);

/* ---- either family (IPv4 kept IPv4-mapped in a) ---- */
struct cl_addr {
	int family;              /* RT_AF_INET or RT_AF_INET6 */
	rt_in6_addr a;
	u32 scope;               /* zone (interface index) of link-local IPv6 */
};
#define CL_ADDRSTRLEN 64         /* RT_INET6_ADDRSTRLEN + "%" + interface */
/* family: 0 any (getaddrinfo order, RFC 6724), 4 or 6. Numeric forms
 * "A.B.C.D" and "IPV6[%IF]". 0 or -1 (message printed). */
int  cl_resolve_af(const char *name, int family, struct cl_addr *out);
/* Literal only (no lookups, no message). 0 or -1. */
int  cl_parse_addr(const char *s, struct cl_addr *out);
char *cl_addr_str(const struct cl_addr *a, char *buf);  /* buf >= CL_ADDRSTRLEN */
/* Reverse lookup (or the numeric form). */
char *cl_addr_name(const struct cl_addr *a, int numeric, char *buf, u32 len);
/* Fill a sockaddr_in / sockaddr_in6 (buffer >= 28 bytes); returns its length. */
u32  cl_sockaddr(const struct cl_addr *a, u16 port, void *sa);
/* Back from a sockaddr (either family; IPv4-mapped stays AF_INET6). */
void cl_from_sockaddr(const void *sa, struct cl_addr *out, u16 *port);
/* cl_connect for either family */
int  cl_connect_addr(int fd, const struct cl_addr *a, u16 port, u32 ms);
char *cl_ip6(const u8 *a16, char *buf);                /* buf >= RT_INET6_ADDRSTRLEN */
/* interface name of an index ("" if unknown), static buffer */
const char *cl_ifname(u32 index);
u32  cl_classful(u32 addr);

/* ---- Riptide interfaces ---- */
/* Fetch a net.riptide.* dump (RT_CTL_*). Returns an AllocVec'd buffer
 * (free with FreeVec) and the header, or NULL (message printed). */
void *cl_dump(int what, struct rt_ctl_hdr *hdr);
static inline void *cl_rec(void *dump, const struct rt_ctl_hdr *h, u32 i)
{
	return (u8 *)dump + sizeof(*h) + i * h->recsize;
}
/* Find an interface record by name or index in an ifaces dump. */
struct rt_ctl_iface *cl_find_iface(void *dump, const struct rt_ctl_hdr *h, const char *name);
struct rt_ctl_iface *cl_iface_by_index(void *dump, const struct rt_ctl_hdr *h, u32 index);
/* Reorder a routes dump for display like Linux: default routes first,
 * then by destination address, longer prefixes first (stable). */
void cl_sort_routes(void *dump, const struct rt_ctl_hdr *h);
/* Send a command line to the RIPTIDE port. The reply text is printed
 * (when print) or left in the static buffer returned by cl_reply().
 * Returns 0 or the errno result; -1 if Riptide is not running. */
int  cl_command(const char *line, int print);
const char *cl_reply(void);
/* Build a command line from printf style pieces (static buffer). */
const char *cl_line(const char *fmt, ...) RT_PRINTF(1, 2);

#endif
