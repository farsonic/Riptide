/*
 * Riptide - top level: initialisation, stack thread entry points,
 * connection listing, configuration, resolver and DHCP client.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RIPTIDE_H
#define RIPTIDE_H

#include "rt_types.h"
#include "rt_errno.h"
#include "rt_os.h"
#include "rt_buf.h"
#include "rt_timer.h"
#include "rt_netif.h"
#include "rt_ip.h"
#include "rt_bsd.h"
#include "rt_socket.h"
#include "rt_sysctl.h"
#include "rt_stats.h"

#define RIPTIDE_VERSION_STRING "0.9"

/* Initialise every module and create lo0. Call once, without the lock. */
int  rt_init(void);
/* Called by the stack thread with the lock held whenever it wakes up:
 * runs due timers and deferred work. Returns ms until it must be called
 * again (RT_TIMER_NONE: only when kicked or a frame arrives). */
u32  rt_stack_poll(void);

extern char rt_hostname[64];
extern char rt_domainname[64];

/* ---- connection listing (netstat) ---- */
struct rt_pcb_info {
	u8  proto;           /* RT_IPPROTO_TCP / UDP / raw protocol */
	u8  state;           /* RT_TCPS_* for TCP, 0 otherwise */
	u16 lport, fport;    /* host order */
	u32 laddr, faddr;    /* network order */
	u32 sendq, recvq;    /* bytes queued */
	u32 rtt_us, cwnd;    /* TCP only */
	u32 retrans;
};
int rt_tcp_list(struct rt_pcb_info *out, int max);   /* returns total count */
int rt_udp_list(struct rt_pcb_info *out, int max);
int rt_raw_list(struct rt_pcb_info *out, int max);

/* ---- configuration file (src/core/config.c) ----
 * Line oriented, '#' comments. Generic keywords handled by the core:
 *   hostname NAME | domain NAME | nameserver A.B.C.D | host NAME A.B.C.D
 *   route add|del DEST[/BITS]|default GATEWAY
 *   sysctl NAME=VALUE
 *   ifconfig IFNAME A.B.C.D[/BITS] [peer A.B.C.D] [mtu N] [up|down]
 *   dhcp IFNAME
 * Unknown keywords are passed to 'unknown' (OS glue: "interface" lines
 * that open SANA-II devices / TUN devices). Returns 0 or -errno and fills
 * err with a message. */
int rt_config_line(const char *line, char *err, u32 errlen,
                   int (*unknown)(int argc, char **argv, char *err, u32 errlen));
/* Split a line into whitespace separated words (quotes allowed); modifies
 * line in place. Returns argc. */
int rt_config_split(char *line, char **argv, int max);
/* Parse dotted quad (network order result). 0 on success. */
int rt_parse_ipv4(const char *s, u32 *addr);
/* Format network order address into buf (>=16 bytes). Returns buf. */
char *rt_format_ipv4(u32 addr, char *buf);

/* ---- resolver (src/core/resolv.c), blocking, called WITHOUT the lock ----
 * Uses its own internal UDP socket. Results: count of addresses or a
 * negative h_errno style code. */
#define RT_HOST_NOT_FOUND 1
#define RT_TRY_AGAIN      2
#define RT_NO_RECOVERY    3
#define RT_NO_DATA        4
int  rt_dns_add_server(u32 addr);
void rt_dns_clear_servers(void);
int  rt_dns_get_servers(u32 *out, int max);
void rt_dns_set_search(const char *domain);
int  rt_hosts_add(const char *name, u32 addr);   /* static host table */
void rt_hosts_clear(void);
/* Forward lookup; numeric strings and the static table are handled
 * without network traffic. canon (may be NULL) gets the canonical name. */
int  rt_resolve_name(const char *name, u32 *addrs, int max, char *canon, u32 canonlen);
/* Reverse lookup (PTR). Returns 0 and the name, or -h_errno. */
int  rt_resolve_addr(u32 addr, char *name, u32 namelen);
extern int rt_dns_disallowed;   /* MiamiDisallowDNS() */
/* BIND style raw interface (miami.library res_* / dn_* calls), called
 * WITHOUT the lock. res_mkquery: only op 0 (QUERY); returns length or -1.
 * res_send: answer length (copy truncated to anslen) or -h_errno.
 * res_query/querydomain/search: answer length, or -h_errno when the
 * rcode is not NOERROR or there are no answers (BIND semantics).
 * dn_expand: compressed length consumed or -1; dn_skipname likewise. */
int  rt_res_mkquery(int op, const char *dname, int cls, int type, u8 *buf, int buflen);
int  rt_res_send(const u8 *msg, int msglen, u8 *answer, int anslen);
int  rt_res_query(const char *dname, int cls, int type, u8 *answer, int anslen);
int  rt_res_querydomain(const char *name, const char *domain, int cls, int type,
                        u8 *answer, int anslen);
int  rt_res_search(const char *dname, int cls, int type, u8 *answer, int anslen);
int  rt_dn_expand(const u8 *msg, const u8 *eom, const u8 *src, char *dst, int dstsize);
int  rt_dn_skipname(const u8 *src, const u8 *eom);

/* ---- static netdb tables (src/core/netdb.c) ---- */
struct rt_servent_info { const char *name; const char *aliases[4]; u16 port; const char *proto; };
struct rt_protoent_info { const char *name; const char *aliases[4]; int proto; };
const struct rt_servent_info *rt_getservbyname(const char *name, const char *proto);
const struct rt_servent_info *rt_getservbyport(u16 port /* host order */, const char *proto);
const struct rt_protoent_info *rt_getprotobyname(const char *name);
const struct rt_protoent_info *rt_getprotobynumber(int proto);

/* ---- DHCP client (src/core/dhcp.c, RFC 2131) ---- */
#define RT_DHCP_EV_BOUND   1
#define RT_DHCP_EV_RENEWED 2
#define RT_DHCP_EV_LOST    3
int  rt_dhcp_start(struct rt_netif *nif, void (*cb)(struct rt_netif *nif, int event));
void rt_dhcp_stop(struct rt_netif *nif);  /* releases the lease */
/* Is DHCP configured and bound on nif? */
int  rt_dhcp_bound(const struct rt_netif *nif);

#endif
