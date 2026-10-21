/*
 * Riptide - speedtest: fixed-point helpers (coordinates, distances, rates,
 * dates). No floating point: many 68020 machines have no FPU.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "speedtest.h"

int st_parse_coord(const char *s, s32 *udeg)
{
	u32 ip = 0, frac = 0, scale = 1000000;
	int neg = 0, digits = 0;

	if (!s)
		return -1;
	while (*s == ' ')
		s++;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	while (*s >= '0' && *s <= '9') {
		ip = ip * 10 + (u32)(*s++ - '0');
		digits++;
		if (ip > 360)
			return -1;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (scale > 1) {
				scale /= 10;
				frac += (u32)(*s - '0') * scale;
			}
			s++;
			digits++;
		}
	}
	while (*s == ' ')
		s++;
	if (!digits || *s)
		return -1;
	*udeg = (s32)(ip * 1000000UL + frac) * (neg ? -1 : 1);
	return 0;
}

/* cos(d) for d = 0..90 degrees, Q15 */
static const u16 cos_q15[91] = {
	32768, 32763, 32748, 32723, 32688, 32643, 32588, 32524, 32449, 32365,
	32270, 32166, 32052, 31928, 31795, 31651, 31499, 31336, 31164, 30983,
	30792, 30592, 30382, 30163, 29935, 29698, 29452, 29197, 28932, 28660,
	28378, 28088, 27789, 27482, 27166, 26842, 26510, 26170, 25822, 25466,
	25102, 24730, 24351, 23965, 23571, 23170, 22763, 22348, 21926, 21498,
	21063, 20622, 20174, 19720, 19261, 18795, 18324, 17847, 17364, 16877,
	16384, 15886, 15384, 14876, 14365, 13848, 13328, 12803, 12275, 11743,
	11207, 10668, 10126, 9580, 9032, 8481, 7927, 7371, 6813, 6252,
	5690, 5126, 4560, 3993, 3425, 2856, 2286, 1715, 1144, 572,
	0
};

/* cos of |lat| (micro-degrees, <= 90e6), Q15, linearly interpolated */
static u32 cos_udeg(u32 a)
{
	u32 d = a / 1000000UL, f = a % 1000000UL;

	if (d >= 90)
		return 0;
	return cos_q15[d] - (u32)(((u32)(cos_q15[d] - cos_q15[d + 1]) * (f / 1000UL)) / 1000UL);
}

u64 st_isqrt64(u64 v)
{
	u64 r = 0, bit = (u64)1 << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

u32 st_distance(s32 lat1, s32 lon1, s32 lat2, s32 lon2)
{
	s32 dlat = lat2 - lat1, dlon = lon2 - lon1, mean = (s32)(((s64)lat1 + lat2) / 2);
	s64 x;
	u64 d;

	if (dlon > 180000000L)
		dlon -= 360000000L;
	else if (dlon < -180000000L)
		dlon += 360000000L;
	if (mean < 0)
		mean = -mean;
	x = ((s64)dlon * (s64)cos_udeg((u32)mean)) / 32768;
	d = st_isqrt64((u64)(x * x) + (u64)((s64)dlat * dlat));       /* micro-degrees */
	/* 1 degree of a great circle on a 6371 km sphere = 111.19493 km */
	d = d * 11119493ULL / 1000000000ULL;
	return d > 2001508UL ? 2001508UL : (u32)d;                   /* half the circumference */
}

char *st_fmt_ratio(char *buf, u32 size, u64 num, u64 den, int dec)
{
	u64 q, r, m = 1, f;
	int i;

	if (!den) {
		num = 0;
		den = 1;
	}
	for (i = 0; i < dec; i++)
		m *= 10;
	q = num / den;
	r = num % den;
	/* round half up; r < den, so r * m * 2 does not overflow for den < 2^50 */
	f = (r * m * 2 + den) / (den * 2);
	if (f >= m) {
		q++;
		f -= m;
	}
	if (dec)
		rt_snprintf(buf, size, "%llu.%0*llu", (unsigned long long)q, dec, (unsigned long long)f);
	else
		rt_snprintf(buf, size, "%llu", (unsigned long long)q);
	return buf;
}

char *st_fmt_time(char *buf, u32 size, u32 t)
{
	/* civil date from days since 1970 (Howard Hinnant's algorithm) */
	u32 days = t / 86400UL, secs = t % 86400UL;
	s32 z = (s32)days + 719468L, era = z / 146097L;
	u32 doe = (u32)(z - era * 146097L);
	u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	s32 y = (s32)yoe + era * 400;
	u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	u32 mp = (5 * doy + 2) / 153;
	u32 d = doy - (153 * mp + 2) / 5 + 1;
	u32 m = mp < 10 ? mp + 3 : mp - 9;

	if (m <= 2)
		y++;
	rt_snprintf(buf, size, "%04ld-%02lu-%02luT%02lu:%02lu:%02lu", (long)y, (unsigned long)m, (unsigned long)d,
	            (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
	return buf;
}

const char *st_strerror(int e)
{
	switch (e) {
	case RT_EINTR: return "Interrupted system call";
	case RT_ENOMEM: return "Cannot allocate memory";
	case RT_EMFILE: return "Too many open files";
	case RT_ENFILE: return "Too many open files in system";
	case RT_EPIPE: return "Broken pipe";
	case RT_EINVAL: return "Invalid argument";
	case RT_ENAMETOOLONG: return "URL too long";
	case RT_EADDRINUSE: return "Address already in use";
	case RT_EADDRNOTAVAIL: return "Cannot assign requested address";
	case RT_ENETDOWN: return "Network is down";
	case RT_ENETUNREACH: return "Network is unreachable";
	case RT_ECONNABORTED: return "Software caused connection abort";
	case RT_ECONNRESET: return "Connection reset by peer";
	case RT_ENOBUFS: return "No buffer space available";
	case RT_ENOTCONN: return "Socket is not connected";
	case RT_ETIMEDOUT: return "Connection timed out";
	case RT_ECONNREFUSED: return "Connection refused";
	case RT_EHOSTDOWN: return "Host is down";
	case RT_EHOSTUNREACH: return "No route to host";
	case RT_EIO: return "Input/output error";
	}
	return "Unknown error";
}
