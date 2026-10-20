# Riptide

Riptide is a new TCP/IP stack for AmigaOS 3.x (68020+). It is a drop-in
replacement for MiamiDx 1.0c: we reverse engineered MiamiDx's binary
interfaces and reimplemented them on a new protocol core whose TCP follows
the modern Linux TCP stack.

* **Compatible:** publishes `bsdsocket.library` 4 (AmiTCP API, 46 LVOs),
  `miami.library` 21 (Miami API, 83 vectors) and `usergroup.library` 4, all
  with the same LVO layouts as MiamiDx. These were verified against the
  MiamiDx binary and the official SDKs.
* **Modern TCP:** CUBIC with HyStart, and NewReno. SACK and D-SACK, RACK-TLP,
  PRR, F-RTO with DSACK/Eifel undo, timestamps with PAWS, window scaling and
  ECN. RTO per RFC 6298 with Linux's estimator, delayed ACK with quickack,
  and receive/send buffer auto-tuning. SYN cookies, RFC 5961 protections,
  RFC 6528 ISNs and RFC 6056 port selection.
* **Networking:** SANA-II devices (Ethernet or point-to-point), plus a
  direct loader for Miami's MNI drivers. Also loopback, ARP, ICMP, UDP, raw
  sockets, IPv4 fragmentation, path-MTU discovery, a DHCP client and a DNS
  resolver.
* **Tools:** Linux style `ip` (iproute2 syntax), `ifconfig`, `route`, `arp`,
  `netstat`, `traceroute`, `ping`, `host`, `sysctl`, `hostname`, `nc`, and
  a BSD style `ftp` client (passive mode through NAT, ftp:// URLs).
  Interfaces, routes, ARP entries, DNS servers and DHCP can be inspected
  and changed while the stack runs. Third-party tools can use the BSD
  routing/ARP ioctls, the `net.riptide.*` tables via `MiamiSysCtl()` and
  the `RIPTIDE` command port (see `docs/AMIGA.md`).

## Documentation

* `docs/RE-NOTES.md`: reverse-engineering notes on MiamiDx (library tables,
  MNI driver ABI, protocol function map, sysctls).
* `docs/DESIGN.md`: architecture and coding rules.
* `docs/AMIGA.md`: installing and configuring Riptide on an Amiga, the network tools, and the
  interfaces for tools (tables, ioctls, the `RIPTIDE` port).

## Building

```
make && ./tests/run_tests.sh build/host       # Linux host build + tests
make CROSS=m68k-linux-gnu-                     # big-endian 68020 build (run tests under qemu-m68k)
make -f Makefile.amiga                         # AmigaOS: build/amiga/{Riptide,rtcheck,ip,ping,...} (vbcc + NDK 3.2)
make -f Makefile.amiga check                   # hunk checks + vamos self test
tests/amiga/run_fsuae.sh build/amiga           # full-system test on FS-UAE + AROS ROM
tests/amiga/run_fsuae_tools.sh build/amiga     # every network tool on FS-UAE (loopback + uaenet.device)
tests/amiga/run_fsuae_ftp.sh build/amiga       # ftp client against the ftpd-test server over loopback
```

The Amiga build needs vbcc, vasm and vlink in `$VBCC` (default `/opt/vbcc`)
and the NDK 3.2 headers from Aminet in `/opt/ndk3.2`. See `docs/AMIGA.md`.

## Status

Tested on the Linux host (native, with ASan/UBSan, and big-endian under
qemu-m68k) against the real Linux kernel TCP over TUN. On AmigaOS it is
tested under vamos and in FS-UAE with the AROS Kickstart (loopback). Still to
verify: SANA-II receive on real hardware, and MNI drivers. Not implemented:
IPv6, multicast/IGMP, SSL, packet-filter hooks, and the MiamiDx-private
`miami.library` calls used by Miami's own NetStat, DiG and NSLookup.

Licence: GPL-2.0-only. MiamiDx is © Nordic Global Inc.; this repository
contains none of its code or SDK files.
