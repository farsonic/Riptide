/*
 * Riptide - unit tests of the driver counters (drvstats.c): queue
 * accounting and low-water marks, the batch histogram, error tables, the
 * transmit counters, the net.riptide.drivers dump (by name and MIB, short
 * buffers, driver info hook) and "interface NAME|all resetstats".
 * Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	rt_buf_free(b);
	return 0;
}

static struct rt_netif eth0, eth1, eth2;
static struct rt_drv_stats d0, d1;
static int resets;
static u8 dumpbuf[8192];

static void info0(struct rt_netif *nif, struct rt_ctl_driver *r)
{
	rt_strlcpy(r->kind, "sana2", sizeof(r->kind));
	rt_strlcpy(r->driver, "test.device", sizeof(r->driver));
	r->unit = 3;
	r->flags |= RT_CTL_DRVF_GS | RT_CTL_DRVF_SS;
	r->gs_overruns = 7;
	r->nspecial = 1;
	r->special[0].type = 0x10002;
	r->special[0].count = 5;
}

static void reset0(struct rt_netif *nif)
{
	resets++;
}

static int fetch(u16 *recsize)
{
	struct rt_ctl_hdr h;
	u32 len = 0;
	int r;

	rt_lock();
	r = rt_sysctl_byname("net.riptide.drivers", NULL, &len, NULL, 0);
	if (r == 0) {
		CHECK(len >= sizeof(h) && len <= sizeof(dumpbuf));
		len = sizeof(dumpbuf);
		r = rt_sysctl_byname("net.riptide.drivers", dumpbuf, &len, NULL, 0);
	}
	rt_unlock();
	if (r < 0)
		return -1;
	memcpy(&h, dumpbuf, sizeof(h));
	CHECK(h.version == RT_CTL_VERSION);
	CHECK(len == sizeof(h) + h.count * h.recsize);
	*recsize = h.recsize;
	return (int)h.count;
}

static const struct rt_ctl_driver *find(const char *name)
{
	u16 rs = 0;
	int n = fetch(&rs), i;

	CHECK(rs == sizeof(struct rt_ctl_driver));
	for (i = 0; i < n; i++) {
		const struct rt_ctl_driver *r = (const void *)(dumpbuf + sizeof(struct rt_ctl_hdr) + (u32)i * rs);
		if (!strcmp(r->name, name))
			return r;
	}
	return NULL;
}

static int cmd(const char *line)
{
	char out[256];
	int r;

	rt_lock();
	r = rt_ctl_command(line, out, sizeof(out), NULL);
	rt_unlock();
	return r;
}

static void add_if(struct rt_netif *n, const char *name)
{
	rt_strlcpy(n->name, name, sizeof(n->name));
	n->type = RT_IFT_ETHER;
	n->hwlen = 6;
	n->mtu = 1500;
	n->flags = RT_IFF_BROADCAST;
	n->link_output = fake_output;
	rt_lock();
	CHECK(rt_netif_add(n) >= 0);
	rt_unlock();
}

static void test_buckets(void)
{
	CHECK(rt_drv_batch_bucket(1) == 0);
	CHECK(rt_drv_batch_bucket(2) == 1);
	CHECK(rt_drv_batch_bucket(3) == 1);
	CHECK(rt_drv_batch_bucket(4) == 2);
	CHECK(rt_drv_batch_bucket(7) == 2);
	CHECK(rt_drv_batch_bucket(8) == 3);
	CHECK(rt_drv_batch_bucket(15) == 3);
	CHECK(rt_drv_batch_bucket(16) == 4);
	CHECK(rt_drv_batch_bucket(31) == 4);
	CHECK(rt_drv_batch_bucket(32) == 5);
	CHECK(rt_drv_batch_bucket(1000) == 5);
}

static void test_errtab(void)
{
	struct rt_drv_err t[RT_DRV_NERR];
	int i;

	memset(t, 0, sizeof(t));
	rt_drv_count_err(t, -2);
	rt_drv_count_err(t, -2);
	rt_drv_count_err(t, 10);
	CHECK(t[0].code == -2 && t[0].count == 2);
	CHECK(t[1].code == 10 && t[1].count == 1);
	for (i = 0; i < 20; i++)
		rt_drv_count_err(t, 100 + i);
	/* 5 slots left before the catch-all: codes 100..104, the rest go to "other" */
	CHECK(t[2].code == 100 && t[6].code == 104 && t[7].code == RT_CTL_DRV_ERR_OTHER);
	CHECK(t[7].count == 15);
	rt_drv_count_err(t, -2);
	CHECK(t[0].count == 3);
}

static void test_counters(void)
{
	const struct rt_ctl_driver *r;
	int i;

	add_if(&eth0, "eth0");
	add_if(&eth1, "eth1");
	add_if(&eth2, "eth2");          /* no counters: not in the table */
	rt_lock();
	rt_drv_attach(&eth0, &d0);
	d0.info = info0;
	d0.reset = reset0;
	d0.rxq[RT_DRV_RX_IP].cfg = 8;
	d0.rxq[RT_DRV_RX_IP6].cfg = 2;
	d0.tx_slots = 32;
	for (i = 0; i < 8; i++)
		rt_drv_rx_queued(&eth0, RT_DRV_RX_IP);
	rt_drv_rx_queued(&eth0, RT_DRV_RX_IP6);
	rt_drv_rx_queued(&eth0, RT_DRV_RX_IP6);
	rt_drv_attach(&eth1, &d1);
	d1.noqueue = 1;
	rt_unlock();

	/* before any wakeup the low-water mark reads as the queue length */
	r = find("eth0");
	CHECK(r != NULL);
	if (!r)
		return;
	CHECK(r->index == eth0.index);
	CHECK(r->rxq[0].cfg == 8 && r->rxq[0].queued == 8 && r->rxq[0].low == 8 && r->rxq[0].empty == 0);
	CHECK(r->rxq[2].queued == 2 && r->rxq[1].queued == 0);
	CHECK(!strcmp(r->kind, "sana2") && !strcmp(r->driver, "test.device") && r->unit == 3);
	CHECK(r->gs_overruns == 7 && r->nspecial == 1 && r->special[0].count == 5);
	CHECK(r->tx_slots == 32);
	CHECK(find("eth2") == NULL);
	r = find("eth1");
	CHECK(r && (r->flags & RT_CTL_DRVF_NOQ) && r->kind[0] == 0);

	rt_lock();
	/* a burst of 5 frames: the driver had 3 reads left at the wakeup */
	rt_drv_rx_avail(&eth0, RT_DRV_RX_IP, 3);
	for (i = 0; i < 5; i++) {
		rt_drv_rx_done(&eth0, RT_DRV_RX_IP, 100, 0);
		rt_drv_rx_queued(&eth0, RT_DRV_RX_IP);
	}
	rt_drv_batch(&eth0, 5);
	/* a wakeup that found the queue empty, then one error */
	rt_drv_rx_avail(&eth0, RT_DRV_RX_IP, 0);
	rt_drv_rx_avail(&eth0, RT_DRV_RX_IP, 0);
	rt_drv_rx_done(&eth0, RT_DRV_RX_IP, 0, -2);
	rt_drv_batch(&eth0, 1);
	rt_drv_batch(&eth0, 40);
	rt_drv_batch(&eth0, 0);         /* ignored */
	rt_drv_rx_nobuf(&eth0);
	rt_drv_tx_start(&eth0, 60);
	rt_drv_tx_start(&eth0, 1514);
	rt_drv_tx_done(&eth0, 0);
	rt_drv_tx_start(&eth0, 100);
	rt_drv_tx_done(&eth0, 11);
	rt_drv_tx_full(&eth0);
	eth0.stats.ipackets = 99;
	rt_unlock();

	r = find("eth0");
	CHECK(r != NULL);
	if (!r)
		return;
	CHECK(r->rx_frames == 5 && r->rx_bytes == 500);
	CHECK(r->rxq[0].queued == 7 && r->rxq[0].low == 0 && r->rxq[0].empty == 2);
	CHECK(r->rxq[2].low == 2 && r->rxq[2].empty == 0);
	CHECK(r->rx_errors == 1 && r->rxerr[0].code == -2 && r->rxerr[0].count == 1);
	CHECK(r->batch[0] == 1 && r->batch[2] == 1 && r->batch[5] == 1 && r->batch_max == 40);
	CHECK(r->rx_nobuf == 1);
	CHECK(r->tx_frames == 3 && r->tx_bytes == 1674);
	CHECK(r->tx_inflight == 1 && r->tx_inflight_max == 2);
	CHECK(r->tx_errors == 1 && r->txerr[0].code == 11 && r->txerr[0].count == 1);
	CHECK(r->tx_full == 1);

	/* reset: counters zero, queue state and configuration kept */
	rt_host_advance_clock(5000);
	r = find("eth0");
	CHECK(r && r->since_s >= 5);
	CHECK(cmd("interface eth0 resetstats") == 0);
	CHECK(resets == 1);
	CHECK(eth0.stats.ipackets == 0);
	r = find("eth0");
	CHECK(r != NULL);
	if (!r)
		return;
	CHECK(r->since_s == 0);
	CHECK(r->rx_frames == 0 && r->rx_errors == 0 && r->rxerr[0].count == 0 && r->batch_max == 0);
	CHECK(r->batch[0] == 0 && r->tx_full == 0 && r->tx_errors == 0 && r->rx_nobuf == 0);
	CHECK(r->rxq[0].cfg == 8 && r->rxq[0].queued == 7 && r->rxq[0].low == 7 && r->rxq[0].empty == 0);
	CHECK(r->tx_inflight == 1 && r->tx_inflight_max == 1);

	rt_lock();
	rt_drv_tx_start(&eth1, 10);
	eth2.stats.opackets = 4;
	rt_unlock();
	CHECK(cmd("interface all resetstats") == 0);
	CHECK(resets == 2 && eth2.stats.opackets == 0);
	r = find("eth1");
	CHECK(r && r->tx_frames == 0);
	CHECK(cmd("interface nosuch resetstats") == -RT_ENXIO);
}

static void test_short_buffer(void)
{
	u8 small[sizeof(struct rt_ctl_hdr) + sizeof(struct rt_ctl_driver) + 8];
	struct rt_ctl_hdr h;
	u32 len = sizeof(small);
	s32 mib[3] = { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, RT_CTL_DRIVERS };
	int r;

	rt_lock();
	r = rt_sysctl_bymib(mib, 3, small, &len, NULL, 0);
	rt_unlock();
	CHECK(r == -RT_ENOMEM);
	memcpy(&h, small, sizeof(h));
	CHECK(h.count == 1 && len == sizeof(h) + 2 * sizeof(struct rt_ctl_driver));
}

int main(void)
{
	CHECK(sizeof(struct rt_ctl_driver) % 4 == 0);
	CHECK(rt_init() == 0);
	rt_log_level = 0;               /* the "ran out of reads" warning is expected */
	test_buckets();
	test_errtab();
	test_counters();
	test_short_buffer();
	printf("t_drvstats_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
