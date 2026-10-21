/*
 * Riptide - Linux TAP driver (L2 Ethernet, IFF_TAP | IFF_NO_PI): an
 * Ethernet interface whose other end is the Linux kernel, so ARP, NDP,
 * SLAAC and multicast are exercised against a real independent stack.
 * The kernel side sees every frame Riptide sends and delivers every frame
 * it routes to the TAP device; Riptide keeps a hardware multicast filter
 * (nif->mcast: the groups the IPv6 layer joined) like a NIC would.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* Riptide headers first: <net/if.h> defines ifr_* macros that would
 * rename the fields of struct rt_ifreq. */
#include "rt/riptide.h"
#include "rt/rt_host.h"

#include <net/if.h>
#include <linux/if_tun.h>

#define ETH_HLEN     14
#define TAP_RX_BURST 64
#define TAP_MAX_MC   32

struct tap_dev {
	struct rt_netif nif;     /* first */
	int fd;
	char name[IFNAMSIZ];
	u8 mc[TAP_MAX_MC][6];    /* multicast filter */
	u32 mc_refs[TAP_MAX_MC];
	int nmc;
	struct rt_drv_stats drv;
};

static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int tap_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct tap_dev *t = nif->driver;
	u8 *h;
	ssize_t r;

	if (rt_buf_headroom(b) < ETH_HLEN) {
		struct rt_buf *nb = rt_buf_copy_expand(b, ETH_HLEN);
		rt_buf_free(b);
		if (!nb)
			return -RT_ENOBUFS;
		b = nb;
	}
	h = rt_buf_push(b, ETH_HLEN);
	memcpy(h, dsthw ? dsthw : bcast, 6);
	memcpy(h + 6, nif->hwaddr, 6);
	rt_put_be16(h + 12, ethertype);
	rt_drv_tx_start(nif, b->len);
	r = write(t->fd, b->data, b->len);
	rt_drv_tx_done(nif, r < 0 ? -errno : 0);
	rt_buf_free(b);
	if (r < 0)
		return errno == EAGAIN ? -RT_ENOBUFS : -RT_EIO;
	return 0;
}

static void tap_info(struct rt_netif *nif, struct rt_ctl_driver *r)
{
	const struct tap_dev *t = nif->driver;

	rt_strlcpy(r->kind, "tap", sizeof(r->kind));
	rt_snprintf(r->driver, sizeof(r->driver), "/dev/net/tun %s", t->name);
}

/* lock held */
static int tap_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct tap_dev *t = nif->driver;
	int i;

	for (i = 0; i < t->nmc; i++)
		if (!memcmp(t->mc[i], hw, 6))
			break;
	if (join) {
		if (i < t->nmc) {
			t->mc_refs[i]++;
			return 0;
		}
		if (t->nmc >= TAP_MAX_MC)
			return -RT_ENOBUFS;
		memcpy(t->mc[t->nmc], hw, 6);
		t->mc_refs[t->nmc++] = 1;
		return 0;
	}
	if (i < t->nmc && --t->mc_refs[i] == 0) {
		t->nmc--;
		memmove(t->mc[i], t->mc[i + 1], (size_t)(t->nmc - i) * 6);
		memmove(&t->mc_refs[i], &t->mc_refs[i + 1], (size_t)(t->nmc - i) * sizeof(u32));
	}
	return 0;
}

static int mc_wanted(const struct tap_dev *t, const u8 *dst)
{
	int i;

	if (t->nif.flags & (RT_IFF_ALLMULTI | RT_IFF_PROMISC))
		return 1;
	if (dst[0] == 0x01 && dst[1] == 0x00 && dst[2] == 0x5e)
		return 1;                  /* IPv4 multicast: no IGMP filter in Riptide */
	for (i = 0; i < t->nmc; i++)
		if (!memcmp(t->mc[i], dst, 6))
			return 1;
	return 0;
}

/* Stack thread, lock held. */
static void tap_rx(void *arg)
{
	struct tap_dev *t = arg;
	u32 room = (u32)t->nif.mtu + ETH_HLEN, got = 0;
	int i;

	for (i = 0; i < TAP_RX_BURST; i++) {
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM - ETH_HLEN, room);  /* IP header aligned after the pull */
		u8 src[6];
		u16 et, flags = 0;
		ssize_t n;

		if (!b) {
			rt_drv_rx_nobuf(&t->nif);
			break;
		}
		n = read(t->fd, b->data, room);
		if (n < ETH_HLEN) {
			rt_buf_free(b);
			if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					rt_drv_rx_done(&t->nif, -1, 0, -errno);
				break;
			}
			rt_drv_rx_done(&t->nif, -1, 0, RT_CTL_DRV_ERR_LENGTH);
			continue;
		}
		b->len = (u32)n;
		got++;
		rt_drv_rx_done(&t->nif, -1, (u32)n, 0);
		if (!memcmp(b->data, bcast, 6)) {
			flags = RT_BUFF_BCAST;
		} else if (b->data[0] & 1) {
			if (!mc_wanted(t, b->data)) {
				rt_buf_free(b);
				continue;
			}
			flags = RT_BUFF_MCAST;
		} else if (memcmp(b->data, t->nif.hwaddr, 6) && !(t->nif.flags & RT_IFF_PROMISC)) {
			rt_buf_free(b);
			continue;
		}
		memcpy(src, b->data + 6, 6);
		et = rt_get_be16(b->data + 12);
		rt_buf_pull(b, ETH_HLEN);
		rt_netif_input(&t->nif, b, et, src, flags);
	}
	rt_drv_batch(&t->nif, got);
}

struct rt_netif *rt_host_tap_open(const char *ifname, const u8 mac[6])
{
	struct ifreq ifr;
	struct tap_dev *t;
	int fd, r;

	fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return NULL;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	if (ifname)
		strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		close(fd);
		return NULL;
	}
	t = calloc(1, sizeof(*t));
	if (!t) {
		close(fd);
		return NULL;
	}
	t->fd = fd;
	memcpy(t->name, ifr.ifr_name, IFNAMSIZ);
	t->name[IFNAMSIZ - 1] = '\0';
	rt_strlcpy(t->nif.name, t->name, sizeof(t->nif.name));
	t->nif.type = RT_IFT_ETHER;
	t->nif.hwlen = 6;
	memcpy(t->nif.hwaddr, mac, 6);
	t->nif.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	t->nif.mtu = 1500;
	t->nif.link_output = tap_output;
	t->nif.mcast = tap_mcast;
	t->nif.driver = t;
	t->drv.noqueue = 1;
	t->drv.info = tap_info;
	rt_drv_attach(&t->nif, &t->drv);

	rt_lock();
	r = rt_netif_add(&t->nif);
	rt_unlock();
	if (r < 0 || rt_host_register_fd(fd, tap_rx, t) < 0) {
		if (r == 0) {
			rt_lock();
			rt_netif_remove(&t->nif);
			rt_unlock();
		}
		close(fd);
		free(t);
		return NULL;
	}
	return &t->nif;
}

const char *rt_host_tap_name(const struct rt_netif *nif)
{
	const struct tap_dev *t = nif->driver;
	return t ? t->name : "";
}
