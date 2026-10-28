/*
 * Riptide - DNS message helpers shared by resolv.c and its unit tests.
 * Not part of the contract.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_RESOLV_INTERNAL_H
#define RT_RESOLV_INTERNAL_H

#include "rt/rt_types.h"

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_TYPE_PTR    12
#define DNS_TYPE_AAAA   28
#define DNS_TYPE_OPT    41
#define DNS_CLASS_IN    1
#define DNS_NAMEBUF     256
#define DNS_MAX_CNAME   8
#define DNS_IGNORE      (-1000)

struct dns_result {
	u32 *addrs;               /* A results (network order) */
	rt_in6_addr *addrs6;      /* AAAA results */
	int  max;
	int  naddr;
	u32  ttl;                 /* smallest TTL seen in the answer chain */
	char canon[DNS_NAMEBUF];  /* end of the CNAME chain */
	char name[DNS_NAMEBUF];   /* PTR result */
	char target[DNS_NAMEBUF]; /* scratch */
	char tmp[DNS_NAMEBUF];    /* scratch */
};

int dns_name_eq(const char *a, const char *b);
int dns_build_query(u8 *buf, u32 buflen, u16 id, const char *name, u16 qtype);
int dns_build_query_class(u8 *buf, u32 buflen, u16 id, const char *name, u16 qclass, u16 qtype);
/* EDNS0 OPT record: append to a query / find in a message */
int dns_add_opt(u8 *buf, u32 len, u32 buflen, u16 udpsize);
int dns_find_opt(const u8 *msg, u32 len, u16 *udpsize);
int dns_expand(const u8 *msg, u32 len, u32 off, char *out, u32 outlen, u32 *next);
int dns_expand_ex(const u8 *msg, u32 len, u32 off, char *out, u32 outlen, u32 *next,
                  int lenient);
/* mdns.c: A/AAAA records for name in a reply to our legacy unicast query
 * (count, *ttl the smallest TTL; DNS_IGNORE when it is not one) */
int mdns_parse_answer(const u8 *m, u32 len, u16 id, const char *name, u16 qtype, u32 *a4,
                      rt_in6_addr *a6, int max, u32 *ttl);
int dns_parse_response(const u8 *msg, u32 len, u16 id, const char *qname, u16 qtype,
                       struct dns_result *res);

#endif
