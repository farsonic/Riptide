/*
 * Riptide - helpers shared by the TCP tests (tests/t_tcp_*.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef T_TCP_COMMON_H
#define T_TCP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stderr); } } while (0)
#define CHECKV(c, fmt, ...) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s (" fmt ")\n", __FILE__, __LINE__, #c, __VA_ARGS__); \
	fflush(stderr); } } while (0)

static inline struct rt_sockaddr_in t_sa(u32 ip_host, u16 port)
{
	struct rt_sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = rt_htonl(ip_host);
	s.sin_port = rt_htons(port);
	return s;
}

static inline double t_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Deterministic data pattern so the receiver verifies every byte. */
static inline u8 t_pat(u64 off, u32 seed)
{
	u32 x = (u32)(off >> 8) * 2654435761u ^ seed;
	x ^= x >> 13;
	return (u8)(x + (u32)off);
}

static inline void t_fill(u8 *buf, u32 len, u64 off, u32 seed)
{
	u32 i;
	for (i = 0; i < len; i++)
		buf[i] = t_pat(off + i, seed);
}

/* Send 'total' bytes of pattern on fd. Returns bytes sent. */
static inline u64 t_send_pattern(struct rt_sockctx *c, int fd, u64 total, u32 seed, u32 chunk)
{
	static __thread u8 buf[65536];
	u64 off = 0;

	if (chunk > sizeof(buf))
		chunk = sizeof(buf);
	while (off < total) {
		u32 n = (u32)((total - off) < chunk ? (total - off) : chunk);
		int r;
		t_fill(buf, n, off, seed);
		r = rt_api_send(c, fd, buf, n, 0);
		if (r <= 0) {
			fprintf(stderr, "send error %d at %llu\n", r, (unsigned long long)off);
			break;
		}
		off += (u32)r;
		if ((u32)r < n) {
			/* partial: resend the rest (pattern recomputed) */
		}
	}
	return off;
}

/* Receive until EOF verifying the pattern. Returns bytes received,
 * *bad = number of corrupt bytes. */
static inline u64 t_recv_pattern(struct rt_sockctx *c, int fd, u32 seed, u64 *bad, u32 chunk)
{
	static __thread u8 buf[65536];
	u64 off = 0;

	*bad = 0;
	if (chunk > sizeof(buf))
		chunk = sizeof(buf);
	for (;;) {
		int r = rt_api_recv(c, fd, buf, chunk, 0), i;
		if (r <= 0) {
			if (r < 0)
				fprintf(stderr, "recv error %d at %llu\n", r, (unsigned long long)off);
			break;
		}
		for (i = 0; i < r; i++)
			if (buf[i] != t_pat(off + i, seed))
				(*bad)++;
		off += (u32)r;
	}
	return off;
}

static inline int t_get_info(struct rt_sockctx *c, int fd, struct rt_tcp_info *ti)
{
	u32 len = sizeof(*ti);
	memset(ti, 0, sizeof(*ti));
	return rt_api_getsockopt(c, fd, RT_IPPROTO_TCP, RT_TCP_INFO, ti, &len);
}

static inline void t_print_info(const char *tag, const struct rt_tcp_info *ti)
{
	printf("%s: state %u ca %u opts 0x%x wscale %u/%u mss %u/%u rtt %uus var %u min %u rto %ums "
	       "cwnd %u ssthresh %u retrans %u reord %u snd_wnd %u rcv_wnd %u cc %s\n",
	       tag, ti->tcpi_state, ti->tcpi_ca_state, ti->tcpi_options, ti->tcpi_snd_wscale,
	       ti->tcpi_rcv_wscale, ti->tcpi_snd_mss, ti->tcpi_rcv_mss, ti->tcpi_rtt_us,
	       ti->tcpi_rttvar_us, ti->tcpi_min_rtt_us, ti->tcpi_rto_us / 1000, ti->tcpi_snd_cwnd,
	       ti->tcpi_snd_ssthresh, ti->tcpi_total_retrans, ti->tcpi_reordering,
	       ti->tcpi_snd_wnd, ti->tcpi_rcv_wnd, ti->tcpi_ca_name);
	printf("%s: segs in %u (%u data) out %u (%u data) acks out %u (delayed %u dup %u sack blocks %u) "
	       "dupacks in %u recov %u rto %u tlp %u zwin %u/%u busy %u rwnd %u sndbuf %u unread %u zwin %u ms\n",
	       tag, ti->tcpi_segs_in, ti->tcpi_data_segs_in, ti->tcpi_segs_out, ti->tcpi_data_segs_out,
	       ti->tcpi_acks_out, ti->tcpi_delayed_acks_out, ti->tcpi_dupacks_out, ti->tcpi_sack_blocks_out,
	       ti->tcpi_dupacks_in, ti->tcpi_fast_recoveries, ti->tcpi_rto_timeouts, ti->tcpi_tlp_probes,
	       ti->tcpi_zwin_out, ti->tcpi_zwin_in, ti->tcpi_busy_ms, ti->tcpi_rwnd_limited_ms,
	       ti->tcpi_sndbuf_limited_ms, ti->tcpi_unread_ms, ti->tcpi_zwin_ms);
}

static inline void t_print_stats(const char *tag)
{
	struct rt_tcp_stats *s = &rt_tcpstat;
	printf("%s: out %u in %u retrans %u rst %u timeouts %u fastretr %u ssretr %u tlp %u/%u "
	       "sackrec %u renorec %u rackloss %u dsack %u/%u undo %u spurRTO %u ofo %u/%u "
	       "hp %u/%u delack %u winprobe %u chal %u csumerr %u\n",
	       tag, s->OutSegs, s->InSegs, s->RetransSegs, s->OutRsts, s->TCPTimeouts,
	       s->TCPFastRetrans, s->TCPSlowStartRetrans, s->TCPLossProbes,
	       s->TCPLossProbeRecovery, s->TCPSackRecovery, s->TCPRenoRecovery, s->TCPRackLoss,
	       s->TCPDSACKRecv, s->TCPDSACKOldSent + s->TCPDSACKOfoSent, s->TCPFullUndo,
	       s->TCPSpuriousRTOs, s->TCPOFOQueue, s->TCPOFODrop, s->TCPHPHits, s->TCPHPAcks,
	       s->DelayedACKs, s->TCPWinProbe, s->TCPChallengeACK, s->InCsumErrors);
}

static inline int t_set_int(struct rt_sockctx *c, int fd, int level, int name, int v)
{
	return rt_api_setsockopt(c, fd, level, name, &v, sizeof(v));
}

static inline int t_set_nbio(struct rt_sockctx *c, int fd, int on)
{
	s32 v = on;
	return rt_api_ioctl(c, fd, RT_FIONBIO, &v);
}

static inline int t_sysctl(const char *name, s32 v)
{
	int r;
	rt_lock();
	r = rt_sysctl_byname(name, NULL, NULL, &v, sizeof(v));
	rt_unlock();
	return r;
}

#endif
