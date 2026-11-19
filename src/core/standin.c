/*
 * Riptide - stand-in IPv4 addresses for IPv6 hosts, so that programs that know only IPv4 reach
 * hosts that have only IPv6 addresses: RFC 6535's "Bump-in-the-Host", function-mapping form,
 * without translating packets. Shared by the ports:
 *  - the Amiga (bsdsocket.library): the IPv4-only lookups (gethostbyname() & co., through
 *    rt_resolve_name4() in resolv.c) answer a name that has an IPv6 address but no IPv4 one with
 *    a stand-in; an AF_INET socket that connects or sends to a stand-in carries its traffic over
 *    IPv6 to the real address, and the program sees the stand-in as the peer (socket_api.c);
 *  - the Mac (src/mac/mactcp.c): the same for MacTCP programs, with its own resolver.
 * gethostbyaddr() of a stand-in gives back the name it was made for (rt_resolve_addr()).
 *
 * The stand-ins come from 198.18.0.0/15 (RFC 2544's benchmarking range, never seen on the
 * Internet), not RFC 6535's RFC 1918 ranges, which are often a home network's own; `standin
 * A.B.C.D/N` (or the Mac's `mactcp standin`) picks another range.
 *
 * A stand-in is a slot of a small table: slot i is base + 1 + i, so it keeps its IPv6 address
 * while it is in use (a socket or stream holds it), and an idle slot is given to a new address
 * only when every slot is taken, the one used longest ago first (RFC 6535 4.3: mappings are kept
 * a long time, as programs keep addresses longer than DNS says). Every call expects the stack
 * lock held (the Mac's lock is its single deferred task).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt/riptide.h"

#define SLOTS        RT_STANDIN_SLOTS
#define BASE_DEFAULT 0xC6120000UL       /* 198.18.0.0, host order */
#define MASK_DEFAULT 0xFFFE0000UL       /* /15 */

struct slot {
	rt_in6_addr a6;
	char name[64];                  /* what it was made for ("" for an address seen on its own) */
	u32 used_ms;                    /* rt_now_ms when it was last handed out or used */
	u16 holders;                    /* sockets and streams using it now */
	u8 taken;
};
static struct slot slots[SLOTS];
static u32 base = BASE_DEFAULT, mask = MASK_DEFAULT;    /* host order */
s32 rt_standin_enable = 1;              /* net.inet.standin.enable */
s32 rt_standin_prefer6;                 /* net.inet.standin.prefer6 */

static const struct rt_sysctl standin_sysctl[] = {
	{ "net.inet.standin.enable", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_standin_enable, 0, 1, NULL, "IPv4 stand-ins for IPv6-only hosts, for IPv4-only programs (RFC 6535)" },
	{ "net.inet.standin.prefer6", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_standin_prefer6, 0, 1, NULL, "stand-ins also for names with IPv4 addresses, so IPv6 is used" },
};

void rt_standin_init(void)
{
	rt_sysctl_register(standin_sysctl, (int)RT_ARRAY_SIZE(standin_sysctl));
}

/* a restart of the configuration: the default range again (the sysctls come back by themselves) */
void rt_standin_reset(void)
{
	if (base != BASE_DEFAULT || mask != MASK_DEFAULT) {
		base = BASE_DEFAULT;
		mask = MASK_DEFAULT;
		memset(slots, 0, sizeof slots);
	}
}

/* v4 (network order) is in the stand-in range */
int rt_standin_is(u32 v4)
{
	u32 h = rt_ntohl(v4), i = h - base - 1;
	return (h & mask) == base && i < SLOTS;
}

/* the stand-in (network order) for an IPv6 address, made or found again; the name, when there is
   one, is kept. 0: switched off, or every slot held */
u32 rt_standin_for(const rt_in6_addr *a6, const char *name)
{
	int i, pick = -1;
	if (!rt_standin_enable)
		return 0;
	for (i = 0; i < SLOTS; i++)
		if (slots[i].taken && !memcmp(&slots[i].a6, a6, sizeof *a6)) {
			pick = i;
			break;
		}
	if (pick < 0) {
		/* a free slot, else the idle one used longest ago */
		for (i = 0; i < SLOTS && pick < 0; i++)
			if (!slots[i].taken)
				pick = i;
		if (pick < 0) {
			u32 now = rt_now_ms(), oldest = 0;
			for (i = 0; i < SLOTS; i++)
				if (!slots[i].holders && now - slots[i].used_ms >= oldest) {
					oldest = now - slots[i].used_ms;
					pick = i;
				}
		}
		if (pick < 0)
			return 0;                       /* every slot held by an open socket */
		memset(&slots[pick], 0, sizeof slots[pick]);
		slots[pick].taken = 1;
		slots[pick].a6 = *a6;
	}
	if (name && *name)
		rt_strlcpy(slots[pick].name, name, sizeof slots[pick].name);
	slots[pick].used_ms = rt_now_ms();
	return rt_htonl(base + 1 + (u32)pick);
}

/* the IPv6 address of a stand-in, and the name it was made for; 0 if v4 is not one in use (or
   stand-ins are switched off: the address is then an ordinary IPv4 one, RFC 6535 3.1) */
int rt_standin_lookup(u32 v4, rt_in6_addr *a6, char *name, u32 namelen)
{
	struct slot *s;
	if (!rt_standin_enable || !rt_standin_is(v4))
		return 0;
	s = &slots[rt_ntohl(v4) - base - 1];
	if (!s->taken)
		return 0;
	if (a6)
		*a6 = s->a6;
	if (name && namelen)
		rt_strlcpy(name, s->name, namelen);
	s->used_ms = rt_now_ms();
	return 1;
}

/* a socket or stream starts using a stand-in (hold 1) or lets it go (hold 0): held slots are
   never given away */
void rt_standin_hold(u32 v4, int hold)
{
	struct slot *s;
	if (!rt_standin_is(v4))
		return;
	s = &slots[rt_ntohl(v4) - base - 1];
	if (hold) {
		s->holders++;
		s->used_ms = rt_now_ms();
	} else if (s->holders) {
		s->holders--;
	}
}

/* slots taken and held, for `standin status` and the tests */
void rt_standin_count(int *taken, int *held)
{
	int i;
	*taken = *held = 0;
	for (i = 0; i < SLOTS; i++) {
		*taken += slots[i].taken;
		*held += slots[i].holders != 0;
	}
}

static int set_range(const char *p)
{
	u32 a = 0, part, bits = 15, k;
	const char *e = p;
	for (k = 0; k < 4; k++) {
		part = (u32)rt_strtoul(p, &e, 10);
		if (e == p || part > 255 || (k < 3 && *e != '.'))
			return -1;
		a = a << 8 | part;
		p = e + 1;
	}
	if (*e == '/') {
		bits = (u32)rt_strtoul(e + 1, &e, 10);
		if (bits < 8 || bits > 25)
			return -1;                      /* room for the slots */
	}
	if (*e)
		return -1;
	mask = 0xFFFFFFFFUL << (32 - bits);
	base = a & mask;
	memset(slots, 0, sizeof slots);
	return 0;
}

/* riptide.conf: standin on|off|prefer|status|A.B.C.D[/N]; the Mac's older spelling, mactcp ipv6
   prefer|on|off and mactcp standin A.B.C.D[/N], too */
int rt_standin_command(int argc, char **argv, char *err, u32 errlen)
{
	int mac = argc >= 1 && !rt_strcasecmp(argv[0], "mactcp");
	if (mac && argc == 3 && !rt_strcasecmp(argv[1], "ipv6")) {
		if (!rt_strcasecmp(argv[2], "prefer"))
			rt_standin_prefer6 = 1;
		else if (!rt_strcasecmp(argv[2], "on") || !rt_strcasecmp(argv[2], "off"))
			rt_standin_prefer6 = 0;         /* stand-ins for IPv6-only names are always made */
		else
			goto usage;
		return 0;
	}
	if (mac && argc == 3 && !rt_strcasecmp(argv[1], "standin")) {
		if (set_range(argv[2]) < 0)
			goto usage;
		return 0;
	}
	if (!mac && argc == 2) {
		const char *v = argv[1];
		if (!rt_strcasecmp(v, "status")) {
			int taken, held, bits = 0;
			u32 m = mask;
			while (m) {
				bits++;
				m <<= 1;
			}
			rt_standin_count(&taken, &held);
			rt_snprintf(err, errlen, "stand-ins %s%s, %lu.%lu.%lu.%lu/%d, %d of %d in use, %d held\n",
			            rt_standin_enable ? "on" : "off", rt_standin_prefer6 ? " (IPv6 preferred)" : "",
			            (unsigned long)(base >> 24), (unsigned long)((base >> 16) & 255),
			            (unsigned long)((base >> 8) & 255), (unsigned long)(base & 255),
			            bits, taken, SLOTS, held);
			return 0;
		}
		if (!rt_strcasecmp(v, "on") || !rt_strcasecmp(v, "off")) {
			rt_standin_enable = !rt_strcasecmp(v, "on");
			rt_standin_prefer6 = 0;
			return 0;
		}
		if (!rt_strcasecmp(v, "prefer")) {
			rt_standin_enable = 1;
			rt_standin_prefer6 = 1;
			return 0;
		}
		if (set_range(v) == 0)
			return 0;
	}
usage:
	if (mac)
		rt_snprintf(err, errlen, "mactcp ipv6 prefer|on|off, or mactcp standin A.B.C.D[/N]");
	else
		rt_snprintf(err, errlen, "standin on|off|prefer|status, or standin A.B.C.D[/N]");
	return -RT_EINVAL;
}
