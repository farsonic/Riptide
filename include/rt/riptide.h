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
#include "rt_ip6.h"
#include "rt_bsd.h"
#include "rt_socket.h"
#include "rt_sysctl.h"
#include "rt_stats.h"
#include "rt_ctl.h"
#include "rt_diag.h"
#include "rt_netsvc.h"
#include "rt_qos.h"

#include "rt_version.h"

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
	u32 laddr, faddr;    /* network order (IPv4 sockets) */
	u32 sendq, recvq;    /* bytes queued */
	u32 rtt_us, cwnd;    /* TCP only */
	u32 retrans;
	u8  v6;              /* an AF_INET6 socket: see laddr6/faddr6 */
	u8  v6only;
	rt_in6_addr laddr6, faddr6;  /* both families (IPv4 as IPv4-mapped) */
};
int rt_tcp_list(struct rt_pcb_info *out, int max);   /* returns total count */
int rt_tcp_info_list(struct rt_ctl_tcpinfo *out, int max);  /* net.riptide.tcpinfo */
/* "background ..." (configuration and RIPTIDE port), see tcp_ledbat.c */
int rt_tcp_bg_command(int argc, char **argv, char *out, u32 outlen);
void rt_tcp_bg_status(char *buf, u32 len);   /* "" without rules */
/* one line on socket buffer memory for STATUS */
void rt_tcp_mem_status(char *buf, u32 len);
/* the same as numbers (KB); returns 0 normal, 1 under pressure, 2 at the limit */
u8 rt_tcp_mem_info(u32 *used_kb, u32 *pressure_kb, u32 *high_kb);
int rt_udp_list(struct rt_pcb_info *out, int max);
int rt_raw_list(struct rt_pcb_info *out, int max);

/* ---- configuration file (src/core/config.c) ----
 * Line oriented, '#' comments. Generic keywords handled by the core:
 *   hostname NAME | domain NAME | nameserver A.B.C.D | host NAME A.B.C.D
 *   route add|del DEST[/BITS]|default GATEWAY
 *   sysctl NAME=VALUE
 *   ifconfig IFNAME A.B.C.D[/BITS] [peer A.B.C.D] [mtu N] [up|down]
 *   dhcp IFNAME
 * IPv6 (see docs/AMIGA.md):
 *   ipv6 [IFNAME] on|off
 *   ifconfig IFNAME inet6 ADDR[/PLEN] [nodad] [delete] | ifconfig IFNAME inet6 [-]ifdisabled
 *   route add|del [-inet6] DEST[/PLEN]|default [via] GW[%IF] [dev IF] (IPv6 when -inet6
 *     is given or an address contains ':')
 *   nameserver IPV6[%IF] | host NAME IPV6 | arp add|del IPV6 [MAC] dev IF
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
void rt_dns_remove_server(u32 addr);
void rt_dns_clear_servers(void);
int  rt_dns_get_servers(u32 *out, int max);
/* one domain or a list (spaces or commas), at most six */
void rt_dns_set_search(const char *domain);
const char *rt_dns_search_list(void);    /* "a.example b.example", "" = none */
int  rt_hosts_add(const char *name, u32 addr);   /* static host table */
int  rt_hosts_remove(const char *name);          /* every entry of that name; -ENOENT */
void rt_hosts_clear(void);
/* Forward lookup; numeric strings and the static table are handled
 * without network traffic. canon (may be NULL) gets the canonical name. */
int  rt_resolve_name(const char *name, u32 *addrs, int max, char *canon, u32 canonlen);
/* Reverse lookup (PTR). Returns 0 and the name, or -h_errno. */
int  rt_resolve_addr(u32 addr, char *name, u32 namelen);
extern int rt_dns_disallowed;   /* RiptideDisallowDNS() */
extern s32 rt_dns_edns_size;    /* dns.edns_size: EDNS0 UDP payload, 0 = off */
extern s32 rt_dns_tcp;          /* dns.tcp_fallback: TC answers again over TCP */
/* "dns edns_size N | edns on|off | tcp on|off | negttl N | failttl N | flush |
 * reset | status" (lock held) */
int  rt_dns_command(int argc, char **argv, char *out, u32 outlen);
/* lookups answered by the cache, by a cached negative answer, and not
 * repeated after a failure (dns.stats.*) */
void rt_dns_cache_stats(u32 *hits, u32 *neg_hits, u32 *fail_hits);
/* BIND style raw interface (res_* / dn_* calls for library clients), called
 * WITHOUT the lock. res_mkquery: only op 0 (QUERY); returns length or -1.
 * res_send: answer length (copy truncated to anslen) or -h_errno.
 * res_query/querydomain/search: answer length, or -h_errno when the
 * rcode is not NOERROR or there are no answers (BIND semantics).
 * dn_expand: compressed length consumed or -1; dn_skipname likewise. */
/* IPv6: AAAA lookups (numeric, hosts table and "localhost" -> ::1 without
 * traffic), PTR in ip6.arpa (in-addr.arpa for IPv4-mapped addresses), and
 * servers of either family (IPv4 servers are kept IPv4-mapped; servers
 * learned from RAs or DHCPv6 expire). */
int  rt_resolve_name6(const char *name, rt_in6_addr *addrs, int max, char *canon, u32 canonlen);
/* A and AAAA in parallel (RFC 8305 3: an A answer waits at most
 * rt_he_resolution_delay ms for the AAAA one): AAAA results first, then
 * IPv4 ones IPv4-mapped, unsorted. family RT_AF_UNSPEC, RT_AF_INET or
 * RT_AF_INET6. Count or the error of rt_resolve_name(). No lock held. */
int  rt_resolve_name46(const char *name, rt_in6_addr *addrs, int max, int family, char *canon, u32 canonlen);

/* ---- Happy Eyeballs v2 (src/core/happy.c, RFC 8305) ---- */
extern s32 rt_he_enable, rt_he_attempt_delay, rt_he_resolution_delay, rt_he_prefer, rt_he_aaaa_filter;
struct rt_he_result {
	rt_in6_addr addr;         /* the address that answered (IPv4-mapped for IPv4) */
	u32 ms;                   /* time to connect, lookups included */
	u16 naddrs;               /* addresses the name had */
	u16 attempts;             /* connection attempts started */
	u16 index;                /* the winner's position in the order tried */
};
/* Connect a new socket of 'type' (0 = stream) in context c to host:port
 * (host order port; host a name or a literal), racing IPv6 and IPv4.
 * family RT_AF_UNSPEC, RT_AF_INET or RT_AF_INET6; timeout_ms 0 = 75 s.
 * Returns the connected descriptor (blocking mode) or -errno; res may be
 * NULL. Called without the lock. */
int  rt_he_connect(struct rt_sockctx *c, const char *host, u16 port, int family, int type, u32 timeout_ms,
                   struct rt_he_result *res);
/* IPv6 can reach global addresses (a route and a source); lock held */
int  rt_he_v6_usable(void);
/* "happy_eyeballs on|off | delay MS | resolution_delay MS |
 *  prefer auto|ipv6|ipv4 | aaaa_filter on|off | status" */
int  rt_he_command(int argc, char **argv, char *out, u32 outlen);
void rt_he_init(void);
int  rt_resolve_addr6(const rt_in6_addr *addr, char *name, u32 namelen);
int  rt_hosts_add6(const char *name, const rt_in6_addr *addr);
/* All servers (IPv4-mapped for IPv4), in order; returns the total. */
int  rt_dns_get_servers6(rt_in6_addr *out, u32 *ifindex, u8 *source, int max);
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
/* networks (getnetent()): net in host order, like struct netent */
struct rt_netent_info { const char *name; const char *aliases[4]; u32 net; };
/* entry i of each table, NULL past the end (getservent() & co.) */
const struct rt_servent_info *rt_servent_at(unsigned i);
const struct rt_protoent_info *rt_protoent_at(unsigned i);
const struct rt_netent_info *rt_netent_at(unsigned i);

/* ---- DHCP client (src/core/dhcp.c, RFC 2131) ---- */
#define RT_DHCP_EV_BOUND   1
#define RT_DHCP_EV_RENEWED 2
#define RT_DHCP_EV_LOST    3
int  rt_dhcp_start(struct rt_netif *nif, void (*cb)(struct rt_netif *nif, int event));
void rt_dhcp_stop(struct rt_netif *nif);  /* releases the lease */
/* Is DHCP configured and bound on nif? */
int  rt_dhcp_bound(const struct rt_netif *nif);
/* DHCP state of nif for tools (RT_DHCPS_* in rt_ctl.h). Returns
 * RT_DHCPS_NONE if no client runs on nif. Lease fields are filled when
 * bound (server/router network order, seconds). */
struct rt_dhcp_info {
	int state;
	u32 server, router, addr, lease_s, left_s;
	u32 ll_addr;          /* link-local address (network order), 0 = none */
	u8  ll_state;         /* RT_LL_* */
	u8  nroutes;          /* classless static routes (option 121/249) */
	u32 ll_conflicts;
};
int  rt_dhcp_get_info(const struct rt_netif *nif, struct rt_dhcp_info *info);
/* Restart the lease (DHCPREQUEST now when bound, else rediscover). */
int  rt_dhcp_renew(struct rt_netif *nif);
/* Option decoding, exported for tests. Classless routes (121/249, RFC
 * 3442): count (at most max stored), -1 if malformed. Domain search (119,
 * RFC 3397): space-separated list, count or -1 if malformed. */
#define RT_DHCP_MAX_ROUTES  16
#define RT_DHCP_SEARCH_MAX  6
#define RT_DHCP_SEARCH_LEN  256
struct rt_dhcp_route { u32 dst, mask, gw; };    /* network order, gw 0 = on-link */
int  rt_dhcp_parse_classless(const u8 *p, u32 len, struct rt_dhcp_route *out, int max);
int  rt_dhcp_parse_search(const u8 *p, u32 len, char *out, u32 outlen);
/* Link-local fallback for nif's DHCP client: on (1), off (0), -1 =
 * follow net.inet.ip.linklocal, -2 = unchanged; wait_s > 0 overrides
 * net.inet.ip.linklocal_wait.
 * Takes effect at once (off drops a link-local address). */
int  rt_dhcp_set_linklocal(struct rt_netif *nif, int on, int wait_s);
int  rt_dhcp_get_linklocal(const struct rt_netif *nif, int *wait_s);

/* ---- IPv4 link-local addresses (src/core/ipv4ll.c, RFC 3927) ---- */
#define RT_LL_OFF      0
#define RT_LL_PROBE    1      /* choosing a 169.254 address */
#define RT_LL_ANNOUNCE 2      /* claimed, announcing */
#define RT_LL_BOUND    3
extern s32 rt_ll_enable;      /* net.inet.ip.linklocal */
extern s32 rt_ll_wait_s;      /* net.inet.ip.linklocal_wait */
int  rt_ipv4ll_is_ll(u32 addr);                  /* 169.254/16? */
int  rt_ipv4ll_start(struct rt_netif *nif);       /* dhcp.c, tests */
void rt_ipv4ll_stop(struct rt_netif *nif);        /* drops the address */
/* the claimed address (0 = none); state RT_LL_*, conflicts so far */
u32  rt_ipv4ll_info(const struct rt_netif *nif, u8 *state, u32 *conflicts);

#endif
