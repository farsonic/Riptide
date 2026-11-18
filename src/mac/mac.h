/* src/mac/mac.h - Riptide on the classic Mac OS: what the Mac layer's files share.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_H
#define RT_MAC_H
#include "rt_types.h"
#include "rt_socket.h"
struct rtmac_api;

/* drvr.S: trap glue */
void *mac_newptr_sys(u32 size);
u64   mac_microseconds(void);
int   mac_pbcontrol(void *pb, int async);
int   mac_pbopen(void *pb, int slot);
int   mac_slotmanager(void *spb, int selector);
int   mac_dtinstall(void *dt);
void  mac_insxtime(void *tm);
void  mac_primetime(void *tm, s32 ms);
void **mac_getresource(u32 type, int id);
void  mac_releaseresource(void **h);
void  mac_complete(void *pb, int result);
void  mac_call_pascal1(void *proc, u32 arg, void *a0);
void  mac_call_asr(void *proc, void *stream, u32 event, void *udata, u32 reason, void *icmp, int tcp);
void  mac_enet_ph(void);
void  mac_tx_done(void);
void  mac_dt_entry(void);
void  mac_tm_entry(void);
short mac_pb(void *pb, long trap);
short mac_findfolder(int vref, long type, int create, short *vrefout, long *dirout);

/* the receive ring the protocol handler fills (net_mac.c owns it) */
extern u8 *mac_rx_ring;
extern volatile u16 mac_rx_head, mac_rx_tail;
extern u16 mac_rx_slots, mac_rx_mask;
extern volatile u32 mac_rx_dropped;
#define MAC_RX_SLOT 1536

/* os_mac.c */
extern u32 mac_stack_top;
void mac_schedule(void);                       /* any level: run the stack's deferred task soon */
void mac_stack_run(void);                      /* the deferred task's body */
int  mac_os_init(u32 heap_bytes);
void mac_log_line(const char *s);
const char *mac_log_ring(u32 *len, u32 *pos);
static inline u16 mac_irq_off(void) { u16 sr; __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) :: "memory"); return sr; }
static inline void mac_irq_restore(u16 sr) { __asm__ volatile("move.w %0,%%sr" :: "d"(sr) : "memory"); }
static inline u32 mac_ticks(void) { return *(volatile u32 *)0x16A; }

/* os_mac.c and api_mac.c: a program calling the core in its own context (RTMAC_API) */
struct rt_sleeper { int woken; };
struct mac_client {
	struct mac_client *next;
	void (*yield)(void *arg);                    /* the program's: take its events */
	int (*brk)(void *arg);                       /* the program's: was a stop asked for */
	void *arg;
	struct rt_sleeper sl;
	int consumed_break;
	struct rt_sockctx sc;
};
void mac_core_enter(struct mac_client *c);     /* the core is the program's (the deferred task waits) */
void mac_core_leave(void);
extern const struct rtmac_api mac_api;

/* net_mac.c */
int  net_mac_open(const char *name, unsigned unit, int mtu);
void net_mac_poll(void);                       /* stack context: the received frames into the core */
void net_mac_tx_done(void *pb);
extern struct rt_netif *mac_nif;
extern char mac_net_why[64];

/* ipp.c: the MacTCP interface */
int  ipp_control(void *pb);
void ipp_poll(void);
int  ipp_status(void *pb);

/* mactcp.c: MacTCP's TCP and UDP streams, and the resolver's lookups (ipp.c hands them over) */
int  mactcp_call(void *pb, struct rt_sockctx *ctx);   /* 1: pending, else the result */
void mactcp_poll(struct rt_sockctx *ctx);

/* main_mac.c: the keywords of riptide.conf the Mac layer handles (interface) */
int  mac_config_unknown(int argc, char **argv, char *err, u32 errlen);

/* parameter block fields (Inside Macintosh: Files / Devices; MacTCP.h) */
#define PB_QLINK(pb)     (*(void **)((u8 *)(pb) + 0))
#define PB_IOTRAP(pb)    (*(u16 *)((u8 *)(pb) + 6))
#define PB_COMPLETION(pb) (*(void **)((u8 *)(pb) + 12))
#define PB_RESULT(pb)    (*(volatile s16 *)((u8 *)(pb) + 16))
#define PB_NAME(pb)      (*(u8 **)((u8 *)(pb) + 18))
#define PB_REFNUM(pb)    (*(s16 *)((u8 *)(pb) + 24))
#define PB_CSCODE(pb)    (*(s16 *)((u8 *)(pb) + 26))
#define PB_W(pb, o)      (*(u16 *)((u8 *)(pb) + (o)))
#define PB_L(pb, o)      (*(u32 *)((u8 *)(pb) + (o)))
#define PB_P(pb, o)      (*(void **)((u8 *)(pb) + (o)))

#endif
