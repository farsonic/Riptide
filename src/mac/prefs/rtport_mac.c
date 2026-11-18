/*
 * Riptide - RiptidePrefs on the classic Mac OS: the stack's public port. On the Amiga the editor
 * talks to a running Riptide through the RIPTIDE message port (riptide_port.h: COMMAND, TABLES,
 * RESTART, STATUS). Here FindPort("RIPTIDE") answers when Riptide's .IPP driver is installed, and
 * a message put to that port is carried to the driver as a private .IPP Control call
 * (src/mac/ipp.c, RTMAC_PORT) and replied at once, so prefs_io.c and status_amiga.c work as they
 * are. Also cmdlib's cl_command(), cl_request() and cl_reply(), which prefs_io.c uses.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mui/mm.h"
#include "rt/riptide_port.h"
#include "mac_ipp_private.h"

static struct MsgPort rt_port;
static short ipp_ref;
static int ipp_checked, ipp_riptide;
static char *replybuf;
#define REPLYSIZE 4096

/* .IPP's reference number, and whether it is Riptide's (a MacTCP .IPP does not know RTMAC_PING) */
static int ipp_ok(void)
{
	if (!ipp_checked) {
		UBYTE pb[80];
		unsigned char name[6] = { 4, '.', 'I', 'P', 'P', 0 };
		ipp_checked = 1;
		memset(pb, 0, sizeof pb);
		*(unsigned char **)(pb + 18) = name;
		if (mac_pb(pb, TRAP_OPEN) != 0)
			return 0;
		ipp_ref = *(short *)(pb + 24);
		memset(pb, 0, sizeof pb);
		*(short *)(pb + 24) = ipp_ref;
		*(short *)(pb + 26) = RTMAC_PING;
		ipp_riptide = mac_pb(pb, TRAP_CONTROL) == 0 && *(ULONG *)(pb + 28) == RTMAC_MAGIC;
		mm_log(".IPP refnum %d: %s", ipp_ref, ipp_riptide ? "Riptide" : "not Riptide (MacTCP?)");
	}
	return ipp_riptide;
}

struct MsgPort *mm_riptide_port(void)
{
	return ipp_ok() ? &rt_port : NULL;
}

/* a RiptideMsg put to the RIPTIDE port: to the driver, and replied */
void mm_riptide_put(struct Message *msg)
{
	struct RiptideMsg *m = (struct RiptideMsg *)msg;
	struct rtmac_port_pb p;
	memset(&p, 0, sizeof p);
	p.ioCRefNum = ipp_ref;
	p.csCode = RTMAC_PORT;
	p.cmd = m->cmd;
	p.text = m->text;
	p.textlen = RIPTIDE_TEXTLEN;
	if (msg->mn_Length >= sizeof(*m) && m->reply && m->replysize > 1) {
		p.reply = (char *)m->reply;
		p.replysize = m->replysize;
	} else {
		p.reply = m->text;
		p.replysize = RIPTIDE_TEXTLEN;
	}
	if (mac_pb(&p, TRAP_CONTROL) != 0) {
		m->result = RT_EINVAL;
		rt_strlcpy(p.reply, "unknown command\n", p.replysize);
		p.replylen = (ULONG)strlen(p.reply);
	} else {
		m->result = (LONG)p.result;
	}
	if (msg->mn_Length >= sizeof(*m)) {
		m->replylen = p.replylen;
		m->flags = p.flags;
	}
	ReplyMsg(msg);
}

int cl_request(u32 cmd, const char *line, int print)
{
	struct MsgPort *reply, *port;
	struct RiptideMsg *m;
	int r;
	(void)print;
	if (!replybuf)
		replybuf = mm_alloc(REPLYSIZE);
	reply = CreateMsgPort();
	m = mm_alloc(sizeof(*m));
	if (!replybuf || !reply || !m) {
		r = RT_ENOMEM;
		goto out;
	}
	replybuf[0] = 0;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = cmd;
	m->reply = (STRPTR)replybuf;
	m->replysize = REPLYSIZE;
	if (line)
		rt_strlcpy(m->text, line, sizeof(m->text));
	port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
	if (!port) {
		rt_strlcpy(replybuf, "Riptide is not running", REPLYSIZE);
		r = -1;
		goto out;
	}
	PutMsg(port, &m->msg);
	WaitPort(reply);
	GetMsg(reply);
	r = (int)m->result;
out:
	if (m)
		mm_free(m);
	if (reply)
		DeleteMsgPort(reply);
	return r;
}

int cl_command(const char *line, int print)
{
	return cl_request(RTCMD_COMMAND, line, print);
}

const char *cl_reply(void)
{
	return replybuf ? replybuf : "";
}

const char *cl_strerror(int err)
{
	switch (err) {
	case RT_ENOMEM: return "out of memory";
	case RT_EINVAL: return "invalid argument";
	case RT_EBUSY: return "busy";
	}
	return "error";
}
