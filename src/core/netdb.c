/*
 * Riptide - built-in services and protocols databases (the common entries
 * of /etc/services and /etc/protocols; IANA port numbers).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "rt/riptide.h"

#define S(n, p, pr)          { n, { NULL }, p, pr }
#define SA(n, a, p, pr)      { n, { a, NULL }, p, pr }
#define SA2(n, a, b, p, pr)  { n, { a, b, NULL }, p, pr }

static const struct rt_servent_info services[] = {
	S("tcpmux", 1, "tcp"),
	S("echo", 7, "tcp"),            S("echo", 7, "udp"),
	SA2("discard", "sink", "null", 9, "tcp"), SA2("discard", "sink", "null", 9, "udp"),
	SA("systat", "users", 11, "tcp"),
	S("daytime", 13, "tcp"),        S("daytime", 13, "udp"),
	S("netstat", 15, "tcp"),
	SA("qotd", "quote", 17, "tcp"),
	SA2("chargen", "ttytst", "source", 19, "tcp"), SA2("chargen", "ttytst", "source", 19, "udp"),
	S("ftp-data", 20, "tcp"),
	S("ftp", 21, "tcp"),
	S("ssh", 22, "tcp"),
	S("telnet", 23, "tcp"),
	SA("smtp", "mail", 25, "tcp"),
	SA("time", "timserver", 37, "tcp"), SA("time", "timserver", 37, "udp"),
	SA("nameserver", "name", 42, "udp"),
	SA("whois", "nicname", 43, "tcp"),
	S("domain", 53, "tcp"),         S("domain", 53, "udp"),
	S("bootps", 67, "udp"),
	S("bootpc", 68, "udp"),
	S("tftp", 69, "udp"),
	S("gopher", 70, "tcp"),
	S("finger", 79, "tcp"),
	SA2("http", "www", "www-http", 80, "tcp"),
	S("kerberos", 88, "tcp"),       S("kerberos", 88, "udp"),
	SA("pop2", "postoffice", 109, "tcp"),
	SA("pop3", "pop-3", 110, "tcp"),
	SA("sunrpc", "portmapper", 111, "tcp"), SA("sunrpc", "portmapper", 111, "udp"),
	SA2("auth", "ident", "tap", 113, "tcp"),
	S("sftp", 115, "tcp"),
	SA2("nntp", "readnews", "untp", 119, "tcp"),
	S("ntp", 123, "tcp"),           S("ntp", 123, "udp"),
	SA("netbios-ns", "nbns", 137, "udp"),
	SA("netbios-dgm", "nbdgm", 138, "udp"),
	SA("netbios-ssn", "nbssn", 139, "tcp"),
	SA("imap", "imap2", 143, "tcp"),
	S("snmp", 161, "udp"),
	S("snmp-trap", 162, "udp"),
	S("irc", 194, "tcp"),
	S("ldap", 389, "tcp"),
	S("https", 443, "tcp"),
	SA("microsoft-ds", "smb", 445, "tcp"),
	SA("exec", "rexec", 512, "tcp"),
	SA("biff", "comsat", 512, "udp"),
	S("login", 513, "tcp"),
	SA("who", "whod", 513, "udp"),
	SA("shell", "cmd", 514, "tcp"),
	S("syslog", 514, "udp"),
	S("printer", 515, "tcp"),
	S("talk", 517, "udp"),
	S("ntalk", 518, "udp"),
	SA("route", "router", 520, "udp"),
	S("timed", 525, "udp"),
	S("uucp", 540, "tcp"),
	S("submission", 587, "tcp"),
	S("ldaps", 636, "tcp"),
	S("rsync", 873, "tcp"),
	S("ftps-data", 989, "tcp"),
	S("ftps", 990, "tcp"),
	S("telnets", 992, "tcp"),
	S("imaps", 993, "tcp"),
	S("ircs", 994, "tcp"),
	S("pop3s", 995, "tcp"),
	S("socks", 1080, "tcp"),
	S("openvpn", 1194, "udp"),
	S("nfs", 2049, "udp"),          S("nfs", 2049, "tcp"),
	S("cvspserver", 2401, "tcp"),
	S("mysql", 3306, "tcp"),
	S("svn", 3690, "tcp"),
	S("sip", 5060, "udp"),
	S("xmpp-client", 5222, "tcp"),
	S("postgresql", 5432, "tcp"),
	S("x11", 6000, "tcp"),
	SA("ircd", "irc-alt", 6667, "tcp"),
	SA("http-alt", "webcache", 8080, "tcp"),
};

static const struct rt_protoent_info protocols[] = {
	{ "ip",   { "IP", NULL },   RT_IPPROTO_IP },
	{ "icmp", { "ICMP", NULL }, RT_IPPROTO_ICMP },
	{ "igmp", { "IGMP", NULL }, RT_IPPROTO_IGMP },
	{ "ggp",  { "GGP", NULL },  3 },
	{ "ipencap", { "IP-ENCAP", NULL }, 4 },
	{ "tcp",  { "TCP", NULL },  RT_IPPROTO_TCP },
	{ "egp",  { "EGP", NULL },  8 },
	{ "pup",  { "PUP", NULL },  12 },
	{ "udp",  { "UDP", NULL },  RT_IPPROTO_UDP },
	{ "idp",  { "IDP", NULL },  22 },
	{ "gre",  { "GRE", NULL },  47 },
	{ "esp",  { "IPSEC-ESP", NULL }, 50 },
	{ "ah",   { "IPSEC-AH", NULL }, 51 },
	{ "raw",  { "RAW", NULL },  RT_IPPROTO_RAW },
};

static int proto_match(const char *want, const char *have)
{
	return !want || !*want || rt_strcasecmp(want, have) == 0;
}

const struct rt_servent_info *rt_getservbyname(const char *name, const char *proto)
{
	unsigned i, k;

	if (!name)
		return NULL;
	for (i = 0; i < RT_ARRAY_SIZE(services); i++) {
		const struct rt_servent_info *s = &services[i];
		if (!proto_match(proto, s->proto))
			continue;
		if (strcmp(s->name, name) == 0)
			return s;
		for (k = 0; k < RT_ARRAY_SIZE(s->aliases) && s->aliases[k]; k++)
			if (strcmp(s->aliases[k], name) == 0)
				return s;
	}
	return NULL;
}

const struct rt_servent_info *rt_getservbyport(u16 port, const char *proto)
{
	unsigned i;

	for (i = 0; i < RT_ARRAY_SIZE(services); i++)
		if (services[i].port == port && proto_match(proto, services[i].proto))
			return &services[i];
	return NULL;
}

const struct rt_protoent_info *rt_getprotobyname(const char *name)
{
	unsigned i, k;

	if (!name)
		return NULL;
	for (i = 0; i < RT_ARRAY_SIZE(protocols); i++) {
		const struct rt_protoent_info *p = &protocols[i];
		if (rt_strcasecmp(p->name, name) == 0)
			return p;
		for (k = 0; k < RT_ARRAY_SIZE(p->aliases) && p->aliases[k]; k++)
			if (rt_strcasecmp(p->aliases[k], name) == 0)
				return p;
	}
	return NULL;
}

const struct rt_protoent_info *rt_getprotobynumber(int proto)
{
	unsigned i;

	for (i = 0; i < RT_ARRAY_SIZE(protocols); i++)
		if (protocols[i].proto == proto)
			return &protocols[i];
	return NULL;
}
