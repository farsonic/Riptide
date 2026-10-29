/*
 * Riptide - rs_test: calls every Roadshow extension of bsdsocket.library
 * (version 4) on a running Riptide with the loopback interface, through
 * the SDK in sdk/ (proto/bsdsocket.h, libraries/bsdsocket.h) and the NDK.
 * Packet filter on lo0 (read, filter, notify signal, timeout, write),
 * interfaces, routes, DNS, statistics, monitoring hooks (in the caller and
 * in the stack), configuration data, the netdb enumeration, address
 * conversion, the reentrant resolver calls, the Roadshow getaddrinfo()
 * family, address allocation messages and the SocketBaseTagList() codes.
 *
 *   rs_test            (Riptide running, "host testhost 127.0.0.5" configured)
 *
 * Prints one line per check and "rs_test: N passed, M failed"; returns 0
 * when everything passed, 10 otherwise, 20 without bsdsocket.library 4.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

struct Library *SocketBase;

/* ##private in the FD file, so not in the SDK headers */
LONG __ChangeRouteTagList(__reg("a6") void *, __reg("a0") APTR tags) = "\tjsr\t-426(a6)";
#define ChangeRouteTagList(tags) __ChangeRouteTagList(SocketBase, (tags))
LONG __ipf_open(__reg("a6") void *, __reg("d0") LONG channel) = "\tjsr\t-762(a6)";
#define ipf_open(ch) __ipf_open(SocketBase, (ch))

/* ---- network definitions (normally from the SDK's netinclude) ---- */
#define AF_INET     2
#define AF_INET6    28
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define FIONBIO     0x8004667eUL
#define EACCES      13
#define ENXIO       6
#define ENOENT      2
#define EBUSY       16
#define EINVAL      22
#define EWOULDBLOCK 35
#define ECONNREFUSED 61
#define RTF_UP      0x1
#define RTF_GATEWAY 0x2
#define RTF_HOST    0x4
#define DLT_NULL    0
#define LOG_ERR     3

struct sockaddr_in {
	UBYTE sin_len, sin_family;
	UWORD sin_port;
	ULONG sin_addr;
	UBYTE sin_zero[8];
};
struct hostent {
	char *h_name;
	char **h_aliases;
	LONG h_addrtype, h_length;
	char **h_addr_list;
};
struct addrinfo {                /* Roadshow layout: ai_addr before ai_canonname */
	LONG ai_flags, ai_family, ai_socktype, ai_protocol;
	ULONG ai_addrlen;
	struct sockaddr_in *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};
struct bpf_insn { UWORD code; UBYTE jt, jf; ULONG k; };
struct bpf_program { ULONG bf_len; struct bpf_insn *bf_insns; };
#define BIOCGBLEN     0x40044266UL
#define BIOCSBLEN     0xc0044266UL
#define BIOCSETF      0x80084267UL
#define BIOCFLUSH     0x20004268UL
#define BIOCGDLT      0x4004426aUL
#define BIOCGETIF     0x4020426bUL
#define BIOCSETIF     0x8020426cUL
#define BIOCSRTIMEOUT 0x8008426dUL
#define BIOCGSTATS    0x4008426fUL
#define BIOCIMMEDIATE 0x80044270UL
#define BIOCVERSION   0x40044271UL
#define FIONREAD      0x4004667fUL

static int passes, fails;

static void check_(const char *what, int ok, const char *detail)
{
	if (ok)
		passes++;
	else
		fails++;
	printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

static char dbuf[128];
#define D(...) (sprintf(dbuf, __VA_ARGS__), dbuf)
/* the detail text is formatted after the condition was evaluated */
#define check(what, cond, detail) do { int ok_ = (cond); check_(what, ok_, detail); } while (0)

static ULONG ip(int a, int b, int c, int d)
{
	return ((ULONG)a << 24) | ((ULONG)b << 16) | ((ULONG)c << 8) | (ULONG)d;
}

static struct sockaddr_in sin_of(ULONG addr, UWORD port)
{
	struct sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = AF_INET;
	s.sin_port = port;
	s.sin_addr = addr;
	return s;
}

static LONG sbt_get(ULONG code, ULONG *val)
{
	struct TagItem t[2];
	t[0].ti_Tag = SBTM_GETVAL(code);
	t[0].ti_Data = 0;
	t[1].ti_Tag = TAG_DONE;
	if (SocketBaseTagList(t) != 0)
		return -1;
	*val = t[0].ti_Data;
	return 0;
}

static LONG sbt_set(ULONG code, ULONG val)
{
	struct TagItem t[2];
	t[0].ti_Tag = SBTM_SETVAL(code);
	t[0].ti_Data = val;
	t[1].ti_Tag = TAG_DONE;
	return SocketBaseTagList(t);
}

/* ---- hooks ---- */
static volatile LONG errhook_calls, errhook_code, loghook_calls, mon_calls[8], mon_verdict[8], mon_socket;
static char loghook_text[80];

static ULONG errhook_fn(__reg("a0") struct Hook *h, __reg("a2") APTR obj, __reg("a1") struct ErrorHookMsg *m)
{
	if (m->ehm_Size >= 12 && m->ehm_Action == EHMA_Set_errno) {
		errhook_calls++;
		errhook_code = m->ehm_Code;
	}
	return 0;
}

static ULONG loghook_fn(__reg("a0") struct Hook *h, __reg("a2") APTR obj, __reg("a1") struct LogHookMessage *m)
{
	loghook_calls++;
	if (m->lhm_Message)
		strncpy(loghook_text, (char *)m->lhm_Message, sizeof(loghook_text) - 1);
	return 0;
}

static ULONG mon_fn(__reg("a0") struct Hook *h, __reg("a2") APTR obj, __reg("a1") APTR msg)
{
	LONG type = (LONG)h->h_Data;
	mon_calls[type]++;
	if (type == MHT_Connect)
		mon_socket = ((struct ConnectMonitorMsg *)msg)->cmm_Socket;
	return (ULONG)mon_verdict[type];
}

static struct Hook errhook, loghook, monhooks[7];

static void init_hook(struct Hook *h, APTR fn, LONG data)
{
	memset(h, 0, sizeof(*h));
	h->h_Entry = (ULONG (*)())fn;
	h->h_Data = (APTR)data;
}

/* ---- the tests ---- */

static void test_tags(void)
{
	static const ULONG yes[] = { SBTC_HAVE_ROUTING_API, SBTC_HAVE_INTERFACE_API, SBTC_HAVE_MONITORING_API,
	                             SBTC_HAVE_STATUS_API, SBTC_HAVE_DNS_API, SBTC_HAVE_LOCAL_DATABASE_API,
	                             SBTC_HAVE_ADDRESS_CONVERSION_API, SBTC_HAVE_SERVER_API,
	                             SBTC_HAVE_ROADSHOWDATA_API, SBTC_HAVE_GETHOSTADDR_R_API };
	struct TagItem t[2];
	SBQUAD_T q;
	ULONG v, ttl;
	int i, ok = 1;

	check("SBTC_NUM_PACKET_FILTER_CHANNELS", sbt_get(SBTC_NUM_PACKET_FILTER_CHANNELS, &v) == 0 && v == 16, NULL);
	for (i = 0; i < (int)(sizeof(yes) / sizeof(yes[0])); i++)
		if (sbt_get(yes[i], &v) != 0 || v != TRUE)
			ok = 0;
	check("SBTC_HAVE_*_API", ok, NULL);
	check("SBTC_HAVE_KERNEL_MEMORY_API is FALSE", sbt_get(SBTC_HAVE_KERNEL_MEMORY_API, &v) == 0 && v == 0, NULL);
	check("SBTC_COMPAT43", sbt_get(SBTC_COMPAT43, &v) == 0 && v == 0 && sbt_set(SBTC_COMPAT43, 0) == 0, NULL);
	check("SBTC_SYSTEM_STATUS", sbt_get(SBTC_SYSTEM_STATUS, &v) == 0, D("0x%lx", v));
	check("SBTC_IP_DEFAULT_TTL", sbt_get(SBTC_IP_DEFAULT_TTL, &ttl) == 0 && ttl >= 1 && ttl <= 255, D("%lu", ttl));
	check("SBTC_IP_DEFAULT_TTL set", sbt_set(SBTC_IP_DEFAULT_TTL, 77) == 0 && sbt_get(SBTC_IP_DEFAULT_TTL, &v) == 0 &&
	      v == 77 && sbt_set(SBTC_IP_DEFAULT_TTL, ttl) == 0, NULL);
	check("SBTC_UDP_CHECKSUM", sbt_get(SBTC_UDP_CHECKSUM, &v) == 0 && v == 1, NULL);
	check("SBTC_ICMP_PROCESS_ECHO", sbt_get(SBTC_ICMP_PROCESS_ECHO, &v) == 0 && v == IR_Process &&
	      sbt_set(SBTC_ICMP_PROCESS_ECHO, IR_Ignore) == 0 && sbt_get(SBTC_ICMP_PROCESS_ECHO, &v) == 0 &&
	      v == IR_Ignore && sbt_set(SBTC_ICMP_PROCESS_ECHO, IR_Process) == 0, NULL);
	check("SBTC_IP_FORWARDING", sbt_get(SBTC_IP_FORWARDING, &v) == 0 && v == 0, NULL);
	t[0].ti_Tag = SBTM_GETREF(SBTC_GET_BYTES_RECEIVED);
	t[0].ti_Data = (ULONG)&q;
	t[1].ti_Tag = TAG_DONE;
	q.sbq_High = q.sbq_Low = 0xdeadbeef;
	check("SBTC_GET_BYTES_RECEIVED", SocketBaseTagList(t) == 0 && q.sbq_High != 0xdeadbeef, NULL);
	check("SBTC_IPF_API_VERSION", sbt_get(SBTC_IPF_API_VERSION, &v) == 0 && v == 0, NULL);
	check("SBTC_IDN_DEFAULT_CHARACTER_SET", sbt_get(SBTC_IDN_DEFAULT_CHARACTER_SET, &v) == 0 && v == IDNCS_ASCII,
	      NULL);

	/* the error hook */
	init_hook(&errhook, (APTR)errhook_fn, 0);
	check("SBTC_ERROR_HOOK set", sbt_set(SBTC_ERROR_HOOK, (ULONG)&errhook) == 0, NULL);
	bpf_close(13);
	check("SBTC_ERROR_HOOK called", errhook_calls == 1 && errhook_code == ENXIO, D("%ld calls, code %ld",
	      (long)errhook_calls, (long)errhook_code));
	sbt_set(SBTC_ERROR_HOOK, 0);
	bpf_close(13);
	check("SBTC_ERROR_HOOK removed", errhook_calls == 1, NULL);

	/* the log hook */
	init_hook(&loghook, (APTR)loghook_fn, 0);
	check("SBTC_LOG_HOOK set", sbt_set(SBTC_LOG_HOOK, (ULONG)&loghook) == 0, NULL);
	{
		LONG args[1];
		args[0] = 42;
		vsyslog(LOG_ERR, (CONST_STRPTR)"rs_test log %ld", args);
	}
	check("SBTC_LOG_HOOK called", loghook_calls == 1 && strstr(loghook_text, "rs_test log 42") != NULL,
	      loghook_text);
	sbt_set(SBTC_LOG_HOOK, 0);
}

static void test_interfaces(void)
{
	struct List *l = ObtainInterfaceList();
	struct Node *n;
	struct TagItem t[10];
	struct sockaddr_in addr, mask;
	LONG mtu = 0, state = -1, metric = -1, bind = -1, hwtype = -1;
	ULONG sent = 0;
	int lo = 0, cnt = 0;
	BYTE sig;

	check("ObtainInterfaceList", l != NULL, NULL);
	if (l) {
		for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) {
			cnt++;
			if (n->ln_Name && !strcmp(n->ln_Name, "lo0"))
				lo = 1;
		}
		check("ObtainInterfaceList has lo0", lo, D("%d interfaces", cnt));
		ReleaseInterfaceList(l);
	}
	t[0].ti_Tag = IFQ_MTU;               t[0].ti_Data = (ULONG)&mtu;
	t[1].ti_Tag = IFQ_Address;           t[1].ti_Data = (ULONG)&addr;
	t[2].ti_Tag = IFQ_NetMask;           t[2].ti_Data = (ULONG)&mask;
	t[3].ti_Tag = IFQ_State;             t[3].ti_Data = (ULONG)&state;
	t[4].ti_Tag = IFQ_PacketsSent;       t[4].ti_Data = (ULONG)&sent;
	t[5].ti_Tag = IFQ_Metric;            t[5].ti_Data = (ULONG)&metric;
	t[6].ti_Tag = IFQ_AddressBindType;   t[6].ti_Data = (ULONG)&bind;
	t[7].ti_Tag = IFQ_HardwareType;      t[7].ti_Data = (ULONG)&hwtype;
	t[8].ti_Tag = TAG_USER + 0x12345;    t[8].ti_Data = 0;      /* unknown: left alone */
	t[9].ti_Tag = TAG_DONE;
	check("QueryInterfaceTagList(lo0)", QueryInterfaceTagList((STRPTR)"lo0", t) == 0 && mtu > 0 &&
	      addr.sin_addr == ip(127, 0, 0, 1) && mask.sin_addr == ip(255, 0, 0, 0) && state == SM_Up &&
	      bind == IFABT_Static, D("mtu %ld state %ld metric %ld bind %ld hwtype %ld sent %lu", (long)mtu,
	      (long)state, (long)metric, (long)bind, (long)hwtype, (unsigned long)sent));
	t[0].ti_Tag = TAG_DONE;
	check("QueryInterfaceTagList(nosuch0) fails", QueryInterfaceTagList((STRPTR)"nosuch0", t) == -1 &&
	      Errno() == ENXIO, NULL);

	/* an alias address, and the address change signal */
	sig = AllocSignal(-1);
	sbt_set(SBTC_SIG_ADDRESS_CHANGE_MASK, 1UL << sig);
	SetSignal(0, 1UL << sig);
	t[0].ti_Tag = IFC_AddAliasAddress;
	t[0].ti_Data = (ULONG)"127.0.0.9";
	t[1].ti_Tag = TAG_DONE;
	check("ConfigureInterfaceTagList IFC_AddAliasAddress", ConfigureInterfaceTagList((STRPTR)"lo0", t) == 0, NULL);
	check("SBTC_SIG_ADDRESS_CHANGE_MASK signalled", (SetSignal(0, 1UL << sig) & (1UL << sig)) != 0, NULL);
	check("In_LocalAddr(alias)", In_LocalAddr(ip(127, 0, 0, 9)) == 1, NULL);
	t[0].ti_Tag = IFC_DeleteAliasAddress;
	check("ConfigureInterfaceTagList IFC_DeleteAliasAddress", ConfigureInterfaceTagList((STRPTR)"lo0", t) == 0,
	      NULL);
	sbt_set(SBTC_SIG_ADDRESS_CHANGE_MASK, 0);
	FreeSignal(sig);
	t[0].ti_Tag = IFC_Metric;
	t[0].ti_Data = 3;
	t[1].ti_Tag = TAG_DONE;
	check("ConfigureInterfaceTagList IFC_Metric", ConfigureInterfaceTagList((STRPTR)"lo0", t) == 0, NULL);
	t[0].ti_Tag = IFQ_Metric;
	t[0].ti_Data = (ULONG)&metric;
	check("QueryInterfaceTagList IFQ_Metric", QueryInterfaceTagList((STRPTR)"lo0", t) == 0 && metric == 3, NULL);
	t[0].ti_Tag = IFC_Metric;
	t[0].ti_Data = 0;
	ConfigureInterfaceTagList((STRPTR)"lo0", t);
	t[0].ti_Tag = IFC_Address;
	t[0].ti_Data = (ULONG)"not an address";
	check("ConfigureInterfaceTagList bad address fails", ConfigureInterfaceTagList((STRPTR)"lo0", t) == -1, NULL);
	t[0].ti_Tag = TAG_DONE;
	check("AddInterfaceTagList(missing device) fails",
	      AddInterfaceTagList((STRPTR)"rs9", (STRPTR)"nosuch.device", 0, t) == -1, D("errno %ld", Errno()));
	check("RemoveInterface(nosuch0) fails", RemoveInterface((STRPTR)"nosuch0", FALSE) == 0, NULL);
}

static void test_routes(void)
{
	struct TagItem t[3];
	UBYTE *r, *p;
	int found = 0, hostonly = 1;

	t[0].ti_Tag = RTA_DestinationHost;
	t[0].ti_Data = (ULONG)"10.20.30.40";
	t[1].ti_Tag = RTA_Gateway;
	t[1].ti_Data = (ULONG)"127.0.0.1";
	t[2].ti_Tag = TAG_DONE;
	check("AddRouteTagList", AddRouteTagList(t) == 0, D("errno %ld", Errno()));
	r = GetRouteInfo(AF_INET, 0);
	check("GetRouteInfo(AF_INET, 0)", r != NULL, NULL);
	for (p = r; p && *(UWORD *)p; p += *(UWORD *)p) {
		ULONG flags = *(ULONG *)(p + 6);
		struct sockaddr_in *d = (struct sockaddr_in *)(p + 74);
		if (p[2] != 3)
			break;
		if (d->sin_addr == ip(10, 20, 30, 40) && (flags & (RTF_UP | RTF_GATEWAY | RTF_HOST)) ==
		    (RTF_UP | RTF_GATEWAY | RTF_HOST))
			found = 1;
	}
	check("GetRouteInfo has the host route", found, NULL);
	if (r)
		FreeRouteInfo(r);
	r = GetRouteInfo(0, RTF_HOST);
	for (p = r; p && *(UWORD *)p; p += *(UWORD *)p)
		if (!(*(ULONG *)(p + 6) & RTF_HOST))
			hostonly = 0;
	check("GetRouteInfo(AF_UNSPEC, RTF_HOST)", r != NULL && hostonly, NULL);
	if (r)
		FreeRouteInfo(r);
	check("GetRouteInfo(AF_INET6) fails", GetRouteInfo(AF_INET6, 0) == NULL, NULL);
	t[1].ti_Data = (ULONG)"127.0.0.2";
	check("ChangeRouteTagList", ChangeRouteTagList(t) == 0, D("errno %ld", Errno()));
	t[0].ti_Tag = RTA_Destination;
	t[1].ti_Tag = TAG_DONE;
	check("DeleteRouteTagList", DeleteRouteTagList(t) == 0, D("errno %ld", Errno()));
	check("DeleteRouteTagList again fails", DeleteRouteTagList(t) == -1, NULL);
	t[0].ti_Tag = RTA_DefaultGateway;
	t[0].ti_Data = (ULONG)"no-such-host-name.invalid";
	check("AddRouteTagList bad gateway fails", AddRouteTagList(t) == -1, NULL);
}

static LONG dns_count(const char *a)
{
	struct List *l = ObtainDomainNameServerList();
	struct DomainNameServerNode *n;
	LONG c = 0;

	if (!l)
		return -100;
	for (n = (struct DomainNameServerNode *)l->lh_Head; n->dnsn_MinNode.mln_Succ;
	     n = (struct DomainNameServerNode *)n->dnsn_MinNode.mln_Succ)
		if (!strcmp((char *)n->dnsn_Address, a))
			c = n->dnsn_UseCount;
	ReleaseDomainNameServerList(l);
	return c;
}

static void test_dns(void)
{
	char old[64], buf[64];
	LONG had;

	check("AddDomainNameServer", AddDomainNameServer((STRPTR)"10.0.0.53") == 0 && dns_count("10.0.0.53") == 1, NULL);
	check("AddDomainNameServer nests", AddDomainNameServer((STRPTR)"10.0.0.53") == 0 && dns_count("10.0.0.53") == 2,
	      NULL);
	check("RemoveDomainNameServer", RemoveDomainNameServer((STRPTR)"10.0.0.53") == 0 && dns_count("10.0.0.53") == 1,
	      NULL);
	check("RemoveDomainNameServer last", RemoveDomainNameServer((STRPTR)"10.0.0.53") == 0 &&
	      dns_count("10.0.0.53") == 0, NULL);
	check("RemoveDomainNameServer unknown fails", RemoveDomainNameServer((STRPTR)"10.0.0.53") == -1 &&
	      Errno() == ENOENT, NULL);
	check("AddDomainNameServer bad address fails", AddDomainNameServer((STRPTR)"ten") == -1 && Errno() == EINVAL,
	      NULL);
	old[0] = 0;
	had = GetDefaultDomainName((STRPTR)old, sizeof(old));
	SetDefaultDomainName((STRPTR)"rs-test.example");
	check("Set/GetDefaultDomainName", GetDefaultDomainName((STRPTR)buf, sizeof(buf)) == TRUE &&
	      !strcmp(buf, "rs-test.example"), buf);
	check("GetDefaultDomainName truncates", GetDefaultDomainName((STRPTR)buf, 5) == TRUE && !strcmp(buf, "rs-t"),
	      buf);
	SetDefaultDomainName((STRPTR)(had ? old : ""));
}

static void test_netdb(void)
{
	struct protoent { char *p_name; char **p_aliases; LONG p_proto; } *pe;
	struct servent { char *s_name; char **s_aliases; LONG s_port; char *s_proto; } *se;
	struct netent { char *n_name; char **n_aliases; LONG n_addrtype; ULONG n_net; } *ne;
	int n = 0, tcp = 0, ftp = 0;

	setprotoent(1);
	while ((pe = getprotoent()) != NULL && n < 500) {
		n++;
		if (pe->p_proto == 6 && !strcmp(pe->p_name, "tcp"))
			tcp = 1;
	}
	endprotoent();
	check("getprotoent", n > 5 && tcp, D("%d entries", n));
	n = 0;
	setservent(1);
	while ((se = getservent()) != NULL && n < 2000) {
		n++;
		if (se->s_port == 21 && !strcmp(se->s_name, "ftp"))
			ftp = 1;
	}
	endservent();
	check("getservent", n > 20 && ftp, D("%d entries", n));
	setnetent(1);
	ne = getnetent();
	check("getnetent", ne && !strcmp(ne->n_name, "loopback") && ne->n_net == 127 && getnetent() == NULL, NULL);
	endnetent();
	check("getnetent after endnetent", (ne = getnetent()) != NULL && ne->n_net == 127, NULL);
}

static void test_conversion(void)
{
	ULONG a = 0;
	UBYTE a6[16];
	char txt[48];

	check("inet_aton", inet_aton((STRPTR)"192.168.1.2", &a) == 1 && a == ip(192, 168, 1, 2), NULL);
	check("inet_aton bad", inet_aton((STRPTR)"192.168.1.x", &a) == 0, NULL);
	check("inet_pton/inet_ntop IPv4", inet_pton(AF_INET, (STRPTR)"10.1.2.3", &a) == 1 &&
	      inet_ntop(AF_INET, &a, (STRPTR)txt, sizeof(txt)) == (STRPTR)txt && !strcmp(txt, "10.1.2.3"), txt);
	check("inet_pton/inet_ntop IPv6", inet_pton(AF_INET6, (STRPTR)"fe80::1", a6) == 1 &&
	      inet_ntop(AF_INET6, a6, (STRPTR)txt, sizeof(txt)) != NULL && !strcmp(txt, "fe80::1"), txt);
	check("inet_ntop short buffer", inet_ntop(AF_INET, &a, (STRPTR)txt, 4) == NULL, NULL);
	check("In_LocalAddr", In_LocalAddr(ip(127, 0, 0, 1)) == 1 && In_LocalAddr(ip(8, 8, 8, 8)) == 0, NULL);
	check("In_CanForward", In_CanForward(ip(8, 8, 8, 8)) == 1 && In_CanForward(ip(127, 0, 0, 1)) == 0 &&
	      In_CanForward(ip(224, 0, 0, 1)) == 0 && In_CanForward(ip(0, 1, 2, 3)) == 0, NULL);
	check("ProcessIsServer", ProcessIsServer(NULL) == FALSE, NULL);
	check("ObtainServerSocket", ObtainServerSocket() == -1, NULL);
	check("mbuf_get (no kernel memory API)", mbuf_get() == NULL, NULL);
	check("ipf_open (no IP filter)", ipf_open(0) == -1, NULL);
}

static void test_resolver(void)
{
	struct hostent he, *h;
	char buf[512];
	LONG herr = -5;
	ULONG a = ip(127, 0, 0, 5);
	struct addrinfo hints, *res = NULL;
	char host[64], serv[16];

	h = gethostbyname_r((STRPTR)"testhost", &he, buf, sizeof(buf), &herr);
	check("gethostbyname_r", h == &he && herr == 0 && he.h_length == 4 &&
	      *(ULONG *)he.h_addr_list[0] == a && he.h_addr_list[1] == NULL, h ? he.h_name : "NULL");
	h = gethostbyname_r((STRPTR)"testhost", &he, buf, 8, &herr);
	check("gethostbyname_r small buffer", h == NULL, NULL);
	h = gethostbyaddr_r((STRPTR)&a, 4, AF_INET, &he, buf, sizeof(buf), &herr);
	check("gethostbyaddr_r", h == &he && !strcmp(he.h_name, "testhost"), h ? he.h_name : "NULL");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 2;              /* AI_CANONNAME */
	check("getaddrinfo", getaddrinfo((STRPTR)"testhost", (STRPTR)"80", &hints, &res) == 0 && res &&
	      res->ai_family == AF_INET && res->ai_addrlen == 16 && res->ai_addr->sin_port == 80 &&
	      res->ai_addr->sin_addr == a && res->ai_canonname && !strcmp(res->ai_canonname, "testhost") &&
	      res->ai_flags == 2, NULL);
	if (res)
		freeaddrinfo(res);
	check("getaddrinfo no name", getaddrinfo(NULL, NULL, NULL, &res) == -2, NULL);
	hints.ai_flags = 0x1000;
	check("getaddrinfo bad flags", getaddrinfo((STRPTR)"testhost", NULL, &hints, &res) == -1, NULL);
	check("gai_strerror", gai_strerror(-2) != NULL && strstr((char *)gai_strerror(-2), "not known") != NULL,
	      (char *)gai_strerror(-2));
	{
		struct sockaddr_in s = sin_of(a, 21);
		check("getnameinfo", getnameinfo(&s, sizeof(s), (STRPTR)host, sizeof(host), (STRPTR)serv, sizeof(serv),
		                                 0) == 0 && !strcmp(host, "testhost") && !strcmp(serv, "ftp"), host);
		check("getnameinfo NI_NUMERICHOST|NI_NUMERICSERV", getnameinfo(&s, sizeof(s), (STRPTR)host, sizeof(host),
		      (STRPTR)serv, sizeof(serv), 1 | 2) == 0 && !strcmp(host, "127.0.0.5") && !strcmp(serv, "21"), host);
	}
}

static void test_stats(void)
{
	static const LONG types[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10 };
	static const LONG sizes[] = { 184, 36, 96, 28, 32, 10, 196, 36, -1, -1 };
	ULONG st[64];
	UBYTE pcd[24 * 32];
	struct sockaddr_in s = sin_of(ip(127, 0, 0, 1), 7777);
	LONG fd, n, i, ok = 1, listening = 0;

	for (i = 0; i < 10; i++) {
		n = GetNetworkStatistics(types[i], NETWORKSTATUS_VERSION, NULL, 0);
		if (n < 0 || (sizes[i] >= 0 && n != sizes[i]))
			ok = 0;
	}
	check("GetNetworkStatistics sizes", ok, NULL);
	fd = socket(AF_INET, SOCK_DGRAM, 0);        /* some IP traffic first */
	sendto(fd, "x", 1, 0, &s, sizeof(s));
	CloseSocket(fd);
	Delay(5);
	check("GetNetworkStatistics(NETSTATUS_ip)", GetNetworkStatistics(2, NETWORKSTATUS_VERSION, st, sizeof(st)) == 96 &&
	      st[14] > 0, D("ips_total %lu ips_delivered %lu ips_localout %lu", st[0], st[13], st[14]));
	check("GetNetworkStatistics bad version fails", GetNetworkStatistics(2, 2, st, sizeof(st)) == -1, NULL);
	check("GetNetworkStatistics bad type fails", GetNetworkStatistics(8, 1, st, sizeof(st)) == -1, NULL);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	bind(fd, &s, sizeof(s));
	listen(fd, 2);
	n = GetNetworkStatistics(9, NETWORKSTATUS_VERSION, pcd, sizeof(pcd));
	for (i = 0; i + 24 <= n; i += 24)
		if (*(UWORD *)(pcd + i + 10) == 7777 && *(LONG *)(pcd + i + 20) == 1)
			listening = 1;
	check("GetNetworkStatistics(NETSTATUS_tcp_sockets)", listening, D("%ld bytes", (long)n));
	CloseSocket(fd);
}

static void test_roadshowdata(void)
{
	struct List *r = ObtainRoadshowData(ORD_ReadAccess), *w, *w2;
	struct RoadshowDataNode *n;
	LONG ttl = -1, v = 63;
	ULONG cur;
	int cnt = 0;

	check("ObtainRoadshowData(ORD_ReadAccess)", r != NULL, NULL);
	if (!r)
		return;
	for (n = (struct RoadshowDataNode *)r->lh_Head; n->rdn_MinNode.mln_Succ;
	     n = (struct RoadshowDataNode *)n->rdn_MinNode.mln_Succ) {
		cnt++;
		if (!strcmp((char *)n->rdn_Name, "ip.defttl") && n->rdn_Type == RDNT_Integer && n->rdn_Length == 4)
			ttl = *(LONG *)n->rdn_Data;
	}
	sbt_get(SBTC_IP_DEFAULT_TTL, &cur);
	check("RoadshowData ip.defttl", ttl == (LONG)cur, D("%d options, ip.defttl %ld", cnt, (long)ttl));
	check("ChangeRoadshowData read access fails", ChangeRoadshowData(r, (STRPTR)"ip.defttl", 4, &v) == FALSE &&
	      Errno() == EACCES, NULL);
	w = ObtainRoadshowData(ORD_WriteAccess);
	w2 = ObtainRoadshowData(ORD_WriteAccess);
	check("ObtainRoadshowData(ORD_WriteAccess) once", w != NULL && w2 == NULL && Errno() == EBUSY, NULL);
	check("ChangeRoadshowData", w && ChangeRoadshowData(w, (STRPTR)"IP.DefTTL", 4, &v) == TRUE &&
	      sbt_get(SBTC_IP_DEFAULT_TTL, &cur) == 0 && cur == 63, NULL);
	check("ChangeRoadshowData unknown", w && ChangeRoadshowData(w, (STRPTR)"no.such", 4, &v) == FALSE &&
	      Errno() == ENOENT, NULL);
	check("ChangeRoadshowData size", w && ChangeRoadshowData(w, (STRPTR)"ip.defttl", 2, &v) == FALSE, NULL);
	if (w) {
		ChangeRoadshowData(w, (STRPTR)"ip.defttl", 4, &ttl);
		ReleaseRoadshowData(w);
	}
	ReleaseRoadshowData(r);
	ReleaseRoadshowData(NULL);
}

/* a UDP datagram 127.0.0.1:sport -> 127.0.0.1:dport with checksum 0 */
static int udp_packet(UBYTE *p, UWORD sport, UWORD dport, const char *data)
{
	int dl = strlen(data), len = 28 + dl, i;
	ULONG sum = 0;

	memset(p, 0, 28);
	p[0] = 0x45;
	*(UWORD *)(p + 2) = len;
	p[8] = 64;
	p[9] = 17;
	*(ULONG *)(p + 12) = ip(127, 0, 0, 1);
	*(ULONG *)(p + 16) = ip(127, 0, 0, 1);
	for (i = 0; i < 20; i += 2)
		sum += *(UWORD *)(p + i);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	*(UWORD *)(p + 10) = ~sum;
	*(UWORD *)(p + 20) = sport;
	*(UWORD *)(p + 22) = dport;
	*(UWORD *)(p + 24) = 8 + dl;
	memcpy(p + 28, data, dl);
	return len;
}

static void test_bpf(void)
{
	/* UDP to port 9999 only (DLT_NULL: IP at offset 4) */
	struct bpf_insn f[] = {
		{ 0x30, 0, 0, 13 },              /* ldb [4 + 9]: protocol */
		{ 0x15, 0, 3, 17 },
		{ 0x28, 0, 0, 26 },              /* ldh [4 + 22]: destination port */
		{ 0x15, 0, 1, 9999 },
		{ 0x06, 0, 0, 0xffff },
		{ 0x06, 0, 0, 0 },
	};
	struct bpf_program prog;
	struct sockaddr_in to = sin_of(ip(127, 0, 0, 1), 9999), rs = sin_of(ip(127, 0, 0, 1), 9998);
	ULONG blen = 2048, v, st[2], tv[2];
	UWORD ver[2];
	char ifr[32];
	static UBYTE buf[4096], pk[64];
	LONG ch, ch2, n, s, r, one = 1;
	BYTE sig = AllocSignal(-1);

	ch = bpf_open(-1);
	check("bpf_open(-1)", ch >= 0, D("channel %ld", (long)ch));
	if (ch < 0)
		return;
	check("bpf_open(busy channel) fails", bpf_open(ch) == -1 && Errno() == EBUSY, NULL);
	check("bpf_open(16) fails", bpf_open(16) == -1 && Errno() == ENXIO, NULL);
	check("BIOCVERSION", bpf_ioctl(ch, BIOCVERSION, ver) == 0 && ver[0] == 1 && ver[1] == 1, NULL);
	check("BIOCSBLEN", bpf_ioctl(ch, BIOCSBLEN, &blen) == 0 && blen == 2048, NULL);
	memset(ifr, 0, sizeof(ifr));
	strcpy(ifr, "lo0");
	check("BIOCSETIF lo0", bpf_ioctl(ch, BIOCSETIF, ifr) == 0, NULL);
	check("BIOCGBLEN", bpf_ioctl(ch, BIOCGBLEN, &v) == 0 && v == 2048, NULL);
	check("BIOCGDLT", bpf_ioctl(ch, BIOCGDLT, &v) == 0 && v == DLT_NULL, NULL);
	memset(ifr, 0, sizeof(ifr));
	check("BIOCGETIF", bpf_ioctl(ch, BIOCGETIF, ifr) == 0 && !strcmp(ifr, "lo0"), NULL);
	prog.bf_len = 6;
	prog.bf_insns = f;
	check("BIOCSETF", bpf_ioctl(ch, BIOCSETF, &prog) == 0, NULL);
	check("BIOCIMMEDIATE", bpf_ioctl(ch, BIOCIMMEDIATE, &one) == 0, NULL);
	check("bpf_set_notify_mask", bpf_set_notify_mask(ch, 1UL << sig) == 0, NULL);
	check("bpf_set_interrupt_mask", bpf_set_interrupt_mask(ch, SIGBREAKF_CTRL_E) == 0, NULL);
	check("bpf_data_waiting (empty)", bpf_data_waiting(ch) == 0, NULL);
	SetSignal(0, 1UL << sig);

	s = socket(AF_INET, SOCK_DGRAM, 0);
	sendto(s, "rs_test", 7, 0, &to, sizeof(to));
	Delay(10);
	check("bpf notify signal", (SetSignal(0, 1UL << sig) & (1UL << sig)) != 0, NULL);
	check("bpf_data_waiting", bpf_data_waiting(ch) == 1, NULL);
	check("FIONREAD", bpf_ioctl(ch, FIONREAD, &v) == 0 && v > 0, D("%lu", v));
	check("bpf_read wrong size fails", bpf_read(ch, buf, 100) == -1 && Errno() == EINVAL, NULL);
	n = bpf_read(ch, buf, 2048);
	check("bpf_read", n == 20 + 4 + 28 + 7 && *(UWORD *)(buf + 16) == 20 && *(ULONG *)(buf + 8) == 39 &&
	      *(ULONG *)(buf + 20) == AF_INET && buf[24] == 0x45 && !memcmp(buf + 52, "rs_test", 7), D("%ld bytes", n));
	check("BIOCGSTATS", bpf_ioctl(ch, BIOCGSTATS, st) == 0 && st[0] >= 1 && st[1] == 0, D("recv %lu drop %lu",
	      st[0], st[1]));

	/* a read that times out, and one interrupted */
	v = 0;
	bpf_ioctl(ch, BIOCIMMEDIATE, &v);
	tv[0] = 0;
	tv[1] = 200000;
	check("BIOCSRTIMEOUT", bpf_ioctl(ch, BIOCSRTIMEOUT, tv) == 0, NULL);
	check("bpf_read timeout", bpf_read(ch, buf, 2048) == 0, NULL);
	tv[1] = 0;
	bpf_ioctl(ch, BIOCSRTIMEOUT, tv);
	Signal(FindTask(NULL), SIGBREAKF_CTRL_E);
	check("bpf_read interrupted", bpf_read(ch, buf, 2048) == -1, D("errno %ld", Errno()));
	SetSignal(0, SIGBREAKF_CTRL_E);

	/* bpf_write: a datagram into lo0, received by a socket */
	r = socket(AF_INET, SOCK_DGRAM, 0);
	bind(r, &rs, sizeof(rs));
	IoctlSocket(r, FIONBIO, (char *)&one);
	*(ULONG *)pk = AF_INET;
	n = udp_packet(pk + 4, 4321, 9998, "written");
	check("bpf_write", bpf_write(ch, pk, n + 4) == n + 4, D("errno %ld", Errno()));
	Delay(10);
	memset(buf, 0, 16);
	check("bpf_write delivered", recv(r, buf, sizeof(buf), 0) == 7 && !memcmp(buf, "written", 7), NULL);
	CloseSocket(r);
	CloseSocket(s);
	ch2 = bpf_open(-1);
	check("bpf_close", bpf_close(ch) == 0 && bpf_close(ch) == -1 && Errno() == ENXIO, NULL);
	if (ch2 >= 0)
		bpf_close(ch2);
	FreeSignal(sig);
}

static void test_monitor(void)
{
	struct sockaddr_in l = sin_of(ip(127, 0, 0, 1), 7778), u = sin_of(ip(127, 0, 0, 1), 9997);
	LONG ls, s, r, one = 1, i;
	char buf[16];

	for (i = 0; i <= MHT_Bind; i++)
		init_hook(&monhooks[i], (APTR)mon_fn, i);
	check("AddNetMonitorHookTagList bad type fails", AddNetMonitorHookTagList(99, &monhooks[0], NULL) == -1 &&
	      Errno() == EINVAL, NULL);
	for (i = 0; i <= MHT_Bind; i++)
		if (AddNetMonitorHookTagList(i, &monhooks[i], NULL) != 0)
			break;
	check("AddNetMonitorHookTagList (every type)", i == MHT_Bind + 1, NULL);

	/* MHT_Bind, MHT_Connect (refused, then allowed), MHT_TCP_Connect */
	ls = socket(AF_INET, SOCK_STREAM, 0);
	check("MHT_Bind", bind(ls, &l, sizeof(l)) == 0 && mon_calls[MHT_Bind] == 1, NULL);
	listen(ls, 2);
	s = socket(AF_INET, SOCK_STREAM, 0);
	mon_verdict[MHT_Connect] = EACCES;
	check("MHT_Connect refuses", connect(s, &l, sizeof(l)) == -1 && Errno() == EACCES && mon_socket == s, NULL);
	mon_verdict[MHT_Connect] = 0;
	mon_verdict[MHT_TCP_Connect] = MA_DropWithReset;
	check("MHT_TCP_Connect resets", connect(s, &l, sizeof(l)) == -1 && Errno() == ECONNREFUSED &&
	      mon_calls[MHT_TCP_Connect] >= 1, D("errno %ld", Errno()));
	CloseSocket(s);
	mon_verdict[MHT_TCP_Connect] = MA_Continue;
	s = socket(AF_INET, SOCK_STREAM, 0);
	check("MHT_Connect allows", connect(s, &l, sizeof(l)) == 0 && mon_calls[MHT_Connect] == 3, NULL);
	CloseSocket(s);
	CloseSocket(ls);

	/* MHT_Send, MHT_UDP (drop), MHT_Packet */
	r = socket(AF_INET, SOCK_DGRAM, 0);
	bind(r, &u, sizeof(u));
	IoctlSocket(r, FIONBIO, (char *)&one);
	s = socket(AF_INET, SOCK_DGRAM, 0);
	check("MHT_Send", sendto(s, "a", 1, 0, &u, sizeof(u)) == 1 && mon_calls[MHT_Send] == 1, NULL);
	Delay(5);
	check("MHT_UDP sees the datagram", recv(r, buf, sizeof(buf), 0) == 1 && mon_calls[MHT_UDP] >= 1, NULL);
	mon_verdict[MHT_UDP] = MA_Drop;
	sendto(s, "b", 1, 0, &u, sizeof(u));
	Delay(5);
	check("MHT_UDP drops", recv(r, buf, sizeof(buf), 0) == -1 && Errno() == EWOULDBLOCK, NULL);
	mon_verdict[MHT_UDP] = MA_Continue;
	mon_verdict[MHT_Send] = EACCES;
	check("MHT_Send refuses", send(s, "c", 1, 0) == -1 && Errno() == EACCES, NULL);
	mon_verdict[MHT_Send] = 0;
	check("MHT_Packet", mon_calls[MHT_Packet] >= 2, D("%ld calls", (long)mon_calls[MHT_Packet]));
	CloseSocket(s);
	CloseSocket(r);
	for (i = 0; i <= MHT_Bind; i++)
		RemoveNetMonitorHook(&monhooks[i]);
	RemoveNetMonitorHook(NULL);
	i = mon_calls[MHT_Packet];
	s = socket(AF_INET, SOCK_DGRAM, 0);
	sendto(s, "d", 1, 0, &u, sizeof(u));
	CloseSocket(s);
	check("RemoveNetMonitorHook", mon_calls[MHT_Packet] == i && mon_calls[MHT_Send] == 3, NULL);
}

static void test_aam(void)
{
	struct MsgPort *port = CreateMsgPort();
	struct AddressAllocationMessage *m = NULL, mm;
	struct TagItem t[6];
	LONG r;

	t[0].ti_Tag = CAAMTA_ReplyPort;           t[0].ti_Data = (ULONG)port;
	t[1].ti_Tag = CAAMTA_DNSTableSize;        t[1].ti_Data = 4;
	t[2].ti_Tag = CAAMTA_RouterTableSize;     t[2].ti_Data = 2;
	t[3].ti_Tag = CAAMTA_RecordLeaseExpiration; t[3].ti_Data = TRUE;
	t[4].ti_Tag = CAAMTA_ClientIdentifier;    t[4].ti_Data = (ULONG)"rs-test";
	t[5].ti_Tag = TAG_DONE;
	r = CreateAddrAllocMessageA(AAM_VERSION, AAMP_DHCP, (STRPTR)"lo0", &m, t);
	check("CreateAddrAllocMessageA", r == CAAME_Success && m && m->aam_DNSTable && m->aam_DNSTableSize == 4 &&
	      m->aam_RouterTableSize == 2 && m->aam_LeaseExpires && m->aam_Timeout >= AAM_TIMEOUT_MIN &&
	      m->aam_ClientIdentifier && !strcmp((char *)m->aam_ClientIdentifier, "rs-test"), D("%ld", (long)r));
	check("CreateAddrAllocMessageA errors", CreateAddrAllocMessageA(AAM_VERSION, AAMP_DHCP, (STRPTR)"nosuch0", &mm, NULL)
	      == CAAME_Interface_not_found && CreateAddrAllocMessageA(9, AAMP_DHCP, (STRPTR)"lo0", &mm, NULL) ==
	      CAAME_Invalid_version && CreateAddrAllocMessageA(AAM_VERSION, 7, (STRPTR)"lo0", &mm, NULL) ==
	      CAAME_Invalid_protocol && CreateAddrAllocMessageA(AAM_VERSION, AAMP_DHCP, (STRPTR)"lo0", NULL, NULL) ==
	      CAAME_Invalid_result_ptr, NULL);
	if (m) {
		BeginInterfaceConfig(m);
		WaitPort(port);
		GetMsg(port);
		check("BeginInterfaceConfig(lo0)", m->aam_Result == AAMR_InterfaceWrongType, D("%ld", (long)m->aam_Result));
		AbortInterfaceConfig(m);
		DeleteAddrAllocMessage(m);
	}
	memset(&mm, 0, sizeof(mm));
	mm.aam_Message.mn_ReplyPort = port;
	mm.aam_Message.mn_Length = sizeof(mm);
	mm.aam_Version = 1;
	mm.aam_Protocol = AAMP_DHCP;
	strcpy(mm.aam_InterfaceName, "nosuch0");
	BeginInterfaceConfig(&mm);
	WaitPort(port);
	GetMsg(port);
	check("BeginInterfaceConfig(nosuch0)", mm.aam_Result == AAMR_InterfaceNotKnown, NULL);
	mm.aam_Version = 99;
	BeginInterfaceConfig(&mm);
	WaitPort(port);
	GetMsg(port);
	check("BeginInterfaceConfig(version 99)", mm.aam_Result == AAMR_VersionUnknown, NULL);
	DeleteAddrAllocMessage(&mm);        /* not ours: ignored */
	DeleteAddrAllocMessage(NULL);
	DeleteMsgPort(port);
}

int main(void)
{
	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) {
		printf("rs_test: cannot open bsdsocket.library version 4\n");
		return 20;
	}
	test_tags();
	test_interfaces();
	test_routes();
	test_dns();
	test_netdb();
	test_conversion();
	test_resolver();
	test_stats();
	test_roadshowdata();
	test_bpf();
	test_monitor();
	test_aam();
	printf("rs_test: %d passed, %d failed\n", passes, fails);
	CloseLibrary(SocketBase);
	return fails ? 10 : 0;
}
