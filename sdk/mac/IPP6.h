/*
 * IPP6.h - the .IPP6 programming interface: IPv6 (and IPv4) for classic Mac programs, the MacTCP
 * way. Interface version 2. The specification is Riptide's docs/IPP6.md; the offsets in the
 * comments below are its definition, and tests/t_ipp6_layout.c checks this header against them.
 *
 * Standalone: no other header is needed (Universal Interfaces may be included before it, and
 * MacTCP.h too: the csCodes, result codes and event codes MacTCP.h defines are the same numbers
 * and are then not defined again). C89, for MPW C, THINK C, Symantec C, CodeWarrior 68k and GCC
 * for the m68k. 68k code only (no routine descriptors for PowerPC).
 *
 * Detection: Gestalt(gestaltIPP6, &version) answers noErr and the version; OpenDriver("\p.IPP6")
 * answers the reference number. Without it, fall back to MacTCP's .IPP (docs/IPP6.md, section 2).
 *
 * This header may be copied into programs of any licence (it is an interface description); the
 * rest of Riptide is GPL-2.0-only.
 */
#ifndef IPP6_H
#define IPP6_H

/* ---- alignment: the blocks are laid out with 68k (2-byte) alignment, as MacTCP.h's are ---- */
#if defined(__MWERKS__) || defined(__MRC__) || defined(__SC__) || defined(applec) || defined(THINK_C)
#pragma options align=mac68k
#define IPP6_ALIGN_POP 1
#elif defined(__GNUC__) && !defined(__m68k__) && !defined(__mc68000__)
#pragma pack(push, 2)                       /* a compiler for another processor (the layout test) */
#define IPP6_PACK_POP 1
#endif

/* ---- types -------------------------------------------------------------------------------- */
#ifdef IPP6_LAYOUT_TEST
/* tests/t_ipp6_layout.c on a 32- or 64-bit host: every pointer and long as the 68k's 32 bits */
typedef unsigned int IP6UInt32;
typedef int          IP6SInt32;
#define IP6_PTR(t)   IP6UInt32
#else
typedef unsigned long IP6UInt32;
typedef long          IP6SInt32;
#define IP6_PTR(t)   t
#endif
typedef unsigned char  IP6UInt8;
typedef unsigned short IP6UInt16;
typedef short          IP6SInt16;

/* pascal procedures: the notification routines */
#if defined(pascal) || defined(THINK_C) || defined(__MWERKS__) || defined(applec) || defined(__SC__) || defined(__MRC__)
#define IPP6_PASCAL pascal
#else
#define IPP6_PASCAL                         /* GCC: give an assembly routine (docs/IPP6.md, section 9) */
#endif

#define gestaltIPP6        0x69707036UL     /* 'ipp6': answers the interface version */
#define IPP6_VERSION       2                /* this header's version (2: ip6ConnectName) */
#define IPP6_DRIVER_NAME   "\p.IPP6"        /* OpenDriver's name (a Pascal string) */
#define IPP6_DRIVER_NAME_C ".IPP6"

/* ---- the address: 20 bytes, bytes and words only (the same layout everywhere) -------------- */
typedef struct IP6Address {
	IP6UInt8  addr[16];                 /*  0: network byte order; IPv4 is ::ffff:a.b.c.d */
	IP6UInt16 port;                     /* 16: the port (the 68k's order is the network's) */
	IP6UInt16 scope;                    /* 18: interface index of a link-local address; 0 = default */
} IP6Address;
#define IP6_ADDRSTRLEN 46                   /* text of an address, NUL included (without a scope) */

/* IPv4-mapped addresses: a = IP6Address *, v4 = MacTCP's ip_addr (unsigned long) */
#define IP6_SET_V4(a, v4) do { int ip6_i_; for (ip6_i_ = 0; ip6_i_ < 10; ip6_i_++) (a)->addr[ip6_i_] = 0; \
	(a)->addr[10] = 0xFF; (a)->addr[11] = 0xFF; (a)->addr[12] = (IP6UInt8)((v4) >> 24); \
	(a)->addr[13] = (IP6UInt8)((v4) >> 16); (a)->addr[14] = (IP6UInt8)((v4) >> 8); \
	(a)->addr[15] = (IP6UInt8)(v4); } while (0)
#define IP6_IS_V4(a) (!(a)->addr[0] && !(a)->addr[1] && !(a)->addr[2] && !(a)->addr[3] && !(a)->addr[4] && \
	!(a)->addr[5] && !(a)->addr[6] && !(a)->addr[7] && !(a)->addr[8] && !(a)->addr[9] && \
	(a)->addr[10] == 0xFF && (a)->addr[11] == 0xFF)
#define IP6_GET_V4(a) ((IP6UInt32)(a)->addr[12] << 24 | (IP6UInt32)(a)->addr[13] << 16 | \
	(IP6UInt32)(a)->addr[14] << 8 | (IP6UInt32)(a)->addr[15])
#define IP6_IS_LINKLOCAL(a) ((a)->addr[0] == 0xFE && ((a)->addr[1] & 0xC0) == 0x80)

/* ---- csCodes ------------------------------------------------------------------------------ */
#ifndef __MACTCP__
enum {                                      /* MacTCP's numbers (MacTCP.h) */
	UDPCreate = 20, UDPRead = 21, UDPBfrReturn = 22, UDPWrite = 23, UDPRelease = 24, UDPMaxMTUSize = 25,
	TCPCreate = 30, TCPPassiveOpen = 31, TCPActiveOpen = 32, TCPSend = 34, TCPNoCopyRcv = 35,
	TCPRcvBfrReturn = 36, TCPRcv = 37, TCPClose = 38, TCPAbort = 39, TCPStatus = 40, TCPRelease = 42,
	TCPGlobalInfo = 43
};
#endif
enum {                                      /* .IPP6's own */
	ip6GetInfo = 70, ip6GetAddrs = 71, ip6Resolve = 72, ip6ResolveAddr = 73, ip6ResolveCancel = 74,
	ip6AddrToStr = 75, ip6StrToAddr = 76,
	ip6ConnectName = 77                 /* version 2: TCPActiveOpen by name, Happy Eyeballs (TCP6iopb) */
};

/* ---- result codes ------------------------------------------------------------------------- */
#ifndef __MACTCP__
enum {                                      /* MacTCP's, the same meanings (docs/IPP6.md, section 10) */
	inProgress = 1,
	connectionClosing = -23005, invalidLength = -23006, connectionExists = -23007,
	connectionDoesntExist = -23008, insufficientResources = -23009, invalidStreamPtr = -23010,
	streamAlreadyOpen = -23011, connectionTerminated = -23012, invalidBufPtr = -23013,
	invalidRDS = -23014, invalidWDS = -23014, openFailed = -23015, commandTimeout = -23016,
	duplicateSocket = -23017, ipDestDeadErr = -23033, nameSyntaxErr = -23041, noNameServer = -23044,
	authNameErr = -23045, noAnsErr = -23046, outOfMemory = -23048
};
#endif
enum {                                      /* .IPP6's own: -23060 down (-23066..-23079 kept) */
	ip6BadAddrErr = -23060,             /* an address the call cannot use */
	ip6ScopeErr = -23061,               /* a scope that names no interface */
	ip6TruncatedErr = -23062,           /* the text did not fit the buffer */
	ip6CancelledErr = -23063,           /* a lookup withdrawn by ip6ResolveCancel */
	ip6NotPendingErr = -23064,          /* ip6ResolveCancel: no such lookup in progress */
	ip6NoSourceErr = -23065             /* no address of the destination's family to send from */
};

/* ---- notification events and reasons (MacTCP's) --------------------------------------------- */
#ifndef __MACTCP__
enum { TCPClosing = 1, TCPULPTimeout = 2, TCPTerminate = 3, TCPDataArrival = 4, TCPUrgent = 5,
       TCPICMPReceived = 6 };
enum { TCPRemoteAbort = 2, TCPNetworkFailure = 3, TCPSecPrecMismatch = 4, TCPULPTimeoutTerminate = 5,
       TCPULPAbort = 6, TCPULPClose = 7, TCPServiceError = 8 };
enum { UDPDataArrival = 1, UDPICMPReceived = 2 };
enum { timeoutValue = 0x80, timeoutAction = 0x40, typeOfService = 0x20, precedence = 0x10 };
#endif
/* TCPStatus's connectionState (MacTCP's numbers) */
enum { kIP6StateClosed = 0, kIP6StateListen = 2, kIP6StateSynSent = 6, kIP6StateEstablished = 8,
       kIP6StateFinWait1 = 10, kIP6StateFinWait2 = 12, kIP6StateCloseWait = 14, kIP6StateClosing = 16 };

/* ip6GetInfo's features, ip6GetAddrs' flags, ip6Resolve's flags */
enum { kIP6HasIPv4 = 0x01, kIP6HasIPv6 = 0x02, kIP6HasLinkLocal = 0x04, kIP6HasIPv6Route = 0x08,
       kIP6HasNameServer = 0x10 };
enum { kIP6AddrIPv4 = 0x01, kIP6AddrLinkLocal = 0x02, kIP6AddrTemporary = 0x04, kIP6AddrDeprecated = 0x08,
       kIP6AddrTentative = 0x10, kIP6AddrPrimary = 0x20, kIP6AddrAutoconf = 0x40 };
enum { kIP6OrderDefault = 0, kIP6OrderRFC6724 = 1, kIP6OrderIPv6First = 2, kIP6OrderIPv4First = 3,
       kIP6OrderMask = 3, kIP6OnlyIPv6 = 0x04, kIP6OnlyIPv4 = 0x08, kIP6NoSearch = 0x10 };

/* ---- routines ----------------------------------------------------------------------------- */
struct TCP6iopb;
struct UDP6iopb;
struct IP6iopb;
/* an ICMP report for the notification routines: never sent by version 1 (icmpMsg is NULL) */
typedef struct ICMP6Report {
	IP6UInt32  streamPtr;               /*  0 */
	IP6Address localAddr;               /*  4 */
	IP6Address remoteAddr;              /* 24 */
	IP6SInt16  reportType;              /* 44 */
	IP6UInt16  optionalAddlInfo;        /* 46 */
	IP6UInt32  optionalAddlInfoPtr;     /* 48 */
} ICMP6Report;                              /* 52 bytes */
#ifdef IPP6_LAYOUT_TEST
typedef IP6UInt32 TCP6NotifyUPP, UDP6NotifyUPP, TCP6IOCompletionUPP, UDP6IOCompletionUPP, IP6IOCompletionUPP;
#else
typedef IPP6_PASCAL void (*TCP6NotifyUPP)(IP6UInt32 tcpStream, IP6UInt16 eventCode, char *userDataPtr,
                                          IP6UInt16 terminReason, ICMP6Report *icmpMsg);
typedef IPP6_PASCAL void (*UDP6NotifyUPP)(IP6UInt32 udpStream, IP6UInt16 eventCode, char *userDataPtr,
                                          ICMP6Report *icmpMsg);
/* completion routines: C functions of the block (the block is also in A0, the result in D0) */
typedef void (*TCP6IOCompletionUPP)(struct TCP6iopb *pb);
typedef void (*UDP6IOCompletionUPP)(struct UDP6iopb *pb);
typedef void (*IP6IOCompletionUPP)(struct IP6iopb *pb);
#endif
typedef IP6_PTR(char *) IP6Ptr;
typedef IP6_PTR(IP6Address *) IP6AddressPtr;

/* the Device Manager's header (CntrlParam), 28 bytes, then the stream (TCP, UDP) at 28 */
#define IP6_PB_HEADER(completion) \
	IP6_PTR(void *) qLink;              /*  0 */ \
	IP6SInt16 qType;                    /*  4 */ \
	IP6SInt16 ioTrap;                   /*  6 */ \
	IP6Ptr    ioCmdAddr;                /*  8 */ \
	completion ioCompletion;            /* 12 */ \
	volatile IP6SInt16 ioResult;        /* 16 */ \
	IP6_PTR(unsigned char *) ioNamePtr; /* 18 */ \
	IP6SInt16 ioVRefNum;                /* 22 */ \
	IP6SInt16 ioCRefNum;                /* 24 */ \
	IP6SInt16 csCode;                   /* 26 */

/* ==== TCP: TCP6iopb, 130 bytes ================================================================ */
typedef struct TCP6CreatePB {               /* TCPCreate (30), TCPRelease (42): MacTCP's */
	IP6Ptr        rcvBuff;              /* 32 */
	IP6UInt32     rcvBuffLen;           /* 36 */
	TCP6NotifyUPP notifyProc;           /* 40 */
	IP6Ptr        userDataPtr;          /* 44 */
} TCP6CreatePB;

typedef struct TCP6OpenPB {                 /* TCPPassiveOpen (31), TCPActiveOpen (32) */
	IP6UInt8   ulpTimeoutValue;         /* 32 */
	IP6UInt8   ulpTimeoutAction;        /* 33 */
	IP6UInt8   validityFlags;           /* 34 */
	IP6UInt8   commandTimeoutValue;     /* 35 */
	IP6Address remoteAddr;              /* 36: addr 36, port 52, scope 54 */
	IP6Address localAddr;               /* 56: addr 56, port 72, scope 74 */
	IP6UInt8   tosFlags;                /* 76 */
	IP6UInt8   precedence;              /* 77 */
	IP6UInt8   dontFrag;                /* 78 */
	IP6UInt8   timeToLive;              /* 79 */
	IP6UInt8   security;                /* 80 */
	IP6UInt8   optionCnt;               /* 81 */
	IP6UInt8   options[40];             /* 82 */
	IP6Ptr     userDataPtr;             /* 122 */
} TCP6OpenPB;

typedef struct TCP6ConnectNamePB {          /* ip6ConnectName (77, version 2): TCPActiveOpen's fields, and */
	IP6UInt8   ulpTimeoutValue;         /* 32: seconds for the whole connect, lookup included (0x80); 0 = 2 min */
	IP6UInt8   ulpTimeoutAction;        /* 33 */
	IP6UInt8   validityFlags;           /* 34 */
	IP6UInt8   commandTimeoutValue;     /* 35: not used */
	IP6Address remoteAddr;              /* 36: in: the port (52); out: the address that answered */
	IP6Address localAddr;               /* 56: out: this end */
	IP6UInt8   tosFlags;                /* 76: ignored, as TCPActiveOpen's */
	IP6UInt8   precedence;              /* 77 */
	IP6UInt8   dontFrag;                /* 78 */
	IP6UInt8   timeToLive;              /* 79 */
	IP6UInt8   security;                /* 80 */
	IP6UInt8   optionCnt;               /* 81 */
	IP6Ptr     hostName;                /* 82: in: the host name or an address literal, a C string */
	IP6UInt16  flags;                   /* 86: in: kIP6Order..., kIP6OnlyIPv6, kIP6OnlyIPv4, kIP6NoSearch */
	IP6UInt16  addrCount;               /* 88: out: the addresses the name had (of the families asked) */
	IP6UInt16  attempts;                /* 90: out: connection attempts started */
	IP6UInt32  elapsed;                 /* 92: out: ms from the call to the connection, lookup included */
	IP6UInt8   reserved[26];            /* 96: in: 0 */
	IP6Ptr     userDataPtr;             /* 122 */
} TCP6ConnectNamePB;

typedef struct TCP6SendPB {                 /* TCPSend (34): MacTCP's */
	IP6UInt8  ulpTimeoutValue;          /* 32 */
	IP6UInt8  ulpTimeoutAction;         /* 33 */
	IP6UInt8  validityFlags;            /* 34 */
	IP6UInt8  pushFlag;                 /* 35 */
	IP6UInt8  urgentFlag;               /* 36 */
	IP6UInt8  filler;                   /* 37 */
	IP6Ptr    wdsPtr;                   /* 38 */
	IP6UInt32 sendFree;                 /* 42 */
	IP6UInt16 sendLength;               /* 46 */
	IP6Ptr    userDataPtr;              /* 48 */
} TCP6SendPB;

typedef struct TCP6ReceivePB {              /* TCPNoCopyRcv (35), TCPRcvBfrReturn (36), TCPRcv (37): MacTCP's */
	IP6UInt8  commandTimeoutValue;      /* 32 */
	IP6UInt8  filler;                   /* 33 */
	IP6UInt8  markFlag;                 /* 34 */
	IP6UInt8  urgentFlag;               /* 35 */
	IP6Ptr    rcvBuff;                  /* 36 */
	IP6UInt16 rcvBuffLen;               /* 40 */
	IP6Ptr    rdsPtr;                   /* 42 */
	IP6UInt16 rdsLength;                /* 46 */
	IP6UInt16 secondTimeStamp;          /* 48 */
	IP6Ptr    userDataPtr;              /* 50 */
} TCP6ReceivePB;

typedef struct TCP6ClosePB {                /* TCPClose (38): MacTCP's */
	IP6UInt8 ulpTimeoutValue;           /* 32 */
	IP6UInt8 ulpTimeoutAction;          /* 33 */
	IP6UInt8 validityFlags;             /* 34 */
	IP6UInt8 filler;                    /* 35 */
	IP6Ptr   userDataPtr;               /* 36 */
} TCP6ClosePB;

typedef struct TCP6AbortPB {                /* TCPAbort (39): MacTCP's */
	IP6Ptr userDataPtr;                 /* 32 */
} TCP6AbortPB;

typedef struct TCP6StatusPB {               /* TCPStatus (40) */
	IP6UInt8   ulpTimeoutValue;         /* 32 */
	IP6UInt8   ulpTimeoutAction;        /* 33 */
	IP6SInt32  unused;                  /* 34 */
	IP6Address remoteAddr;              /* 38: addr 38, port 54, scope 56 */
	IP6Address localAddr;               /* 58: addr 58, port 74, scope 76 */
	IP6UInt8   tosFlags;                /* 78 */
	IP6UInt8   precedence;              /* 79 */
	IP6UInt8   connectionState;         /* 80 */
	IP6UInt8   filler;                  /* 81 */
	IP6UInt16  sendWindow;              /* 82 */
	IP6UInt16  rcvWindow;               /* 84 */
	IP6UInt16  amtUnackedData;          /* 86 */
	IP6UInt16  amtUnreadData;           /* 88 */
	IP6Ptr     securityLevelPtr;        /* 90 */
	IP6UInt32  sendUnacked;             /* 94 */
	IP6UInt32  sendNext;                /* 98 */
	IP6UInt32  congestionWindow;        /* 102 */
	IP6UInt32  rcvNext;                 /* 106 */
	IP6UInt32  srtt;                    /* 110 */
	IP6UInt32  lastRTT;                 /* 114 */
	IP6UInt32  sendMaxSegSize;          /* 118 */
	IP6Ptr     connStatPtr;             /* 122 */
	IP6Ptr     userDataPtr;             /* 126 */
} TCP6StatusPB;

typedef struct TCP6GlobalInfoPB {           /* TCPGlobalInfo (43): MacTCP's */
	IP6Ptr    tcpParamPtr;              /* 32 */
	IP6Ptr    tcpStatsPtr;              /* 36 */
	IP6_PTR(IP6UInt32 *) tcpCDBTable;   /* 40 */
	IP6Ptr    userDataPtr;              /* 44 */
	IP6UInt16 maxTCPConnections;        /* 48 */
} TCP6GlobalInfoPB;

typedef struct TCP6iopb {
	IP6_PB_HEADER(TCP6IOCompletionUPP)
	IP6UInt32 tcpStream;                /* 28 */
	union {                             /* 32 */
		TCP6CreatePB     create;
		TCP6OpenPB       open;
		TCP6SendPB       send;
		TCP6ReceivePB    receive;
		TCP6ClosePB      close;
		TCP6AbortPB      abort;
		TCP6StatusPB     status;
		TCP6GlobalInfoPB globalInfo;
		TCP6ConnectNamePB connectName;
	} csParam;
} TCP6iopb;

/* ==== UDP: UDP6iopb, 86 bytes ================================================================= */
typedef struct UDP6CreatePB {               /* UDPCreate (20), UDPRelease (24): MacTCP's */
	IP6Ptr        rcvBuff;              /* 32 */
	IP6UInt32     rcvBuffLen;           /* 36 */
	UDP6NotifyUPP notifyProc;           /* 40 */
	IP6UInt16     localPort;            /* 44 */
	IP6Ptr        userDataPtr;          /* 46 */
	IP6UInt16     endingPort;           /* 50 */
} UDP6CreatePB;

typedef struct UDP6SendPB {                 /* UDPWrite (23) */
	IP6UInt16  reserved;                /* 32 */
	IP6Address remoteAddr;              /* 34: addr 34, port 50, scope 52 */
	IP6Ptr     wdsPtr;                  /* 54 */
	IP6UInt8   checkSum;                /* 58 */
	IP6UInt8   filler;                  /* 59 */
	IP6UInt16  sendLength;              /* 60 */
	IP6Ptr     userDataPtr;             /* 62 */
	IP6UInt16  localPort;               /* 66 */
} UDP6SendPB;

typedef struct UDP6ReceivePB {              /* UDPRead (21), UDPBfrReturn (22) */
	IP6UInt16  timeOut;                 /* 32 */
	IP6Address remoteAddr;              /* 34: addr 34, port 50, scope 52 */
	IP6Ptr     rcvBuff;                 /* 54 */
	IP6UInt16  rcvBuffLen;              /* 58 */
	IP6UInt16  secondTimeStamp;         /* 60 */
	IP6Ptr     userDataPtr;             /* 62 */
	IP6Address destAddr;                /* 66: addr 66, port 82, scope 84 */
} UDP6ReceivePB;

typedef struct UDP6MTUPB {                  /* UDPMaxMTUSize (25) */
	IP6UInt16  mtuSize;                 /* 32 */
	IP6Address remoteAddr;              /* 34 */
	IP6Ptr     userDataPtr;             /* 54 */
} UDP6MTUPB;

typedef struct UDP6iopb {
	IP6_PB_HEADER(UDP6IOCompletionUPP)
	IP6UInt32 udpStream;                /* 28 */
	union {                             /* 32 */
		UDP6CreatePB  create;
		UDP6SendPB    send;
		UDP6ReceivePB receive;
		UDP6MTUPB     mtu;
	} csParam;
} UDP6iopb;

/* ==== the interface's own calls: IP6iopb, 80 bytes ============================================ */
typedef struct IP6InfoPB {                  /* ip6GetInfo (70) */
	IP6UInt32 version;                  /* 28 */
	IP6UInt32 features;                 /* 32: kIP6Has... */
	IP6UInt16 maxTCPStreams;            /* 36 */
	IP6UInt16 maxUDPStreams;            /* 38 */
	IP6UInt16 maxLookups;               /* 40 */
	IP6UInt16 order;                    /* 42: kIP6OrderRFC6724, ...IPv6First, ...IPv4First */
	IP6_PTR(const char *) implementation;   /* 44: a C string, the driver's */
	IP6Ptr    userDataPtr;              /* 48 */
} IP6InfoPB;

typedef struct IP6AddrInfo {                /* ip6GetAddrs' entries: 32 bytes */
	IP6Address addr;                    /*  0 */
	IP6UInt8   prefixLen;               /* 20 */
	IP6UInt8   flags;                   /* 21: kIP6Addr... */
	IP6UInt16  ifIndex;                 /* 22 */
	char       ifName[8];               /* 24: a C string */
} IP6AddrInfo;

typedef struct IP6AddrsPB {                 /* ip6GetAddrs (71) */
	IP6_PTR(IP6AddrInfo *) buffer;      /* 28 */
	IP6UInt32 bufferLen;                /* 32: bytes */
	IP6UInt16 count;                    /* 36 */
	IP6UInt16 total;                    /* 38 */
	IP6UInt32 reserved;                 /* 40 */
	IP6Ptr    userDataPtr;              /* 44 */
} IP6AddrsPB;

typedef struct IP6ResolvePB {               /* ip6Resolve (72), ip6ResolveAddr (73), ip6AddrToStr (75), ip6StrToAddr (76) */
	IP6Ptr        hostName;             /* 28: the name or text (in), or the buffer for it (out) */
	IP6UInt16     hostNameLen;          /* 32: the buffer's size (73, 75) */
	IP6UInt16     flags;                /* 34: kIP6Order..., kIP6Only..., kIP6NoSearch (72) */
	IP6Address    addr;                 /* 36: 73, 75 in; 76 out */
	IP6AddressPtr results;              /* 56: 72 */
	IP6UInt16     maxResults;           /* 60: 72 in */
	IP6UInt16     count;                /* 62: 72 out */
	IP6UInt16     port;                 /* 64: 72, 76 in: put into the results */
	IP6UInt16     timeout;              /* 66: 72, 73: seconds, 0 = 10 */
	IP6Ptr        canonName;            /* 68: 72: a buffer, or NULL */
	IP6UInt16     canonNameLen;         /* 72 */
	IP6UInt16     reserved;             /* 74 */
	IP6Ptr        userDataPtr;          /* 76 */
} IP6ResolvePB;

typedef struct IP6CancelPB {                /* ip6ResolveCancel (74) */
	IP6_PTR(struct IP6iopb *) target;   /* 28 */
	IP6Ptr userDataPtr;                 /* 32 */
} IP6CancelPB;

typedef struct IP6iopb {
	IP6_PB_HEADER(IP6IOCompletionUPP)
	union {                             /* 28 */
		IP6InfoPB    info;
		IP6AddrsPB   addrs;
		IP6ResolvePB resolve;
		IP6CancelPB  cancel;
	} csParam;
} IP6iopb;

/* the sizes are the definition: a compiler that lays the blocks out otherwise stops here */
typedef char ipp6_check_address[sizeof(IP6Address) == 20 ? 1 : -1];
typedef char ipp6_check_addrinfo[sizeof(IP6AddrInfo) == 32 ? 1 : -1];
typedef char ipp6_check_tcp[sizeof(TCP6iopb) == 130 ? 1 : -1];
typedef char ipp6_check_udp[sizeof(UDP6iopb) == 86 ? 1 : -1];
typedef char ipp6_check_ip[sizeof(IP6iopb) == 80 ? 1 : -1];

#if defined(IPP6_ALIGN_POP)
#pragma options align=reset
#undef IPP6_ALIGN_POP
#elif defined(IPP6_PACK_POP)
#pragma pack(pop)
#undef IPP6_PACK_POP
#endif
#endif
