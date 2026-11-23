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
void  mac_enet_ph0(void), mac_enet_ph1(void), mac_enet_ph2(void), mac_enet_ph3(void);
void  mac_tx_done(void);
void  mac_dt_entry(void);
void  mac_tm_entry(void);
short mac_pb(void *pb, long trap);
short mac_findfolder(int vref, long type, int create, short *vrefout, long *dirout);
short mac_readxpram(void *buf, u32 count_addr);    /* not at interrupt time */
short mac_setdatetime(u32 secs);                   /* local seconds since 1904; a program's context only */

/* the receive rings the protocol handlers fill, one per card (net_mac.c owns them; drvr.S's
   mac_enet_phN fills mac_rx[N], and knows this layout: 16 bytes) */
#define MAC_CARDS 4
struct mac_rxring {
	u8 *ring;
	volatile u16 head, tail;
	u16 slots, mask;
	volatile u32 dropped;
};
extern struct mac_rxring mac_rx[MAC_CARDS];
#define MAC_RX_SLOT 1536

/* os_mac.c */
extern u32 mac_stack_top;
void mac_schedule(void);                       /* any level: run the stack's deferred task soon */
void mac_stack_run(void);                      /* the deferred task's body */
int  mac_os_init(u32 heap_bytes);
void mac_log_line(const char *s);
const char *mac_log_ring(u32 *len, u32 *pos);
void mac_clock_init(void);                     /* at Open: the time zone, NTP's clock hook */
void mac_clock_run(void);                      /* accRun (a program's context): set the clock if NTP asked */
void mac_tz_read(void);                        /* the Map control panel's time zone; not at interrupt time */
static inline u16 mac_irq_off(void) { u16 sr; __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) :: "memory"); return sr; }
static inline void mac_irq_restore(u16 sr) { __asm__ volatile("move.w %0,%%sr" :: "d"(sr) : "memory"); }
static inline u32 mac_ticks(void) { return *(volatile u32 *)0x16A; }

/* os_mac.c and api_mac.c: a program calling the core in its own context (RTMAC_API) */
/* a sleeper: 'woken' set by a waker; a program's (api_mac.c) carries its process too, so the
   waker can end its WaitNextEvent at once (WakeUpProcess) instead of at its next tick */
struct rt_sleeper { int woken; int has_psn; u32 psn[2]; };
short mac_current_process(u32 psn[2]);
short mac_wakeup_process(const u32 psn[2]);
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
int  mac_core_taken(void);                     /* a program has the core, or the deferred task runs it */
extern const struct rtmac_api mac_api;

/* net_mac.c: every Ethernet card the Slot Manager lists can be an interface of its own */
struct mac_cardsel {
	int unit;                                  /* -1: not by its place in the Slot Manager's list */
	int slot, id;                              /* -1: any */
	const char *name;                          /* "ethernet", "wifi" or part of the driver's name; NULL: any */
	int metric;                                /* -1: the card's default (wired 10, WiFi 20) */
};
int  net_mac_open(const char *name, const struct mac_cardsel *sel, int mtu);
int  net_mac_cards(void);                      /* cards found (the Slot Manager's catNetwork/typeEtherNet list) */
int  net_mac_card_info(int i, char *buf, u32 len);   /* "slot 9 id $83 .Network_WiFi_PiStorm (wifi)" */
void net_mac_poll(void);                       /* stack context: the received frames into the core */
void net_mac_tx_done(void *pb);
extern struct rt_netif *mac_nif;               /* the interface MacTCP's calls report (the preferred one) */
extern char mac_net_why[64];
/* a restart of the network (main_mac.c mac_restart), in a program's context */
void net_mac_restart_begin(void);              /* driver calls may wait for the earlier ones */
void net_mac_restart_end(void);
struct rt_netif *net_mac_nif(int i);           /* the i-th open card's interface, NULL past the last */
int  net_mac_same(struct rt_netif *n, const struct mac_cardsel *sel);   /* the selection names this card */
void net_mac_renew(struct rt_netif *n, int mtu, int metric);  /* a card that stays: as just opened */
void net_mac_close(struct rt_netif *n);        /* a card that goes: detached from its driver */

/* ipp.c: the MacTCP interface */
int  ipp_control(void *pb);
void ipp_poll(void);
int  ipp_status(void *pb);

/* mactcp.c: MacTCP's TCP and UDP streams, and the resolver's lookups (ipp.c hands them over) */
int  mactcp_call(void *pb, struct rt_sockctx *ctx);   /* 1: pending, else the result */
void mactcp_poll(struct rt_sockctx *ctx);

/* main_mac.c: the keywords of riptide.conf the Mac layer handles (interface) */
int  mac_config_unknown(int argc, char **argv, char *err, u32 errlen);
/* main_mac.c: RTCMD_RESTART, riptide.conf read again while the Mac runs; in a program's context
   with the core taken (ipp.c). which: "" the file Riptide reads at its start (the In Use copy
   first), or RiptidePrefs' ENV:Riptide/... (In Use) or ENVARC:Riptide/... (the saved one). The
   report goes to out. 0 or -errno. */
int  mac_restart(const char *which, char *out, u32 olen);

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
