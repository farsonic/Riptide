/*
 * Riptide - driver counters (struct rt_drv_stats, net.riptide.drivers).
 *
 * Drivers count what the interface statistics cannot show: how many
 * receive requests they keep queued and how close the queue came to
 * running empty (a frame that arrives while the driver holds no read is
 * dropped by the hardware driver, invisibly to the stack), how many frames
 * each wakeup brings, buffer and transmit shortages and errors by code.
 * Everything runs under the stack lock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"
#include "rt/rt_ctl.h"

#define LOW_UNSET 0xffffffffu
#define WARN_INTERVAL_MS 60000u

static const char *const rxq_name[RT_DRV_NRXQ] = { "IPv4", "ARP", "IPv6" };

void rt_drv_attach(struct rt_netif *nif, struct rt_drv_stats *d)
{
	int q;

	for (q = 0; q < RT_DRV_NRXQ; q++)
		d->rxq[q].low = LOW_UNSET;
	d->reset_ms = rt_now_ms();
	nif->drv = d;
	if (rt_qos_default && nif->type != RT_IFT_LOOP)
		rt_qos_enable(nif, 1);
}

void rt_drv_rx_queued(struct rt_netif *nif, int q)
{
	struct rt_drv_stats *d = nif->drv;

	if (d && q >= 0 && q < RT_DRV_NRXQ)
		d->rxq[q].queued++;
}

void rt_drv_count_err(struct rt_drv_err *tab, s32 code)
{
	int i;

	for (i = 0; i < RT_DRV_NERR - 1; i++) {
		if (tab[i].count && tab[i].code == code)
			break;
		if (!tab[i].count) {
			tab[i].code = code;
			break;
		}
	}
	if (i == RT_DRV_NERR - 1)
		tab[i].code = RT_CTL_DRV_ERR_OTHER;
	tab[i].count++;
}

void rt_drv_rx_done(struct rt_netif *nif, int q, u32 len, s32 err)
{
	struct rt_drv_stats *d = nif->drv;

	if (!d)
		return;
	if (q >= 0 && q < RT_DRV_NRXQ && d->rxq[q].queued)
		d->rxq[q].queued--;
	if (err) {
		d->rx_errors++;
		rt_drv_count_err(d->rxerr, err);
		return;
	}
	d->rx_frames++;
	d->rx_bytes += len;
}

void rt_drv_rx_avail(struct rt_netif *nif, int q, u32 avail)
{
	struct rt_drv_stats *d = nif->drv;
	u32 now;

	if (!d || q < 0 || q >= RT_DRV_NRXQ)
		return;
	if (avail < d->rxq[q].low)
		d->rxq[q].low = avail;
	if (avail)
		return;
	d->rxq[q].empty++;
	now = rt_now_ms();
	if (d->warned && now - d->warn_ms < WARN_INTERVAL_MS)
		return;
	d->warned = 1;
	d->warn_ms = now;
	if (q == RT_DRV_RX_IP)
		rt_log(RT_LOG_WARN, "%s: the driver ran out of %s reads (%lu queued, empty %lu times): "
		       "frames may have been dropped; raise reads=N on the interface line",
		       nif->name, rxq_name[q], (unsigned long)d->rxq[q].cfg, (unsigned long)d->rxq[q].empty);
	else
		rt_log(RT_LOG_WARN, "%s: the driver ran out of %s reads (%lu queued, empty %lu times): "
		       "frames may have been dropped", nif->name, rxq_name[q],
		       (unsigned long)d->rxq[q].cfg, (unsigned long)d->rxq[q].empty);
}

int rt_drv_batch_bucket(u32 n)
{
	int b = 0;

	while (n > 1 && b < RT_DRV_NBATCH - 1) {
		n >>= 1;
		b++;
	}
	return b;
}

void rt_drv_batch(struct rt_netif *nif, u32 n)
{
	struct rt_drv_stats *d = nif->drv;

	if (!d || !n)
		return;
	d->batch[rt_drv_batch_bucket(n)]++;
	if (n > d->batch_max)
		d->batch_max = n;
}

void rt_drv_rx_nobuf(struct rt_netif *nif)
{
	if (nif->drv)
		nif->drv->rx_nobuf++;
}

void rt_drv_tx_start(struct rt_netif *nif, u32 len)
{
	struct rt_drv_stats *d = nif->drv;

	if (!d)
		return;
	d->tx_frames++;
	d->tx_bytes += len;
	if (!d->tx_inflight)
		d->tx_progress_ms = rt_now_ms();
	d->tx_inflight++;
	if (d->tx_inflight > d->tx_inflight_max)
		d->tx_inflight_max = d->tx_inflight;
}

void rt_drv_tx_done(struct rt_netif *nif, s32 err)
{
	struct rt_drv_stats *d = nif->drv;

	if (!d)
		return;
	if (d->tx_inflight)
		d->tx_inflight--;
	d->tx_progress_ms = rt_now_ms();
	if (err) {
		d->tx_errors++;
		rt_drv_count_err(d->txerr, err);
	}
	if (nif->qos)
		rt_qos_run(nif);
	/* UDP pacing: wake the datagram sockets that wait for room */
	if (nif->tx_waiters && rt_netif_tx_room(nif, 0)) {
		nif->tx_waiters = 0;
		rt_udp_tx_wake(nif);
	}
}

void rt_drv_tx_full(struct rt_netif *nif)
{
	if (nif->drv)
		nif->drv->tx_full++;
}

void rt_netif_reset_stats(struct rt_netif *nif)
{
	struct rt_drv_stats *d = nif->drv;
	int q;

	memset(&nif->stats, 0, sizeof(nif->stats));
	if (!d)
		return;
	for (q = 0; q < RT_DRV_NRXQ; q++) {
		d->rxq[q].low = LOW_UNSET;
		d->rxq[q].empty = 0;
	}
	d->rx_frames = d->rx_bytes = d->tx_frames = d->tx_bytes = 0;
	memset(d->batch, 0, sizeof(d->batch));
	d->batch_max = 0;
	d->rx_nobuf = d->rx_errors = d->tx_errors = 0;
	memset(d->rxerr, 0, sizeof(d->rxerr));
	memset(d->txerr, 0, sizeof(d->txerr));
	d->tx_inflight_max = d->tx_inflight;
	d->tx_full = 0;
	d->warned = 0;
	d->reset_ms = rt_now_ms();
	if (d->reset)
		d->reset(nif);
}

int rt_drv_fill(struct rt_netif *nif, struct rt_ctl_driver *r)
{
	struct rt_drv_stats *d = nif->drv;
	int q, i;

	if (!d)
		return -RT_ENOENT;
	memset(r, 0, sizeof(*r));
	rt_strlcpy(r->name, nif->name, sizeof(r->name));
	r->index = nif->index;
	r->since_s = (rt_now_ms() - d->reset_ms) / 1000u;
	r->rx_frames = d->rx_frames;
	r->rx_bytes = d->rx_bytes;
	r->tx_frames = d->tx_frames;
	r->tx_bytes = d->tx_bytes;
	for (q = 0; q < RT_DRV_NRXQ; q++) {
		r->rxq[q].cfg = d->rxq[q].cfg;
		r->rxq[q].queued = d->rxq[q].queued;
		r->rxq[q].low = d->rxq[q].low == LOW_UNSET ? d->rxq[q].queued : d->rxq[q].low;
		r->rxq[q].empty = d->rxq[q].empty;
	}
	for (i = 0; i < RT_DRV_NBATCH; i++)
		r->batch[i] = d->batch[i];
	r->batch_max = d->batch_max;
	r->rx_nobuf = d->rx_nobuf;
	r->rx_errors = d->rx_errors;
	r->tx_errors = d->tx_errors;
	for (i = 0; i < RT_DRV_NERR; i++) {
		r->rxerr[i].code = d->rxerr[i].code;
		r->rxerr[i].count = d->rxerr[i].count;
		r->txerr[i].code = d->txerr[i].code;
		r->txerr[i].count = d->txerr[i].count;
	}
	r->tx_slots = d->tx_slots;
	r->tx_inflight = d->tx_inflight;
	r->tx_inflight_max = d->tx_inflight_max;
	r->tx_full = d->tx_full;
	if (d->noqueue)
		r->flags |= RT_CTL_DRVF_NOQ;
	if (d->info)
		d->info(nif, r);
	return 0;
}
