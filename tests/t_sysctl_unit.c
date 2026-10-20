/*
 * Riptide - unit tests of the sysctl registry (sysctl.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>

#include "rt/riptide.h"
#include "../src/core/sock_internal.h"

/* TCP may not be in the library yet: weak stubs keep this test linkable. */
__attribute__((weak)) struct rt_tcp_stats rt_tcpstat;
__attribute__((weak)) const struct rt_proto_ops rt_tcp_ops;
__attribute__((weak)) void rt_tcp_init(void) {}
__attribute__((weak)) void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip) { rt_buf_free(b); }
__attribute__((weak)) void rt_tcp_icmp_error(u8 t, u8 c, u32 i, const struct rt_iphdr *in, u32 l) {}
__attribute__((weak)) int rt_tcp_list(struct rt_pcb_info *o, int m) { return 0; }

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static s32 v_int = 5, v_ro = 7, h_value = 100;
static char v_str[8] = "abc";
static struct { u32 a, b; } v_blob = { 1, 2 };
static int handler_calls;

static int my_handler(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	handler_calls++;
	if (oldlen) {
		if (old) {
			if (*oldlen < 4)
				return -RT_ENOMEM;
			memcpy(old, &h_value, 4);
		}
		*oldlen = 4;
	}
	if (newp) {
		s32 v;
		if (newlen != 4)
			return -RT_EINVAL;
		memcpy(&v, newp, 4);
		h_value = v * 2;
	}
	return 0;
}

static const struct rt_sysctl tab[] = {
	{ "test.int", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 99, 1, 1 }, &v_int, -10, 10, NULL, "int" },
	{ "test.ro", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 99, 1, 2 }, &v_ro, 0, 100, NULL, "ro" },
	{ "test.str", RT_SYSCTL_STRING, RT_SYSCTL_RW, 0, { 0 }, v_str, 0, sizeof(v_str), NULL, "str" },
	{ "test.blob", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 0, { 0 }, &v_blob, 0, sizeof(v_blob), NULL, "blob" },
	{ "test.handler", RT_SYSCTL_INT, RT_SYSCTL_RW, 2, { 99, 2 }, NULL, 0, 0, my_handler, "h" },
};
static const struct rt_sysctl dup_tab[] = {
	{ "test.int", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &v_ro, 0, 0, NULL, "dup" },
};

int main(void)
{
	s32 v, mib[3] = { 99, 1, 1 };
	u32 len;
	char buf[64];
	const struct rt_sysctl *e;
	int i, n, found;

	rt_lock();
	rt_sysctl_init();
	rt_resolv_init();
	rt_socket_init();
	rt_sysctl_register(tab, (int)RT_ARRAY_SIZE(tab));
	rt_sysctl_register(tab, (int)RT_ARRAY_SIZE(tab));   /* ignored */
	rt_sysctl_register(dup_tab, 1);                      /* first wins */

	/* INT by name */
	len = sizeof(v);
	CHECK(rt_sysctl_byname("test.int", &v, &len, NULL, 0) == 0 && v == 5 && len == 4);
	v = 9;
	CHECK(rt_sysctl_byname("test.int", NULL, NULL, &v, 4) == 0 && v_int == 9);
	v = 11;
	CHECK(rt_sysctl_byname("test.int", NULL, NULL, &v, 4) == -RT_EINVAL && v_int == 9);
	v = -10;
	CHECK(rt_sysctl_byname("test.int", NULL, NULL, &v, 4) == 0 && v_int == -10);
	CHECK(rt_sysctl_byname("test.int", NULL, NULL, &v, 2) == -RT_EINVAL);
	len = 2;
	CHECK(rt_sysctl_byname("test.int", &v, &len, NULL, 0) == -RT_ENOMEM);
	len = 0;
	CHECK(rt_sysctl_byname("test.int", NULL, &len, NULL, 0) == 0 && len == 4);
	CHECK(rt_sysctl_byname("test.nothere", NULL, NULL, NULL, 0) == -RT_ENOENT);
	CHECK(rt_sysctl_byname("test", NULL, NULL, NULL, 0) == -RT_ENOENT);
	/* old + new at once: old value first */
	v = 3;
	{
		s32 old;
		len = 4;
		CHECK(rt_sysctl_byname("test.int", &old, &len, &v, 4) == 0 && old == -10 && v_int == 3);
	}

	/* MIB */
	len = 4;
	CHECK(rt_sysctl_bymib(mib, 3, &v, &len, NULL, 0) == 0 && v == 3);
	mib[2] = 2;
	len = 4;
	CHECK(rt_sysctl_bymib(mib, 3, &v, &len, NULL, 0) == 0 && v == 7);
	CHECK(rt_sysctl_bymib(mib, 3, NULL, NULL, &v, 4) == -RT_EPERM);
	CHECK(rt_sysctl_bymib(mib, 2, &v, &len, NULL, 0) == -RT_ENOENT);
	CHECK(rt_sysctl_bymib(mib, 0, &v, &len, NULL, 0) == -RT_ENOENT);
	CHECK(rt_sysctl_bymib(mib, 99, &v, &len, NULL, 0) == -RT_ENOENT);

	/* read only */
	v = 1;
	CHECK(rt_sysctl_byname("test.ro", NULL, NULL, &v, 4) == -RT_EPERM && v_ro == 7);
	CHECK(rt_sysctl_set_text("test.ro", "3") == -RT_EPERM);

	/* string */
	len = sizeof(buf);
	CHECK(rt_sysctl_byname("test.str", buf, &len, NULL, 0) == 0 && !strcmp(buf, "abc") && len == 4);
	CHECK(rt_sysctl_byname("test.str", NULL, NULL, "hello", 5) == 0 && !strcmp(v_str, "hello"));
	CHECK(rt_sysctl_byname("test.str", NULL, NULL, "1234567", 8) == 0 && !strcmp(v_str, "1234567"));
	CHECK(rt_sysctl_byname("test.str", NULL, NULL, "12345678", 8) == -RT_EINVAL);
	CHECK(rt_sysctl_byname("test.str", NULL, NULL, "a\0b", 3) == -RT_EINVAL);
	len = 3;
	CHECK(rt_sysctl_byname("test.str", buf, &len, NULL, 0) == -RT_ENOMEM && !strcmp(buf, "12"));

	/* opaque */
	{
		struct { u32 a, b; } out;
		len = sizeof(out);
		CHECK(rt_sysctl_byname("test.blob", &out, &len, NULL, 0) == 0 && out.a == 1 && out.b == 2);
		len = 4;
		CHECK(rt_sysctl_byname("test.blob", &out, &len, NULL, 0) == -RT_ENOMEM);
		CHECK(rt_sysctl_byname("test.blob", NULL, NULL, &out, 8) == -RT_EPERM);
	}

	/* handler */
	v = 21;
	CHECK(rt_sysctl_byname("test.handler", NULL, NULL, &v, 4) == 0 && h_value == 42);
	len = 4;
	CHECK(rt_sysctl_byname("test.handler", &v, &len, NULL, 0) == 0 && v == 42 && handler_calls == 2);

	/* text interface */
	CHECK(rt_sysctl_set_text("test.int", "7") == 0 && v_int == 7);
	CHECK(rt_sysctl_set_text("test.int=-3", NULL) == 0 && v_int == -3);
	CHECK(rt_sysctl_set_text("test.int 4", NULL) == 0 && v_int == 4);
	CHECK(rt_sysctl_set_text("test.int", "0x0a") == 0 && v_int == 10);
	CHECK(rt_sysctl_set_text("test.int", "on") == 0 && v_int == 1);
	CHECK(rt_sysctl_set_text("test.int", "off") == 0 && v_int == 0);
	CHECK(rt_sysctl_set_text("test.int", "12") == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("test.int", "x") == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("test.int", "3x") == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("test.int", "") == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("test.int", "99999999999") == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("test.int", NULL) == -RT_EINVAL);
	CHECK(rt_sysctl_set_text("nope=1", NULL) == -RT_ENOENT);
	CHECK(rt_sysctl_set_text("test.str=xyz", NULL) == 0 && !strcmp(v_str, "xyz"));
	CHECK(rt_sysctl_set_text("test.handler", "5") == 0 && h_value == 10);
	e = rt_sysctl_find("test.int");
	CHECK(e && rt_sysctl_get_text(e, buf, sizeof(buf)) == 1 && !strcmp(buf, "0"));
	e = rt_sysctl_find("test.str");
	CHECK(e && rt_sysctl_get_text(e, buf, sizeof(buf)) == 3 && !strcmp(buf, "xyz"));
	e = rt_sysctl_find("test.blob");
	CHECK(e && rt_sysctl_get_text(e, buf, sizeof(buf)) > 0 && strstr(buf, "8 bytes"));
	e = rt_sysctl_find("test.handler");
	CHECK(e && rt_sysctl_get_text(e, buf, sizeof(buf)) == 2 && !strcmp(buf, "10"));

	/* iteration covers everything once */
	for (i = 0, n = 0, found = 0; (e = rt_sysctl_iter(i)) != NULL; i++, n++)
		if (!strcmp(e->name, "test.handler"))
			found++;
	CHECK(found == 1 && n >= 5 + 6 + 3);
	CHECK(rt_sysctl_iter(-1) == NULL);

	/* generic entries */
	CHECK(rt_sysctl_set_text("kern.hostname", "amiga") == 0 && !strcmp(rt_hostname, "amiga"));
	{
		s32 kmib[2] = { 1, 10 };
		len = sizeof(buf);
		CHECK(rt_sysctl_bymib(kmib, 2, buf, &len, NULL, 0) == 0 && !strcmp(buf, "amiga"));
		kmib[1] = 22;
		CHECK(rt_sysctl_bymib(kmib, 2, NULL, NULL, "example.org", 11) == 0 &&
		      !strcmp(rt_domainname, "example.org"));
	}
	{
		struct rt_udp_stats us;
		rt_udpstat.InDatagrams = 1234;
		len = sizeof(us);
		CHECK(rt_sysctl_byname("net.inet.udp.stats", &us, &len, NULL, 0) == 0 &&
		      len == sizeof(us) && us.InDatagrams == 1234);
		len = sizeof(struct rt_tcp_stats);
		CHECK(rt_sysctl_byname("net.inet.tcp.stats", NULL, &len, NULL, 0) == 0 &&
		      len == sizeof(struct rt_tcp_stats));
		len = sizeof(struct rt_ip_stats);
		CHECK(rt_sysctl_byname("net.inet.ip.stats", NULL, &len, NULL, 0) == 0);
		CHECK(rt_sysctl_byname("net.inet.icmp.stats", NULL, &len, NULL, 0) == 0);
	}
	{
		s32 smib[2] = { 11, 0 }, dmib[3] = { 9, 0, 2 }, tmib[4] = { 4, 2, 6, 8 };
		len = 4;
		CHECK(rt_sysctl_bymib(smib, 2, &v, &len, NULL, 0) == 0 && v == RT_SOMAXCONN);
		v = 1000;
		CHECK(rt_sysctl_bymib(smib, 2, NULL, NULL, &v, 4) == -RT_EINVAL);
		len = 4;
		CHECK(rt_sysctl_bymib(dmib, 3, &v, &len, NULL, 0) == 0 && v == 1);
		len = 4;
		CHECK(rt_sysctl_bymib(tmib, 4, &v, &len, NULL, 0) == 0 && v > 0);
		CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	}
	rt_unlock();

	printf("t_sysctl_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
