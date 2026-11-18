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

#endif
