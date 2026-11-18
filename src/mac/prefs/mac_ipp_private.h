/*
 * Riptide on the classic Mac OS: the private .IPP Control calls between RiptidePrefs and the
 * stack (src/mac/ipp.c answers them; src/mac/prefs/rtport_mac.c makes them). MacTCP's own csCodes
 * are below 300; these are 0x5250 ('RP') and up, which no MacTCP program sends.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_IPP_PRIVATE_H
#define RT_MAC_IPP_PRIVATE_H

#define RTMAC_PING  0x5250      /* answers RTMAC_MAGIC in the first long of csParam */
#define RTMAC_PORT  0x5251      /* a RIPTIDE port message (riptide_port.h): cmd, text, reply */
#define RTMAC_DNS   0x5252      /* a lookup for the resolver's 'dnrp' code (struct rtmac_dns_pb) */
#define RTMAC_API   0x5253      /* the stack's calls for programs (struct rtmac_api), in csParam's first long */
#define RTMAC_MAGIC 0x52495054UL /* 'RIPT' */

/* the parameter block of RTMAC_PORT: CntrlParam's header, then the message */
struct rtmac_port_pb {
	unsigned char hdr[12];          /* qLink, qType, ioTrap, ioCmdAddr */
	void *ioCompletion;
	short ioResult;
	unsigned char *ioNamePtr;
	short ioVRefNum;
	short ioCRefNum;                /* 24 */
	short csCode;                   /* 26 */
	unsigned long cmd;              /* 28: RTCMD_* */
	char *text;                     /* the command line (or list of tables) */
	unsigned long textlen;
	char *reply;                    /* the reply buffer */
	unsigned long replysize;
	long result;                    /* out: 0 or errno */
	unsigned long replylen;         /* out */
	unsigned long flags;            /* out: RTMSGF_* */
};
/* m68k GCC aligns longs to 2 bytes, as the Device Manager's blocks are: cmd lands at 28 */
_Static_assert(__builtin_offsetof(struct rtmac_port_pb, cmd) == 28, "rtmac_port_pb");


/* RTMAC_DNS: a name to addresses (ptr 0) or an address to its name (ptr 1, the address in
 * addrs[0]); completes when the answer is there. The fields after cname are the 'dnrp' code's. */
struct rtmac_dns_pb {
	unsigned char hdr[12];
	void *ioCompletion;
	short ioResult;
	unsigned char *ioNamePtr;
	short ioVRefNum;
	short ioCRefNum;
	short csCode;
	long ptr;                       /* 28 */
	const char *name;               /* 32 */
	unsigned long addrs[4];         /* 36: network order */
	unsigned long count;            /* 52 */
	char cname[256];                /* 56 */
	void *rtn, *proc, *udata;       /* the caller's hostInfo, result procedure and user data */
	long busy;
};
_Static_assert(__builtin_offsetof(struct rtmac_dns_pb, ptr) == 28, "rtmac_dns_pb");

/* RTMAC_API: the core's socket interface for programs, called directly (src/mac/api_mac.c). A
 * call runs in the calling program's context, as a bsdsocket.library call runs in the calling
 * task on the Amiga: it may wait (a blocking recv, select, a name lookup), and while it waits it
 * calls the program's yield function, which takes the program's events (WaitNextEvent) and so
 * lets the Mac, and the stack's deferred task, go on. break_pending says whether the user asked
 * to stop (Command-period): a waiting call then returns -EINTR, or, for select with
 * SIGBREAKF_CTRL_C in its signal mask, that bit. Results are as the core's: 0 or more, or
 * -errno. One client per program: attach at the start, detach (which closes its sockets) at
 * the end. The functions are plain C (GCC's m68k convention) and keep a5. */
#define RTMAC_API_MAGIC   0x52544150UL     /* 'RTAP' */
#define RTMAC_API_VERSION 1
struct rtmac_api {
	unsigned long magic, version, size;
	void *(*attach)(void (*yield)(void *arg), int (*break_pending)(void *arg), void *arg);
	void (*detach)(void *cl);
	int (*socket)(void *cl, int domain, int type, int protocol);
	int (*bind)(void *cl, int fd, const void *addr, unsigned long alen);
	int (*listen)(void *cl, int fd, int backlog);
	int (*accept)(void *cl, int fd, void *addr, unsigned long *alen);
	int (*connect)(void *cl, int fd, const void *addr, unsigned long alen);
	int (*sendto)(void *cl, int fd, const void *buf, unsigned long len, int flags, const void *to,
	              unsigned long tolen);
	int (*recvfrom)(void *cl, int fd, void *buf, unsigned long len, int flags, void *from,
	                unsigned long *fromlen);
	int (*shutdown)(void *cl, int fd, int how);
	int (*setsockopt)(void *cl, int fd, int level, int name, const void *val, unsigned long len);
	int (*getsockopt)(void *cl, int fd, int level, int name, void *val, unsigned long *len);
	int (*getsockname)(void *cl, int fd, void *addr, unsigned long *alen);
	int (*getpeername)(void *cl, int fd, void *addr, unsigned long *alen);
	int (*ioctl)(void *cl, int fd, unsigned long req, void *arg);
	int (*close)(void *cl, int fd);
	/* fd sets of 32-bit words; ms < 0: no limit; *sigmask in: SIGBREAKF_CTRL_C or 0, out: got */
	int (*select)(void *cl, int nfds, unsigned long *rd, unsigned long *wr, unsigned long *ex, long ms,
	              unsigned long *sigmask);
	/* names: rt_resolve_name46 (16-byte addresses, IPv4 mapped), rt_resolve_addr(6) */
	int (*resolve)(void *cl, const char *name, void *addrs, int max, int family, char *canon,
	               unsigned long canonlen);
	int (*resolve_addr)(void *cl, unsigned long addr, char *name, unsigned long len);
	int (*resolve_addr6)(void *cl, const void *addr16, char *name, unsigned long len);
	/* riptide.library's RiptideSysCtl (net.riptide.* tables and the rest) */
	int (*sysctl)(void *cl, const long *mib, unsigned long miblen, void *old, unsigned long *oldlen,
	              const void *newp, unsigned long newlen);
	/* a riptide.conf line, as the RIPTIDE port's COMMAND; the reply text in out */
	int (*command)(void *cl, const char *line, char *out, unsigned long outlen);
	int (*if_index)(void *cl, const char *name);                    /* 0: no such interface */
	int (*if_name)(void *cl, unsigned long index, char *name16);    /* 0, or -ENXIO */
	int (*hostname)(void *cl, char *name, unsigned long len);
};
#endif
