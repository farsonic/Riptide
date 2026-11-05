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
#define S2_DMACopyToBuff32   (S2_Dummy + 8)    /* SANA-II R3 */
#define S2_DMACopyFromBuff32 (S2_Dummy + 9)

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
#define S2WERR_BAD_EVENT       12   /* S2_ONEVENT: an event bit the driver does not know */
#define S2WERR_IS_CONFIGURED   15

#define S2EVENT_ERROR    (1UL << 0)
#define S2EVENT_TX       (1UL << 1)
#define S2EVENT_RX       (1UL << 2)
#define S2EVENT_ONLINE   (1UL << 3)
#define S2EVENT_OFFLINE  (1UL << 4)
#define S2EVENT_BUFF     (1UL << 5)
#define S2EVENT_HARDWARE (1UL << 6)
#define S2EVENT_SOFTWARE (1UL << 7)
/* SANA-II wireless extensions (Neil Cafferkey; Prism2.device, AROS,
 * WiFiPi.device): association with a network gained / lost */
#define S2EVENT_CONNECT    (1UL << 9)
#define S2EVENT_DISCONNECT (1UL << 10)

/* ---- SANA-II wireless extensions (the same public definitions) ----
 * Association (SSID, keys) is the business of a tool like WirelessManager;
 * Riptide only reads the state. */
#define S2_GETSIGNALQUALITY 0xc010   /* ios2_StatData: struct Sana2SignalQuality */
#define S2_GETNETWORKS      0xc011
#define S2_SETOPTIONS       0xc012
#define S2_SETKEY           0xc013
#define S2_GETNETWORKINFO   0xc014   /* ios2_Data: an exec memory pool; reply:
                                      * ios2_StatData a tag list allocated in it */
#define S2_GETRADIOBANDS    0xc017
#define S2_GETCRYPTTYPES    0xc018

/* tags of S2_GETNETWORKINFO / S2_GETNETWORKS (TAG_USER based) */
#define S2INFO_SSID           (TAG_USER + 0)    /* NUL-terminated name */
#define S2INFO_BSSID          (TAG_USER + 1)    /* 6 bytes */
#define S2INFO_AuthTypes      (TAG_USER + 2)
#define S2INFO_AssocID        (TAG_USER + 3)
#define S2INFO_Encryption     (TAG_USER + 4)
#define S2INFO_PortType       (TAG_USER + 5)
#define S2INFO_BeaconInterval (TAG_USER + 6)
#define S2INFO_Channel        (TAG_USER + 7)
#define S2INFO_Signal         (TAG_USER + 8)
#define S2INFO_Noise          (TAG_USER + 9)
#define S2INFO_Capabilities   (TAG_USER + 10)
#define S2INFO_InfoElements   (TAG_USER + 11)
#define S2INFO_WPAInfo        (TAG_USER + 12)
#define S2INFO_Band           (TAG_USER + 13)
#define S2INFO_DefaultKeyNo   (TAG_USER + 14)

struct Sana2SignalQuality {
	LONG SignalLevel;          /* dBm */
	LONG NoiseLevel;           /* dBm */
};

#endif
