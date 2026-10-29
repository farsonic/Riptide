#ifndef LIBRARIES_BSDSOCKET_H
#define LIBRARIES_BSDSOCKET_H
/*
 * bsdsocket.library - the SocketBaseTagList() codes and the definitions of
 * the Roadshow extensions (bsdsocket.library version 4) that Riptide
 * provides: packet filter, interface, route, DNS, monitoring, statistics
 * and configuration data APIs. Written from the Roadshow SDK
 * documentation; layouts follow the 68k ABI (2-byte alignment).
 *
 *   #include <proto/bsdsocket.h>
 *   SocketBase = OpenLibrary("bsdsocket.library", 4);
 *
 * The socket structures and constants (struct sockaddr_in, AF_INET, ...)
 * come from the usual network headers (AmiTCP or Roadshow SDK). See
 * docs/AMIGA.md, "Roadshow extensions".
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef EXEC_PORTS_H
#include <exec/ports.h>
#endif
#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif
#ifndef UTILITY_HOOKS_H
#include <utility/hooks.h>
#endif
#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif

/* ---- SocketBaseTagList ---- */
#define SBTF_VAL 0x0000
#define SBTF_REF 0x8000
#define SBTB_CODE 1
#define SBTS_CODE 0x3FFF
#define SBTF_GET 0x0
#define SBTF_SET 0x1
#define SBTM_GETREF(code) (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE))
#define SBTM_GETVAL(code) (TAG_USER | (((code) & SBTS_CODE) << SBTB_CODE))
#define SBTM_SETREF(code) (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)
#define SBTM_SETVAL(code) (TAG_USER | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)

#define SBTC_BREAKMASK      1
#define SBTC_SIGIOMASK      2
#define SBTC_SIGURGMASK     3
#define SBTC_SIGEVENTMASK   4
#define SBTC_ERRNO          6
#define SBTC_HERRNO         7
#define SBTC_DTABLESIZE     8
#define SBTC_FDCALLBACK     9
#define SBTC_LOGSTAT        10
#define SBTC_LOGTAGPTR      11
#define SBTC_LOGFACILITY    12
#define SBTC_LOGMASK        13
#define SBTC_ERRNOSTRPTR    14
#define SBTC_HERRNOSTRPTR   15
#define SBTC_IOERRNOSTRPTR  16
#define SBTC_S2ERRNOSTRPTR  17
#define SBTC_S2WERRNOSTRPTR 18
#define SBTC_ERRNOBYTEPTR   21
#define SBTC_ERRNOWORDPTR   22
#define SBTC_ERRNOLONGPTR   24
#define SBTC_HERRNOLONGPTR  25
#define SBTC_RELEASESTRPTR  29

#define FDCB_FREE  0
#define FDCB_ALLOC 1
#define FDCB_CHECK 2

/* ---- SocketBaseTagList() codes ---- */
#define SBTC_COMPAT43                    30
#define SBTC_NUM_PACKET_FILTER_CHANNELS  40
#define SBTC_HAVE_ROUTING_API            41
#define SBTC_UDP_CHECKSUM                42
#define SBTC_IP_FORWARDING               43
#define SBTC_IP_DEFAULT_TTL              44
#define SBTC_ICMP_MASK_REPLY             45
#define SBTC_ICMP_SEND_REDIRECTS         46
#define SBTC_HAVE_INTERFACE_API          47
#define SBTC_ICMP_PROCESS_ECHO           48
#define SBTC_ICMP_PROCESS_TSTAMP         49
#define SBTC_HAVE_MONITORING_API         50
#define SBTC_CAN_SHARE_LIBRARY_BASES     51
#define SBTC_LOG_FILE_NAME               52
#define SBTC_HAVE_STATUS_API             53
#define SBTC_HAVE_DNS_API                54
#define SBTC_LOG_HOOK                    55
#define SBTC_SYSTEM_STATUS               56
#define SBTC_SIG_ADDRESS_CHANGE_MASK     57
#define SBTC_IPF_API_VERSION             58
#define SBTC_HAVE_LOCAL_DATABASE_API     59
#define SBTC_HAVE_ADDRESS_CONVERSION_API 60
#define SBTC_HAVE_KERNEL_MEMORY_API      61
#define SBTC_IP_FILTER_HOOK              62
#define SBTC_HAVE_SERVER_API             63
#define SBTC_GET_BYTES_RECEIVED          64
#define SBTC_GET_BYTES_SENT              65
#define SBTC_IDN_DEFAULT_CHARACTER_SET   66
#define SBTC_HAVE_ROADSHOWDATA_API       67
#define SBTC_ERROR_HOOK                  68
#define SBTC_HAVE_GETHOSTADDR_R_API      69

#define IDNCS_ASCII 0

/* SBTC_SYSTEM_STATUS */
#define SBSYSSTAT_Interfaces       (1L << 0)
#define SBSYSSTAT_PTP_Interfaces   (1L << 1)
#define SBSYSSTAT_BCast_Interfaces (1L << 2)
#define SBSYSSTAT_Resolver         (1L << 3)
#define SBSYSSTAT_Routes           (1L << 4)
#define SBSYSSTAT_DefaultRoute     (1L << 5)

/* ICMP echo / time stamp processing */
#define IR_Process 0
#define IR_Ignore  1
#define IR_Drop    2

/* 64-bit counters */
typedef struct {
	ULONG sbq_High;
	ULONG sbq_Low;
} SBQUAD_T;

/* SBTC_ERROR_HOOK */
struct ErrorHookMsg {
	ULONG ehm_Size;
	ULONG ehm_Action;
	LONG  ehm_Code;
};
#define EHMA_Set_errno   1
#define EHMA_Set_h_errno 2

/* SBTC_LOG_HOOK */
struct LogHookMessage {
	LONG   lhm_Size;
	LONG   lhm_Priority;
	struct DateStamp lhm_Date;
	STRPTR lhm_Tag;
	ULONG  lhm_ID;
	STRPTR lhm_Message;
};

/* ---- routes ---- */
#define RTA_BASE            (TAG_USER + 1600)
#define RTA_Destination     (RTA_BASE + 1)
#define RTA_Gateway         (RTA_BASE + 2)
#define RTA_DefaultGateway  (RTA_BASE + 3)
#define RTA_DestinationHost (RTA_BASE + 4)
#define RTA_DestinationNet  (RTA_BASE + 5)

/* ---- interfaces ---- */
#define IFA_BASE              (TAG_USER + 1700)
#define IFA_IPType            (IFA_BASE + 1)
#define IFA_ARPType           (IFA_BASE + 2)
#define IFA_NumReadRequests   (IFA_BASE + 3)
#define IFA_NumWriteRequests  (IFA_BASE + 4)
#define IFA_NumARPRequests    (IFA_BASE + 5)
#define IFA_PacketFilterMode  (IFA_BASE + 6)
#define IFA_PointToPoint      (IFA_BASE + 7)
#define IFA_Reserved0         (IFA_BASE + 8)
#define IFA_Multicast         (IFA_BASE + 9)
#define IFA_DownGoesOffline   (IFA_BASE + 10)
#define IFA_ReportOffline     (IFA_BASE + 11)
#define IFA_RequiresInitDelay (IFA_BASE + 12)
#define IFA_CopyMode          (IFA_BASE + 13)
#define IFA_HardwareAddress   (IFA_BASE + 14)
#define IFA_SetDebugMode      (IFA_BASE + 15)
#define IFA_LimitMTU          (IFA_BASE + 16)

#define IFC_BASE               (TAG_USER + 1800)
#define IFC_Address            (IFC_BASE + 1)
#define IFC_NetMask            (IFC_BASE + 2)
#define IFC_DestinationAddress (IFC_BASE + 3)
#define IFC_BroadcastAddress   (IFC_BASE + 4)
#define IFC_Metric             (IFC_BASE + 5)
#define IFC_AddAliasAddress    (IFC_BASE + 6)
#define IFC_DeleteAliasAddress (IFC_BASE + 7)
#define IFC_State              (IFC_BASE + 8)
#define IFC_GetPeerAddress     (IFC_BASE + 9)
#define IFC_GetDNS             (IFC_BASE + 10)
#define IFC_AssociatedRoute    (IFC_BASE + 12)
#define IFC_AssociatedDNS      (IFC_BASE + 13)
#define IFC_ReleaseAddress     (IFC_BASE + 14)
#define IFC_SetDebugMode       (IFC_BASE + 15)
#define IFC_Complete           (IFC_BASE + 16)
#define IFC_LimitMTU           (IFC_BASE + 17)

#define IFQ_BASE                   (TAG_USER + 1900)
#define IFQ_DeviceName             (IFQ_BASE + 1)
#define IFQ_DeviceUnit             (IFQ_BASE + 2)
#define IFQ_HardwareAddressSize    (IFQ_BASE + 3)
#define IFQ_HardwareAddress        (IFQ_BASE + 4)
#define IFQ_MTU                    (IFQ_BASE + 5)
#define IFQ_BPS                    (IFQ_BASE + 6)
#define IFQ_HardwareType           (IFQ_BASE + 7)
#define IFQ_PacketsReceived        (IFQ_BASE + 8)
#define IFQ_PacketsSent            (IFQ_BASE + 9)
#define IFQ_BadData                (IFQ_BASE + 10)
#define IFQ_Overruns               (IFQ_BASE + 11)
#define IFQ_UnknownTypes           (IFQ_BASE + 12)
#define IFQ_LastStart              (IFQ_BASE + 13)
#define IFQ_Address                (IFQ_BASE + 14)
#define IFQ_DestinationAddress     (IFQ_BASE + 15)
#define IFQ_BroadcastAddress       (IFQ_BASE + 16)
#define IFQ_NetMask                (IFQ_BASE + 17)
#define IFQ_Metric                 (IFQ_BASE + 18)
#define IFQ_State                  (IFQ_BASE + 19)
#define IFQ_AddressBindType        (IFQ_BASE + 20)
#define IFQ_AddressLeaseExpires    (IFQ_BASE + 21)
#define IFQ_PrimaryDNSAddress      (IFQ_BASE + 22)
#define IFQ_SecondaryDNSAddress    (IFQ_BASE + 23)
#define IFQ_NumReadRequests        (IFQ_BASE + 24)
#define IFQ_MaxReadRequests        (IFQ_BASE + 25)
#define IFQ_NumWriteRequests       (IFQ_BASE + 26)
#define IFQ_MaxWriteRequests       (IFQ_BASE + 27)
#define IFQ_GetBytesIn             (IFQ_BASE + 28)
#define IFQ_GetBytesOut            (IFQ_BASE + 29)
#define IFQ_GetDebugMode           (IFQ_BASE + 30)
#define IFQ_GetSANA2CopyStats      (IFQ_BASE + 31)
#define IFQ_NumReadRequestsPending (IFQ_BASE + 32)
#define IFQ_NumWriteRequestsPending (IFQ_BASE + 33)
#define IFQ_HardwareMTU            (IFQ_BASE + 34)
#define IFQ_OutputDrops            (IFQ_BASE + 35)
#define IFQ_InputDrops             (IFQ_BASE + 36)
#define IFQ_OutputErrors           (IFQ_BASE + 37)
#define IFQ_InputErrors            (IFQ_BASE + 38)
#define IFQ_OutputMulticasts       (IFQ_BASE + 39)
#define IFQ_InputMulticasts        (IFQ_BASE + 40)
#define IFQ_IPDrops                (IFQ_BASE + 41)
#define IFQ_ARPDrops               (IFQ_BASE + 42)

/* IFQ_GetSANA2CopyStats: the number of calls of each SANA-II copy function */
struct SANA2CopyStats {
	ULONG s2cs_DMAIn, s2cs_DMAOut, s2cs_ByteIn, s2cs_ByteOut, s2cs_WordOut;
};

#define IFABT_Unknown 0
#define IFABT_Static  1
#define IFABT_Dynamic 2

#define SM_Offline 0
#define SM_Online  1
#define SM_Down    2
#define SM_Up      3

/* ---- monitoring hooks ---- */
#define MHT_ICMP        0
#define MHT_UDP         1
#define MHT_TCP_Connect 2
#define MHT_Connect     3
#define MHT_Send        4
#define MHT_Packet      5
#define MHT_Bind        6

#define MA_Continue      0
#define MA_Ignore        (-1)
#define MA_Drop          (-2)
#define MA_DropWithReset (-3)

struct ConnectMonitorMsg {
	LONG   cmm_Size;
	STRPTR cmm_Caller;
	LONG   cmm_Socket;
	APTR   cmm_Name;
	LONG   cmm_NameLen;
};
struct BindMonitorMsg {
	LONG   bmm_Size;
	STRPTR bmm_Caller;
	LONG   bmm_Socket;
	APTR   bmm_Name;
	LONG   bmm_NameLen;
};
struct SendMonitorMessage {
	LONG   smm_Size;
	STRPTR smm_Caller;
	LONG   smm_Socket;
	APTR   smm_Buffer;
	LONG   smm_Len;
	LONG   smm_Flags;
	APTR   smm_To;
	LONG   smm_ToLen;
	APTR   smm_Msg;
};
/* MHT_TCP_Connect, MHT_UDP, MHT_ICMP: source, destination, header */
struct ProtocolMonitorMessage {
	LONG   pm_Size;
	APTR   pm_Src;
	APTR   pm_Dst;
	APTR   pm_Header;
};
struct PacketMonitorMessage {
	LONG pmm_Size;
	LONG pmm_Direction;
	APTR pmm_LinkLayerData;
	LONG pmm_LinkLayerSize;
	APTR pmm_PacketData;
	LONG pmm_PacketSize;
};
#define PMMD_Receive 0
#define PMMD_Send    1

/* ---- statistics ---- */
#define NETWORKSTATUS_VERSION 1

/* ---- address allocation (DHCP) ---- */
struct AddressAllocationMessage {
	struct Message aam_Message;
	LONG   aam_Reserved;
	LONG   aam_Result;
	LONG   aam_Version;
	LONG   aam_Protocol;
	char   aam_InterfaceName[16];
	LONG   aam_Timeout;
	ULONG  aam_LeaseTime;
	ULONG  aam_RequestedAddress;
	STRPTR aam_ClientIdentifier;
	ULONG  aam_Address;
	ULONG  aam_ServerAddress;
	ULONG  aam_SubnetMask;
	STRPTR aam_NAKMessage;
	LONG   aam_NAKMessageSize;
	ULONG *aam_RouterTable;
	LONG   aam_RouterTableSize;
	ULONG *aam_DNSTable;
	LONG   aam_DNSTableSize;
	ULONG *aam_StaticRouteTable;
	LONG   aam_StaticRouteTableSize;
	STRPTR aam_HostName;
	LONG   aam_HostNameSize;
	STRPTR aam_DomainName;
	LONG   aam_DomainNameSize;
	UBYTE *aam_BOOTPMessage;
	LONG   aam_BOOTPMessageSize;
	struct DateStamp *aam_LeaseExpires;
	WORD   aam_Unicast;          /* BOOL: version 2 */
};
#define AAM_VERSION         2
#define AAM_VERSION_MINIMUM 1
#define AAM_TIMEOUT_MIN     10

#define AAMR_Success            0
#define AAMR_Aborted            1
#define AAMR_InterfaceNotKnown  2
#define AAMR_InterfaceWrongType 3
#define AAMR_AddressKnown       4
#define AAMR_VersionUnknown     5
#define AAMR_NoMemory           6
#define AAMR_Timeout            7
#define AAMR_AddressInUse       8
#define AAMR_AddrChangeFailed   9
#define AAMR_MaskChangeFailed   10
#define AAMR_Busy               11
#define AAMR_Ignored            (-1)

#define AAMP_BOOTP    0
#define AAMP_DHCP     1
#define AAMP_SLOWAUTO 2
#define AAMP_FASTAUTO 3

#define DHCP_INFINITE_LEASE_TIME 0xFFFFFFFFUL

#define CAAMTA_BASE                  (TAG_USER + 2000)
#define CAAMTA_Timeout               (CAAMTA_BASE + 1)
#define CAAMTA_LeaseTime             (CAAMTA_BASE + 2)
#define CAAMTA_RequestedAddress      (CAAMTA_BASE + 3)
#define CAAMTA_ClientIdentifier      (CAAMTA_BASE + 4)
#define CAAMTA_NAKMessageSize        (CAAMTA_BASE + 5)
#define CAAMTA_RouterTableSize       (CAAMTA_BASE + 6)
#define CAAMTA_DNSTableSize          (CAAMTA_BASE + 7)
#define CAAMTA_StaticRouteTableSize  (CAAMTA_BASE + 8)
#define CAAMTA_HostNameSize          (CAAMTA_BASE + 9)
#define CAAMTA_DomainNameSize        (CAAMTA_BASE + 10)
#define CAAMTA_BOOTPMessageSize      (CAAMTA_BASE + 11)
#define CAAMTA_RecordLeaseExpiration (CAAMTA_BASE + 12)
#define CAAMTA_ReplyPort             (CAAMTA_BASE + 13)
#define CAAMTA_RequestUnicast        (CAAMTA_BASE + 14)

#define CAAME_Success                     0
#define CAAME_Invalid_result_ptr          1
#define CAAME_Not_enough_memory           2
#define CAAME_Invalid_version             3
#define CAAME_Invalid_protocol            4
#define CAAME_Invalid_interface_name      5
#define CAAME_Interface_not_found         6
#define CAAME_Invalid_client_identifier   7
#define CAAME_Client_identifier_too_short 8
#define CAAME_Client_identifier_too_long  9

/* ---- DNS ---- */
struct DomainNameServerNode {
	struct MinNode dnsn_MinNode;
	LONG   dnsn_Size;
	STRPTR dnsn_Address;
	LONG   dnsn_UseCount;        /* < 0: configured statically */
};

/* ---- configuration data ---- */
struct RoadshowDataNode {
	struct MinNode rdn_MinNode;
	STRPTR rdn_Name;
	UWORD  rdn_Flags;
	WORD   rdn_Type;
	ULONG  rdn_Length;
	APTR   rdn_Data;
};
#define RDNT_Integer   0
#define RDNF_ReadOnly  (1 << 0)
#define ORD_ReadAccess  0
#define ORD_WriteAccess 1

#endif /* LIBRARIES_BSDSOCKET_H */
