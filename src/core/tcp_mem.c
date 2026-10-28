/*
 * Riptide - global limits for socket buffer memory (Linux tcp_mem).
 *
 * Every TCP connection auto-tunes its buffers up to net.inet.tcp.rmem_max
 * (and wmem_max); a browser with a dozen connections could take
 * megabytes, more than an 8 MB A1200 can spare. So all socket buffer
 * memory is counted (TCP receive, out-of-order and send queues, UDP and
 * raw receive queues) against three watermarks, in KB:
 *
 *   mem_pressure  above it the stack is "under pressure" until the use
 *                 falls below mem_low: buffers stop growing (receive
 *                 auto-tuning, send buffer growth, the receive window's
 *                 rcv_ssthresh) and new connections get a window scale
 *                 for 64 KB only;
 *   mem_high      above it a connection that holds more than 8 KB gets
 *                 no more: received data is dropped (the sender
 *                 retransmits) and writers wait; windows are clamped to
 *                 four segments.
 *
 * The defaults follow the free memory when the stack starts: a quarter
 * of it for mem_high, a sixth and an eighth for mem_pressure and mem_low
 * (Chip RAM only: of what is left after 256 KB for the display and the
 * system). The checks come with every segment, so they are cheap while
 * all packet buffers together (rt_buf_bytes_total(), an upper bound of
 * what the sockets hold) stay below a watermark; only above one is the
 * use counted by walking the connections, at most once per millisecond
 * below mem_pressure and at every check above it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

s32 sysctl_tcp_mem_low = 16384;        /* KB; tcp_mem_init() sets them */
s32 sysctl_tcp_mem_pressure = 24576;
s32 sysctl_tcp_mem_high = 32768;

static u32 used_bytes, used_stamp;
static u8 used_valid;
static u8 in_pressure;
static u32 pressure_start;

struct tcp_sock *tcp_first(void);

u32 tcp_mem_used(void)
{
	u32 now = tcp_jiffies(), n = 0;
	struct tcp_sock *tp;
	struct rt_socket *so;

	/* near the limits every check counts again (loopback moves hundreds
	 * of KB in a millisecond) */
	if (used_valid && now == used_stamp && used_bytes >> 10 < (u32)sysctl_tcp_mem_pressure)
		return used_bytes;
	for (tp = tcp_first(); tp; tp = tp->all_next)
		n += tp->rmem_alloc + tp->wmem_queued;
	for (so = rt_all_sockets; so; so = so->next_all)
		if (so->type != RT_SOCK_STREAM)
			n += so->rcvq.bytes + so->rcvq.qlen * (u32)sizeof(struct rt_buf);
	used_bytes = n;
	used_stamp = now;
	used_valid = 1;
	return n;
}

/* all packet buffers are below kb: so are the sockets' */
static int below(s32 kb)
{
	return (rt_buf_bytes_total() >> 10) < (u32)kb;
}

int tcp_under_memory_pressure(void)
{
	u32 kb, now;

	if (!in_pressure && below(sysctl_tcp_mem_pressure))
		return 0;
	kb = tcp_mem_used() >> 10;

	if (!in_pressure && kb > (u32)sysctl_tcp_mem_pressure) {
		in_pressure = 1;
		pressure_start = tcp_jiffies();
		rt_tcpstat.TCPMemoryPressures++;
		rt_log(RT_LOG_INFO, "tcp: memory pressure, %lu KB in socket buffers", (unsigned long)kb);
	} else if (in_pressure && kb < (u32)sysctl_tcp_mem_low) {
		now = tcp_jiffies();
		in_pressure = 0;
		rt_tcpstat.TCPMemoryPressuresMs += now - pressure_start;
	}
	return in_pressure;
}

int tcp_mem_over_high(void)
{
	if (below(sysctl_tcp_mem_high))
		return 0;
	return (tcp_mem_used() >> 10) >= (u32)sysctl_tcp_mem_high;
}

u32 tcp_rmem_max_now(void)
{
	if (tcp_under_memory_pressure())
		return RT_MIN((u32)sysctl_tcp_rmem_max, TCP_MAX_WINDOW);
	return (u32)sysctl_tcp_rmem_max;
}

/* 0 normal, 1 under pressure, 2 above mem_high */
static u32 mem_state(void)
{
	if (tcp_mem_over_high())
		return 2;
	return tcp_under_memory_pressure() ? 1u : 0u;
}

/* the socket memory fields of net.riptide.status */
u8 rt_tcp_mem_info(u32 *used_kb, u32 *pressure_kb, u32 *high_kb)
{
	*used_kb = tcp_mem_used() >> 10;
	*pressure_kb = (u32)sysctl_tcp_mem_pressure;
	*high_kb = (u32)sysctl_tcp_mem_high;
	return (u8)mem_state();
}

void rt_tcp_mem_status(char *buf, u32 len)
{
	static const char *const st[3] = { "", ", under pressure", ", at the limit" };

	rt_snprintf(buf, len, "TCP memory: %lu KB in socket buffers (pressure at %ld KB, limit %ld KB)%s\n",
	            (unsigned long)(tcp_mem_used() >> 10), (long)sysctl_tcp_mem_pressure,
	            (long)sysctl_tcp_mem_high, st[mem_state()]);
}

static int ro_int(void *old, u32 *oldlen, const void *newp, s32 v)
{
	if (newp)
		return -RT_EPERM;
	if (old) {
		if (!oldlen || *oldlen < sizeof(v))
			return -RT_ENOMEM;
		memcpy(old, &v, sizeof(v));
	}
	if (oldlen)
		*oldlen = sizeof(v);
	return 0;
}

static int mem_used_sysctl(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	return ro_int(old, oldlen, newp, (s32)(tcp_mem_used() >> 10));
}

static int mem_state_sysctl(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	return ro_int(old, oldlen, newp, (s32)mem_state());
}

#define NO_MIB 0, { 0, 0, 0, 0, 0, 0 }
static const struct rt_sysctl mem_sysctls[] = {
	{ "net.inet.tcp.mem_low", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_mem_low, 16, 4194304,
	  NULL, "Socket memory: pressure ends below (KB)" },
	{ "net.inet.tcp.mem_pressure", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_mem_pressure, 16, 4194304,
	  NULL, "Socket memory: buffers stop growing above (KB)" },
	{ "net.inet.tcp.mem_high", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_mem_high, 16, 4194304,
	  NULL, "Socket memory: hard limit (KB)" },
	{ "net.inet.tcp.mem_used", RT_SYSCTL_INT, RT_SYSCTL_RO, NO_MIB, NULL, 0, 0,
	  mem_used_sysctl, "Socket memory in use (KB)" },
	{ "net.inet.tcp.mem_state", RT_SYSCTL_INT, RT_SYSCTL_RO, NO_MIB, NULL, 0, 2,
	  mem_state_sysctl, "0 normal, 1 under pressure, 2 at the limit" },
};

void tcp_mem_init(void)
{
	int chip = 0;
	u32 avail = rt_os_mem_avail(&chip) >> 10, low, pres, high;

	if (avail) {
		if (chip)
			avail = avail > 512u ? avail - 256u : avail / 2u;
		low = RT_MAX(avail / 8u, 96u);
		pres = RT_MAX(avail / 6u, 128u);
		high = RT_MAX(avail / 4u, 192u);
		high = RT_MIN(high, 65536u);
		pres = RT_MIN(pres, high * 3u / 4u);
		low = RT_MIN(low, pres * 3u / 4u);
		sysctl_tcp_mem_low = (s32)low;
		sysctl_tcp_mem_pressure = (s32)pres;
		sysctl_tcp_mem_high = (s32)high;
	}
	in_pressure = 0;
	used_valid = 0;
	rt_sysctl_register(mem_sysctls, (int)RT_ARRAY_SIZE(mem_sysctls));
}
