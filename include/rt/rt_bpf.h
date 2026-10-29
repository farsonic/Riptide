/*
 * Riptide - Berkeley Packet Filter channels (src/core/bpf.c), the
 * packet filter of the Roadshow bsdsocket.library API (bpf_open() & co.).
 *
 * A channel is attached to one interface (BIOCSETIF) and sees every frame
 * it receives and sends, as the driver does: received frames where they
 * enter rt_netif_input(), sent ones where they leave rt_netif_xmit() (the
 * loopback interface only on delivery, so each packet appears once).
 * Ethernet interfaces present DLT_EN10MB frames (the 14 byte header is
 * rebuilt from the addresses the driver reported), the others DLT_NULL
 * (a 4 byte address family in host order before the packet). Accepted
 * packets are stored, each behind a struct bpf_hdr, in the classic BSD
 * double buffer: bpf_read() returns the hold buffer, packets go into the
 * store buffer; when both are full they are dropped.
 *
 * The ioctl argument structures use the Amiga ABI (32-bit longs and
 * pointers, 2-byte alignment of the 68k compilers); the host tests use
 * the same field order with native pointers. Everything here is called
 * with the stack lock held.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_BPF_INCLUDED_H
#define RT_BPF_INCLUDED_H

#include "rt_types.h"
#include "rt_bsd.h"

struct rt_netif;
struct rt_buf;

#define RT_BPF_NCHAN        16      /* SBTC_NUM_PACKET_FILTER_CHANNELS */
#define RT_BPF_MAXINSNS     512
#define RT_BPF_MAXBUFSIZE   0x8000
#define RT_BPF_MINBUFSIZE   32
#define RT_BPF_BUFSIZE      4096    /* default ("bpf.bufsize") */
#define RT_BPF_MEMWORDS     16
#define RT_BPF_ALIGNMENT    4
#define RT_BPF_WORDALIGN(x) (((x) + (RT_BPF_ALIGNMENT - 1)) & ~(u32)(RT_BPF_ALIGNMENT - 1))
#define RT_BPF_HDRLEN       18      /* sizeof(struct bpf_hdr) on the Amiga */
#define RT_BPF_MAJOR_VERSION 1
#define RT_BPF_MINOR_VERSION 1

/* link types (BIOCGDLT) */
#define RT_DLT_NULL   0
#define RT_DLT_EN10MB 1

/* instruction classes and fields */
#define RT_BPF_CLASS(c) ((c) & 0x07)
#define RT_BPF_LD    0x00
#define RT_BPF_LDX   0x01
#define RT_BPF_ST    0x02
#define RT_BPF_STX   0x03
#define RT_BPF_ALU   0x04
#define RT_BPF_JMP   0x05
#define RT_BPF_RET   0x06
#define RT_BPF_MISC  0x07
#define RT_BPF_SIZE(c) ((c) & 0x18)
#define RT_BPF_W     0x00
#define RT_BPF_H     0x08
#define RT_BPF_B     0x10
#define RT_BPF_MODE(c) ((c) & 0xe0)
#define RT_BPF_IMM   0x00
#define RT_BPF_ABS   0x20
#define RT_BPF_IND   0x40
#define RT_BPF_MEM   0x60
#define RT_BPF_LEN   0x80
#define RT_BPF_MSH   0xa0
#define RT_BPF_OP(c) ((c) & 0xf0)
#define RT_BPF_ADD   0x00
#define RT_BPF_SUB   0x10
#define RT_BPF_MUL   0x20
#define RT_BPF_DIV   0x30
#define RT_BPF_OR    0x40
#define RT_BPF_AND   0x50
#define RT_BPF_LSH   0x60
#define RT_BPF_RSH   0x70
#define RT_BPF_NEG   0x80
#define RT_BPF_JA    0x00
#define RT_BPF_JEQ   0x10
#define RT_BPF_JGT   0x20
#define RT_BPF_JGE   0x30
#define RT_BPF_JSET  0x40
#define RT_BPF_SRC(c) ((c) & 0x08)
#define RT_BPF_K     0x00
#define RT_BPF_X     0x08
#define RT_BPF_RVAL(c) ((c) & 0x18)
#define RT_BPF_A     0x10
#define RT_BPF_MISCOP(c) ((c) & 0xf8)
#define RT_BPF_TAX   0x00
#define RT_BPF_TXA   0x80

struct rt_bpf_insn {
	u16 code;
	u8  jt, jf;
	u32 k;
};

/* BIOCSETF */
struct rt_bpf_program {
	u32 bf_len;
	struct rt_bpf_insn *bf_insns;
};

/* BIOCGSTATS */
struct rt_bpf_stat {
	u32 bs_recv, bs_drop;
};

/* BIOCVERSION */
struct rt_bpf_version {
	u16 bv_major, bv_minor;
};

/* BIOCSRTIMEOUT / BIOCGRTIMEOUT: struct timeval of the Amiga */
struct rt_bpf_timeval {
	u32 tv_sec, tv_usec;
};

/* ioctl requests (rt_bsd.h encoding, the sizes of the Amiga structures) */
#define RT_BPF_IO(g, n)   RT_IOC(RT_IOC_VOID, g, n, 0)
#define RT_BIOCGBLEN      RT_IOR('B', 102, 4)
#define RT_BIOCSBLEN      RT_IOWR('B', 102, 4)
#define RT_BIOCSETF       RT_IOW('B', 103, 8)
#define RT_BIOCFLUSH      RT_BPF_IO('B', 104)
#define RT_BIOCPROMISC    RT_BPF_IO('B', 105)
#define RT_BIOCGDLT       RT_IOR('B', 106, 4)
#define RT_BIOCGETIF      RT_IOR('B', 107, 32)
#define RT_BIOCSETIF      RT_IOW('B', 108, 32)
#define RT_BIOCSRTIMEOUT  RT_IOW('B', 109, 8)
#define RT_BIOCGRTIMEOUT  RT_IOR('B', 110, 8)
#define RT_BIOCGSTATS     RT_IOR('B', 111, 8)
#define RT_BIOCIMMEDIATE  RT_IOW('B', 112, 4)
#define RT_BIOCVERSION    RT_IOR('B', 113, 4)

/* Validate a filter program (BSD bpf_validate()): 0 or -RT_EINVAL. */
int rt_bpf_validate(const struct rt_bpf_insn *f, u32 n);
/* Run a validated program over a packet of wirelen bytes, buflen of them
 * at p. Returns the number of bytes to accept (0 = reject). A NULL
 * program accepts everything. */
u32 rt_bpf_filter(const struct rt_bpf_insn *f, const u8 *p, u32 wirelen, u32 buflen);

/* Nonzero while a channel is attached to an interface. */
extern u8 rt_bpf_on;
/* Buffer size of new channels (RT_BPF_MINBUFSIZE..RT_BPF_MAXBUFSIZE). */
extern u32 rt_bpf_bufsize;
/* OS hook: send 'mask' to the task that set a notify mask (AmigaOS
 * Signal()); NULL on the host. */
extern void (*rt_bpf_signal)(void *task, u32 mask);

/* Channels. 'owner' identifies the opener (the library's per-task
 * context); another owner gets -RT_EPERM. ch < 0 in rt_bpf_open() takes
 * the first free channel. All return a value >= 0 or -errno. */
int  rt_bpf_open(int ch, void *owner);
int  rt_bpf_close(int ch, void *owner);
void rt_bpf_close_owner(void *owner);
/* Blocks (dropping the lock) until the hold buffer has data, the read
 * timeout passes or one of the signals in *intr arrives (-RT_EINTR; on
 * return *intr holds those that did). len must be the buffer size. */
int  rt_bpf_read(int ch, void *owner, void *buf, u32 len, u32 *intr);
/* Send one complete link level frame on the channel's interface. */
int  rt_bpf_write(int ch, void *owner, const void *buf, u32 len);
int  rt_bpf_ioctl(int ch, void *owner, u32 cmd, void *arg);
/* 1 if a read would return data at once, else 0. */
int  rt_bpf_waiting(int ch, void *owner);
int  rt_bpf_set_notify(int ch, void *owner, void *task, u32 mask);
int  rt_bpf_set_intr(int ch, void *owner, u32 mask);
/* Interrupt mask of a channel (for the caller of rt_bpf_read()). */
u32  rt_bpf_intr_mask(int ch, void *owner);

/* A frame met the driver (netif.c, qos.c). out: 1 sent, 0 received;
 * srchw/dsthw: link addresses as known (NULL: broadcast / unknown);
 * flags: RT_BUFF_BCAST/MCAST of received frames. */
void rt_bpf_tap(struct rt_netif *nif, const struct rt_buf *b, int out, u16 ethertype,
                const u8 *srchw, const u8 *dsthw, u16 flags);
/* The interface goes away: its channels are detached. */
void rt_bpf_if_gone(struct rt_netif *nif);

#endif
