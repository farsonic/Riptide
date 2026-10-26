/*
 * Riptide - protocol statistics (names follow the Linux SNMP MIB counters
 * so `netstat -s` output is familiar). Owned by the respective modules.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_STATS_H
#define RT_STATS_H

#include "rt_types.h"

struct rt_ip_stats {
	u32 InReceives, InHdrErrors, InAddrErrors, InUnknownProtos, InDiscards,
	    InDelivers, OutRequests, OutDiscards, OutNoRoutes, ReasmReqds,
	    ReasmOKs, ReasmFails, ReasmTimeout, FragOKs, FragFails, FragCreates,
	    ForwDatagrams, InBcast;
};
struct rt_icmp_stats {
	u32 InMsgs, InErrors, InDestUnreachs, InTimeExcds, InEchos, InEchoReps,
	    InRedirects, OutMsgs, OutErrors, OutDestUnreachs, OutEchoReps,
	    OutRateLimited;
};
struct rt_udp_stats {
	u32 InDatagrams, NoPorts, InErrors, OutDatagrams, RcvbufErrors,
	    InCsumErrors;
	/* 0.17 (appended: older readers check the record size): sends that
	 * found no transmit room on the interface (pacing: EWOULDBLOCK or a
	 * wait), and datagrams sent with RiptideSendMMsg() */
	u32 TxNoRoom, MMsgDatagrams;
};
struct rt_tcp_stats {
	/* RFC 4022 / Linux Tcp: */
	u32 ActiveOpens, PassiveOpens, AttemptFails, EstabResets, CurrEstab,
	    InSegs, OutSegs, RetransSegs, InErrs, OutRsts, InCsumErrors;
	/* Linux TcpExt: subset */
	u32 SyncookiesSent, SyncookiesRecv, SyncookiesFailed, ListenOverflows,
	    ListenDrops, TCPTimeouts, TCPLossProbes, TCPLossProbeRecovery,
	    TCPFastRetrans, TCPSlowStartRetrans, TCPSackRecovery, TCPRenoRecovery,
	    TCPSACKReorder, TCPDSACKRecv, TCPDSACKOldSent, TCPDSACKOfoSent,
	    TCPFullUndo, TCPSpuriousRTOs, TCPAbortOnData, TCPAbortOnClose,
	    TCPAbortOnTimeout, TCPAbortOnLinger, TCPChallengeACK, TCPSYNChallenge,
	    PAWSEstab, TCPOFOQueue, TCPOFODrop, TCPRcvQDrop, DelayedACKs,
	    TCPHPHits, TCPPureAcks, TCPHPAcks, TCPZeroWindowDrop, TCPWinProbe,
	    TCPKeepAlive, TCPRackLoss, TCPECNMarks, TimeWaitRecycled,
	    TCPMTUPFail, TCPMTUPSuccess;
	/* Riptide 0.15 (appended: older readers check the record size):
	 * Fast Open (Linux TCPFastOpenActive[Fail], TCPFastOpenCookieReqd),
	 * lost TFO SYNs, global TFO switch-offs, connects ended by the
	 * connect timeout / by ICMP unreachable, TCP_CORK ceiling flushes */
	u32 TCPFastOpenActive, TCPFastOpenActiveFail, TCPFastOpenCookieReqd,
	    TCPFastOpenSynLost, TCPFastOpenBlackhole, TCPConnectTimeouts,
	    TCPConnectUnreach, TCPCorkFlush;
	/* 0.15: socket memory limits (tcp_mem.c) */
	u32 TCPMemoryPressures;    /* times the use crossed net.inet.tcp.mem_pressure */
	u32 TCPMemoryPressuresMs;  /* time spent under pressure (ended periods) */
	u32 TCPMemLimitDrops;      /* segments dropped above net.inet.tcp.mem_high */
};

struct rt_igmp_stats {
	u32 InMsgs, InErrors, InQueriesV1, InQueriesV2, InQueriesV3, InBadQueries,
	    InReports, InOurReports, InLeaves, OutMsgs, OutReportsV1, OutReportsV2,
	    OutReportsV3, OutLeaves;
};

extern struct rt_ip_stats   rt_ipstat;
extern struct rt_icmp_stats rt_icmpstat;
extern struct rt_udp_stats  rt_udpstat;
extern struct rt_tcp_stats  rt_tcpstat;
extern struct rt_igmp_stats rt_igmpstat;

#endif
