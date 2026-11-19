/* src/mac/dnr/dnrp.c - Riptide on the classic Mac OS: the resolver of MacTCP programs (stage 3).
 *
 * Every MacTCP program carries Apple's DNR glue (dnr.c), which looks for a file holding 'dnrp': type
 * 'cdev' creator 'ztcp' in the Control Panels folder, then 'cdev' 'mtcp' in the System Folder, then
 * 'cdev' 'mtcp' in the Control Panels folder (tools/mkdnr.py: the Riptide DNR file is 'cdev' 'mtcp'
 * in the System Folder). It loads the first 'dnrp' and calls it for OpenResolver, StrToAddr,
 * AddrToStr, AddrToName and the rest; when that fails, StrToAddr answers notOpenErr (-28). This is that code, in the Riptide DNR file: dotted addresses are answered at once (noErr,
 * no result procedure, as MacTCP does with its cache), names and addresses go to Riptide's .IPP
 * driver as a private call (RTMAC_DNS, answered by src/mac/mactcp.c) and the answer reaches the
 * program's result procedure from the call's completion routine, with StrToAddr having returned
 * cacheFault meanwhile, as MacTCP's resolver does.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "prefs/mac_ipp_private.h"

#define OPENRESOLVER  1
#define CLOSERESOLVER 2
#define STRTOADDR     3
#define ADDRTOSTR     4
#define ENUMCACHE     5
#define ADDRTONAME    6
#define HXINFO        7
#define MXINFO        8
#define cacheFault    -23042
#define noResultProc  -23043
#define noNameServer  -23044
#define noAnsErr      -23046
#define outOfMemory   -23048
#define openFailed    -23015

struct hostInfo {
	long rtnCode;
	char cname[255];
	char pad;
	unsigned long addr[4];
};
_Static_assert(__builtin_offsetof(struct hostInfo, addr) == 260, "hostInfo");

short dnrp_pb(void *pb, long trap);
void dnrp_call_result(void *proc, void *hostinfo, void *udata);
void dnrp_done(void);

#define NQ 8
static struct q { struct rtmac_dns_pb pb; char name[256]; } qs[NQ];
static short ipp_ref;

static void zero(void *p, unsigned long n) { char *c = p; while (n--) *c++ = 0; }
static void copy(char *d, const char *s, unsigned long n)
{
	while (n > 1 && *s) { *d++ = *s++; n--; }
	if (n) *d = 0;
}

static long open_ipp(void)
{
	unsigned char pb[80];
	static const unsigned char name[] = "\x04.IPP";
	if (ipp_ref)
		return 0;
	zero(pb, sizeof pb);
	*(const unsigned char **)(pb + 18) = name;
	if (dnrp_pb(pb, 0xA000) != 0)
		return openFailed;
	ipp_ref = *(short *)(pb + 24);
	/* only Riptide's .IPP knows the lookups */
	zero(pb, sizeof pb);
	*(short *)(pb + 24) = ipp_ref;
	*(short *)(pb + 26) = RTMAC_PING;
	if (dnrp_pb(pb, 0xA004) != 0 || *(unsigned long *)(pb + 28) != RTMAC_MAGIC) {
		ipp_ref = 0;
		return openFailed;
	}
	return 0;
}

static int dotted(const char *s, unsigned long *a)
{
	unsigned long v = 0, part = 0;
	int dots = 0, digits = 0;
	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10 + (unsigned long)(*s - '0');
			if (part > 255 || ++digits > 3)
				return 0;
		} else if (*s == '.' && digits) {
			v = v << 8 | part;
			part = 0;
			digits = 0;
			dots++;
		} else if (*s == ' ') {
			continue;
		} else {
			return 0;
		}
	}
	if (dots != 3 || !digits)
		return 0;
	*a = v << 8 | part;
	return 1;
}

/* the lookup's completion (dnrp_done, interrupt time): the answer into the caller's hostInfo,
   then its result procedure */
void dnrp_complete(struct rtmac_dns_pb *pb)
{
	struct hostInfo *h = pb->rtn;
	unsigned long i;
	h->rtnCode = pb->ioResult;
	copy(h->cname, pb->cname, sizeof h->cname);
	for (i = 0; i < 4; i++)
		h->addr[i] = i < pb->count && !pb->ptr ? pb->addrs[i] : 0;
	pb->busy = 0;
	if (pb->proc)
		dnrp_call_result(pb->proc, h, pb->udata);
}

static long lookup(int ptr, const char *name, unsigned long addr, struct hostInfo *h, void *proc, void *udata)
{
	struct q *q = 0;
	int i;
	if (!proc)
		return noResultProc;
	if (open_ipp())
		return noNameServer;
	for (i = 0; i < NQ; i++)
		if (!qs[i].pb.busy) {
			q = &qs[i];
			break;
		}
	if (!q)
		return outOfMemory;
	zero(&q->pb, sizeof q->pb);
	if (name)
		copy(q->name, name, sizeof q->name);
	q->pb.ioCRefNum = ipp_ref;
	q->pb.csCode = RTMAC_DNS;
	q->pb.ioCompletion = (void *)dnrp_done;
	q->pb.ptr = ptr;
	q->pb.name = q->name;
	q->pb.addrs[0] = addr;
	q->pb.rtn = h;
	q->pb.proc = proc;
	q->pb.udata = udata;
	q->pb.busy = 1;
	h->rtnCode = cacheFault;
	if (dnrp_pb(&q->pb, 0xA404) < 0) {
		q->pb.busy = 0;
		return noNameServer;
	}
	return cacheFault;
}

static void addr_str(unsigned long a, char *s)
{
	int k;
	for (k = 24; k >= 0; k -= 8) {
		unsigned v = (unsigned)((a >> k) & 255);
		if (v >= 100)
			*s++ = (char)('0' + v / 100);
		if (v >= 10)
			*s++ = (char)('0' + v / 10 % 10);
		*s++ = (char)('0' + v % 10);
		if (k)
			*s++ = '.';
	}
	*s = 0;
}

long dnrp_main(long sel, long a, long b, long c, long d)
{
	switch (sel) {
	case OPENRESOLVER:
		return open_ipp() ? openFailed : 0;
	case CLOSERESOLVER:
		/* Apple's glue disposes of this code next: lookups still waiting must never complete
		   into it (dnrp_done is here), so the driver withdraws them */
		if (ipp_ref) {
			unsigned char pb[80];
			zero(pb, sizeof pb);
			*(short *)(pb + 24) = ipp_ref;
			*(short *)(pb + 26) = RTMAC_DNSCANCEL;
			*(void **)(pb + 28) = (void *)qs;
			*(void **)(pb + 32) = (void *)(qs + NQ);
			dnrp_pb(pb, 0xA004);
			for (int i = 0; i < NQ; i++)
				qs[i].pb.busy = 0;
		}
		return 0;
	case STRTOADDR: {
		struct hostInfo *h = (struct hostInfo *)b;
		unsigned long ad;
		/* a dotted address "is returned immediately" (MacTCP Programmer's Guide, StrToAddr): noErr,
		   rtnCode noErr, the result procedure not called */
		if (dotted((const char *)a, &ad)) {
			zero(h, sizeof *h);
			copy(h->cname, (const char *)a, sizeof h->cname);
			h->addr[0] = ad;
			return 0;
		}
		return lookup(0, (const char *)a, 0, h, (void *)c, (void *)d);
	}
	case ADDRTOSTR:
		addr_str((unsigned long)a, (char *)b);
		return 0;
	case ADDRTONAME:
		return lookup(1, 0, (unsigned long)a, (struct hostInfo *)b, (void *)c, (void *)d);
	case ENUMCACHE:
	case HXINFO:
	case MXINFO:
		return noAnsErr;
	}
	return noAnsErr;
}
