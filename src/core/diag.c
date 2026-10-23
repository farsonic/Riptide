/*
 * Riptide - diagnostics: the section profiler ("profile") and the debug
 * log ring ("debug"). See rt_diag.h.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Everything here runs with the stack lock held, except rt_prof_add()
 * for the counters that are measured outside the lock (each counter has
 * a single writer; a reset racing with it only loses that interval).
 * Reports convert ticks with 32-bit arithmetic only (no 64-bit mulu.l /
 * divu.l, which the 68060 traps).
 */
#include "infra.h"

#ifdef RT_NO_DIAG
/* built without diagnostics: the commands say so */
int (*rt_dbg_dump_hook)(const char *path, char *err, u32 errlen);
int rt_prof_command(int argc, char **argv, char *out, u32 outlen)
{
	rt_strlcpy(out, "profile: not in this build (RT_NO_DIAG)", outlen);
	return -RT_EOPNOTSUPP;
}
int rt_prof_report(char *out, u32 outlen)
{
	return rt_prof_command(0, NULL, out, outlen);
}
int rt_dbg_command(int argc, char **argv, char *out, u32 outlen)
{
	rt_strlcpy(out, "debug: not in this build (RT_NO_DIAG)", outlen);
	return -RT_EOPNOTSUPP;
}
u32 rt_dbg_snapshot(char *buf, u32 len)
{
	return 0;
}
void rt_dbg_line(u32 cat, const char *text)
{
}
void rt_diag_init(void)
{
	rt_timer_init(&dbg_sample_timer, dbg_sample, NULL);
}
#else

/* ---- 64-bit helpers for the reports ---- */

/* n / d, saturated to 32 bits; bit by bit, rarely called */
static u32 div64(u64 n, u32 d)
{
	u64 q = 0, r = 0;
	int i;

	if (!d)
		return 0xffffffffu;
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		q <<= 1;
		if (r >= d) {
			r -= d;
			q |= 1u;
		}
	}
	return q >> 32 ? 0xffffffffu : (u32)q;
}

/* ticks -> microseconds, saturated to 32 bits */
static u32 ticks_us(u64 t)
{
	u32 hz = rt_prof_hz(), s, r;

	if (hz == 1000000u)
		return t >> 32 ? 0xffffffffu : (u32)t;
	s = div64(t, hz);
	if (s >= 4294u)
		return 0xffffffffu;
	r = (u32)(t - rt_mul32x32(s, hz));
	return s * 1000000u + div64(rt_mul32x32(r, 1000000u), hz);
}

/* ======================================================================
 * Profiler
 * ====================================================================== */

#define PROF_MAXDEPTH 16

struct prof_frame {
	u32 start;
	u8  s;
};

volatile int rt_prof_on;
int rt_prof_depth;
static struct prof_frame prof_stack[PROF_MAXDEPTH];
static u32 prof_last;                  /* ticks when the top frame last resumed */
static u32 prof_overflow;              /* entries beyond PROF_MAXDEPTH */
static u64 prof_total[RT_PROF_N];
static u32 prof_count[RT_PROF_N];
static u32 prof_max[RT_PROF_N];        /* ticks */
static u32 prof_unbalanced;            /* exits without entry, unwinds */
static u64 prof_t0_us, prof_t1_us;     /* run: start, stop (0 = running) */
static u32 prof_rx0;                   /* received bytes at the start */
static u32 prof_tx0;                   /* TCP data segments sent at the start */
u32 rt_prof_ev[RT_EV_N];

static const char *const ev_name[RT_EV_N] = {
	"clock reads", "clock (cached)", "locks", "lock waits", "rt_malloc", "buf allocs",
	"buf cache miss", "kicks", "app wakeups", "driver tx", "driver rx", "stack loops",
};

static const char *const prof_name[RT_PROF_N] = {
	"stack loop", "timers", "ip input", "ip6 input", "tcp input", "udp input",
	"tcp output", "checksum", "copy to app", "copy from app", "driver send",
	"socket send", "driver rx copy", "lock wait app", "lock wait rt", "idle",
};

void rt_prof_enter(int s)
{
	u32 t = rt_prof_ticks();

	if (rt_prof_depth >= PROF_MAXDEPTH) {
		prof_overflow++;
		return;
	}
	if (rt_prof_depth)
		prof_total[prof_stack[rt_prof_depth - 1].s] += t - prof_last;
	prof_stack[rt_prof_depth].s = (u8)s;
	prof_stack[rt_prof_depth].start = t;
	rt_prof_depth++;
	prof_last = t;
}

static void prof_pop(u32 t)
{
	struct prof_frame *f = &prof_stack[--rt_prof_depth];
	u32 incl = t - f->start;

	prof_total[f->s] += t - prof_last;
	prof_count[f->s]++;
	if (incl > prof_max[f->s])
		prof_max[f->s] = incl;
	prof_last = t;
}

void rt_prof_exit(int s)
{
	if (prof_overflow) {
		prof_overflow--;
		return;
	}
	if (!rt_prof_depth || prof_stack[rt_prof_depth - 1].s != s) {
		/* profiling started inside this section */
		prof_unbalanced++;
		return;
	}
	prof_pop(rt_prof_ticks());
}

void rt_prof_unwind(void)
{
	u32 t = rt_prof_ticks();

	if (rt_prof_depth)
		prof_unbalanced++;
	while (rt_prof_depth)
		prof_pop(t);
	prof_overflow = 0;
}

void rt_prof_add(int c, u32 ticks)
{
	if (c < 0 || c >= RT_PROF_N)
		return;
	prof_total[c] += ticks;
	prof_count[c]++;
	if (ticks > prof_max[c])
		prof_max[c] = ticks;
}

static u32 rx_bytes_now(void)
{
	struct rt_netif *n;
	u32 sum = 0;

	for (n = rt_netif_list(); n; n = n->next)
		sum += n->stats.ibytes;
	return sum;
}

static void prof_reset(void)
{
	int i;

	for (i = 0; i < RT_PROF_N; i++) {
		prof_total[i] = 0;
		prof_count[i] = 0;
		prof_max[i] = 0;
	}
	rt_prof_depth = 0;
	prof_overflow = 0;
	prof_unbalanced = 0;
	prof_t0_us = rt_now_us();
	prof_t1_us = 0;
	prof_rx0 = rx_bytes_now();
	prof_tx0 = rt_tcpstat.OutSegs;
	for (i = 0; i < RT_EV_N; i++)
		rt_prof_ev[i] = 0;
}

static u64 prof_wall_us(void)
{
	if (!prof_t0_us)
		return 0;
	return (prof_t1_us ? prof_t1_us : rt_now_us()) - prof_t0_us;
}

static void fill_prof(struct rt_ctl_prof *r, int i)
{
	u64 us = 0;
	u32 c, t;

	memset(r, 0, sizeof(*r));
	if (i < 0) {
		rt_strlcpy(r->name, "wall", sizeof(r->name));
		r->kind = RT_CTL_PROF_WALL;
		r->count = prof_t0_us && !prof_t1_us && rt_prof_on;
		us = prof_wall_us();
		r->rx_bytes = prof_t0_us ? rx_bytes_now() - prof_rx0 : 0;
		r->total_us = us >> 32 ? 0xffffffffu : (u32)us;
		r->total_ms = div64(us, 1000u);
		return;
	}
	rt_strlcpy(r->name, prof_name[i], sizeof(r->name));
	r->kind = i < RT_PROF_NSECT ? RT_CTL_PROF_SECTION : RT_CTL_PROF_COUNTER;
	r->count = prof_count[i];
	r->total_us = ticks_us(prof_total[i]);
	/* total_ms without saturation: ticks / (hz / 1000) */
	c = rt_prof_hz() / 1000u;
	t = div64(prof_total[i], c ? c : 1u);
	r->total_ms = t;
	r->max_us = ticks_us(prof_max[i]);
}

static int h_profile(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_ctl_hdr h;
	u32 need = sizeof(h) + (RT_PROF_N + 1) * sizeof(struct rt_ctl_prof), fit, i;

	if (!oldlen)
		return 0;
	if (!old) {
		*oldlen = need;
		return 0;
	}
	if (*oldlen < sizeof(h)) {
		*oldlen = need;
		return -RT_ENOMEM;
	}
	fit = (*oldlen - (u32)sizeof(h)) / sizeof(struct rt_ctl_prof);
	if (fit > RT_PROF_N + 1)
		fit = RT_PROF_N + 1;
	h.version = RT_CTL_VERSION;
	h.recsize = sizeof(struct rt_ctl_prof);
	h.count = fit;
	memcpy(old, &h, sizeof(h));
	for (i = 0; i < fit; i++) {
		struct rt_ctl_prof r;
		fill_prof(&r, (int)i - 1);
		memcpy((u8 *)old + sizeof(h) + i * sizeof(r), &r, sizeof(r));
	}
	*oldlen = need;
	return fit < RT_PROF_N + 1 ? -RT_ENOMEM : 0;
}

/* "12.3" for value/10^dec... value in units of 1/10 */
static void fmt_tenths(char *b, u32 len, u32 tenths)
{
	rt_snprintf(b, len, "%lu.%lu", (unsigned long)(tenths / 10u), (unsigned long)(tenths % 10u));
}

int rt_prof_report(char *out, u32 outlen)
{
	struct rt_ctl_prof w, p;
	u8 order[RT_PROF_N];
	u32 tot[RT_PROF_N];            /* no record array: library stacks are small */
	int n = 0, i, j;
	u32 o = 0, wall_us, kb, sect_us = 0, other;
	char ms[16], pc[16], perkb[16];

	if (!out || !outlen)
		return 0;
	out[0] = 0;
	if (!prof_t0_us) {
		rt_strlcpy(out, "profile: not started (profile start)\n", outlen);
		return (int)strlen(out);
	}
	fill_prof(&w, -1);
	wall_us = w.total_us ? w.total_us : 1u;
	kb = w.rx_bytes / 1024u;
#define ADD(...) do { if (o + 1 < outlen) { o += (u32)rt_snprintf(out + o, outlen - o, __VA_ARGS__); \
	if (o >= outlen) o = outlen - 1; } } while (0)
	fmt_tenths(ms, sizeof(ms), div64((u64)w.total_us, 100u));
	ADD("profile: %s, %s ms wall clock, %lu KB received (all interfaces), clock %lu Hz\n",
	    rt_prof_on ? "running" : "stopped", ms, (unsigned long)kb, (unsigned long)rt_prof_hz());
	ADD("%-15s%8s%11s%7s%9s%9s%8s\n", "section", "calls", "total ms", "%wall", "us/call", "max us", "us/KB");
	for (i = 0; i < RT_PROF_N; i++) {
		fill_prof(&p, i);
		tot[i] = p.total_us;
		if (p.count || p.total_us)
			order[n++] = (u8)i;
		if (i < RT_PROF_NSECT)
			sect_us += p.total_us;
	}
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && tot[order[j]] > tot[order[j - 1]]; j--) {
			u8 t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (i = 0; i < n; i++) {
		fill_prof(&p, order[i]);
		fmt_tenths(ms, sizeof(ms), div64((u64)p.total_us, 100u));
		fmt_tenths(pc, sizeof(pc), div64(rt_mul32x32(p.total_us, 1000u), wall_us));
		if (kb)
			rt_snprintf(perkb, sizeof(perkb), "%lu", (unsigned long)(p.total_us / kb));
		else
			rt_strlcpy(perkb, "-", sizeof(perkb));
		ADD("%-15s%8lu%11s%7s%9lu%9lu%8s\n", p.name, (unsigned long)p.count, ms, pc,
		    (unsigned long)(p.count ? p.total_us / p.count : 0), (unsigned long)p.max_us, perkb);
	}
	other = sect_us + tot[RT_PROF_IDLE] < w.total_us ? w.total_us - sect_us - tot[RT_PROF_IDLE] : 0;
	fmt_tenths(ms, sizeof(ms), div64((u64)other, 100u));
	fmt_tenths(pc, sizeof(pc), div64(rt_mul32x32(other, 1000u), wall_us));
	ADD("%-15s%8s%11s%7s\n", "not in Riptide", "", ms, pc);
	ADD("Sections exclude the sections they call; \"max us\" includes them.\n"
	    "Counters (driver rx copy, lock waits, idle) overlap the rest.\n");
	if (prof_unbalanced)
		ADD("(%lu sections cut short by a lock release or a start inside them)\n",
		    (unsigned long)prof_unbalanced);
	{
		u32 segs = rt_tcpstat.OutSegs - prof_tx0, k = 0;
		char per[24];
		ADD("events (per received KB / per TCP segment sent, %lu sent):\n", (unsigned long)segs);
		for (i = 0; i < RT_EV_N; i++) {
			u32 v = rt_prof_ev[i];
			fmt_tenths(per, sizeof(per), segs ? div64(rt_mul32x32(v, 10u), segs) : 0);
			ADD("  %-15s%8lu %6lu/KB %6s/seg%s", ev_name[i], (unsigned long)v,
			    (unsigned long)(kb ? v / kb : 0), per, ++k % 2 ? "" : "\n");
		}
		if (k % 2)
			ADD("\n");
	}
#undef ADD
	return (int)o;
}

int rt_prof_command(int argc, char **argv, char *out, u32 outlen)
{
	const char *op = argc >= 2 ? argv[1] : "show";

	if (!rt_strcasecmp(op, "show")) {
		rt_prof_report(out, outlen);
		return 0;
	}
	if (!rt_strcasecmp(op, "start")) {
		rt_prof_on = 0;
		prof_reset();
		rt_prof_on = 1;
		rt_snprintf(out, outlen, "profile: started\n");
		return 0;
	}
	if (!rt_strcasecmp(op, "stop")) {
		if (rt_prof_on) {
			rt_prof_on = 0;
			rt_prof_unwind();
			prof_t1_us = rt_now_us();
		}
		rt_snprintf(out, outlen, "profile: stopped\n");
		return 0;
	}
	if (!rt_strcasecmp(op, "reset")) {
		int on = rt_prof_on;
		rt_prof_on = 0;
		prof_reset();
		if (!on)
			prof_t1_us = prof_t0_us;
		rt_prof_on = on;
		rt_snprintf(out, outlen, "profile: reset\n");
		return 0;
	}
	rt_snprintf(out, outlen, "usage: profile start|stop|reset|show");
	return -RT_EINVAL;
}

/* ======================================================================
 * Debug log ring
 * ====================================================================== */

#define DBG_DEFAULT_KB 16
#define DBG_MAX_KB     1024

u32 rt_dbg_mask;
int (*rt_dbg_dump_hook)(const char *path, char *err, u32 errlen);

static char *dbg_ring;
static u32 dbg_size;        /* bytes */
static u32 dbg_pos;         /* next write */
static u32 dbg_used;        /* <= dbg_size */
static u32 dbg_lost;        /* bytes overwritten */
static u32 dbg_lines;
static u64 dbg_t0;          /* time stamps count from here */
static u32 dbg_interval = 500;  /* tcpstat period, ms */
static struct rt_timer dbg_sample_timer;

static const struct {
	const char *name;
	u32 bit;
} dbg_cats[] = {
	{ "tcp", RT_DBG_TCP }, { "ip", RT_DBG_IP }, { "ip6", RT_DBG_IP6 },
	{ "driver", RT_DBG_DRIVER }, { "timer", RT_DBG_TIMER }, { "socket", RT_DBG_SOCKET },
	{ "dns", RT_DBG_DNS }, { "dhcp", RT_DBG_DHCP }, { "arp", RT_DBG_ARP }, { "log", RT_DBG_LOG },
	{ "tcpstat", RT_DBG_TCPSTAT },
	/* aliases */
	{ "sana2", RT_DBG_DRIVER }, { "mni", RT_DBG_DRIVER }, { "timers", RT_DBG_TIMER },
	{ "sock", RT_DBG_SOCKET }, { "udp", RT_DBG_SOCKET }, { "icmp", RT_DBG_IP },
	{ "icmp6", RT_DBG_IP6 }, { "dhcp6", RT_DBG_DHCP }, { "nd", RT_DBG_ARP }, { "nd6", RT_DBG_ARP },
};
#define DBG_NCATS 11        /* the first DBG_NCATS entries are the real names */

static const char *dbg_cat_name(u32 cat)
{
	int i;

	for (i = 0; i < DBG_NCATS; i++)
		if (cat & dbg_cats[i].bit)
			return dbg_cats[i].name;
	return "?";
}

static void dbg_put(const char *s, u32 n)
{
	u32 c;

	if (!dbg_ring || !n)
		return;
	if (n > dbg_size) {
		s += n - dbg_size;
		n = dbg_size;
	}
	c = RT_MIN(n, dbg_size - dbg_pos);
	memcpy(dbg_ring + dbg_pos, s, c);
	if (c < n)
		memcpy(dbg_ring, s + c, n - c);
	dbg_pos = (dbg_pos + n) % dbg_size;
	if (dbg_used + n > dbg_size) {
		dbg_lost += dbg_used + n - dbg_size;
		dbg_used = dbg_size;
	} else {
		dbg_used += n;
	}
}

static void dbg_emit(u32 cat, const char *text, u32 tlen)
{
	char pre[40];
	u64 us = rt_now_us() - dbg_t0;
	u32 s = div64(us, 1000000u);
	u32 f = (u32)(us - rt_mul32x32(s, 1000000u));
	int n;

	n = rt_snprintf(pre, sizeof(pre), "%4lu.%06lu %s: ", (unsigned long)s, (unsigned long)f, dbg_cat_name(cat));
	dbg_put(pre, (u32)n);
	while (tlen && (text[tlen - 1] == '\n' || text[tlen - 1] == '\r'))
		tlen--;
	dbg_put(text, tlen);
	dbg_put("\n", 1);
	dbg_lines++;
}

/* The ring belongs to the lock; callers without it (the resolver) get it
 * here. Never call these from an interrupt or a driver callback. */
void rt_dbg_printf(u32 cat, const char *fmt, ...)
{
	char line[200];
	va_list ap;
	int n, took;

	if (!dbg_ring)
		return;
	va_start(ap, fmt);
	n = rt_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	took = !rt_lock_held();
	if (took)
		rt_lock();
	dbg_emit(cat, line, RT_MIN((u32)n, (u32)sizeof(line) - 1));
	if (took)
		rt_unlock();
}

void rt_dbg_line(u32 cat, const char *text)
{
	int took;

	if (!dbg_ring || !(rt_dbg_mask & cat) || !text)
		return;
	took = !rt_lock_held();
	if (took)
		rt_lock();
	dbg_emit(cat, text, (u32)strlen(text));
	if (took)
		rt_unlock();
}

u32 rt_dbg_snapshot(char *buf, u32 len)
{
	u32 start, n, skip = 0, c;

	if (!dbg_ring || !dbg_used)
		return 0;
	start = (dbg_pos + dbg_size - dbg_used) % dbg_size;
	if (dbg_lost) {
		/* the oldest line was partly overwritten: start after it */
		while (skip < dbg_used && dbg_ring[(start + skip) % dbg_size] != '\n')
			skip++;
		if (skip < dbg_used)
			skip++;
	}
	n = dbg_used - skip;
	if (!buf)
		return n;
	if (n > len)
		n = len;
	start = (start + skip) % dbg_size;
	c = RT_MIN(n, dbg_size - start);
	memcpy(buf, dbg_ring + start, c);
	if (c < n)
		memcpy(buf + c, dbg_ring, n - c);
	return n;
}

static void dbg_clear(void)
{
	dbg_pos = dbg_used = dbg_lost = dbg_lines = 0;
	dbg_t0 = rt_now_us();
}

static int dbg_alloc(u32 kb)
{
	char *p = rt_malloc(kb * 1024u);

	if (!p)
		return -RT_ENOMEM;
	rt_free(dbg_ring);
	dbg_ring = p;
	dbg_size = kb * 1024u;
	dbg_clear();
	return 0;
}

static void dbg_status(char *out, u32 outlen)
{
	u32 o, i, any = 0;

	o = (u32)rt_snprintf(out, outlen, "debug: ");
	for (i = 0; i < DBG_NCATS && o < outlen; i++)
		if (rt_dbg_mask & dbg_cats[i].bit) {
			o += (u32)rt_snprintf(out + o, outlen - o, "%s%s", any ? "," : "", dbg_cats[i].name);
			any = 1;
		}
	if (o < outlen && !any)
		o += (u32)rt_snprintf(out + o, outlen - o, "off");
	if (o < outlen && (rt_dbg_mask & RT_DBG_TCPSTAT))
		o += (u32)rt_snprintf(out + o, outlen - o, " (tcpstat every %lu ms)", (unsigned long)dbg_interval);
	if (o < outlen)
		rt_snprintf(out + o, outlen - o, "; ring %lu KB, %lu bytes in %lu lines, %lu bytes overwritten\n",
		            (unsigned long)(dbg_size / 1024u), (unsigned long)dbg_used,
		            (unsigned long)dbg_lines, (unsigned long)dbg_lost);
}

static int dbg_show(u32 nlines, char *out, u32 outlen)
{
	u32 n = rt_dbg_snapshot(NULL, 0), i, j, start, lines = 0;
	char *buf;

	if (!n) {
		rt_strlcpy(out, "debug: the log is empty\n", outlen);
		return 0;
	}
	buf = rt_malloc(n);
	if (!buf)
		return -RT_ENOMEM;
	n = rt_dbg_snapshot(buf, n);
	/* the last nlines lines, as many of them as fit */
	start = n;
	for (i = n; i > 0 && lines < nlines; i = j) {
		j = i - 1;
		while (j > 0 && buf[j - 1] != '\n')
			j--;
		if (n - j >= outlen)
			break;
		start = j;
		lines++;
	}
	if (start == n && n >= outlen)
		start = n - (outlen - 1);      /* not even one line fits */
	memcpy(out, buf + start, n - start);
	out[n - start] = 0;
	rt_free(buf);
	return 0;
}

/* ---- tcpstat: periodic one-line samples of every TCP connection ---- */

#define SAMPLE_MAX 8
static struct {
	u16 lport, fport;
	u32 rx, tx, t;          /* bytes received / acked, rt_now_ms() */
} sample_prev[SAMPLE_MAX];

static u32 sample_rate(u32 bytes, u32 ms)
{
	/* KB/s = bytes * 1000 / 1024 / ms in 32 bits (no 64-bit divide) */
	return ms ? ((bytes >> 10) * 1000u + ((bytes & 1023u) * 1000u >> 10)) / ms : 0;
}

static void dbg_sample(void *arg)
{
	static struct rt_ctl_tcpinfo ti[SAMPLE_MAX];
	u32 now = rt_now_ms();
	int n, i, j;

	if (!(rt_dbg_mask & RT_DBG_TCPSTAT))
		return;
	n = rt_tcp_info_list(ti, SAMPLE_MAX);
	if (n > SAMPLE_MAX)
		n = SAMPLE_MAX;
	for (i = 0; i < n; i++) {
		const struct rt_ctl_tcpinfo *t = &ti[i];
		u32 rx = t->bytes_received_lo, tx = t->bytes_acked_lo, drx = 0, dtx = 0, dt = 0;
		if (t->state == RT_TCPS_LISTEN)
			continue;
		for (j = 0; j < SAMPLE_MAX; j++)
			if (sample_prev[j].lport == t->lport && sample_prev[j].fport == t->fport)
				break;
		if (j < SAMPLE_MAX) {
			drx = rx - sample_prev[j].rx;
			dtx = tx - sample_prev[j].tx;
			dt = now - sample_prev[j].t;
		} else {
			j = 0;
			while (j < SAMPLE_MAX - 1 && sample_prev[j].lport)
				j++;
		}
		sample_prev[j].lport = t->lport;
		sample_prev[j].fport = t->fport;
		sample_prev[j].rx = rx;
		sample_prev[j].tx = tx;
		sample_prev[j].t = now;
		rt_dbg_printf(RT_DBG_TCPSTAT,
		              "%u>%u st %u rx %lu KB/s tx %lu KB/s rtt %lu/%lu ms rto %lu cwnd %lu ssth %lu "
		              "swnd %lu rwnd %lu unread %lu ofo %lu infl %lu retr %lu rtos %lu acks %lu/%lu zw %lu/%lu",
		              (unsigned)t->lport, (unsigned)t->fport, (unsigned)t->state,
		              (unsigned long)sample_rate(drx, dt), (unsigned long)sample_rate(dtx, dt),
		              (unsigned long)(t->srtt_us / 1000u), (unsigned long)(t->rttvar_us / 1000u),
		              (unsigned long)(t->rto_us / 1000u), (unsigned long)t->cwnd,
		              (unsigned long)(t->ssthresh > 0xffff ? 0xffff : t->ssthresh),
		              (unsigned long)t->snd_wnd, (unsigned long)t->rcv_wnd, (unsigned long)t->recvq,
		              (unsigned long)t->ofo_bytes, (unsigned long)t->packets_out,
		              (unsigned long)t->total_retrans, (unsigned long)t->rto_timeouts,
		              (unsigned long)t->acks_out, (unsigned long)t->delayed_acks_out,
		              (unsigned long)t->zwin_out, (unsigned long)t->zwin_in);
	}
	rt_timer_after(&dbg_sample_timer, dbg_interval);
}

static int dbg_lookup(const char *name, u32 *bit)
{
	u32 i;

	if (!rt_strcasecmp(name, "all")) {
		*bit = RT_DBG_ALL;
		return 0;
	}
	for (i = 0; i < RT_ARRAY_SIZE(dbg_cats); i++)
		if (!rt_strcasecmp(name, dbg_cats[i].name)) {
			*bit = dbg_cats[i].bit;
			return 0;
		}
	return -1;
}

int rt_dbg_command(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: debug [CATEGORY[,...]|all|none] [size=KB] | debug show [N] | "
	                            "debug clear | debug dump FILE\n"
	                            "categories: tcp ip ip6 driver timer socket dns dhcp arp log tcpstat; "
	                            "+CAT adds, -CAT removes; interval=MS: tcpstat period";
	u32 mask = rt_dbg_mask, kb = 0;
	int i, plain = 0, r;

	if (argc < 2 || !rt_strcasecmp(argv[1], "status")) {
		dbg_status(out, outlen);
		return 0;
	}
	if (!rt_strcasecmp(argv[1], "show")) {
		u32 nl = argc >= 3 ? (u32)rt_strtoul(argv[2], NULL, 0) : 20u;
		return dbg_show(nl ? nl : 20u, out, outlen);
	}
	if (!rt_strcasecmp(argv[1], "clear")) {
		dbg_clear();
		dbg_status(out, outlen);
		return 0;
	}
	if (!rt_strcasecmp(argv[1], "dump")) {
		if (argc != 3) {
			rt_strlcpy(out, usage, outlen);
			return -RT_EINVAL;
		}
		if (!rt_dbg_dump_hook) {
			rt_strlcpy(out, "debug dump: not supported here", outlen);
			return -RT_EOPNOTSUPP;
		}
		return rt_dbg_dump_hook(argv[2], out, outlen);
	}
	for (i = 1; i < argc; i++) {
		char tok[24], *p = argv[i], *c;
		if (!rt_strncasecmp(p, "interval=", 9)) {
			const char *e;
			u32 ms = (u32)rt_strtoul(p + 9, &e, 0);
			if (!p[9] || *e || ms < 20 || ms > 60000) {
				rt_snprintf(out, outlen, "debug: interval must be 20..60000 ms");
				return -RT_EINVAL;
			}
			dbg_interval = ms;
			continue;
		}
		/* size=KB, also "size KB" / "size = KB" (AmigaDOS ReadArgs
		 * hands "SIZE=64" over as two words) */
		if (!rt_strncasecmp(p, "size=", 5) || !rt_strcasecmp(p, "size")) {
			const char *v = p + 4, *e;
			if (*v == '=')
				v++;
			if (!*v && i + 1 < argc && !strcmp(argv[i + 1], "="))
				i++;
			if (!*v && i + 1 < argc)
				v = argv[++i];
			kb = (u32)rt_strtoul(v, &e, 0);
			if (!*v || *e || kb < 1 || kb > DBG_MAX_KB) {
				rt_snprintf(out, outlen, "debug: size must be 1..%d KB", DBG_MAX_KB);
				return -RT_EINVAL;
			}
			continue;
		}
		/* comma separated list */
		while (*p) {
			u32 bit, l;
			int mode = 0;          /* 0 set, 1 add, -1 remove */
			c = strchr(p, ',');
			l = c ? (u32)(c - p) : (u32)strlen(p);
			if (l >= sizeof(tok))
				l = sizeof(tok) - 1;
			memcpy(tok, p, l);
			tok[l] = 0;
			p = c ? c + 1 : p + strlen(p);
			if (!tok[0])
				continue;
			if (tok[0] == '+' || tok[0] == '-') {
				mode = tok[0] == '+' ? 1 : -1;
				memmove(tok, tok + 1, strlen(tok));
			}
			if (!rt_strcasecmp(tok, "none") || !rt_strcasecmp(tok, "off")) {
				mask = 0;
				plain = 1;
				continue;
			}
			if (dbg_lookup(tok, &bit) < 0) {
				rt_snprintf(out, outlen, "debug: unknown category '%s'\n%s", tok, usage);
				return -RT_EINVAL;
			}
			if (mode == 0 && !plain) {
				mask = 0;
				plain = 1;
			}
			if (mode < 0)
				mask &= ~bit;
			else
				mask |= bit;
		}
	}
	if (kb && kb * 1024u != dbg_size) {
		r = dbg_alloc(kb);
		if (r < 0) {
			rt_snprintf(out, outlen, "debug: no memory for a %lu KB ring", (unsigned long)kb);
			return r;
		}
	}
	if (mask && !dbg_ring) {
		r = dbg_alloc(DBG_DEFAULT_KB);
		if (r < 0) {
			rt_snprintf(out, outlen, "debug: no memory for the ring");
			return r;
		}
	}
	rt_dbg_mask = mask;
	if (mask & RT_DBG_TCPSTAT) {
		if (!rt_timer_pending(&dbg_sample_timer))
			rt_timer_after(&dbg_sample_timer, dbg_interval);
	} else {
		rt_timer_del(&dbg_sample_timer);
	}
	dbg_status(out, outlen);
	return 0;
}

/* ---- registration ---- */

static const struct rt_sysctl diag_sysctls[] = {
	{ "net.riptide.profile", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 3, { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, RT_CTL_PROFILE },
	  NULL, 0, 0, h_profile, "rt_ctl_hdr + struct rt_ctl_prof[]" },
};

void rt_diag_init(void)
{
	rt_timer_init(&dbg_sample_timer, dbg_sample, NULL);
	rt_sysctl_register(diag_sysctls, (int)RT_ARRAY_SIZE(diag_sysctls));
}
#endif /* RT_NO_DIAG */
