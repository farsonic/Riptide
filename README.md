# Riptide

Riptide is a new TCP/IP stack for AmigaOS 3.x (68020+). It is a drop-in
replacement for MiamiDx 1.0c: we reverse engineered MiamiDx's external
interface API and reimplemented them on a new protocol core whose TCP follows
the modern Linux TCP stack. This is simply providing an external experience 
of the interfaces used (bsdsocket.library etc) and not reusing any existing 
Amiga based code. 

Note: This is a hobby project and it is suggested you use a time tested product
if you want stability etc. 

<a href="https://buymeacoffee.com/farsonic" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" width="217" height="60"></a>

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
* **IPv6:** dual-stack host with SLAAC (RFC 7217 stable and RFC 8981
  temporary addresses), NDP, MLDv2, router advertisement routes, MTU and
  DNS (RDNSS/DNSSL), stateless DHCPv6, ICMPv6, fragments and path MTU;
  TCP, UDP and raw sockets over IPv6 with dual-stack `AF_INET6` sockets,
  `getaddrinfo()` with RFC 6724 ordering, AAAA and `ip6.arpa` lookups.
* **Tools:** Linux style `ip` (iproute2 syntax), `ifconfig`, `route`, `arp`,
  `netstat`, `traceroute`, `ping`, `host`, `sysctl`, `hostname`, `nc`, a
  BSD style `ftp` client (passive mode through NAT, ftp:// URLs) and
  `speedtest`, a speedtest.net bandwidth test with the options and output
  of speedtest-cli (an original C implementation).
  Interfaces, routes, ARP entries, DNS servers and DHCP can be inspected
  and changed while the stack runs. Third-party tools can use the BSD
  routing/ARP ioctls, the `net.riptide.*` tables via `MiamiSysCtl()` and
  the `RIPTIDE` command port (see `docs/AMIGA.md`).

## Installing

Unpack `riptide-<version>.lha` (or use the floppy image
`riptide-<version>.adf`) and double-click `Riptide/Install_Riptide`, the
standard Amiga Installer script. It copies Riptide and the tools to `C:`,
writes `ENVARC:Riptide/riptide.conf` for the emulator's `uaenet.device`
with DHCP (or another SANA-II device and a static address), adds Riptide to
`S:User-Startup` and can start it right away. It warns about other running
stacks and emulator bsdsocket emulation, and can update or uninstall.
`RiptidePrefs` (in `SYS:Prefs`), a GadTools settings editor, changes the
configuration later and can apply some settings to the running stack.
Under FS-UAE use `uae_sana2 = 1` and `network_card = a2065`. The manual
steps are in `docs/AMIGA.md`, "Installation"; `make -f Makefile.amiga
package` builds the package.

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
make -f Makefile.amiga package                 # Installer package: build/riptide-<version>.lha (.zip) and .adf
tests/amiga/run_fsuae.sh build/amiga           # full-system test on FS-UAE + AROS ROM
tests/amiga/run_fsuae_tools.sh build/amiga     # every network tool on FS-UAE (loopback + uaenet.device)
tests/amiga/run_fsuae_ftp.sh build/amiga       # ftp client against the ftpd-test server over loopback
tests/amiga/run_fsuae_speedtest.sh build/amiga # speedtest against httpd-test servers over loopback
python3 tools/speedtest_mock.py --port 8080    # mock speedtest.net + server: speedtest --mirror http://10.0.2.2:8080
tests/amiga/run_fsuae_bench.sh build/amiga     # Riptide BENCH: C vs 680x0 assembly checksum/copy
tests/amiga/test_installer.sh                  # Install_Riptide under InstallerLG (INSTALLERLG=)
tests/amiga/run_fsuae_installer.sh             # Install_Riptide with Installer 43.3 on FS-UAE (INSTALLER43=)
```

One binary serves the 68020 to 68060: Riptide picks hand-written
checksum and copy routines (MOVE16 on the 68040/68060) from the CPU type at
startup; see "CPU support" in `docs/AMIGA.md`.

The Amiga build needs vbcc, vasm and vlink in `$VBCC` (default `/opt/vbcc`)
and the NDK 3.2 headers from Aminet in `/opt/ndk3.2`. See `docs/AMIGA.md`.

## Status

Tested on the Linux host (native, with ASan/UBSan, and big-endian under
qemu-m68k) against the real Linux kernel TCP over TUN. On AmigaOS it is
tested under vamos and in FS-UAE with the AROS Kickstart (loopback). Still to
verify: SANA-II receive on real hardware, and MNI drivers. IPv6 (dual-stack
host: SLAAC, NDP, MLDv2, stateless DHCPv6) is tested against gVisor's
netstack; `t_tap` repeats it against a Linux kernel with IPv6 over TAP.
Not implemented: stateful DHCPv6, IPv4 multicast/IGMP, SSL, packet-filter
hooks, and the MiamiDx-private
`miami.library` calls used by Miami's own NetStat, DiG and NSLookup.

Licence: GPL-2.0-only. MiamiDx is © Nordic Global Inc.; this repository
contains none of its code or SDK files. `speedtest` follows the behaviour
of speedtest-cli by Matt Martz (Apache-2.0) but contains none of its code.
