/*
 * Riptide - simulated Ethernet: frames as AF_UNIX datagrams between two
 * processes, each running its own stack instance.
 * Frame format on the "wire": dst mac(6) src mac(6) ethertype(be16) payload.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

#define ETH_HLEN     14
#define WIRE_RX_BURST 64
#define WIRE_TXQ_MAX  1024     /* frames waiting for the transmit thread */

/* Frames to send, copied out of rt_bufs (the buffer allocator belongs to
 * the stack lock; the transmit thread never touches it). */
struct wire_frame {
	struct wire_frame *next;
	u32 len;
	u8 data[];
};

struct wire_dev {
	struct rt_netif nif;
	int fd;
	struct sockaddr_un peer;
	/* Transmit ring serviced by its own thread, like a NIC: the stack
	 * never blocks on a full peer queue (AF_UNIX keeps only a handful of
	 * datagrams queued), and two stacks sending to each other cannot
	 * deadlock. */
	pthread_mutex_t txlock;
	pthread_cond_t txcond;
	struct wire_frame *txhead, *txtail;
	u32 txlen;
	pthread_t txthread;
};

static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static int wire_count;

static int wire_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct wire_dev *w = nif->driver;
	struct wire_frame *f;

	pthread_mutex_lock(&w->txlock);
	if (w->txlen >= WIRE_TXQ_MAX) {
		pthread_mutex_unlock(&w->txlock);
		rt_buf_free(b);
		return -RT_ENOBUFS;            /* ring full: tail drop */
	}
	pthread_mutex_unlock(&w->txlock);
	f = malloc(sizeof(*f) + ETH_HLEN + b->len);
	if (!f) {
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
	memcpy(f->data, dsthw ? dsthw : bcast, 6);
	memcpy(f->data + 6, nif->hwaddr, 6);
	rt_put_be16(f->data + 12, ethertype);
	memcpy(f->data + ETH_HLEN, b->data, b->len);
	f->len = ETH_HLEN + b->len;
	f->next = NULL;
	rt_buf_free(b);
	pthread_mutex_lock(&w->txlock);
	if (w->txtail)
		w->txtail->next = f;
	else
		w->txhead = f;
	w->txtail = f;
	w->txlen++;
	pthread_cond_signal(&w->txcond);
	pthread_mutex_unlock(&w->txlock);
	return 0;
}

static void *wire_tx_main(void *arg)
{
	struct wire_dev *w = arg;

	for (;;) {
		struct wire_frame *f;
		ssize_t r;
		int tries;

		pthread_mutex_lock(&w->txlock);
		while (!w->txhead)
			pthread_cond_wait(&w->txcond, &w->txlock);
		f = w->txhead;
		w->txhead = f->next;
		if (!w->txhead)
			w->txtail = NULL;
		w->txlen--;
		pthread_mutex_unlock(&w->txlock);
		/* Wait for room at the peer (a real link serialises); give up
		 * after ~2 s, e.g. when the peer process is stuck. poll() cannot
		 * be used: on an unconnected AF_UNIX datagram socket POLLOUT
		 * says nothing about the peer's queue. */
		for (tries = 0; tries < 4000; tries++) {
			r = sendto(w->fd, f->data, f->len, MSG_DONTWAIT,
			           (struct sockaddr *)&w->peer, sizeof(w->peer));
			if (r >= 0 || errno != EAGAIN)
				break;      /* sent, or no peer: the frame is lost */
			usleep(500);
		}
		free(f);
	}
	return NULL;
}

static void wire_rx(void *arg)
{
	struct wire_dev *w = arg;
	int i;

	for (i = 0; i < WIRE_RX_BURST; i++) {
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM - ETH_HLEN, 1600);
		u8 src[6];
		u16 et, flags = 0;
		ssize_t n;

		if (!b)
			return;
		n = recv(w->fd, b->data, 1600, MSG_DONTWAIT);
		if (n < ETH_HLEN) {
			rt_buf_free(b);
			if (n < 0)
				return;
			continue;                  /* runt */
		}
		b->len = (u32)n;
		if (memcmp(b->data, bcast, 6) == 0)
			flags = RT_BUFF_BCAST;
		else if (b->data[0] & 1)
			flags = RT_BUFF_MCAST;
		else if (memcmp(b->data, w->nif.hwaddr, 6) != 0 && !(w->nif.flags & RT_IFF_PROMISC)) {
			rt_buf_free(b);            /* not for us */
			continue;
		}
		memcpy(src, b->data + 6, 6);
		et = rt_get_be16(b->data + 12);
		rt_buf_pull(b, ETH_HLEN);
		rt_netif_input(&w->nif, b, et, src, flags);
	}
}

struct rt_netif *rt_host_wire_open(const char *my_sock_path, const char *peer_sock_path,
                                   const u8 mac[6])
{
	struct sockaddr_un me;
	struct wire_dev *w;
	int fd, r, rcv = 1 << 20;

	if (strlen(my_sock_path) >= sizeof(me.sun_path) || strlen(peer_sock_path) >= sizeof(me.sun_path))
		return NULL;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return NULL;
	memset(&me, 0, sizeof(me));
	me.sun_family = AF_UNIX;
	strcpy(me.sun_path, my_sock_path);
	unlink(my_sock_path);
	if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) {
		close(fd);
		return NULL;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	w = calloc(1, sizeof(*w));
	if (!w) {
		close(fd);
		return NULL;
	}
	w->fd = fd;
	pthread_mutex_init(&w->txlock, NULL);
	pthread_cond_init(&w->txcond, NULL);
	w->peer.sun_family = AF_UNIX;
	strcpy(w->peer.sun_path, peer_sock_path);
	w->nif.type = RT_IFT_ETHER;
	w->nif.hwlen = 6;
	memcpy(w->nif.hwaddr, mac, 6);
	w->nif.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	w->nif.mtu = 1500;
	w->nif.link_output = wire_output;
	w->nif.driver = w;

	rt_lock();
	rt_snprintf(w->nif.name, sizeof(w->nif.name), "eth%d", wire_count++);
	r = rt_netif_add(&w->nif);
	rt_unlock();
	if (r < 0 || rt_host_register_fd(fd, wire_rx, w) < 0) {
		if (r == 0) {
			rt_lock();
			rt_netif_remove(&w->nif);
			rt_unlock();
		}
		close(fd);
		free(w);
		return NULL;
	}
	/* Frames queued before the thread runs simply wait for it. If it
	 * cannot start, the interface stays registered but mute. */
	if (pthread_create(&w->txthread, NULL, wire_tx_main, w) == 0)
		pthread_detach(w->txthread);
	return &w->nif;
}
