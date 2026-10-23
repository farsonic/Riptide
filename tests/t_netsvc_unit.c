/*
 * Riptide - unit tests of the SNTP arithmetic (offset/delay, NTP time stamp
 * conversion across the 2036 era boundary), the ntp/timezone/mdns command
 * parsing and the mDNS answer parser. No network.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/resolv_internal.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct ntp_tv { u32 sec, usec; };
int rt_ntp_compute(const struct ntp_tv *t1, const struct ntp_tv *t2, const struct ntp_tv *t3,
                   const struct ntp_tv *t4, s32 *off_sec, s32 *off_usec, s32 *delay_us);
void rt_ntp_ts_to_tv(const u8 *p, struct ntp_tv *t);

static struct ntp_tv tv(u32 s, u32 u) { struct ntp_tv t = { s, u }; return t; }

static void test_compute(void)
{
	struct ntp_tv t1, t2, t3, t4;
	s32 os, ou, d;

	/* server 100.25 s ahead, 20 ms each way, 1 ms processing */
	t1 = tv(1000000000u, 0);
	t2 = tv(1000000100u, 270000);
	t3 = tv(1000000100u, 271000);
	t4 = tv(1000000000u, 41000);
	CHECK(rt_ntp_compute(&t1, &t2, &t3, &t4, &os, &ou, &d) == 0);
	CHECK(os == 100 && ou == 250000);
	CHECK(d == 40000);
	/* server behind by 0.5 s: offset -1 s + 500000 us */
	t2 = tv(999999999u, 520000);
	t3 = tv(999999999u, 521000);
	CHECK(rt_ntp_compute(&t1, &t2, &t3, &t4, &os, &ou, &d) == 0);
	CHECK(os == -1 && ou == 500000);
	/* (0.52 + 0.48) / 2 = 0.5 s */
	t2 = tv(1000000000u, 520000);
	t3 = tv(1000000000u, 521000);
	t4 = tv(1000000000u, 41000);
	CHECK(rt_ntp_compute(&t1, &t2, &t3, &t4, &os, &ou, &d) == 0);
	CHECK(os == 0 && ou == 500000 && d == 40000);
	/* a Unix clock that wrapped back to 1970 against a 2025 server */
	t1 = tv(5, 0);
	t2 = tv(1750000000u, 0);
	t3 = tv(1750000000u, 0);
	t4 = tv(5, 10000);
	CHECK(rt_ntp_compute(&t1, &t2, &t3, &t4, &os, &ou, &d) == 0);
	CHECK(os == 1749999994 && ou == 995000 && d == 10000);
	/* absurd delay: rejected */
	t4 = tv(5000, 0);
	CHECK(rt_ntp_compute(&t1, &t2, &t3, &t4, &os, &ou, &d) < 0);
}

static void test_era(void)
{
	u8 p[8];
	struct ntp_tv t;

	/* 2025-01-01 00:00:00 UTC = 1735689600 Unix = 3944678400 NTP era 0 */
	rt_put_be32(p, 3944678400u);
	rt_put_be32(p + 4, 0x80000000u);
	rt_ntp_ts_to_tv(p, &t);
	CHECK(t.sec == 1735689600u && t.usec == 500000);
	/* 2036-02-07 06:28:16 UTC: NTP era 1 second 0 = Unix 2085978496 */
	rt_put_be32(p, 0);
	rt_put_be32(p + 4, 0);
	rt_ntp_ts_to_tv(p, &t);
	CHECK(t.sec == 2085978496u && t.usec == 0);
	/* 2040-01-01 00:00:00 UTC: era 1 second 123010304 */
	rt_put_be32(p, 123010304u);
	rt_ntp_ts_to_tv(p, &t);
	CHECK(t.sec == 2208988800u);
	/* the last microsecond of a second */
	rt_put_be32(p, 3944678400u);
	rt_put_be32(p + 4, 0xffffffffu);
	rt_ntp_ts_to_tv(p, &t);
	CHECK(t.sec == 1735689600u && t.usec == 999999);
}

static int cmd(const char *line, char *out, u32 len)
{
	int r;

	rt_lock();
	r = rt_config_line(line, out, len, NULL);
	rt_unlock();
	return r;
}

static void test_commands(void)
{
	char out[256];

	CHECK(cmd("timezone +10:00", out, sizeof(out)) == 0);
	CHECK(rt_tz_offset == 36000 && rt_tz_configured);
	CHECK(cmd("timezone -3:30", out, sizeof(out)) == 0);
	CHECK(rt_tz_offset == -12600);
	CHECK(cmd("timezone 5", out, sizeof(out)) == 0);
	CHECK(rt_tz_offset == 18000);
	CHECK(cmd("timezone", out, sizeof(out)) == 0 && strstr(out, "+05:00"));
	CHECK(cmd("timezone +15:00", out, sizeof(out)) < 0);
	CHECK(cmd("timezone +1:75", out, sizeof(out)) < 0);
	CHECK(cmd("timezone x", out, sizeof(out)) < 0);
	CHECK(cmd("timezone auto", out, sizeof(out)) == 0 && !rt_tz_configured && rt_tz_offset == rt_tz_system);

	CHECK(cmd("ntp status", out, sizeof(out)) == 0 && !strncmp(out, "NTP: off.", 9));
	CHECK(cmd("ntp interval 10", out, sizeof(out)) < 0);
	CHECK(cmd("ntp interval 7200", out, sizeof(out)) == 0 && !strcmp(out, "NTP: interval 7200 s"));
	CHECK(cmd("ntp step 250", out, sizeof(out)) == 0 && !strcmp(out, "NTP: step 250 ms"));
	CHECK(cmd("ntp battclock on", out, sizeof(out)) == 0 && rt_ntp_battclock == 1);
	CHECK(cmd("ntp battclock off", out, sizeof(out)) == 0 && rt_ntp_battclock == 0);
	CHECK(cmd("ntp battclock maybe", out, sizeof(out)) < 0);
	CHECK(cmd("ntp a b c d e", out, sizeof(out)) < 0);
	CHECK(cmd("ntp 192.0.2.1 time.example.org", out, sizeof(out)) == 0 && strstr(out, "2 servers"));
	CHECK(cmd("ntp status", out, sizeof(out)) == 0 && strstr(out, "not synchronised"));
	CHECK(cmd("ntp off", out, sizeof(out)) == 0 && !strcmp(out, "NTP: off"));
	CHECK(cmd("ntp", out, sizeof(out)) == 0 && !strncmp(out, "NTP: off.", 9));
	CHECK(cmd("ntp STATUS", out, sizeof(out)) == 0 && !strncmp(out, "NTP: off.", 9));   /* Amiga style */
	CHECK(cmd("ntp BattClock ON", out, sizeof(out)) == 0 && rt_ntp_battclock == 1);
	CHECK(cmd("timezone AUTO", out, sizeof(out)) == 0);

	CHECK(cmd("mdns status", out, sizeof(out)) == 0 && !strncmp(out, "mDNS: ", 6));
	CHECK(cmd("mdns off", out, sizeof(out)) == 0);
	CHECK(cmd("mdns status", out, sizeof(out)) == 0 && !strcmp(out, "mDNS: off"));
	CHECK(cmd("mdns on", out, sizeof(out)) == 0);
	CHECK(cmd("mdns sideways", out, sizeof(out)) < 0);
}

/* ---- mDNS answer parser ---- */
struct msg { u8 b[512]; u32 len; };
static void put8(struct msg *m, u8 v) { m->b[m->len++] = v; }
static void put16(struct msg *m, u16 v) { rt_put_be16(m->b + m->len, v); m->len += 2; }
static void put32(struct msg *m, u32 v) { rt_put_be32(m->b + m->len, v); m->len += 4; }
static void putname(struct msg *m, const char *n)
{
	while (*n) {
		const char *d = strchr(n, '.');
		u32 l = d ? (u32)(d - n) : (u32)strlen(n);
		put8(m, (u8)l);
		memcpy(m->b + m->len, n, l);
		m->len += l;
		n += l;
		if (*n == '.')
			n++;
	}
	put8(m, 0);
}

static void test_mdns_parse(void)
{
	struct msg m;
	u32 a4[4], ttl;
	rt_in6_addr a6[4];
	int n;

	/* legacy unicast answer: echoed question, A with cache flush (some
	 * responders set it anyway), an AAAA (link-local, skipped) and a
	 * global AAAA in the additional section */
	memset(&m, 0, sizeof(m));
	put16(&m, 0x1234); put16(&m, 0x8400); put16(&m, 1); put16(&m, 2); put16(&m, 0); put16(&m, 1);
	putname(&m, "Beta.local"); put16(&m, DNS_TYPE_A); put16(&m, 1);
	putname(&m, "beta.local"); put16(&m, DNS_TYPE_A); put16(&m, 0x8001); put32(&m, 10); put16(&m, 4);
	put8(&m, 192); put8(&m, 168); put8(&m, 1); put8(&m, 7);
	put16(&m, 0xc00c); put16(&m, DNS_TYPE_AAAA); put16(&m, 0x8001); put32(&m, 10); put16(&m, 16);
	put16(&m, 0xfe80); m.len += 12; put16(&m, 1);
	put16(&m, 0xc00c); put16(&m, DNS_TYPE_AAAA); put16(&m, 1); put32(&m, 5); put16(&m, 16);
	put16(&m, 0x2001); put16(&m, 0xdb8); m.len += 10; put16(&m, 2);
	n = mdns_parse_answer(m.b, m.len, 0x1234, "beta.local", DNS_TYPE_A, a4, NULL, 4, &ttl);
	CHECK(n == 1 && a4[0] == rt_htonl(0xc0a80107u) && ttl == 10);
	n = mdns_parse_answer(m.b, m.len, 0x1234, "beta.local.", DNS_TYPE_AAAA, NULL, a6, 4, &ttl);
	CHECK(n == 1 && a6[0].b[0] == 0x20 && a6[0].b[15] == 2 && ttl == 5);
	CHECK(mdns_parse_answer(m.b, m.len, 0x1235, "beta.local", DNS_TYPE_A, a4, NULL, 4, &ttl) == DNS_IGNORE);
	CHECK(mdns_parse_answer(m.b, m.len, 0x1234, "gamma.local", DNS_TYPE_A, a4, NULL, 4, &ttl) == DNS_IGNORE);
	/* truncated anywhere: never more than what is there, never a crash */
	{
		u32 l;
		for (l = 0; l < m.len; l++)
			CHECK(mdns_parse_answer(m.b, l, 0x1234, "beta.local", DNS_TYPE_A, a4, NULL, 4, &ttl) <= 1);
	}
	/* a query (QR clear) is not an answer */
	m.b[2] = 0;
	CHECK(mdns_parse_answer(m.b, m.len, 0x1234, "beta.local", DNS_TYPE_A, a4, NULL, 4, &ttl) == DNS_IGNORE);
}

int main(void)
{
	rt_init();
	test_compute();
	test_era();
	test_commands();
	test_mdns_parse();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
