/* tests/t_ipp6_layout.c - the .IPP6 parameter blocks (sdk/mac/IPP6.h) against the offsets of the
 * specification (docs/IPP6.md, sections 4 to 8), and MacTCP's own blocks where .IPP6 keeps them
 * byte for byte. On a host the header lays the blocks out with 68k sizes (IPP6_LAYOUT_TEST: 32-bit
 * pointers and longs, 2-byte packing); built for the m68k (make CROSS=m68k-linux-gnu-, run under
 * qemu-m68k) it is checked as a 68k compiler sees it, with real pointers. The driver
 * (src/mac/ipp6.c) uses the same numbers; a field that moves breaks every program, so they are
 * written out here once more, by hand, from the document.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#if !defined(__m68k__) && !defined(__mc68000__)
#define IPP6_LAYOUT_TEST
#endif
#include "../sdk/mac/IPP6.h"

static int fails, checks;

static void at(const char *what, size_t got, size_t want)
{
	checks++;
	if (got != want) {
		printf("FAIL %s: offset %u, the specification says %u\n", what, (unsigned)got, (unsigned)want);
		fails++;
	}
}
#define OFF(type, field, want) at(#type "." #field, offsetof(type, field), want)
#define SIZE(type, want) at("sizeof " #type, sizeof(type), want)

int main(void)
{
	IP6Address a;

	/* section 4: the address */
	SIZE(IP6Address, 20);
	OFF(IP6Address, addr, 0);
	OFF(IP6Address, port, 16);
	OFF(IP6Address, scope, 18);

	/* section 5: the header */
	OFF(TCP6iopb, qLink, 0);
	OFF(TCP6iopb, qType, 4);
	OFF(TCP6iopb, ioTrap, 6);
	OFF(TCP6iopb, ioCmdAddr, 8);
	OFF(TCP6iopb, ioCompletion, 12);
	OFF(TCP6iopb, ioResult, 16);
	OFF(TCP6iopb, ioNamePtr, 18);
	OFF(TCP6iopb, ioVRefNum, 22);
	OFF(TCP6iopb, ioCRefNum, 24);
	OFF(TCP6iopb, csCode, 26);
	OFF(TCP6iopb, tcpStream, 28);
	OFF(TCP6iopb, csParam, 32);
	OFF(UDP6iopb, ioCompletion, 12);
	OFF(UDP6iopb, ioResult, 16);
	OFF(UDP6iopb, csCode, 26);
	OFF(UDP6iopb, udpStream, 28);
	OFF(UDP6iopb, csParam, 32);
	OFF(IP6iopb, ioCompletion, 12);
	OFF(IP6iopb, ioResult, 16);
	OFF(IP6iopb, ioCRefNum, 24);
	OFF(IP6iopb, csCode, 26);
	OFF(IP6iopb, csParam, 28);
	SIZE(TCP6iopb, 130);
	SIZE(UDP6iopb, 86);
	SIZE(IP6iopb, 80);

	/* section 6: TCP. Create, Send, Receive, Close, Abort, GlobalInfo are MacTCP's (TCPPB) */
	OFF(TCP6iopb, csParam.create.rcvBuff, 32);
	OFF(TCP6iopb, csParam.create.rcvBuffLen, 36);
	OFF(TCP6iopb, csParam.create.notifyProc, 40);
	OFF(TCP6iopb, csParam.create.userDataPtr, 44);
	OFF(TCP6iopb, csParam.open.ulpTimeoutValue, 32);
	OFF(TCP6iopb, csParam.open.ulpTimeoutAction, 33);
	OFF(TCP6iopb, csParam.open.validityFlags, 34);
	OFF(TCP6iopb, csParam.open.commandTimeoutValue, 35);
	OFF(TCP6iopb, csParam.open.remoteAddr, 36);
	OFF(TCP6iopb, csParam.open.remoteAddr.port, 52);
	OFF(TCP6iopb, csParam.open.remoteAddr.scope, 54);
	OFF(TCP6iopb, csParam.open.localAddr, 56);
	OFF(TCP6iopb, csParam.open.localAddr.port, 72);
	OFF(TCP6iopb, csParam.open.localAddr.scope, 74);
	OFF(TCP6iopb, csParam.open.tosFlags, 76);
	OFF(TCP6iopb, csParam.open.precedence, 77);
	OFF(TCP6iopb, csParam.open.dontFrag, 78);
	OFF(TCP6iopb, csParam.open.timeToLive, 79);
	OFF(TCP6iopb, csParam.open.security, 80);
	OFF(TCP6iopb, csParam.open.optionCnt, 81);
	OFF(TCP6iopb, csParam.open.options, 82);
	OFF(TCP6iopb, csParam.open.userDataPtr, 122);
	OFF(TCP6iopb, csParam.send.ulpTimeoutValue, 32);
	OFF(TCP6iopb, csParam.send.validityFlags, 34);
	OFF(TCP6iopb, csParam.send.pushFlag, 35);
	OFF(TCP6iopb, csParam.send.urgentFlag, 36);
	OFF(TCP6iopb, csParam.send.wdsPtr, 38);
	OFF(TCP6iopb, csParam.send.sendFree, 42);
	OFF(TCP6iopb, csParam.send.sendLength, 46);
	OFF(TCP6iopb, csParam.send.userDataPtr, 48);
	OFF(TCP6iopb, csParam.receive.commandTimeoutValue, 32);
	OFF(TCP6iopb, csParam.receive.markFlag, 34);
	OFF(TCP6iopb, csParam.receive.urgentFlag, 35);
	OFF(TCP6iopb, csParam.receive.rcvBuff, 36);
	OFF(TCP6iopb, csParam.receive.rcvBuffLen, 40);
	OFF(TCP6iopb, csParam.receive.rdsPtr, 42);
	OFF(TCP6iopb, csParam.receive.rdsLength, 46);
	OFF(TCP6iopb, csParam.receive.secondTimeStamp, 48);
	OFF(TCP6iopb, csParam.receive.userDataPtr, 50);
	OFF(TCP6iopb, csParam.close.ulpTimeoutValue, 32);
	OFF(TCP6iopb, csParam.close.validityFlags, 34);
	OFF(TCP6iopb, csParam.close.userDataPtr, 36);
	OFF(TCP6iopb, csParam.abort.userDataPtr, 32);
	OFF(TCP6iopb, csParam.status.ulpTimeoutValue, 32);
	OFF(TCP6iopb, csParam.status.ulpTimeoutAction, 33);
	OFF(TCP6iopb, csParam.status.unused, 34);
	OFF(TCP6iopb, csParam.status.remoteAddr, 38);
	OFF(TCP6iopb, csParam.status.remoteAddr.port, 54);
	OFF(TCP6iopb, csParam.status.localAddr, 58);
	OFF(TCP6iopb, csParam.status.localAddr.port, 74);
	OFF(TCP6iopb, csParam.status.tosFlags, 78);
	OFF(TCP6iopb, csParam.status.precedence, 79);
	OFF(TCP6iopb, csParam.status.connectionState, 80);
	OFF(TCP6iopb, csParam.status.sendWindow, 82);
	OFF(TCP6iopb, csParam.status.rcvWindow, 84);
	OFF(TCP6iopb, csParam.status.amtUnackedData, 86);
	OFF(TCP6iopb, csParam.status.amtUnreadData, 88);
	OFF(TCP6iopb, csParam.status.securityLevelPtr, 90);
	OFF(TCP6iopb, csParam.status.sendUnacked, 94);
	OFF(TCP6iopb, csParam.status.sendNext, 98);
	OFF(TCP6iopb, csParam.status.congestionWindow, 102);
	OFF(TCP6iopb, csParam.status.rcvNext, 106);
	OFF(TCP6iopb, csParam.status.srtt, 110);
	OFF(TCP6iopb, csParam.status.lastRTT, 114);
	OFF(TCP6iopb, csParam.status.sendMaxSegSize, 118);
	OFF(TCP6iopb, csParam.status.connStatPtr, 122);
	OFF(TCP6iopb, csParam.status.userDataPtr, 126);
	OFF(TCP6iopb, csParam.globalInfo.tcpParamPtr, 32);
	OFF(TCP6iopb, csParam.globalInfo.tcpStatsPtr, 36);
	OFF(TCP6iopb, csParam.globalInfo.tcpCDBTable, 40);
	OFF(TCP6iopb, csParam.globalInfo.userDataPtr, 44);
	OFF(TCP6iopb, csParam.globalInfo.maxTCPConnections, 48);

	/* section 7: UDP. Create is MacTCP's (UDPPB) */
	OFF(UDP6iopb, csParam.create.rcvBuff, 32);
	OFF(UDP6iopb, csParam.create.rcvBuffLen, 36);
	OFF(UDP6iopb, csParam.create.notifyProc, 40);
	OFF(UDP6iopb, csParam.create.localPort, 44);
	OFF(UDP6iopb, csParam.create.userDataPtr, 46);
	OFF(UDP6iopb, csParam.create.endingPort, 50);
	OFF(UDP6iopb, csParam.receive.timeOut, 32);
	OFF(UDP6iopb, csParam.receive.remoteAddr, 34);
	OFF(UDP6iopb, csParam.receive.remoteAddr.port, 50);
	OFF(UDP6iopb, csParam.receive.remoteAddr.scope, 52);
	OFF(UDP6iopb, csParam.receive.rcvBuff, 54);
	OFF(UDP6iopb, csParam.receive.rcvBuffLen, 58);
	OFF(UDP6iopb, csParam.receive.secondTimeStamp, 60);
	OFF(UDP6iopb, csParam.receive.userDataPtr, 62);
	OFF(UDP6iopb, csParam.receive.destAddr, 66);
	OFF(UDP6iopb, csParam.receive.destAddr.port, 82);
	OFF(UDP6iopb, csParam.receive.destAddr.scope, 84);
	OFF(UDP6iopb, csParam.send.reserved, 32);
	OFF(UDP6iopb, csParam.send.remoteAddr, 34);
	OFF(UDP6iopb, csParam.send.remoteAddr.port, 50);
	OFF(UDP6iopb, csParam.send.wdsPtr, 54);
	OFF(UDP6iopb, csParam.send.checkSum, 58);
	OFF(UDP6iopb, csParam.send.sendLength, 60);
	OFF(UDP6iopb, csParam.send.userDataPtr, 62);
	OFF(UDP6iopb, csParam.send.localPort, 66);
	OFF(UDP6iopb, csParam.mtu.mtuSize, 32);
	OFF(UDP6iopb, csParam.mtu.remoteAddr, 34);
	OFF(UDP6iopb, csParam.mtu.userDataPtr, 54);

	/* section 8: the interface's own calls */
	OFF(IP6iopb, csParam.info.version, 28);
	OFF(IP6iopb, csParam.info.features, 32);
	OFF(IP6iopb, csParam.info.maxTCPStreams, 36);
	OFF(IP6iopb, csParam.info.maxUDPStreams, 38);
	OFF(IP6iopb, csParam.info.maxLookups, 40);
	OFF(IP6iopb, csParam.info.order, 42);
	OFF(IP6iopb, csParam.info.implementation, 44);
	OFF(IP6iopb, csParam.info.userDataPtr, 48);
	OFF(IP6iopb, csParam.addrs.buffer, 28);
	OFF(IP6iopb, csParam.addrs.bufferLen, 32);
	OFF(IP6iopb, csParam.addrs.count, 36);
	OFF(IP6iopb, csParam.addrs.total, 38);
	OFF(IP6iopb, csParam.addrs.reserved, 40);
	OFF(IP6iopb, csParam.addrs.userDataPtr, 44);
	SIZE(IP6AddrInfo, 32);
	OFF(IP6AddrInfo, addr, 0);
	OFF(IP6AddrInfo, prefixLen, 20);
	OFF(IP6AddrInfo, flags, 21);
	OFF(IP6AddrInfo, ifIndex, 22);
	OFF(IP6AddrInfo, ifName, 24);
	OFF(IP6iopb, csParam.resolve.hostName, 28);
	OFF(IP6iopb, csParam.resolve.hostNameLen, 32);
	OFF(IP6iopb, csParam.resolve.flags, 34);
	OFF(IP6iopb, csParam.resolve.addr, 36);
	OFF(IP6iopb, csParam.resolve.results, 56);
	OFF(IP6iopb, csParam.resolve.maxResults, 60);
	OFF(IP6iopb, csParam.resolve.count, 62);
	OFF(IP6iopb, csParam.resolve.port, 64);
	OFF(IP6iopb, csParam.resolve.timeout, 66);
	OFF(IP6iopb, csParam.resolve.canonName, 68);
	OFF(IP6iopb, csParam.resolve.canonNameLen, 72);
	OFF(IP6iopb, csParam.resolve.reserved, 74);
	OFF(IP6iopb, csParam.resolve.userDataPtr, 76);
	OFF(IP6iopb, csParam.cancel.target, 28);
	OFF(IP6iopb, csParam.cancel.userDataPtr, 32);
	SIZE(ICMP6Report, 52);

	/* the codes the specification gives */
	at("csCode TCPActiveOpen", TCPActiveOpen, 32);
	at("csCode TCPGlobalInfo", TCPGlobalInfo, 43);
	at("csCode UDPMaxMTUSize", UDPMaxMTUSize, 25);
	at("csCode ip6GetInfo", ip6GetInfo, 70);
	at("csCode ip6StrToAddr", ip6StrToAddr, 76);
	at("ip6BadAddrErr", (size_t)-ip6BadAddrErr, 23060);
	at("ip6NoSourceErr", (size_t)-ip6NoSourceErr, 23065);
	at("gestaltIPP6", gestaltIPP6 == 0x69707036UL, 1);

	/* the IPv4-mapped helpers */
	memset(&a, 0x55, sizeof a);
	IP6_SET_V4(&a, 0xC0A84401UL);
	{
		static const unsigned char want[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 68, 1 };
		at("IP6_SET_V4", memcmp(a.addr, want, 16) == 0, 1);
	}
	at("IP6_IS_V4", IP6_IS_V4(&a) != 0, 1);
	at("IP6_GET_V4", IP6_GET_V4(&a) == 0xC0A84401UL, 1);
	a.addr[0] = 0xFE;
	a.addr[1] = 0x80;
	at("IP6_IS_V4 of fe80::", IP6_IS_V4(&a) != 0, 0);
	at("IP6_IS_LINKLOCAL", IP6_IS_LINKLOCAL(&a) != 0, 1);

	printf("t_ipp6_layout: %d checks, %d failed (%s pointers)\n", checks, fails,
#ifdef IPP6_LAYOUT_TEST
	       "68k-sized"
#else
	       "native m68k"
#endif
	);
	return fails ? 1 : 0;
}
