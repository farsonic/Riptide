/*
 * Riptide - the public message port "RIPTIDE" (AmigaOS only).
 *
 * Include <exec/ports.h> (or Riptide's src/amiga/os headers) first.
 *
 * A client fills a struct RiptideMsg in public memory (MEMF_PUBLIC), sets
 * mn_ReplyPort and mn_Length = sizeof(struct RiptideMsg), and looks up the
 * port and PutMsg()s under Forbid() (the port disappears when Riptide
 * quits). Riptide handles the message in its own process, under the stack
 * lock, and always ReplyMsg()s it; it never waits for the client, so a
 * client that dies cannot block the stack. Commands that start a driver
 * ("interface ...") may take a moment, everything else is immediate.
 * When Riptide shuts down, queued messages are replied with result
 * ESHUTDOWN (58).
 *
 * Commands (cmd):
 *   RTCMD_STATUS   status summary: interfaces, routes, DNS, users
 *   RTCMD_QUIT     quit if no program has the libraries open
 *   RTCMD_COMMAND  text holds one line in the riptide.conf language, plus
 *                  the query verbs of rt_ctl_command() (include/rt/rt_ctl.h):
 *                  e.g. "route replace default via 10.0.2.2",
 *                  "ifconfig eth0 mtu 1400", "dhcp eth0 renew",
 *                  "nameserver del 10.0.2.3", "arp add IP MAC",
 *                  "interface eth1 device=a2065.device unit=0",
 *                  "interface eth1 delete", "sysctl -a",
 *                  "capture start iface=eth0", "capture save RAM:x.pcap".
 *   RTCMD_TABLES   (0.23) read tables of include/rt/rt_ctl.h without opening
 *                  a library: text holds their numbers (RT_CTL_*, decimal,
 *                  separated by spaces, e.g. "42 1 18"); needs a reply
 *                  buffer, which gets for each table a ULONG number, a
 *                  ULONG length and the dump as net.riptide.<number> gives
 *                  it (struct rt_ctl_hdr and the records), the next one at
 *                  the next 4-byte boundary. A table that failed or did not
 *                  fit has length 0 (and RTMSGF_TRUNCATED is set). replylen
 *                  is the bytes used. Older Riptide versions answer EINVAL.
 *
 * Reply: result is 0 or a positive errno (BSD numbering); the text reply
 * (output, or the error message) goes to 'reply' when the client supplies
 * a buffer (version 2 messages: mn_Length >= sizeof(struct RiptideMsg)),
 * otherwise to 'text'. It is always NUL terminated; RTMSGF_TRUNCATED says
 * it did not fit. A reply buffer must be ordinary RAM (Riptide checks it
 * with TypeOfMem()).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RIPTIDE_PORT_H
#define RIPTIDE_PORT_H

#define RIPTIDE_PORTNAME "RIPTIDE"

#define RTCMD_STATUS   1
#define RTCMD_QUIT     2
#define RTCMD_COMMAND  3
#define RTCMD_TABLES   4

#define RTMSGF_TRUNCATED 1

#define RIPTIDE_TEXTLEN 1024

struct RiptideMsg {
	struct Message msg;
	ULONG  cmd;               /* RTCMD_* */
	LONG   result;            /* out: 0 or errno */
	char   text[RIPTIDE_TEXTLEN]; /* in: command line; out: reply without 'reply' */
	/* ---- version 2 (Riptide 0.10) ---- */
	STRPTR reply;             /* in: optional reply buffer, or NULL */
	ULONG  replysize;         /* in: its size in bytes */
	ULONG  replylen;          /* out: length of the reply text */
	ULONG  flags;             /* out: RTMSGF_* */
};

/* size of a version 1 message (STATUS/QUIT clients of Riptide 0.9) */
#define RIPTIDE_MSG_V1_SIZE (sizeof(struct Message) + 8 + RIPTIDE_TEXTLEN)

#endif
