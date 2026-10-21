/*
 * Riptide - SANA-II device interface definitions (written from the SANA-II
 * specification; verified by tests/amiga/check_layouts.sh). Used by both
 * toolchains: the NDK 3.2 Include_H directory does not ship devices/sana2.h.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_SANA2_H
#define RT_AMIGA_SANA2_H

/* ---- SANA-II ---- */
#define SANA2_MAX_ADDR_BITS  128
#define SANA2_MAX_ADDR_BYTES ((SANA2_MAX_ADDR_BITS + 7) / 8)

struct IOSana2Req {
	struct IORequest ios2_Req;
	ULONG ios2_WireError;
	ULONG ios2_PacketType;
	UBYTE ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
	UBYTE ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
	ULONG ios2_DataLength;
	APTR  ios2_Data;
	APTR  ios2_StatData;
	APTR  ios2_BufferManagement;
};

#define SANA2IOB_RAW   7
#define SANA2IOB_BCAST 6
#define SANA2IOB_MCAST 5
#define SANA2IOF_RAW   (1 << SANA2IOB_RAW)
#define SANA2IOF_BCAST (1 << SANA2IOB_BCAST)
#define SANA2IOF_MCAST (1 << SANA2IOB_MCAST)

#define SANA2OPF_MINE  (1 << 0)
#define SANA2OPF_PROM  (1 << 1)

#define S2_Dummy        (TAG_USER + 0xB0000)
#define S2_CopyToBuff   (S2_Dummy + 1)
#define S2_CopyFromBuff (S2_Dummy + 2)
#define S2_PacketFilter (S2_Dummy + 3)

struct Sana2DeviceQuery {
	ULONG SizeAvailable;
	ULONG SizeSupplied;
	ULONG DevQueryFormat;
	ULONG DeviceLevel;
	UWORD AddrFieldSize;     /* bits */
	ULONG MTU;
	ULONG BPS;
	ULONG HardwareType;
	ULONG RawMTU;
};

#define S2WireType_Ethernet  1
#define S2WireType_IEEE802   6
#define S2WireType_Arcnet    7
#define S2WireType_PPP     253
#define S2WireType_SLIP    254
#define S2WireType_CSLIP   255

/* S2_GETGLOBALSTATS */
struct Sana2DeviceStats {
	ULONG PacketsReceived;
	ULONG PacketsSent;
	ULONG BadData;
	ULONG Overruns;
	ULONG Unused;
	ULONG UnknownTypesReceived;
	ULONG Reconfigurations;
	struct timeval LastStart;
};

/* S2_GETSPECIALSTATS: the header is followed by RecordCountMax records */
struct Sana2SpecialStatRecord {
	ULONG  Type;
	ULONG  Count;
	STRPTR String;
};

struct Sana2SpecialStatHeader {
	ULONG RecordCountMax;
	ULONG RecordCountSupplied;
};

#define S2SS_ETHERNET_BADMULTICAST   ((((S2WireType_Ethernet) & 0xffff) << 16) | 0x0000)
#define S2SS_ETHERNET_RETRIES        ((((S2WireType_Ethernet) & 0xffff) << 16) | 0x0001)
#define S2SS_ETHERNET_FIFO_UNDERRUNS ((((S2WireType_Ethernet) & 0xffff) << 16) | 0x0002)

#define S2_START               CMD_NONSTD
#define S2_DEVICEQUERY         (S2_START + 0)
#define S2_GETSTATIONADDRESS   (S2_START + 1)
#define S2_CONFIGINTERFACE     (S2_START + 2)
#define S2_ADDMULTICASTADDRESS (S2_START + 5)
#define S2_DELMULTICASTADDRESS (S2_START + 6)
#define S2_MULTICAST           (S2_START + 7)
#define S2_BROADCAST           (S2_START + 8)
#define S2_TRACKTYPE           (S2_START + 9)
#define S2_UNTRACKTYPE         (S2_START + 10)
#define S2_GETTYPESTATS        (S2_START + 11)
#define S2_GETSPECIALSTATS     (S2_START + 12)
#define S2_GETGLOBALSTATS      (S2_START + 13)
#define S2_ONEVENT             (S2_START + 14)
#define S2_READORPHAN          (S2_START + 15)
#define S2_ONLINE              (S2_START + 16)
#define S2_OFFLINE             (S2_START + 17)

#define S2ERR_NO_ERROR      0
#define S2ERR_NO_RESOURCES  1
#define S2ERR_BAD_ARGUMENT  3
#define S2ERR_BAD_STATE     4
#define S2ERR_BAD_ADDRESS   5
#define S2ERR_MTU_EXCEEDED  6
#define S2ERR_NOT_SUPPORTED 8
#define S2ERR_SOFTWARE      9
#define S2ERR_OUTOFSERVICE  10
#define S2ERR_TX_FAILURE    11

#define S2WERR_GENERIC_ERROR   0
#define S2WERR_NOT_CONFIGURED  1
#define S2WERR_UNIT_ONLINE     2
#define S2WERR_UNIT_OFFLINE    3
#define S2WERR_IS_CONFIGURED   15

#define S2EVENT_ERROR    (1UL << 0)
#define S2EVENT_TX       (1UL << 1)
#define S2EVENT_RX       (1UL << 2)
#define S2EVENT_ONLINE   (1UL << 3)
#define S2EVENT_OFFLINE  (1UL << 4)
#define S2EVENT_BUFF     (1UL << 5)
#define S2EVENT_HARDWARE (1UL << 6)
#define S2EVENT_SOFTWARE (1UL << 7)

#endif
