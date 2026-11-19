/* src/mac/standin.c - Riptide on the classic Mac OS: stand-in IPv4 addresses for IPv6 hosts, so that
 * MacTCP programs, which know only IPv4, reach hosts that have only IPv6 addresses.
 *
 * It works like DNS64 and NAT64 (RFC 6147, RFC 6146), but inside the Mac and without translating
 * packets: when a name has no IPv4 address but an IPv6 one (a DNS AAAA record, or a riptide.conf
 * `host` line with an IPv6 address), the resolver (src/mac/mactcp.c, the RTMAC_DNS lookups) answers
 * with a stand-in from a reserved IPv4 range, 198.18.0.0/15 (RFC 2544's benchmarking range, never
 * seen on the Internet) unless riptide.conf says otherwise (`mactcp standin 10.64.0.0`). A MacTCP
 * stream that opens a stand-in gets a core socket of family IPv6 to the real address; the program
 * sees the stand-in in TCPStatus and its open's remote address, and its own IPv4 address as the
 * local one. AddrToName of a stand-in gives back the name it was made for.
 *
 * A stand-in is a slot of a small table: slot i is base + 1 + i, so it keeps its IPv6 address
 * while it is in use (a stream holds it), and an idle slot is given to a new address only when
 * every slot is taken, the one used longest ago first. `mactcp ipv6 prefer` makes names with both
 * kinds of address answer with a stand-in, so that MacTCP programs use IPv6 where they can; off by
 * default.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "mac.h"
#include "standin.h"

#define SLOTS 64
struct slot {
	rt_in6_addr a6;
	char name[64];                  /* what it was made for ("" for an address seen on its own) */
	u32 used_ms;                    /* rt_now_ms when it was last handed out or opened */
	u16 holders;                    /* streams using it now */
	u8 taken;
};
static struct slot slots[SLOTS];
static u32 base = 0xC6120000UL;         /* 198.18.0.0 */
static u32 mask = 0xFFFE0000UL;         /* /15 */
int standin_prefer6;

int standin_is(u32 v4)
{
	u32 i = v4 - base - 1;
	return (v4 & mask) == base && i < SLOTS;
}

/* the stand-in for an IPv6 address (made, or found again); the name, when there is one, is kept */
u32 standin_for(const rt_in6_addr *a6, const char *name)
{
	int i, pick = -1;
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
			return 0;                       /* every slot held by an open stream */
		memset(&slots[pick], 0, sizeof slots[pick]);
		slots[pick].taken = 1;
		slots[pick].a6 = *a6;
	}
	if (name && *name)
		rt_strlcpy(slots[pick].name, name, sizeof slots[pick].name);
	slots[pick].used_ms = rt_now_ms();
	return base + 1 + (u32)pick;
}

/* the IPv6 address of a stand-in, and the name it was made for; 0 if v4 is not one in use */
int standin_lookup(u32 v4, rt_in6_addr *a6, char *name, u32 namelen)
{
	struct slot *s;
	if (!standin_is(v4))
		return 0;
	s = &slots[v4 - base - 1];
	if (!s->taken)
		return 0;
	if (a6)
		*a6 = s->a6;
	if (name && namelen)
		rt_strlcpy(name, s->name, namelen);
	return 1;
}

/* a stream opens a stand-in (hold 1) or lets it go (hold 0): held slots are never given away */
void standin_hold(u32 v4, int hold)
{
	struct slot *s;
	if (!standin_is(v4))
		return;
	s = &slots[v4 - base - 1];
	if (hold) {
		s->holders++;
		s->used_ms = rt_now_ms();
	} else if (s->holders) {
		s->holders--;
	}
}

/* riptide.conf: mactcp ipv6 prefer|on|off, mactcp standin A.B.C.D[/N] */
int standin_config(int argc, char **argv, char *err, u32 errlen)
{
	if (argc == 3 && !rt_strcasecmp(argv[1], "ipv6")) {
		if (!rt_strcasecmp(argv[2], "prefer"))
			standin_prefer6 = 1;
		else if (!rt_strcasecmp(argv[2], "on") || !rt_strcasecmp(argv[2], "off"))
			standin_prefer6 = 0;            /* stand-ins for IPv6-only names are always made */
		else
			goto usage;
		return 0;
	}
	if (argc == 3 && !rt_strcasecmp(argv[1], "standin")) {
		u32 a = 0, part, bits = 15, k;
		const char *p = argv[2], *e;
		for (k = 0; k < 4; k++) {
			part = (u32)rt_strtoul(p, &e, 10);
			if (e == p || part > 255 || (k < 3 && *e != '.'))
				goto usage;
			a = a << 8 | part;
			p = e + 1;
		}
		if (*e == '/') {
			bits = (u32)rt_strtoul(e + 1, &e, 10);
			if (bits < 8 || bits > 25)
				goto usage;             /* room for the slots */
		}
		if (*e)
			goto usage;
		mask = 0xFFFFFFFFUL << (32 - bits);
		base = a & mask;
		memset(slots, 0, sizeof slots);
		return 0;
	}
usage:
	rt_snprintf(err, errlen, "mactcp ipv6 prefer|on|off, or mactcp standin A.B.C.D[/N]");
	return -RT_EINVAL;
}
