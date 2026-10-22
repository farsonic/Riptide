# Riptide compared with MiamiDx and Roadshow

This document compares Riptide with the two best-known commercial TCP/IP
stacks for classic AmigaOS:

* **MiamiDx 1.0c** (Nordic Global, 2000), the stack whose
  binary interfaces Riptide reimplements. Facts about it come from our
  analysis of the binary, recorded in `docs/RE-NOTES.md` (RE-NOTES for
  short below), unless a source is given.
* **Roadshow** (Olaf Barthel, sold by APC&TCP), the actively maintained
  commercial stack for AmigaOS 2.04 to 3.9. AmigaOS 4 uses a port of it.
  Every Roadshow claim below cites a source. A claim we could not check
  against a primary source is marked **unverified**.
* **Riptide** facts come from this repository: its code (`src/core`,
  `src/amiga`), its tests and `docs/AMIGA.md`.

Roadshow sources used:

| Key | Source |
|---|---|
| [RS-README] | Roadshow product description, as shipped in a public mirror of the Roadshow SDK 1.5 (`$VER: Roadshow-SDK 1.5 (7.12.2019)`): https://ranger.innolan.net/rainlance/amiga-roadshow (README.md). The official site http://roadshow.apc-tcp.de/ returned HTTP 503 when we checked it. |
| [RS-DOC] | Roadshow `bsdsocket.library` autodoc from the same SDK: https://ranger.innolan.net/rainlance/amiga-roadshow/raw/branch/master/doc/bsdsocket.doc |
| [RS-SRC] | Source directory listing of the same SDK: https://ranger.innolan.net/rainlance/amiga-roadshow/src/branch/master/source_code and https://ranger.innolan.net/rainlance/amiga-roadshow/src/branch/master/source_code/Roadshow |
| [RS-1.13] | amiga-news.de, Roadshow 1.13 release (May 2017): https://amiga-news.de/en/news/AN-2017-05-00006-EN.html |
| [RS-1.15] | amiga-news.de, Roadshow 1.15 release (Feb 2024): https://amiga-news.de/en/news/AN-2024-02-00066-EN.html |
| [RS-2012] | amiga-news.de, Roadshow for AmigaOS 2/3 announced (Dec 2012): https://amiga-news.de/en/news/AN-2012-12-00034-EN.html |
| [RS-IPv6] | Hyperion forum thread "Is it road mapped or possible to update Roadshow to include IPv6?" (2016-2017): https://forum.hyperion-entertainment.com/viewtopic.php?p=38960 |

A note on the version we compare: the Roadshow autodoc is from SDK 1.5
(2019), and Roadshow 1.15 (2024) is newer. Features added after the 2019
SDK may be missing here.

## 1. Summary

| | MiamiDx 1.0c | Roadshow | Riptide |
|---|---|---|---|
| Status | Last release 2000; no longer sold | Commercial, maintained (1.15 in Feb 2024 [RS-1.15]) | Open source, in development (0.10) |
| Licence | Commercial shareware with registration key (RE-NOTES, intro) | Commercial; free 15-minute demo [RS-README] | GPL-2.0-only, full source |
| Kernel | 4.4BSD-Lite derived (RE-NOTES 5) | BSD derived, see 2.1 | New clean-room core; TCP modelled on Linux |
| TCP | Reno/NewReno, early SACK, IW 1, 500 ms ticks, T/TCP on | 4.4BSD-style tunables (RFC 1323 switches, `tcp.rttdflt`) [RS-DOC]; modern loss recovery not documented | CUBIC+HyStart, SACK/D-SACK, RACK-TLP, PRR, F-RTO, IW 10, 200 ms RTO min |
| IPv6 | No | Not documented (unverified, see 4) | Yes: dual-stack host (SLAAC, MLDv2, stateless DHCPv6, RDNSS; see 4) |
| Multicast | Yes (multicast routing tools in the package) | Yes [RS-README], [RS-DOC] `-ip-` | Host side: IGMPv3 (v2/v1 fallback) and MLDv2; no multicast routing |
| PPP | Built in (dialer, PPP, PPPoE/PPTP/L2TP) | PPP and PPPoE drivers included [RS-README] | No; needs a SANA-II PPP driver |
| Firewall/NAT | Yes (IPFW, IP-NAT) | "IP packet filter/network address translation (firewall)" [RS-README] | No |
| GUI | MUI/plain GUI, setup wizard | None; text configuration [RS-README] | None; text configuration |
| Drivers | SANA-II, MNI, serial | SANA-II (R3 with DMA) [RS-README] | SANA-II, MNI (direct loader) |
| Tested on real hardware | Yes, widely | Yes, widely | Not yet (emulators and host tests) |

The short version: Riptide's protocol engine is the most modern of the
three, and it is the only one with its source open and its behaviour
tested against a reference TCP (Linux). MiamiDx and Roadshow are complete
products with decades of real-world use and far more features around the
protocol engine: PPP dial-up, firewall, NAT, multicast, packet capture and
(MiamiDx) a GUI. Section 9 lists where Riptide is behind.

## 2. Lineage, licensing and availability

### 2.1 Codebase

* **MiamiDx.** A single 497 KB executable containing stack, dialer, PPP,
  drivers, inetd, NAT, firewall and GUI host (RE-NOTES 1). The kernel is
  4.4BSD-Lite derived: BSD mbufs, `tcpcb`, protosw tables, 4.4BSD sysctl
  MIBs, BSD slow/fast timers (RE-NOTES 5). Compiled with SAS/C for 68020.
* **Roadshow.** Implements the AmiTCP 4.0 API, "This implementation of
  "bsdsocket.library" follows the AmiTCP V4.0 API definition" [RS-DOC],
  extended with its own calls (section 5). Its tunables are 4.4BSD names
  (`tcp.do_rfc1323`, `tcp.rttdflt`, `tcp.mssdflt`, `ip.subnetsarelocal`,
  `if.minclustersize`, "clusters" of 2048 bytes) [RS-DOC]
  `ChangeRoadshowData`, and its SDK contains a `4.4BSD-Lite2` source
  directory [RS-SRC]. A BSD-derived kernel is therefore very likely, but
  the exact lineage is **unverified**; a forum poster says it is based on
  "an old IPStack of NetBSD" [RS-IPv6] (not an official statement).
  Barthel ported it to AmigaOS 4 first; the AmigaOS 2/3 version appeared
  later [RS-2012].
* **Riptide.** Written from scratch. MiamiDx's binary was analysed only
  for its interfaces (library vector tables, MNI driver ABI, structure
  layouts; RE-NOTES), and no code of it was used. The protocol core
  (`src/core`) is portable C99 without floating point. Its TCP follows the
  structure and algorithms of the modern Linux TCP stack (function and RFC
  names in the comments), which is why Riptide is GPL-2.0-only.

### 2.2 Licence and availability

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| Licence | Commercial; registration key (`Miami:MiamiDx.key`, RE-NOTES intro) | Commercial; download, CD or USB [RS-1.15] | GPL-2.0-only |
| Source | Closed | Closed stack; SDK includes source of the tools [RS-README] | Everything, including tests |
| Price | No longer sold | Not stated by a primary source we could read; amiga-news quoted an estimate of 20-25 EUR in 2012 [RS-2012] (current price **unverified**) | Free |
| Demo | — | Time limited to 15 minutes per session [RS-README] | — |

### 2.3 CPU and OS requirements

| | MiamiDx 1.0c | Roadshow | Riptide |
|---|---|---|---|
| CPU | 68020+ (RE-NOTES 7.10) | 68000 or better; optimised builds for 68020-68060 [RS-README], [RS-1.13] | 68020+ |
| OS | AmigaOS 3.x | AmigaOS 2.04 or higher; MorphOS "expected to work" [RS-README] | AmigaOS 3.x (tested with the AROS Kickstart replacement under FS-UAE) |
| RAM | — | 2 MB minimum [RS-README] | Not yet measured on hardware |
| FPU | No | No | No (integer-only core, also for CUBIC) |

## 3. TCP

### 3.1 Feature table

| Feature | MiamiDx 1.0c (RE-NOTES 5, 6, 7) | Roadshow | Riptide (defaults; sysctl in brackets) |
|---|---|---|---|
| Congestion control | 4.4BSD Reno + NewReno | Not documented [RS-DOC] (**unverified**) | CUBIC with HyStart; NewReno selectable (`net.inet.tcp.cc`) |
| Initial window | 1 segment (`initwin` 1) | Not documented (**unverified**) | 10 segments, RFC 6928 (`net.inet.tcp.initcwnd`) |
| SACK (RFC 2018) | Yes, early implementation, bitmask default 208 (SACK + NewReno + "modified fast retransmit") | No SACK switch among the documented tunables [RS-DOC] (**unverified**) | Yes, on (`net.inet.tcp.sack`) |
| D-SACK (RFC 2883) | No | **Unverified** | Yes, on (`dsack`) |
| RACK-TLP (RFC 8985) | No | **Unverified** | Yes: RACK loss detection and tail loss probes (`recovery`) |
| PRR (RFC 6937) | No | **Unverified** | Yes |
| F-RTO (RFC 5682) and undo | No | **Unverified** | F-RTO on (`frto` 2); Eifel/D-SACK undo of spurious recoveries |
| Timestamps / PAWS, window scaling (RFC 7323) | Implemented, `rfc1323` = 0 by default; enabled per interface by a heuristic | Implemented; `tcp.do_rfc1323`, `tcp.do_timestamps`, `tcp.do_win_scale` switches [RS-DOC]; defaults not documented | Both on (`timestamps`, `window_scaling`), with PAWS |
| ECN (RFC 3168) | No | **Unverified** | Accepted when the peer asks (`ecn` 2); requesting is `ecn` 1 |
| RTO estimator | 4.4BSD, 500 ms slow ticks, minimum about 1 s | `tcp.rttdflt` tunable [RS-DOC]; granularity **unverified** | RFC 6298 with Linux's estimator; RTT sampled in microseconds; RTO min 200 ms (`rto_min_ms`) |
| Timer granularity | 2 Hz slow timer, 5 Hz fast timer; in 4.4BSD-Lite the `tcpcb` keeps timers and RTT state as 16-bit tick counts (`t_timer[]`, `t_srtt`, `t_rttvar`); RE-NOTES does not record MiamiDx's exact layout | **Unverified** | Millisecond timers (`rt_now_ms()`), driven by timer.device |
| Delayed ACK | 200 ms fast timer | **Unverified** | Delayed ACK with quickack mode after start and loss, as Linux |
| Buffer sizing | Static 16 KB send/receive | `tcp.sendspace`/`tcp.recvspace` tunables [RS-DOC] | Receive auto-tuning (`moderate_rcvbuf`) up to `rmem_max` 256 KB; send buffer grows to `wmem_max` 256 KB |
| SYN flood protection | None; listen queue 7 | **Unverified** | SYN cookies (`syncookies` 1) |
| RFC 5961 (blind RST/SYN/ACK) | No | **Unverified** | Challenge ACKs with a rate limit |
| ISNs | Not analysed (4.4BSD-Lite uses a predictable increment) | **Unverified** | RFC 6528 (SipHash of the 4-tuple and a secret) |
| Ephemeral ports | 1024-5000 by default ("tiny and predictable", RE-NOTES 7.8) | **Unverified** | 49152-65535, RFC 6056 algorithm 4 (`net.inet.ip.portrange.*`) |
| TIME_WAIT | 4.4BSD 2*MSL (inferred, not analysed) | **Unverified** | Mini-sockets, capped (`max_tw_buckets` 1024), `tw_reuse` for loopback/connect as Linux; orphan limit (`max_orphans`) |
| Path MTU discovery | On | **Unverified** | On (RFC 1191), cached in `ip route show cache`, expires after `mtu_expires` 600 s |
| Keepalive | 2 h idle, 75 s interval | Standard `SO_KEEPALIVE` [RS-DOC]; times **unverified** | 7200 s idle, 75 s interval, 9 probes (`keepidle`, `keepintvl`, `keepcnt`) |
| T/TCP (RFC 1644) | Implemented, on by default (`rfc1644` 1) | Not documented | Not implemented, on purpose: it is obsolete (moved to Historic by RFC 6247) and lets a spoofed SYN execute a transaction |
| Socket options | BSD | `TCP_NODELAY` only: "TCP supports one socket option" [RS-DOC] | `TCP_NODELAY`, `TCP_MAXSEG` and Linux-style knobs (`rt_bsd.h`) |

### 3.2 What the modernisation does in practice

The numbers below are from Riptide's own test suite on the Linux host
build (`tests/t_tcp_wire.c`: two Riptide stacks over a simulated wire with
seeded loss, reordering, delay and rate limits; `tests/t_tcp_tun.c`:
Riptide against the Linux kernel over TUN with `netem`). They were
measured on a fast x86 host, **not on a 68020**: they show how the
algorithms behave, not what an Amiga achieves, where CPU and the network
driver usually limit throughput long before TCP does. Results vary
slightly between runs (seeded but timing dependent).

* **Loss recovery without timeouts.** With 1% random loss, a 20 MB
  transfer ran at 193 Mbit/s against 210 Mbit/s without loss, and needed
  **0 retransmission timeouts**: all 127 losses were found by RACK and
  repaired by fast retransmit, 14 tail losses by TLP probes. (An earlier
  run measured 167 Mbit/s; same result, 0 RTOs.) A BSD stack of MiamiDx's
  generation repairs single losses with fast retransmit too, but a lost
  retransmission or a loss at the end of a burst falls back to a timeout of
  at least about 1 s (500 ms ticks), and the window then restarts at 1
  segment.
* **Heavy loss.** At 5% loss with SACK and RACK the transfer needed 18
  RTOs for 740 retransmissions. With SACK switched off (NewReno only, the
  algorithm closest to a BSD stack) the same loss rate in the other
  direction needed 39 RTOs for 278 retransmissions, and throughput fell to
  about a tenth. Each RTO costs at least 200 ms in Riptide, at least about
  1 s in MiamiDx.
* **Reordering.** With 5% reordering and 1% duplication D-SACK detected 852
  spurious retransmissions and undid 23 window reductions, so the
  connection kept its rate instead of halving it again and again.
* **Start-up on long paths.** With an initial window of 10 segments the
  first round trip carries about 14 KB instead of one 512-1460 byte
  segment. On a 50 ms (or a 300 ms satellite or modem) path, slow start
  from IW1 needs about 3-4 more round trips to reach the same window, which
  dominates short transfers such as web pages and mail.
* **RTO granularity.** Riptide's RTO follows the measured RTT (RFC 6298)
  down to a 200 ms floor, with millisecond timers. MiamiDx's 2 Hz slow
  timer makes the smallest RTO about 1 s and RTT samples are counted in
  500 ms ticks, so on a LAN every timeout is 5-10 times longer than
  needed.
* **Interoperability.** Against the Linux kernel over TUN, transfers in
  both directions, ECN negotiation, an MTU of 576 and 1-2% loss with
  reordering all completed without timeouts.
* **Window size.** MiamiDx's fixed 16 KB windows limit a single
  connection to 16 KB per round trip (about 1.3 Mbit/s at 100 ms), no
  matter how fast the link is. Riptide scales the receive window up to
  256 KB as the connection needs it.

## 4. IP, UDP and ICMP

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| IPv6 | No (`MiamiSupportsIPV6()` returns 0) | Not mentioned in the product description [RS-README] or the autodoc's protocol sections [RS-DOC]; users asking for it in 2016-2017 got no roadmap [RS-IPv6]. No IPv6 as far as we can tell (**unverified**) | Yes, host side: SLAAC with RFC 7217 stable and RFC 8981 temporary addresses, DAD, NDP (RFC 4861 states), MLDv2, RA routes/MTU/RDNSS/DNSSL, stateless DHCPv6, RFC 6724 address selection, PMTU, fragments (RFC 5722/7112); dual-stack sockets (`IPV6_V6ONLY` 0 by default), KAME-numbered API, `getaddrinfo()` with AAAA. No stateful DHCPv6, no ancillary data |
| Multicast / IGMP | Yes; multicast routing tools (mrouted, mtrace) in the package | Yes; `IP_ADD_MEMBERSHIP` etc. documented [RS-DOC], "Multicast support" [RS-README] | Host side. IPv4: IGMPv3 (RFC 3376, exclude mode without source lists) falling back to IGMPv2/v1 queriers, `IP_ADD_MEMBERSHIP`/`IP_DROP_MEMBERSHIP` (BSD per-socket delivery), `IP_MULTICAST_IF`/`TTL`/`LOOP`; no source-specific options. IPv6: MLDv2 and `IPV6_JOIN_GROUP`. No multicast routing |
| Fragment reassembly | Not analysed | **Unverified** | 30 s timeout, at most 64 queues, 64 fragments each, 256 KB memory high-water mark |
| ICMP rate limiting | Not analysed | Echo and timestamp replies can be switched off (`icmp.processecho`, `icmp.procesststamp`) [RS-DOC] | Linux model: global token bucket plus per-destination limit (`net.inet.icmp.ratelimit`, `msgs_per_sec`, `msgs_burst`) |
| Raw sockets | Yes | Yes, with `IP_HDRINCL` [RS-DOC] | Yes (ICMP, `IP_HDRINCL`) |
| Forwarding / router | Yes (`ip.forwarding` 1 by default) | `ip.forwarding` tunable [RS-DOC] | IPv4 no; IPv6 basic forwarding (`net.inet6.ip6.forwarding`, off), no router advertisements |
| ZeroConf (169.254/16) | Not analysed | Yes, as DHCP fallback [RS-README] | No |

## 5. Application interfaces

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| bsdsocket.library | AmiTCP 4 (46 LVOs) | AmiTCP 4.0 API [RS-DOC] plus many extensions (below) | AmiTCP 4, all 46 functions |
| Extended API | miami.library 21: sysctl, getaddrinfo & co., fd callbacks, packet filter hooks, SSL | In bsdsocket.library itself [RS-DOC]: routing (`AddRouteTagList`, `DeleteRouteTagList`, `ChangeRouteTagList`, `GetRouteInfo`), interfaces (`AddInterfaceTagList`, `ConfigureInterfaceTagList`, `QueryInterfaceTagList`, `ObtainInterfaceList`, `RemoveInterface`), DNS (`AddDomainNameServer`, `ObtainDomainNameServerList`, `SetDefaultDomainName`...), monitoring (`AddNetMonitorHookTagList`, `GetNetworkStatistics`), BPF (`bpf_open`...), mbuf access, `ObtainRoadshowData`/`ChangeRoadshowData`, server sockets, `gethostbyname_r`, `getaddrinfo`/`getnameinfo` | `riptide.library` 1 (the MiamiDx API under Riptide names) plus a compat alias under the old name; `net.riptide.*` tables via `RiptideSysCtl()`; the RIPTIDE command port; BSD routing and ARP ioctls |
| usergroup.library | Yes | **Unverified** | Yes, single user |

Riptide does **not** implement any of Roadshow's bsdsocket.library
extensions. Programs that call `AddRouteTagList()`, `ObtainInterfaceList()`,
`bpf_open()`, `ObtainRoadshowData()` and the like get no such vectors
(Riptide's bsdsocket.library ends at LVO -300, `GetSocketEvents()`). Tools
written for Roadshow's API therefore do not work with Riptide; tools written
for the plain AmiTCP API or for MiamiDx's documented API do.

## 6. Drivers and configuration

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| SANA-II | Yes | Yes, SANA-II R3 including DMA access [RS-README] | Yes, with buffer-management callbacks; cooked or raw Ethernet; point-to-point devices |
| MNI (MiamiDx native drivers) | Yes | No | Yes, direct loader with zero-copy receive |
| Serial (SLIP/PPP built in) | Yes | PPP via its `ppp-serial.device`, PPPoE via `ppp-ethernet.device` [RS-1.13], [RS-README] | No |
| Configuration | GUI + settings database (IFF), setup wizard | Text files in `DEVS:` subdirectories, "mount file" style driver configuration, tracked by file notification; no GUI [RS-README] | One text file (`ENV:Riptide/riptide.conf`); every line can also be sent to the running stack (RIPTIDE port) |
| Run-time changes | GUI | Shell commands (`AddNetInterface`, `ConfigureNetInterface`, `AddNetRoute`, `RoadshowControl`, ...) [RS-SRC] | `ip`, `ifconfig`, `route`, `arp`, `sysctl`, `hostname` |

## 7. DHCP, DNS and tools

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| DHCP client | Yes | Yes [RS-README] | Yes (lease renewal, rebinding, static routes win over DHCP ones) |
| Resolver | BIND 4.9-era, DNS cache (32 entries) (RE-NOTES 6, 7.9) | "Built-in DNS cache", IDN (ISO Latin 1 to Punycode) [RS-README] | Own resolver with a 32-entry cache, search domain, static host table, PTR lookups; no IDN, no DNSSEC |
| inetd | Yes (built in) | Yes, "inetd style Internet superserver" [RS-README] | No |
| TCP: handler | Not analysed | Yes [RS-README] | No |
| Packet capture | `MiamiTCPDump` (miamibpf/miamipcap) | `tcpdump`, BPF [RS-README] | No |
| Tools | `Miami*` tools: ping, traceroute, netstat (kvm based), route, arp, ifconfig, sysctl, host, DiG, NSLookup, ftp, telnet, finger, IPFW, IPNatD... | Configuration and status commands (`ShowNetStatus`, `GetNetStatus`, `NetShutdown`, `Online`, `Offline`, `SampleNetSpeed`...) [RS-SRC], plus `ping`, `traceroute`, `wget`, `ftp`, `tcpdump` [RS-1.13], [RS-1.15] | Linux-style `ip` (iproute2), `ifconfig`, `route`, `arp`, `netstat`, `traceroute`, `ping`, `host`, `sysctl`, `hostname`, `nc`, `ftp`; no wget, telnet or dig |

## 8. Security posture

* **MiamiDx** (RE-NOTES 7): T/TCP on by default, no SYN cookies and a
  listen queue of 7, sequential ephemeral ports 1024-5000, 4.4BSD ISNs, a
  BIND 4.9-era resolver with query-ID randomisation not verified, and an
  internal inetd with diagnostic services. It does have a firewall and NAT.
* **Roadshow** ships a packet filter/NAT firewall [RS-README], which
  Riptide does not have. Its TCP hardening (ISNs, port randomisation, SYN
  flood handling, RFC 5961) is not documented: **unverified**.
* **Riptide** implements the current host-side TCP defences: RFC 6528
  ISNs and RFC 6056 port selection with a SipHash secret, SYN cookies,
  RFC 5961 challenge ACKs with rate limiting, PAWS, ICMP rate limiting,
  bounded reassembly and TIME_WAIT memory, and no T/TCP. Fuzz tests feed
  thousands of random and partly in-window segments and random fragments
  into the stack (`tests/t_tcp_unit.c`, `tests/t_infra_unit.c`). It has no
  firewall, so an Amiga running Riptide should sit behind a router.
  `usergroup.library`'s `crypt()` is a placeholder, not DES.

## 9. Known gaps: where MiamiDx or Roadshow are ahead

Riptide is younger and narrower. Be aware of these before choosing it:

* **Maturity on real hardware.** MiamiDx and Roadshow have run on real
  Amigas for decades. Riptide has been tested on Linux (native, ASan/UBSan,
  big-endian under qemu-m68k) and on AmigaOS only in emulation (vamos,
  FS-UAE with the AROS Kickstart). SANA-II receive on real hardware and MNI
  drivers are still to be verified.
* **Performance on real hardware** is not measured yet. Roadshow quotes
  997 KB/s receive on an A3000T with a 40 MHz 68040 [RS-README]. Riptide
  has emulator figures only (section 11), which are not comparable with
  that: the emulated network card costs the CPU almost nothing. Its more
  complex TCP (SACK scoreboard, RACK, CUBIC) costs CPU that a slow Amiga
  may notice.
* **PPP and dial-up.** MiamiDx's main use case was modem/ISDN dial-up with
  its built-in PPP, PPPoE, PPTP and L2TP. Roadshow includes PPP and PPPoE
  drivers. Riptide has no PPP of its own; it relies on a SANA-II PPP
  driver configured as a point-to-point interface.
* **No multicast routing** (MiamiDx ships mrouted and friends); IPv4
  multicast is host side only (IGMP, no source filters). IPv6, which
  neither of the others has, is there, but only host-side: no stateful
  DHCPv6, no RFC 3542 ancillary data, and SANA-II drivers need a
  multicast filter.
* **No firewall, NAT or packet filter hooks**, no BPF/tcpdump, no SOCKS,
  no SSL (`RiptideOpenSSL()` returns NULL), no inetd, no `TCP:` handler,
  no ZeroConf, no IDN.
* **No GUI** (MiamiDx has one; Roadshow has none either).
* **Roadshow's API extensions** (routing, interface, DNS, monitoring, BPF,
  `RoadshowData`) are not implemented, so Roadshow-specific tools do not
  run.
* **MiamiDx's private vectors** (resolver internals, kvm access, settings
  database) return 0, so MiamiDx's own NetStat, DiG and NSLookup do not
  work with Riptide's compat alias.
* **Single IPv4 address per interface**, single usergroup user, no
  forwarding/router mode.

## 10. Testing methodology

| | MiamiDx | Roadshow | Riptide |
|---|---|---|---|
| Public test suite | None known | None known (SDK has example sources [RS-1.15]) | Yes, in the repository |

Riptide is tested in layers, all automated:

* **Host interop.** The portable core runs on Linux and talks to the
  **Linux kernel's TCP over TUN**: connects in both directions, bulk
  transfers, ECN, small MTUs, `netem` loss and reordering, DNS, ICMP, UDP,
  DHCP (17 test programs, `tests/run_tests.sh build/host`).
* **Simulated wire.** Two Riptide instances over a wire with seeded loss,
  reordering, duplication, delay and rate limits, byte-exact verification,
  and counters for every recovery mechanism (`tests/t_tcp_wire.c`).
* **Big-endian 68020.** The same tests built for m68k and run under
  `qemu-m68k`, so byte order and 68020 code generation are covered.
* **Sanitisers and fuzzing.** `make SAN=1` (ASan/UBSan); fuzzers for TCP
  segments, ICMP errors and IP fragments.
* **AmigaOS.** Layout checks of every structure, constant and library
  vector against the NDK 3.2, the AmiTCP SDK and the historical API's SDK;
  hunk/relocation checks; a library self test under vamos; and full-system
  tests in FS-UAE with the AROS Kickstart: about 90 live library checks
  (including the compat alias), every network tool, and a client built
  from the developer SDK alone.

What is missing is the most important layer for users: long-term use on
real Amiga hardware with real SANA-II and MNI drivers.

## 11. Measured throughput (emulated)

`tests/amiga/run_fsuae_netbench.sh` (the speed lab) measures TCP bulk
throughput on FS-UAE. The emulated Amiga runs `tests/amiga/netbench`, which
uses only bsdsocket.library version 3 calls, so the same binary runs on any
stack; it throws received data away and generates sent data, so no disk is
in the path. The other end is `tests/amiga/netbench_host.py` on Linux. The
two are connected by FS-UAE's uaenet.device bridged with libpcap
(`tools/fs-uae`) to a veth pair, with checksum and segmentation offloads
off: no slirp and no NAT in between.

Per CPU preset, one boot runs a 64 KB warm-up, three 2 MB downloads and
three 2 MB uploads (the table shows the medians), one more transfer each
way under `Riptide PROFILE`, and five pings. Times are emulated time, from
the request to the last byte (download) or to the host's confirmation that
it has every byte (upload).

| CPU preset | FS-UAE | Download KB/s | Upload KB/s | Ping ms |
|---|---|---|---|---|
| a1200-020 | A1200, 68020 14 MHz, 8 MB Fast RAM | 447.9 | 269.4 | 7.1 |
| a1200-030 | A1200, 68030 25 MHz, 8 MB Fast RAM | 791.3 | 459.2 | 3.5 |
| a4000-040 | A4000, 68040 25 MHz, 16 MB Zorro III RAM | 1574.2 | 900.6 | 5.7 |
| a4000-060 | A4000, 68060 50 MHz, 16 MB Zorro III RAM | 2760.1 | 1539.8 | 7.7 |

Riptide 0.12 (main d814867), 22 October 2026; KB = 1024 bytes. The three
downloads spread by 1 % on the 68020 and 68030, 2.5 % on the 68040 and
13 % on the 68060; the uploads by 5 % or less on every CPU. The 68060
download is the least stable cell and the most likely to fail the
baseline check by chance. `tests/amiga/netbench-baseline.json`
holds every run; a speed lab run with
`BASELINE=tests/amiga/netbench-baseline.json` fails when a median is more
than 10 % below it.

**How far to trust these numbers.**

* They are emulator numbers. All four presets use FS-UAE's cycle-exact CPU
  timing without JIT, so they do not depend on the speed of the host and
  repeat well. The 68020 timing is close to a real A1200. For the 68040
  and 68060, UAE approximates the caches and pipelines, so treat those
  rows as a rough guide.
* The network card is the emulator's uaenet.device. Moving a frame
  through it costs the Amiga one copy and almost no bus time. A real
  Zorro or PCMCIA Ethernet card costs much more: PIO through a slow bus,
  interrupts, driver code. On real hardware the figures will be lower,
  probably much lower on an A1200 with a PCMCIA card. They are not
  comparable with Roadshow's 997 KB/s on a real A3000T [RS-README].
* The upload is slower than the download on every CPU. The profile says
  why: on the 68020, sending spends most of its time in TCP output and
  waiting for the stack lock, while receiving is dominated by TCP input,
  checksums and copies. That is where Phase 2's optimisation work starts.
* MMU emulation is off. With FS-UAE's 68030 or 68060 MMU emulated, Riptide
  hangs the machine at the first transfer over uaenet.device. The cause
  is not yet known; it may be the emulator, the stack, or both.

**Competitors.** We tried to run the same benchmark with other stacks on
the same emulated machines:

* **AmiTCP 3.0b2** (Aminet `comm/net/AmiTCP-bin-30b2.lha`, GPL-2.0) does not
  run on the AROS ROM that FS-UAE includes. It needs `rexxsyslib.library`,
  which the AROS ROM lacks (`netbench -R` adds a stub), and a few
  configuration fixes. It then starts, but the emulated machine crashes and
  reboots as soon as any program, AmiTCP's own `ifconfig` included, opens
  its bsdsocket.library. AmiTCP needs a real Kickstart ROM. Kickstart is
  copyrighted and not part of this repository; with one
  (`KICKSTART=kick.rom AMITCP_DIR=... run_fsuae_netbench.sh`), the script
  runs AmiTCP the same way. We have not run that combination. The script
  never ships AmiTCP. It copies the files from `AMITCP_DIR` into the test
  disk at run time.
* **Roadshow**: the demo runs for 15 minutes ([RS-README]; long enough for a
  run). It needs AmigaOS 3.x, which means a real Kickstart and Workbench
  that we cannot use here. The official site returned HTTP 503 when we
  checked, so we could not read the demo's licence terms. The script has
  no Roadshow support yet.
* **Miami** (the evaluation version, Aminet `comm/tcp/Miami21feval.lha`)
  needs Kickstart 2.04 or later and MUI, and is set up through its GUI.
  It is not suited to an unattended run without a real Kickstart and
  Workbench, so we have not tried it.

Until a stack runs under the same conditions, this section compares
Riptide only with itself: the baseline exists to catch regressions and to
measure the Phase 2 optimisations.
