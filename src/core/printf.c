/*
 * Riptide - freestanding formatted output and small string helpers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rt_vsnprintf() supports %d %i %u %x %X %o %c %s %p %% with the h, l, ll
 * and z length modifiers, field width (also '*'), the '-' and '0' flags and
 * a precision for %s. It always NUL terminates (when size > 0) and returns
 * the length the full output would have had, like C99 vsnprintf().
 * No floating point and no recursion; the stack frame stays small.
 */
#include "rt/rt_types.h"

struct pf_out {
	char *buf;
	size_t size;
	size_t pos;          /* logical length produced so far */
};

static void pf_putc(struct pf_out *o, char c)
{
	if (o->pos + 1 < o->size)
		o->buf[o->pos] = c;
	o->pos++;
}

static void pf_pad(struct pf_out *o, char c, int n)
{
	while (n-- > 0)
		pf_putc(o, c);
}

/* Emit 'len' bytes of s padded to 'width'. */
static void pf_field(struct pf_out *o, const char *s, int len, int width, int left, char padc)
{
	int i;

	if (!left)
		pf_pad(o, padc, width - len);
	for (i = 0; i < len; i++)
		pf_putc(o, s[i]);
	if (left)
		pf_pad(o, ' ', width - len);
}

int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct pf_out o;
	char tmp[24];              /* 64 bit octal needs 22 digits + sign */

	o.buf = buf;
	o.size = size;
	o.pos = 0;

	while (*fmt) {
		int left = 0, zero = 0, width = 0, prec = -1, lmod = 0, neg = 0;
		unsigned base = 10, upper = 0;
		u64 v;
		int n;
		char c = *fmt++;

		if (c != '%') {
			pf_putc(&o, c);
			continue;
		}
		/* flags */
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt == ' ' || *fmt == '+' || *fmt == '#')
				; /* accepted, ignored */
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}
		/* length: 1 = l, 2 = ll, 3 = z, -1 = h, -2 = hh */
		if (*fmt == 'l') {
			lmod = 1;
			if (*++fmt == 'l') {
				lmod = 2;
				fmt++;
			}
		} else if (*fmt == 'z') {
			lmod = 3;
			fmt++;
		} else if (*fmt == 'h') {
			lmod = -1;
			if (*++fmt == 'h') {
				lmod = -2;
				fmt++;
			}
		}

		c = *fmt;
		if (!c)
			break;
		fmt++;
		switch (c) {
		case '%':
			pf_putc(&o, '%');
			continue;
		case 'c':
			tmp[0] = (char)va_arg(ap, int);
			pf_field(&o, tmp, 1, width, left, ' ');
			continue;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len = 0;

			if (!s)
				s = "(null)";
			while (s[len] && (prec < 0 || len < prec))
				len++;
			pf_field(&o, s, len, width, left, ' ');
			continue;
		}
		case 'p':
			v = (u64)(uintptr_t)va_arg(ap, void *);
			base = 16;
			pf_putc(&o, '0');
			pf_putc(&o, 'x');
			width -= 2;
			goto number;
		case 'd':
		case 'i': {
			s64 sv;

			if (lmod == 2)
				sv = va_arg(ap, long long);
			else if (lmod == 1)
				sv = va_arg(ap, long);
			else if (lmod == 3)
				sv = (s64)va_arg(ap, ptrdiff_t);
			else
				sv = va_arg(ap, int);
			if (lmod == -1)
				sv = (short)sv;
			else if (lmod == -2)
				sv = (signed char)sv;
			if (sv < 0) {
				neg = 1;
				v = (u64)0 - (u64)sv;
			} else {
				v = (u64)sv;
			}
			goto number;
		}
		case 'X':
			upper = 1;
			/* fall through */
		case 'x':
			base = 16;
			goto unsigned_arg;
		case 'o':
			base = 8;
			/* fall through */
		case 'u':
		unsigned_arg:
			if (lmod == 2)
				v = va_arg(ap, unsigned long long);
			else if (lmod == 1)
				v = va_arg(ap, unsigned long);
			else if (lmod == 3)
				v = va_arg(ap, size_t);
			else
				v = va_arg(ap, unsigned int);
			if (lmod == -1)
				v = (unsigned short)v;
			else if (lmod == -2)
				v = (unsigned char)v;
			goto number;
		default:
			/* unknown conversion: print it literally */
			pf_putc(&o, '%');
			pf_putc(&o, c);
			continue;
		}
	number:
		{
			const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
			int len = 0, i;

			do {
				/* u64 division by a small constant: fine on 68020
				 * (libgcc), and only used for formatting. */
				tmp[sizeof(tmp) - 1 - len++] = digits[v % base];
				v /= base;
			} while (v);
			n = len + neg;
			if (left || !zero) {
				if (!left)
					pf_pad(&o, ' ', width - n);
				if (neg)
					pf_putc(&o, '-');
				for (i = 0; i < len; i++)
					pf_putc(&o, tmp[sizeof(tmp) - len + i]);
				if (left)
					pf_pad(&o, ' ', width - n);
			} else {
				if (neg)
					pf_putc(&o, '-');
				pf_pad(&o, '0', width - n);
				for (i = 0; i < len; i++)
					pf_putc(&o, tmp[sizeof(tmp) - len + i]);
			}
		}
	}
	if (size)
		buf[o.pos < size ? o.pos : size - 1] = '\0';
	return (int)o.pos;
}

int rt_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = rt_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}

size_t rt_strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);

	if (size) {
		size_t n = len < size - 1 ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

static int digit_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 10;
	return 99;
}

/* strtoul(3) semantics: leading blanks, optional sign, base 0 detects
 * 0x / 0 prefixes. Saturates at ULONG_MAX. *end points past the last digit
 * consumed (or at s if there was none). */
unsigned long rt_strtoul(const char *s, const char **end, int base)
{
	const char *p = s;
	unsigned long v = 0, max = ~0UL;
	int neg = 0, any = 0, over = 0, d;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')
		p++;
	if (*p == '+' || *p == '-')
		neg = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
	    digit_val(p[2]) < 16) {
		p += 2;
		base = 16;
	} else if (base == 0) {
		base = p[0] == '0' ? 8 : 10;
	}
	if (base < 2 || base > 36) {
		if (end)
			*end = s;
		return 0;
	}
	for (; (d = digit_val(*p)) < base; p++) {
		any = 1;
		if (v > (max - (unsigned long)d) / (unsigned long)base)
			over = 1;
		else
			v = v * (unsigned long)base + (unsigned long)d;
	}
	if (end)
		*end = any ? p : s;
	if (over)
		return max;
	return neg ? (unsigned long)0 - v : v;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int rt_strncasecmp(const char *a, const char *b, size_t n)
{
	while (n--) {
		int ca = lower((unsigned char)*a++), cb = lower((unsigned char)*b++);
		if (ca != cb)
			return ca - cb;
		if (!ca)
			break;
	}
	return 0;
}

int rt_strcasecmp(const char *a, const char *b)
{
	return rt_strncasecmp(a, b, (size_t)-1);
}
