/*
 * Riptide - Linux TUN driver (L3 point to point, IFF_TUN | IFF_NO_PI).
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

#define TUN_RX_BURST 64

struct tun_dev {
	struct rt_netif nif;     /* first: nif->driver points back here too */
	int fd;
	char name[IFNAMSIZ];
};

static int tun_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct tun_dev *t = nif->driver;
	ssize_t r;

	(void)dsthw;
	if (ethertype != RT_ETH_P_IP && ethertype != RT_ETH_P_IPV6) {
		rt_buf_free(b);
		return -RT_EAFNOSUPPORT;
	}
	r = write(t->fd, b->data, b->len);
	rt_buf_free(b);
	if (r < 0)
		return errno == EAGAIN ? -RT_ENOBUFS : -RT_EIO;
	return 0;
}

/* Stack thread, lock held. */
static void tun_rx(void *arg)
{
	struct tun_dev *t = arg;
	u32 room = t->nif.mtu > 1500 ? t->nif.mtu : 1500;
	int i;

	for (i = 0; i < TUN_RX_BURST; i++) {
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, room);
		ssize_t n;

		if (!b)
			return;
		n = read(t->fd, b->data, room);
		if (n <= 0) {
			rt_buf_free(b);
			return;
		}
		b->len = (u32)n;
		rt_netif_input(&t->nif, b, (b->data[0] >> 4) == 4 ? RT_ETH_P_IP : RT_ETH_P_IPV6, NULL, 0);
	}
}

struct rt_netif *rt_host_tun_open(const char *ifname, u32 local, u32 peer_or_0, u32 netmask)
{
	struct ifreq ifr;
	struct tun_dev *t;
	int fd, r;

	fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return NULL;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
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
	t->nif.type = RT_IFT_PTP;
	t->nif.flags = RT_IFF_POINTOPOINT | RT_IFF_NOARP;
	t->nif.mtu = 1500;
	t->nif.link_output = tun_output;
	t->nif.driver = t;

	rt_lock();
	r = rt_netif_add(&t->nif);
	if (r == 0)
		r = rt_netif_set_addr(&t->nif, local, netmask, peer_or_0);
	if (r == 0)
		rt_netif_up(&t->nif);
	else if (rt_netif_by_index(t->nif.index) == &t->nif)
		rt_netif_remove(&t->nif);
	rt_unlock();
	if (r < 0 || rt_host_register_fd(fd, tun_rx, t) < 0) {
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

const char *rt_host_tun_name(const struct rt_netif *nif)
{
	const struct tun_dev *t = nif->driver;
	return t ? t->name : "";
}
