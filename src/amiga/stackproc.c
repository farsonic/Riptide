/*
 * Riptide - the Riptide process: configuration, the main loop that runs
 * timers and completed driver I/O under the stack lock, the public
 * "RIPTIDE" port (STATUS/QUIT) and shutdown.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

#define DEFAULT_CONFIG "ENV:Riptide/riptide.conf"
#define DEFAULT_HOSTS  "ENV:Riptide/hosts"

struct amiga_if *amiga_ifs;

static struct MsgPort *pub_port;
static struct MsgPort *tport;
static struct timerequest *treq;
static int timer_pending;
static u32 timer_deadline;
static int quit_requested;

struct amiga_if *amiga_if_find(const char *name)
{
	struct amiga_if *ai;

	for (ai = amiga_ifs; ai; ai = ai->next)
		if (!strcmp(ai->nif.name, name))
			return ai;
	return NULL;
}

/* ---- configuration ---- */

static const char *kv(const char *arg, const char *key)
{
	u32 l = (u32)strlen(key);

	if (!rt_strncasecmp(arg, key, l) && arg[l] == '=')
		return arg + l + 1;
	return NULL;
}

int amiga_config_unknown(int argc, char **argv, char *err, u32 errlen)
{
	int i;

	if (!rt_strcasecmp(argv[0], "interface")) {
		const char *dev = NULL, *mni = NULL, *opts = NULL, *v;
		unsigned long unit = 0;
		int mtu = 0, raw = 0, r;

		if (argc < 3) {
			rt_snprintf(err, errlen, "usage: interface NAME device=DEVICE|mni=DRIVER [unit=N] [mtu=N] [raw] [options=TEXT]");
			return -RT_EINVAL;
		}
		if (strlen(argv[1]) >= RT_IFNAMSIZ) {
			rt_snprintf(err, errlen, "interface name '%s' too long", argv[1]);
			return -RT_EINVAL;
		}
		if (amiga_if_find(argv[1]) || rt_netif_find(argv[1])) {
			rt_snprintf(err, errlen, "interface %s exists already", argv[1]);
			return -RT_EEXIST;
		}
		for (i = 2; i < argc; i++) {
			if ((v = kv(argv[i], "device")) != NULL)
				dev = v;
			else if ((v = kv(argv[i], "mni")) != NULL)
				mni = v;
			else if ((v = kv(argv[i], "unit")) != NULL)
				unit = rt_strtoul(v, NULL, 0);
			else if ((v = kv(argv[i], "mtu")) != NULL)
				mtu = (int)rt_strtoul(v, NULL, 0);
			else if ((v = kv(argv[i], "options")) != NULL)
				opts = v;
			else if (!rt_strcasecmp(argv[i], "raw"))
				raw = 1;
			else {
				rt_snprintf(err, errlen, "interface: unknown option '%s'", argv[i]);
				return -RT_EINVAL;
			}
		}
		if (!dev == !mni) {
			rt_snprintf(err, errlen, "interface: give either device= or mni=");
			return -RT_EINVAL;
		}
		/* The drivers take the lock themselves and may wait for the
		 * device; drop the configuration lock meanwhile. */
		rt_unlock();
		if (dev)
			r = sana2_open_if(argv[1], dev, (ULONG)unit, mtu, raw, err, errlen);
		else
			r = mni_open_if(argv[1], mni, (ULONG)unit, opts, mtu, err, errlen);
		rt_lock();
		return r;
	}
	if (!rt_strcasecmp(argv[0], "user")) {
		const char *v;
		if (argc < 2) {
			rt_snprintf(err, errlen, "usage: user NAME [uid=N] [gid=N] [home=DIR] [realname=TEXT]");
			return -RT_EINVAL;
		}
		rt_strlcpy(amiga_user_name, argv[1], sizeof(amiga_user_name));
		for (i = 2; i < argc; i++) {
			if ((v = kv(argv[i], "uid")) != NULL)
				amiga_user_uid = (LONG)rt_strtoul(v, NULL, 0);
			else if ((v = kv(argv[i], "gid")) != NULL)
				amiga_user_gid = (LONG)rt_strtoul(v, NULL, 0);
			else if ((v = kv(argv[i], "home")) != NULL)
				rt_strlcpy(amiga_user_home, v, sizeof(amiga_user_home));
			else if ((v = kv(argv[i], "realname")) != NULL)
				rt_strlcpy(amiga_user_realname, v, sizeof(amiga_user_realname));
			else {
				rt_snprintf(err, errlen, "user: unknown option '%s'", argv[i]);
				return -RT_EINVAL;
			}
		}
		return 0;
	}
	if (!rt_strcasecmp(argv[0], "loglevel") && argc >= 2) {
		rt_log_level = (int)rt_strtoul(argv[1], NULL, 0);
		return 0;
	}
	rt_snprintf(err, errlen, "unknown keyword '%s'", argv[0]);
	return -RT_EINVAL;
}

/* Read a text file line by line. Returns -1 if it cannot be opened. */
static BPTR open_quiet(const char *path)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR win = me->pr_WindowPtr;
	BPTR fh;

	/* no "Please insert volume ENV:" requester when ENV: is missing */
	me->pr_WindowPtr = (APTR)-1;
	fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	me->pr_WindowPtr = win;
	return fh;
}

static int for_each_line(const char *path, void (*fn)(char *line, int lineno, const char *path))
{
	BPTR fh = open_quiet(path);
	char *buf, line[256];
	LONG n, i;
	int ll = 0, lineno = 1;

	if (!fh)
		return -1;
	buf = rt_malloc(512);
	if (!buf) {
		Close(fh);
		return -1;
	}
	while ((n = Read(fh, buf, 512)) > 0) {
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				line[ll] = 0;
				fn(line, lineno++, path);
				ll = 0;
			} else if (buf[i] != '\r' && ll < (int)sizeof(line) - 1) {
				line[ll++] = buf[i];
			}
		}
	}
	if (ll) {
		line[ll] = 0;
		fn(line, lineno, path);
	}
	rt_free(buf);
	Close(fh);
	return 0;
}

static int config_errors;

static void config_line(char *line, int lineno, const char *path)
{
	char err[128];
	int r;

	rt_lock();
	r = rt_config_line(line, err, sizeof(err), amiga_config_unknown);
	rt_unlock();
	if (r < 0) {
		config_errors++;
		amiga_out("%s:%d: %s\n", path, lineno, err[0] ? err : rt_strerror(-r));
	}
	amiga_log_flush();
}

static void hosts_line(char *line, int lineno, const char *path)
{
	char *argv[8];
	char *hash = strchr(line, '#');
	u32 a;
	int argc, i;

	if (hash)
		*hash = 0;
	argc = rt_config_split(line, argv, 8);
	if (argc < 2)
		return;
	if (rt_parse_ipv4(argv[0], &a) < 0) {
		amiga_out("%s:%d: bad address '%s'\n", path, lineno, argv[0]);
		return;
	}
	for (i = 1; i < argc; i++)
		rt_hosts_add(argv[i], a);
}

int amiga_load_config(const char *path)
{
	int explicit_path = path != NULL;

	if (!path)
		path = DEFAULT_CONFIG;
	config_errors = 0;
	if (for_each_line(path, config_line) < 0) {
		if (explicit_path) {
			amiga_out("Riptide: cannot open %s\n", path);
			return -1;
		}
		amiga_out("Riptide: no %s, only the loopback interface is configured\n", path);
	}
	for_each_line(DEFAULT_HOSTS, hosts_line);
	return config_errors ? -1 : 0;
}

/* ---- status ---- */

int amiga_status_text(char *buf, u32 len)
{
	struct rt_netif *n;
	struct amiga_if *ai;
	u32 o = 0;
	char a[16], m[16];

	rt_lock();
	o += (u32)rt_snprintf(buf + o, len - o, "Riptide %s, host %s, %d library users\n",
	                      RIPTIDE_VERSION_STRING, rt_hostname[0] ? rt_hostname : "(unset)",
	                      amiga_libs_users());
	for (n = rt_netif_list(); n && o < len; n = n->next) {
		o += (u32)rt_snprintf(buf + o, len - o, "%-6s %s/%s mtu %u %s%s in %lu out %lu err %lu/%lu\n",
		                      n->name, rt_format_ipv4(n->addr, a), rt_format_ipv4(n->netmask, m),
		                      (unsigned)n->mtu, (n->flags & RT_IFF_UP) ? "UP" : "DOWN",
		                      (n->flags & RT_IFF_RUNNING) ? ",RUNNING" : "",
		                      (unsigned long)n->stats.ipackets, (unsigned long)n->stats.opackets,
		                      (unsigned long)n->stats.ierrors, (unsigned long)n->stats.oerrors);
		if (o >= len)
			break;
		ai = amiga_if_find(n->name);
		if (ai && ai->status && o + 8 < len) {
			o += (u32)rt_snprintf(buf + o, len - o, "       ");
			if (o < len)
				o += (u32)ai->status(ai, buf + o, len - o);
			if (o + 1 < len)
				buf[o++] = '\n';
		}
	}
	rt_unlock();
	if (o >= len)
		o = len - 1;
	buf[o] = 0;
	return (int)o;
}

/* ---- main loop ---- */

static void arm_timer(u32 ms)
{
	u32 now = rt_now_ms(), deadline;

	if (!treq)
		return;
	if (ms == RT_TIMER_NONE)
		ms = 10000;          /* idle tick: lets QUIT re-check users */
	deadline = now + ms;
	if (timer_pending) {
		if ((s32)(deadline - timer_deadline) >= 0)
			return;           /* the pending one fires earlier */
		AbortIO(&treq->tr_node);
		WaitIO(&treq->tr_node);
		timer_pending = 0;
	}
	treq->tr_node.io_Command = TR_ADDREQUEST;
	treq->tr_time.tv_secs = ms / 1000u;
	treq->tr_time.tv_micro = (ms % 1000u) * 1000u;
	SendIO(&treq->tr_node);
	timer_pending = 1;
	timer_deadline = deadline;
}

static int try_quit(char *msg, u32 len)
{
	int users = amiga_libs_users();

	if (users == 0 && amiga_libs_remove() == 0) {
		rt_snprintf(msg, len, "Riptide is shutting down.\n");
		quit_requested = 2;
		return 0;
	}
	rt_snprintf(msg, len, "Riptide: the TCP/IP libraries are still open (%d opens); not quitting.\n",
	            users ? users : 1);
	return -1;
}

static void handle_port(void)
{
	struct RiptideMsg *m;

	while ((m = (struct RiptideMsg *)GetMsg(pub_port)) != NULL) {
		if (m->msg.mn_Length < sizeof(*m)) {
			ReplyMsg(&m->msg);
			continue;
		}
		switch (m->cmd) {
		case RTCMD_STATUS:
			amiga_status_text(m->text, sizeof(m->text));
			m->result = 0;
			break;
		case RTCMD_QUIT:
			m->result = try_quit(m->text, sizeof(m->text));
			break;
		default:
			m->result = -1;
			rt_strlcpy(m->text, "unknown command\n", sizeof(m->text));
		}
		ReplyMsg(&m->msg);
	}
}

int amiga_stack_start(const char *config, int selftest)
{
	int r;

	rt_sock_async_hook = amiga_async_hook;
	r = rt_init();
	if (r < 0) {
		amiga_out("Riptide: initialisation failed: %s\n", rt_strerror(-r));
		return -1;
	}
	if (amiga_load_config(config) < 0 && config)
		return -1;
	if (selftest)
		return 0;
	tport = CreateMsgPort();
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
	if (!treq || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &treq->tr_node, 0) != 0) {
		amiga_out("Riptide: cannot open timer.device\n");
		if (treq)
			DeleteIORequest((struct IORequest *)treq);
		treq = NULL;
		return -1;
	}
	pub_port = CreateMsgPort();
	if (!pub_port)
		return -1;
	pub_port->mp_Node.ln_Name = (char *)RIPTIDE_PORTNAME;
	pub_port->mp_Node.ln_Pri = 0;
	AddPort(pub_port);
	if (amiga_libs_init() < 0) {
		amiga_out("Riptide: cannot create the libraries\n");
		return -1;
	}
	amiga_out("Riptide %s running: bsdsocket.library 4, miami.library 21, usergroup.library 4\n",
	          RIPTIDE_VERSION_STRING);
	return 0;
}

void amiga_stack_loop(void)
{
	char msg[128];

	quit_requested = 0;
	for (;;) {
		struct amiga_if *ai;
		ULONG wait = amiga_kick_mask | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F, sigs;
		u32 ms;

		rt_lock();
		for (ai = amiga_ifs; ai; ai = ai->next)
			if (ai->poll)
				ai->poll(ai);
		ms = rt_stack_poll();
		rt_unlock();
		amiga_log_flush();
		if (quit_requested == 2)
			break;
		if (quit_requested == 1 && try_quit(msg, sizeof(msg)) == 0) {
			amiga_out("%s", msg);
			break;
		}
		for (ai = amiga_ifs; ai; ai = ai->next)
			wait |= ai->sigmask;
		if (pub_port)
			wait |= 1UL << pub_port->mp_SigBit;
		if (tport)
			wait |= 1UL << tport->mp_SigBit;
		if (ms == 0) {
			sigs = SetSignal(0, wait) & wait;
		} else {
			arm_timer(ms);
			sigs = Wait(wait);
		}
		if (timer_pending && CheckIO(&treq->tr_node)) {
			WaitIO(&treq->tr_node);
			timer_pending = 0;
		}
		if (pub_port && (sigs & (1UL << pub_port->mp_SigBit)))
			handle_port();
		if (sigs & SIGBREAKF_CTRL_C) {
			if (try_quit(msg, sizeof(msg)) == 0) {
				amiga_out("%s", msg);
				break;
			}
			amiga_out("%s", msg);
			/* ask the users to finish, like MiamiDx, and re-check */
			amiga_libs_signal_users(SIGBREAKF_CTRL_C);
			quit_requested = 1;
		}
		if (sigs & SIGBREAKF_CTRL_F) {
			static char status[1024];
			char *p = status, *nl;
			amiga_status_text(status, sizeof(status));
			/* amiga_out() formats at most 255 characters at a time */
			while (*p) {
				nl = strchr(p, '\n');
				if (nl)
					*nl = 0;
				amiga_out("%s\n", p);
				if (!nl)
					break;
				p = nl + 1;
			}
		}
		if (quit_requested == 2)
			break;
	}
}

void amiga_stack_stop(void)
{
	struct amiga_if *ai, *next;

	if (pub_port) {
		struct Message *m;
		RemPort(pub_port);
		while ((m = GetMsg(pub_port)) != NULL)
			ReplyMsg(m);
		DeleteMsgPort(pub_port);
		pub_port = NULL;
	}
	for (ai = amiga_ifs; ai; ai = next) {
		next = ai->next;
		ai->close(ai);
	}
	amiga_ifs = NULL;
	if (treq) {
		if (timer_pending) {
			AbortIO(&treq->tr_node);
			WaitIO(&treq->tr_node);
		}
		CloseDevice(&treq->tr_node);
		DeleteIORequest((struct IORequest *)treq);
		treq = NULL;
	}
	if (tport)
		DeleteMsgPort(tport);
	tport = NULL;
	amiga_log_flush();
}
