/*
 * Riptide - quality of service: transmit priority bands in front of the
 * network drivers, socket priority (SO_PRIORITY) and DiffServ marking,
 * and "qos rule" policies for programs that set nothing themselves.
 *
 * With "qos on" an interface whose driver reports transmit completions
 * (SANA-II, MNI, TAP: rt_drv_tx_start/rt_drv_tx_done) keeps at most
 * net.inet.ip.qos_inflight frames in the driver; the rest wait in three
 * bands, sent strictly in order:
 *
 *   0 interactive  ARP, ICMP, DNS/NTP/mDNS, TCP segments without data
 *                  (ACKs, SYN, FIN), TCP segments of up to 64 bytes
 *                  (keystrokes), DSCP EF/CS4-CS7/AF4x, TOS "low delay",
 *                  socket priority 6-7
 *   1 default      everything else
 *   2 bulk         DSCP CS1/LE/AF1x, TOS "throughput", socket priority
 *                  1-3 and 5 (Linux pfifo_fast priomap)
 *
 * so a keystroke or an ACK waits behind at most qos_inflight frames, not
 * behind the driver's whole queue (32 frames for SANA-II).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_QOS_H
#define RT_QOS_H

#include "rt_types.h"

struct rt_netif;
struct rt_buf;
struct rt_socket;
struct rt_ctl_qos;

#define RT_QOS_BAND_INTERACTIVE 0
#define RT_QOS_BAND_DEFAULT     1
#define RT_QOS_BAND_BULK        2
#define RT_QOS_NBANDS           3

/* sysctls (net.inet.ip.qos_*) */
extern s32 rt_qos_default;        /* qos_default: "qos on" for interfaces added later */
extern s32 rt_qos_inflight;       /* qos_inflight: frames in the driver at most (1..32) */
extern s32 rt_qos_limit;          /* qos_limit: frames per band at most (8..1024) */
extern s32 rt_qos_prio_dscp;      /* qos_prio_dscp: DSCP from SO_PRIORITY when TOS is 0 */

/* Turn the bands on or off for nif (off sends what is queued). -errno
 * when the driver reports no completions. Lock held. */
int  rt_qos_enable(struct rt_netif *nif, int on);
/* rt_netif_xmit() with nif->qos: queue or send. Consumes b. */
int  rt_qos_xmit(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype);
/* rt_drv_tx_done(): a frame left the driver, send the next ones */
void rt_qos_run(struct rt_netif *nif);
/* band of an outgoing L3 packet (b->data at the IP header) */
int  rt_qos_band(const struct rt_buf *b, u16 ethertype);
/* net.riptide.qos record */
void rt_qos_fill(const struct rt_netif *nif, struct rt_ctl_qos *r);

/* DSCP for socket priority p (0..7) */
u8   rt_qos_prio2dscp(u8 p);
/* The TOS/traffic class byte to send for a socket: 'tos' (what the
 * protocol has, ECN bits included) unless its DSCP part is 0, then the
 * DSCP of a matching "qos rule", else of SO_PRIORITY (qos_prio_dscp). */
u8   rt_qos_sock_tos(const struct rt_socket *so, u8 tos);
/* rt_buf.prio / rt_ip_out.prio for a socket: priority + 1, 0 = none */
u8   rt_qos_sock_prio(const struct rt_socket *so);
/* Apply the "qos rule"s to so (lock held): at socket creation with the
 * program name (lport = rport = 0), and when its ports are known
 * (bind/connect/accept). Never overrides what the program set itself. */
void rt_qos_sock_rules(struct rt_socket *so, const char *program, u16 lport, u16 rport);

/* number of "qos rule"s (0: nothing to match) */
int  rt_qos_nrules(void);
/* set by the OS glue: the calling program's name for "program=" rules */
extern void (*rt_qos_program_hook)(char *buf, u32 len);

/* "qos on|off [IF] | inflight N | limit N | priodscp on|off |
 *  rule ... | rules flush | status" */
int  rt_qos_command(int argc, char **argv, char *out, u32 outlen);
void rt_qos_init(void);

#endif
