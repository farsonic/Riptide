/*
 * Riptide - packet capture (see rt_capture.h).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The ring holds finished pcap records (record header in host order, the
 * 16 byte SLL header, then up to snaplen bytes of the frame), each padded
 * to 4 bytes, so saving is mostly copying. In "wrap" mode the oldest
 * records make room for new ones; otherwise capture stops taking frames
 * when the ring is full and counts them as dropped.
 */
#include "infra.h"
#include "rt/rt_capture.h"

#define CAP_LINKTYPE  113          /* LINKTYPE_LINUX_SLL */
#define CAP_SLL       16
#define CAP_REC       16           /* pcap record header */
#define CAP_SNAP_DEF  128
#define CAP_SIZE_DEF  256          /* KB */
#define CAP_SIZE_MIN  16
#define CAP_SIZE_MAX  16384
#define CAP_CHUNK     4096         /* file write unit */

u8 rt_capture_on;

static struct {
	u8  *ring;
	u32  size;                 /* bytes */
	u32  head, tail;           /* oldest record, next free byte */
	u32  wrap_at;              /* end of the records before tail wrapped */
	u8   wrapped, wrap, running, saving;
	u32  frames, seen, dropped, evicted;
	u32  snaplen;
	u32  base_sec;             /* wall clock at base_us */
	u64  base_us;
	/* filters (0 = any) */
	u32  ifindex;
	u8   proto;                /* 6, 17, or 1 for ICMP and ICMPv6 */
	u8   hostlen;              /* 0, 4 or 16 */
	u16  port;
	u8   host[16];
	char filter[96];           /* as given, for status */
	char file[128];            /* save on stop / shutdown */
} cap;

/* ---- recording ---- */

static u32 rec_len(const u8 *p)
{
	return (CAP_REC + ((const u32 *)p)[2] + 3u) & ~3u;
}

static void evict_oldest(void)
{
	cap.head += rec_len(cap.ring + cap.head);
	cap.frames--;
	cap.evicted++;
	if (cap.wrapped && cap.head >= cap.wrap_at) {
		cap.head = 0;
		cap.wrapped = 0;
	}
	if (!cap.frames)
		cap.head = cap.tail = cap.wrapped = 0;
}

static u8 *ring_alloc(u32 len)
{
	u8 *p;

	for (;;) {
		if (!cap.wrapped) {
			if (cap.tail + len <= cap.size)
				break;
			if (!cap.wrap)
				return NULL;
			cap.wrap_at = cap.tail;
			cap.tail = 0;
			cap.wrapped = 1;
		}
		if (cap.head - cap.tail >= len && (cap.head != cap.tail || !cap.frames))
			break;
		evict_oldest();
	}
	p = cap.ring + cap.tail;
	cap.tail += len;
	return p;
}

/* Wall clock time without 64-bit divides (the 68060 traps them). */
static void stamp(u32 *sec, u32 *usec)
{
	u64 now = rt_now_us(), d64 = now - cap.base_us;
	u32 d, n;

	if (d64 >> 32) {
		cap.base_us = now;
		cap.base_sec = rt_wallclock();
		d = 0;
	} else {
		d = (u32)d64;
	}
	if (d >= 1000000u) {
		n = d / 1000000u;
		cap.base_sec += n;
		cap.base_us += rt_mul32x32(n, 1000000u);
		d -= n * 1000000u;
	}
	*sec = cap.base_sec;
	*usec = d;
}

static int match(const struct rt_netif *nif, const u8 *d, u32 len, u16 type)
{
	u32 off;
	u8 nh;
	int i;

	if (cap.ifindex && nif->index != cap.ifindex)
		return 0;
	if (!cap.proto && !cap.port && !cap.hostlen)
		return 1;
	if (type == RT_ETH_P_IP) {
		if (len < 20 || (d[0] >> 4) != 4)
			return 0;
		if (cap.hostlen == 16 || (cap.hostlen && memcmp(d + 12, cap.host, 4) && memcmp(d + 16, cap.host, 4)))
			return 0;
		nh = d[9];
		off = (u32)(d[0] & 15) << 2;
		if (rt_get_be16(d + 6) & 0x1fff)
			off = len;             /* later fragment: no ports */
	} else if (type == RT_ETH_P_IPV6) {
		if (len < 40 || (d[0] >> 4) != 6)
			return 0;
		if (cap.hostlen == 4 || (cap.hostlen && memcmp(d + 8, cap.host, 16) && memcmp(d + 24, cap.host, 16)))
			return 0;
		nh = d[6];
		off = 40;
		for (i = 0; i < 4 && off + 8 <= len; i++) {
			if (nh == 0 || nh == 43 || nh == 60) {
				nh = d[off];
				off += ((u32)d[off + 1] + 1) << 3;
			} else if (nh == 44) {
				if (rt_get_be16(d + off + 2) & 0xfff8) {
					off = len;     /* later fragment: no ports */
					break;
				}
				nh = d[off];
				off += 8;
			} else {
				break;
			}
		}
		if (nh == 58)
			nh = 1;
	} else {
		return 0;                  /* ARP etc. only without L3 filters */
	}
	if (cap.proto && nh != cap.proto)
		return 0;
	if (cap.port) {
		if ((nh != 6 && nh != 17) || off + 4 > len)
			return 0;
		if (rt_get_be16(d + off) != cap.port && rt_get_be16(d + off + 2) != cap.port)
			return 0;
	}
	return 1;
}

void rt_capture_frame(struct rt_netif *nif, const struct rt_buf *b, int dir,
                      u16 ethertype, const u8 *srchw, u16 flags)
{
	u32 caplen, *h;
	u8 *p;
	u16 pkt;

	if (dir == RT_CAP_OUT && nif->type == RT_IFT_LOOP)
		return;                    /* seen when lo0 delivers it */
	if (!match(nif, b->data, b->len, ethertype))
		return;
	cap.seen++;
	caplen = RT_MIN(b->len, cap.snaplen);
	p = ring_alloc((CAP_REC + CAP_SLL + caplen + 3u) & ~3u);
	if (!p) {
		cap.dropped++;
		return;
	}
	h = (u32 *)p;
	stamp(&h[0], &h[1]);
	h[2] = CAP_SLL + caplen;
	h[3] = CAP_SLL + b->len;
	p += CAP_REC;
	pkt = dir == RT_CAP_OUT ? 4 : (flags & RT_BUFF_BCAST) ? 1 : (flags & RT_BUFF_MCAST) ? 2 : 0;
	rt_put_be16(p, pkt);
	rt_put_be16(p + 2, nif->type == RT_IFT_ETHER ? 1 : nif->type == RT_IFT_LOOP ? 772 : 0xfffe);
	memset(p + 4, 0, 10);
	if (dir == RT_CAP_OUT && nif->type == RT_IFT_ETHER)
		srchw = nif->hwaddr;
	if (srchw && nif->hwlen && nif->hwlen <= 8) {
		rt_put_be16(p + 4, nif->hwlen);
		memcpy(p + 6, srchw, nif->hwlen);
	}
	rt_put_be16(p + 14, ethertype);
	memcpy(p + CAP_SLL, b->data, caplen);
	cap.frames++;
}

/* ---- saving ---- */

static int save(const char *path, char *out, u32 outlen)
{
	u8 *chunk;
	u32 used = 0, pos = cap.head, end, n = 0, l, *h;
	u8 was_on = rt_capture_on, wrapped = cap.wrapped;
	int held = rt_lock_held(), r = 0;
	void *f;

	if (!cap.ring) {
		rt_snprintf(out, outlen, "capture: nothing captured\n");
		return -RT_ENOENT;
	}
	chunk = rt_malloc(CAP_CHUNK);
	if (!chunk) {
		rt_snprintf(out, outlen, "capture: out of memory\n");
		return -RT_ENOMEM;
	}
	/* The ring stays as it is while the lock is dropped: recording
	 * pauses, and 'saving' refuses start/clear from another caller. */
	rt_capture_on = 0;
	cap.saving = 1;
	if (held)
		rt_unlock();
	f = rt_file_create(path);
	if (!f) {
		r = -RT_EIO;
		goto done;
	}
	h = (u32 *)chunk;
	h[0] = 0xa1b2c3d4u;
	((u16 *)chunk)[2] = 2;
	((u16 *)chunk)[3] = 4;
	h[2] = 0;
	h[3] = 0;
	h[4] = CAP_SLL + cap.snaplen;
	h[5] = CAP_LINKTYPE;
	used = 24;
	end = wrapped ? cap.wrap_at : cap.tail;
	while (n < cap.frames) {
		if (pos >= end) {
			pos = 0;           /* only reached when wrapped */
			end = cap.tail;
		}
		l = CAP_REC + ((u32 *)(cap.ring + pos))[2];
		if (used + l > CAP_CHUNK) {
			if (rt_file_write(f, chunk, used) < 0) {
				r = -RT_EIO;
				break;
			}
			used = 0;
		}
		memcpy(chunk + used, cap.ring + pos, l);
		used += l;
		pos += (l + 3u) & ~3u;
		n++;
	}
	if (!r && used && rt_file_write(f, chunk, used) < 0)
		r = -RT_EIO;
	if (rt_file_close(f) < 0 && !r)
		r = -RT_EIO;
done:
	if (held)
		rt_lock();
	rt_free(chunk);
	cap.saving = 0;
	rt_capture_on = was_on && cap.running;
	if (r < 0)
		rt_snprintf(out, outlen, "capture: cannot write '%s'\n", path);
	else
		rt_snprintf(out, outlen, "capture: %lu frames saved to %s\n", (unsigned long)n, path);
	return r;
}

/* ---- commands ---- */

static void status(char *out, u32 outlen)
{
	u32 used = !cap.frames ? 0 : cap.wrapped ? cap.wrap_at - cap.head + cap.tail : cap.tail - cap.head;

	if (!cap.ring) {
		rt_strlcpy(out, "capture: off\n", outlen);
		return;
	}
	rt_snprintf(out, outlen,
	            "capture: %s, %lu frames (%lu of %lu KB), %lu seen, %lu dropped, %lu overwritten\n"
	            "  snaplen %lu, %s%s%s%s%s\n",
	            cap.running ? "running" : "stopped",
	            (unsigned long)cap.frames, (unsigned long)((used + 1023) >> 10),
	            (unsigned long)(cap.size >> 10), (unsigned long)cap.seen,
	            (unsigned long)cap.dropped, (unsigned long)cap.evicted,
	            (unsigned long)cap.snaplen, cap.wrap ? "wrap" : "stop when full",
	            cap.filter[0] ? ", " : "", cap.filter,
	            cap.file[0] ? ", file " : "", cap.file);
}

static void release(void)
{
	rt_capture_on = 0;
	cap.running = 0;
	rt_free(cap.ring);
	cap.ring = NULL;
	cap.size = 0;
	cap.file[0] = 0;
}

static int bad(char *out, u32 outlen, const char *what, const char *arg)
{
	rt_snprintf(out, outlen, "capture: bad %s '%s'\n", what, arg);
	return -RT_EINVAL;
}

static int start(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: capture start [iface=NAME] [proto=tcp|udp|icmp] [port=N] "
	                            "[host=ADDR] [snaplen=N] [size=KB] [wrap] [file=PATH]\n";
	u32 snap = CAP_SNAP_DEF, kb = CAP_SIZE_DEF, ifindex = 0, port = 0, hostlen = 0;
	u8 proto = 0, wrap = 0, host[16];
	const char *file = "";
	const char *end;
	char *k, *v, filter[sizeof(cap.filter)];
	int i;

	filter[0] = 0;
	for (i = 2; i < argc; i++) {
		k = argv[i];
		v = strchr(k, '=');
		if (!rt_strcasecmp(k, "wrap")) {
			wrap = 1;
			continue;
		}
		if (!v) {
			rt_strlcpy(out, usage, outlen);
			return -RT_EINVAL;
		}
		*v++ = 0;
		if (!rt_strcasecmp(k, "iface") || !rt_strcasecmp(k, "if")) {
			struct rt_netif *nif = rt_netif_find(v);
			if (!nif) {
				rt_snprintf(out, outlen, "capture: no interface '%s'\n", v);
				return -RT_ENXIO;
			}
			ifindex = nif->index;
		} else if (!rt_strcasecmp(k, "proto")) {
			if (!rt_strcasecmp(v, "tcp"))
				proto = 6;
			else if (!rt_strcasecmp(v, "udp"))
				proto = 17;
			else if (!rt_strcasecmp(v, "icmp") || !rt_strcasecmp(v, "icmp6"))
				proto = 1;
			else
				return bad(out, outlen, "protocol", v);
		} else if (!rt_strcasecmp(k, "port")) {
			port = (u32)rt_strtoul(v, &end, 10);
			if (*end || !port || port > 65535)
				return bad(out, outlen, "port", v);
		} else if (!rt_strcasecmp(k, "host")) {
			if (strchr(v, ':')) {
				if (rt_inet6_pton(v, (rt_in6_addr *)host) < 0)
					return bad(out, outlen, "address", v);
				hostlen = 16;
			} else {
				u32 a;
				if (rt_parse_ipv4(v, &a) < 0)
					return bad(out, outlen, "address", v);
				memcpy(host, &a, 4);
				hostlen = 4;
			}
		} else if (!rt_strcasecmp(k, "snaplen")) {
			snap = (u32)rt_strtoul(v, &end, 10);
			if (*end || snap < 20 || snap > 65535)
				return bad(out, outlen, "snaplen", v);
		} else if (!rt_strcasecmp(k, "size")) {
			kb = (u32)rt_strtoul(v, &end, 10);
			if (*end || kb < CAP_SIZE_MIN || kb > CAP_SIZE_MAX)
				return bad(out, outlen, "size (KB, 16 to 16384)", v);
		} else if (!rt_strcasecmp(k, "file")) {
			if (strlen(v) >= sizeof(cap.file))
				return bad(out, outlen, "file name", v);
			file = v;
		} else {
			rt_strlcpy(out, usage, outlen);
			return -RT_EINVAL;
		}
		if (rt_strcasecmp(k, "file") && rt_strcasecmp(k, "size") && rt_strcasecmp(k, "snaplen")) {
			u32 l = (u32)strlen(filter);
			rt_snprintf(filter + l, sizeof(filter) - l, "%s%s %s", l ? " " : "", k, v);
		}
	}
	rt_capture_on = 0;
	if (!cap.ring || cap.size != kb << 10) {
		release();
		cap.ring = rt_malloc(kb << 10);
		if (!cap.ring) {
			rt_snprintf(out, outlen, "capture: not enough memory for %lu KB\n", (unsigned long)kb);
			return -RT_ENOMEM;
		}
		cap.size = kb << 10;
	}
	/* a record must always fit in a quarter of the ring */
	cap.snaplen = RT_MIN(snap, (cap.size >> 2) - CAP_REC - CAP_SLL);
	cap.head = cap.tail = cap.wrap_at = 0;
	cap.wrapped = 0;
	cap.frames = cap.seen = cap.dropped = cap.evicted = 0;
	cap.wrap = wrap;
	cap.ifindex = ifindex;
	cap.proto = proto;
	cap.port = (u16)port;
	cap.hostlen = (u8)hostlen;
	memcpy(cap.host, host, hostlen);
	rt_strlcpy(cap.filter, filter, sizeof(cap.filter));
	rt_strlcpy(cap.file, file, sizeof(cap.file));
	cap.base_us = rt_now_us();
	cap.base_sec = rt_wallclock();
	cap.running = 1;
	rt_capture_on = 1;
	status(out, outlen);
	return 0;
}

int rt_capture_command(int argc, char **argv, char *out, u32 outlen)
{
	const char *op = argc >= 2 ? argv[1] : "status";
	char dummy[2];

	if (!out || !outlen) {
		out = dummy;
		outlen = sizeof(dummy);
	}
	out[0] = 0;
	if (cap.saving && rt_strcasecmp(op, "status")) {
		rt_strlcpy(out, "capture: busy saving\n", outlen);
		return -RT_EBUSY;
	}
	if (!rt_strcasecmp(op, "start"))
		return start(argc, argv, out, outlen);
	if (!rt_strcasecmp(op, "stop")) {
		rt_capture_on = 0;
		cap.running = 0;
		if (cap.file[0])
			return save(cap.file, out, outlen);
		status(out, outlen);
		return 0;
	}
	if (!rt_strcasecmp(op, "status")) {
		status(out, outlen);
		return 0;
	}
	if (!rt_strcasecmp(op, "save")) {
		const char *path = argc >= 3 ? argv[2] : cap.file;
		if (!path[0]) {
			rt_strlcpy(out, "usage: capture save FILE\n", outlen);
			return -RT_EINVAL;
		}
		return save(path, out, outlen);
	}
	if (!rt_strcasecmp(op, "clear")) {
		release();
		cap.frames = cap.seen = cap.dropped = cap.evicted = 0;
		status(out, outlen);
		return 0;
	}
	rt_strlcpy(out, "usage: capture start|stop|status|save FILE|clear\n", outlen);
	return -RT_EINVAL;
}

void rt_capture_shutdown(void)
{
	char msg[128];

	if (cap.running && cap.file[0] && !cap.saving) {
		cap.running = 0;
		save(cap.file, msg, sizeof(msg));
		rt_log(RT_LOG_INFO, "%s", msg);
	}
	if (!cap.saving)
		release();
}
