# Riptide on AmigaOS

Riptide is a TCP/IP stack for AmigaOS 3.x on a 68020 or better. It is a
drop-in replacement for a historical commercial Amiga stack (see
`docs/RE-NOTES.md`) and keeps that stack's binary interfaces. It publishes
these libraries:

| Library | Version | API |
|---|---|---|
| `bsdsocket.library` | 4.1 | AmiTCP 4: all 46 functions, `socket()` through `GetSocketEvents()` |
| `riptide.library` | 3.0 | Riptide's own API: `RiptideSysCtl()`, interface state, `getaddrinfo()` & co., `if_*`; 53 documented vectors, reserved vectors up to LVO -522 return 0; then `RiptideConnectName()` (version 2) and `RiptideSendMMsg()`/`RiptideRecvMMsg()` (version 3). See "riptide.library" below. |
| compat alias of `riptide.library` | 21.1 | The same library under the name and version of the historical API it implements, `miami.library` 21, so existing programs keep working. `compat aliases off` removes it. |
| `usergroup.library` | 4.1 | AmiTCP usergroup, 39 functions. Single user only. |

`docs/COMPARISON.md` compares Riptide with that historical stack and with
Roadshow, feature by feature.

**Memory.** The `Riptide` program is about 450 KB. On top of that, each
Ethernet interface keeps 76 receive buffers queued with the driver (48 IPv4,
see `reads=`, plus 24 IPv6 and 4 ARP) of about 1.7 KB each, roughly 130 KB,
and every TCP connection may buffer up to 64 KB in each direction. Fast RAM
is strongly recommended; on a Chip-RAM-only machine little is left for other
programs (the automated FS-UAE tests run with 8 MB Fast RAM).

Riptide runs as one ordinary CLI process. That process owns the timers and
the network drivers. Socket calls run in the calling program's own task,
under one exec semaphore (the stack lock).

## Installation

### With the Installer

The release archive `riptide-<version>.lha` (also as an 880 KB floppy
image, `riptide-<version>.adf`) unpacks to a drawer `Riptide` with a
standard Amiga Installer script. Double-click `Install_Riptide`, or from a
shell:

```
cd Riptide
Installer Install_Riptide
```

It needs the Installer of AmigaOS 3.x (Installer 43.x, in `C:` or
`SYS:Utilities`; the icon's default tool is `Installer`) or InstallerLG.
The icon's tooltypes are `APPNAME=Riptide`, `MINUSER=NOVICE` and
`DEFUSER=AVERAGE`. At Novice level it asks no questions and uses the
defaults below; Average asks which parts to install and how to configure
the network; Expert can also choose the program drawer. Pretend mode
("Pretend to Install") shows what would happen without changing anything.

The script does what the manual steps below describe:

1. **Checks for other stacks.** If Riptide is running, it offers to stop it
   (`Riptide QUIT`). If another `bsdsocket.library` is in memory (another
   stack, or the emulator's bsdsocket emulation) it explains how to turn
   it off and does not start Riptide. Another stack's
   `LIBS:bsdsocket.library` is renamed to `bsdsocket.library.disabled` if
   you agree; it is never deleted.
2. **Copies the programs** `Riptide`, `rtcheck` and the network tools to
   `C:`. With another drawer (Expert), a `Path ... ADD` line is added to the
   startup block. `ftpd-test` is only installed when you select it.
3. **Copies the documentation** to `Riptide/Docs` in the Installer's default
   place (`Work:` if you have it, else `SYS:`) and, if selected, the SDK
   to `Riptide/SDK`. [RiptidePrefs](#riptideprefs), the settings editor,
   goes to `SYS:Prefs` (selected by default).
4. **Writes the configuration** `ENVARC:Riptide/riptide.conf` and copies it
   to `ENV:Riptide/`. The default is the emulator's `uaenet.device`, unit
   0, with DHCP, as in `examples/riptide-uaenet.conf` (the static slirp
   block is there, commented out). At Average level you can enter the host
   name, another SANA-II device (for example
   `DEVS:Networks/a2065.device`) and unit, and a static address, netmask,
   gateway and DNS server. An existing configuration is kept at Novice
   level; otherwise you are asked, and the old file is saved as
   `riptide.conf.bak`. `ENVARC:Riptide/hosts` gets a `localhost` entry if it
   does not exist.
5. **Starts Riptide at boot** with a block in `S:User-Startup`:

   ```
   ;BEGIN Riptide
   Run >NIL: C:Riptide
   ;END Riptide
   ```

   and, if you agree, starts it at once (through `rtcheck START=...
   NOCHECK`, which waits until the libraries are there).

The install locations are kept in `ENVARC:Riptide/BinDir`, `DocDir`,
`SDKDir` and `PrefsDir`. Run the installer again to update Riptide (your configuration is
kept unless you replace it) or to uninstall it: uninstalling deletes the
programs, the documentation and the SDK, empties the `;BEGIN Riptide`
block (the Installer cannot delete a block, only replace it) and deletes
the configuration only when you ask for it.

Under FS-UAE, `uaenet.device` needs `uae_sana2 = 1` and `network_card =
a2065`; in WinUAE add uaenet.device under Hardware > Expansions. Turn the
emulator's bsdsocket.library emulation off in both. Then check the stack
with `Riptide STATUS`, `rtcheck -v` and, under slirp, `ping -c 3 10.0.2.2`.

### By hand

1. Copy `Riptide` to `C:` or anywhere else in your path (and the tools in
   `C/` too, see "Tools").
2. Create the configuration file `ENV:Riptide/riptide.conf` (keep a copy in
   `ENVARC:Riptide/`). Riptide also reads `ENV:Riptide/hosts` if that file
   exists.
3. Start the stack from `S:User-Startup` or from a shell:

   ```
   Run >NIL: Riptide
   ```

   To see the log, redirect it to a file or a console window instead of
   `NIL:`, for example `Run >CON:0/0/640/100/Riptide Riptide`.

Riptide refuses to start if `bsdsocket.library`, `riptide.library`, the
compat alias library or `usergroup.library` is already in the system
library list. It then prints "A TCP/IP stack is already running:" followed
by the id string of each library it found, so you can tell which stack
owns them. Quit the other stack (or the other Riptide) first. Also turn off any emulator bsdsocket emulation,
and remove another stack's `LIBS:bsdsocket.library`, which loads by itself
when a program opens it.

You do not need SetPatch tricks or `LIBS:` files: the libraries exist only
while Riptide runs.

## Command line

```
Riptide [CONFIG=file] [DEBUG=level] [QUIT] [STATUS] [SELFTEST] [BENCH [TX [BYTES]]] [HELP]
Riptide CAPTURE start|stop|status|save FILE|clear [OPTIONS]
Riptide PROFILE START|STOP|RESET|SHOW
Riptide DEBUG CATEGORIES|NONE|STATUS|SHOW [N]|CLEAR|DUMP FILE [SIZE=KB]
Riptide NTP [STATUS|ON|OFF|SYNC|SERVER...|INTERVAL S|STEP MS|BATTCLOCK ON|OFF]
Riptide MDNS [ON|OFF|STATUS] | TIMEZONE [+HH:MM|AUTO]
Riptide DNS [STATUS|EDNS_SIZE N|EDNS ON|OFF|TCP ON|OFF|NEGTTL s|FAILTTL s|FLUSH|RESET]
Riptide BACKGROUND [CLEAR] [program=NAME,...] [port=P,...] [lport=P,...] [mode=...] [target=MS]
```

| Argument | Meaning |
|---|---|
| `CONFIG=file` | Use this configuration file instead of `ENV:Riptide/riptide.conf`. |
| `DEBUG=n` | Log level: 0 = errors, 1 = warnings (the default), 2 = info, 3 = debug. |
| `STATUS` | Ask the running Riptide for a summary: interfaces, addresses, packet counters, driver and DHCP state, NTP and mDNS state, socket buffer memory and background rules, the routing table, DNS servers and library users. |
| `QUIT` | Ask the running Riptide to quit. It refuses while any program still has one of its libraries open. |
| `SELFTEST` | Run the built-in library self test in one task, without starting the stack. It is used under the vamos emulator. |
| `BENCH` | Time the C and assembly checksum and copy routines (see "CPU support") and what one call of the OS services the stack uses per packet costs, and exit. Needs timer.device, so not under vamos. |
| `BENCH TX [BYTES]` | Time the TCP send path without a network or a second task: an upload (default 2 MB, 16 KB `send()`s) to a scripted receiver behind a fake interface that ACKs every second segment, printing µs per segment and the KB/s that leaves, then half of it again under the profiler (`src/core/txbench.c`). |
| `CAPTURE ...` | Packet capture in the running stack; the rest of the line is the command (see "Packet capture"). |
| `PROFILE ...` | Control the running stack's profiler (see "Profiling and debug log"). |
| `DEBUG ...` | With a word instead of a number: control the running stack's debug log (see "Profiling and debug log"). A number is still the log level of a new instance. |
| `NTP ...`, `MDNS ...`, `TIMEZONE ...` | The `ntp`, `mdns` and `timezone` keywords in the running stack, e.g. `Riptide NTP STATUS`, `Riptide NTP SYNC` (see "Time and .local names"). |
| `DNS ...` | The `dns` keyword in the running stack: EDNS0 size, TCP fallback and the cache of the resolver, e.g. `Riptide DNS`, `Riptide DNS EDNS_SIZE 0`, `Riptide DNS FLUSH` (see "Resolver: EDNS0 and TCP"). |
| `BACKGROUND ...` | The `background` keyword in the running stack: which connections yield to others (see "Background transfers"). Without arguments it shows the rules. |

`QUIT`, `STATUS`, `CAPTURE`, `PROFILE`, `DEBUG`, `NTP`, `MDNS`, `DNS`, `TIMEZONE` and `BACKGROUND` talk to the running
stack through the public message port `RIPTIDE`. You can also signal the
Riptide process directly:

* **Ctrl-C** works like `QUIT`. If programs still have the libraries open,
  Riptide sends each of them Ctrl-C, as the historical stack does. It then quits once
  the last one has closed its library.
* **Ctrl-F** prints the status to Riptide's output.

## Configuration file

The file is line oriented. `#` starts a comment and words can be quoted.

### Generic keywords

These are handled by the protocol core. The same lines can be sent to the
running stack at any time (see "The RIPTIDE port" below), which is what
the `ip`, `route`, `sysctl` and `hostname` tools do.

```
hostname NAME
domain NAME                        # also the DNS search domain
search DOMAIN [DOMAIN...]          # the resolver's search list (up to six); "search" alone clears it
nameserver [add] A.B.C.D           # up to the core's limit, in order
nameserver del A.B.C.D | nameserver flush
dns edns_size N | edns on|off | tcp on|off | negttl N | failttl N | flush | reset | status
                                   # resolver, see "Resolver: EDNS0 and TCP"
host NAME A.B.C.D                  # static host table entry
route add|del|replace|change DEST[/BITS]|default [netmask M] [via|gw] GATEWAY
      [dev IF] [metric N] [mtu N] [unreachable]
route flush [dev IF] | route flush cache
sysctl NAME=VALUE                  # e.g. sysctl net.inet.tcp.sack=1
ifconfig IFNAME [A.B.C.D[/BITS]] [netmask M] [broadcast B] [peer A.B.C.D]
      [mtu N] [metric N] [tcpwindow N] [[-]promisc] [[-]arp] [delete] [up|down]
ifconfig IFNAME alias A.B.C.D[/BITS] [netmask M] [broadcast B]   # a further address
ifconfig IFNAME A.B.C.D -alias     # remove one address; "ifconfig IFNAME delete": all
ipv4 [IFNAME] on|off               # IPv4 on that interface (or all, and the default)
ipv6 [IFNAME] on|off               # IPv6 on that interface (or all, and the default)
happy_eyeballs on|off | delay MS | resolution_delay MS | prefer auto|ipv6|ipv4 |
      aaaa_filter on|off | status  # connecting by name, see "Happy Eyeballs"
qos on|off [IFNAME] | inflight N | limit N | priodscp on|off | status
qos rule [proto=tcp|udp] [port=|lport=|rport=P,..] [program=NAME]
      [priority=interactive|default|bulk|0-7] [dscp=EF|CS1|AF41|0-63]
qos rules flush                    # see "Priorities and quality of service"
dhcp IFNAME [start|stop|renew] [linklocal on|off] [wait S]
                                   # run (stop, renew) the DHCP client on IFNAME, see "DHCP"
arp add A.B.C.D MAC [dev IF] | arp del A.B.C.D [dev IF] | arp flush [dev IF]
cpu routines auto|generic|020|040|060   # checksum/copy routines (troubleshooting)
capture start [OPTIONS] | capture stop | capture status | capture save FILE | capture clear
                                   # packet capture, see "Packet capture"
profile start|stop|reset|show      # the profiler (see "Profiling and debug log")
debug CATEGORY[,...]|all|none [size=KB] | debug show [N] | debug clear | debug dump FILE
ntp SERVER [SERVER...] | ntp on|off|sync|status    # SNTP client, see "Time and .local names"
ntp interval SECONDS | ntp step MS | ntp battclock on|off
timezone [+|-]HH[:MM] | timezone auto              # local time = UTC + offset
mdns on|off|status                 # the multicast DNS responder for HOSTNAME.local
```

The short forms of older versions (`route add default 10.0.2.2`,
`route add -net N netmask M G`, `nameserver A.B.C.D`, `dhcp eth0`) keep
working.

**Default route.** A static default route (`route add default ...`) always
wins over the one a DHCP lease installs, also when it is added later; `ip
route` shows them as `proto static` and `proto dhcp`. `route replace
default via X` replaces the default routes of the same metric. When the
lease ends or `dhcp IF stop` runs, the DHCP route goes (a static one with
the same gateway stays); when an interface is removed all its routes go.
While an interface is down its routes stay but are not used (`linkdown`);
learned routes (redirects, path MTU) are dropped.

**TCP switches for side-by-side tests.** All of these can be changed at
run time with the `sysctl` tool and apply to every connection at once,
so two runs of the same download can be compared without restarting:

| sysctl | default | |
|---|---|---|
| `net.inet.tcp.sack`, `timestamps`, `window_scaling` | 1, 1, 1 | the TCP options (`rfc1323` sets timestamps and window scaling together) |
| `net.inet.tcp.ecn` | 1 | ECN (RFC 3168): 1 asked for on outgoing connections and accepted on incoming ones, 2 accepted only, 0 off (before 0.20 the default was 2) |
| `net.inet.tcp.ecn_fallback` | 1 | a retransmitted SYN goes without ECN, and the connection does not use it (RFC 3168 6.1.1.1: a middlebox may drop ECN-setup SYNs) |
| `net.inet.tcp.recvspace`, `sendspace` | 32768, 32768 | initial buffer sizes |
| `net.inet.tcp.rmem_max`, `moderate_rcvbuf` | 65536, 1 | receive buffer auto-tuning and its limit (host test builds: 262144) |
| `net.inet.tcp.delack_segs` | 2 | send an ACK every N full-sized segments (1 = every segment) |
| `net.inet.tcp.delack_ms` | 200 | longest an ACK is delayed (ms) |
| `net.inet.tcp.quickack` | 0 | 1 = never delay ACKs |
| `net.inet.tcp.cc` | cubic | congestion control for new connections (`cubic`, `reno`, `ledbat`) |
| `net.inet.tcp.hystart` | 2 | CUBIC's slow start exit: 2 HyStart++ (RFC 9406), 1 Linux's classic HyStart, 0 none (slow start until a loss) |

**ECN.** Riptide asks for Explicit Congestion Notification on the
connections it opens, as Apple's systems and many Linux distributions do:
a router with an AQM (fq_codel, CAKE, PIE, as in many home routers) can
then mark packets instead of dropping them, and the sender slows down
without a retransmission. Data segments are sent ECT(0); SYNs, ACKs and
retransmissions are not. A peer that does not answer with ECE alone (no
ECN, or ECE and CWR reflected by a middlebox) gets none, and a SYN that
has to be resent goes without ECN. `netstat -s` counts connections using
ECN, requests not agreed, SYNs resent without ECN, segments received with
"congestion experienced" and ACKs with the ECN echo. `IP_TOS` sets the
DSCP only; the two ECN bits stay the stack's. RiptidePrefs: the TCP
page, "ECN" (Default, Off, On, Accept only).

`delack_segs` counts like Linux: an ACK goes out once more than N-1
segments are unacknowledged, but only if the window it advertises is not
smaller than the last one; otherwise the ACK waits until the application
reads (or for the delayed ACK timer). Frames that arrive together are
handled as one batch: the ACKs `delack_segs` calls for within it go out as
one at its end, covering at most `2 * delack_segs` segments (duplicate
ACKs are never held). A reader that empties the window by small reads
gets a window update once the window can open by four segments (or a
quarter of the buffer) or the queue is empty, not after every read. The SANA-II read count is changed
with `ip link set NAME reads N` (see below). `netstat -T` shows the effect
on each connection.

### Connecting, Fast Open, keepalive, cork

Riptide's TCP connection behaviour for clients. Every setting is a sysctl
(change it at run time with the `sysctl` tool or RiptidePrefs' Advanced
page; in `riptide.conf` as `sysctl NAME=VALUE`). New values apply to
connections started afterwards.

| sysctl `net.inet.tcp.` | type, range | default | |
|---|---|---|---|
| `fastopen` | 0/1 | 1 | TCP Fast Open client (RFC 7413) for sockets that ask for it |
| `fastopen_all` | 0/1 | 0 | every `connect()` as with `TCP_FASTOPEN_CONNECT` |
| `fastopen_blackhole` | seconds, 0-86400 | 3600 | Fast Open off for this long (doubling, up to 64 times) after three lost TFO SYNs in a row; 0 never |
| `fastopen_cache` | text (read; writing clears) | | the cookies: `ADDR cookie HEX mss N [syn-lost]`, or `disabled for N s` first |
| `syn_rto_initial_ms` | ms, 200-3000 | 1000 | first SYN retransmission timeout |
| `syn_rto_max_ms` | ms, 200-120000 | 4000 | longest SYN retransmission timeout |
| `syn_backoff` | 0/1 | 1 | 1 doubles the SYN timeout, 0 adds `syn_rto_initial_ms` each time |
| `connect_timeout` | seconds, 0-600 | 20 | give up connecting after this long (`ETIMEDOUT`); 0: only `syn_retries` (6) |
| `syn_icmp_abort` | 0-2 | 1 | ICMP net/host unreachable while connecting: 2 fails the connect at once (Linux), 1 on the second one (after the SYN went out twice), 0 only at the timeout (RFC 1122) |
| `tw_reuse` | 0-2 | 1 | connect() may reuse a 4-tuple in TIME_WAIT (with timestamps, 1 s later); 2: loopback only (Linux's default) |
| `max_tw_buckets` | 0-65536 | 1024 | TIME_WAIT records (about 120 bytes each with the allocation, 120 KB for 1024); beyond that connections close without one |
| `keepalive_all` | seconds, 0-32767 | 0 | keepalive on every connection, after this long idle (0 off) |
| `keepidle`, `keepintvl`, `keepcnt` | s, s, 1-127 | 7200, 75, 9 | keepalive defaults for `SO_KEEPALIVE` |
| `net.inet.ip.portrange.first`, `.last` | 1024-65535 | 49152, 65535 | ephemeral ports |

**Connecting.** Linux retries a SYN after 1, 2, 4, 8, 16 and 32 s, so a
program that connects to a dead host or a broken IPv6 path hangs in
`connect()` for over two minutes. Riptide retries after 1, 2, 4, 4 ... s
and gives up after `connect_timeout` (20 s). Per socket, `TCP_KEEPINIT`
(seconds) or `TCP_USER_TIMEOUT` (ms) replace `connect_timeout`, and
`SO_SNDTIMEO` ends a blocking `connect()` with `ETIMEDOUT` (the attempt
goes on; `connect()` again gives `EALREADY`). An ICMP port unreachable or
a RST fails the connect at once; host and network unreachable (from a
router, or from Riptide itself when ARP or neighbour discovery fail) fail
it on the second report with `syn_icmp_abort` 1, about a second after
`connect()`.

**TCP Fast Open** (RFC 7413, client side) saves a round trip on repeated
connections to the same server: the first connection asks for a cookie
(an empty option in the SYN), later ones send the cookie and the first
data in the SYN, and the server can answer with the SYN-ACK. Only servers
that offer it take part (Linux with `net.ipv4.tcp_fastopen` 2 or 3 on a
listener with `TCP_FASTOPEN`); others ignore the option, and data they did
not take in the SYN is sent again at once. Cookies are kept per server
address (32, the least recently used goes). A program asks for it:

* `setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN_CONNECT (30), &one, 4)` before
  `connect()`: with a cookie for the server, `connect()` returns 0 at
  once and the first `send()` goes out in the SYN; without one, the
  connect is normal (with a cookie request). A socket that is read from,
  waited on for reading (`WaitSelect()`) or shut down before it sent
  anything starts the handshake without data, so servers that speak first
  work too. `TCP_FASTOPEN` (1025, FreeBSD's number) on a socket that is not
  listening does the same.
* `sendto(s, buf, len, MSG_FASTOPEN (0x20000000), &addr, addrlen)` on an
  unconnected TCP socket connects and sends in one call; it returns the
  bytes queued (without a cookie the data follows the handshake; a
  non-blocking socket gets `EWOULDBLOCK` until then).
* `fastopen_all` 1 does the first for every `connect()`, for programs that
  cannot be changed. `connect()` then succeeds at once for servers with a
  known cookie even if they are down: the error comes with the first
  `send()`/`recv()`. Use it for web browsing, not for everything.

Middleboxes that drop SYNs with data or unknown options are detected: a
SYN with the option that times out is retried without it, the server is
marked (after two such losses in a row no TFO to it for 1 minute,
doubling), and three lost TFO SYNs in a row switch TFO off for
`fastopen_blackhole` seconds. `TCP_INFO` sets `tcpi_options` bit 32 when
the server acknowledged data in the SYN. Not offered: the server side
(`TCP_FASTOPEN` on a listener gives `EOPNOTSUPP`).

**TIME_WAIT and ports.** The side that closes first keeps the connection
in TIME_WAIT for 60 s. With `tw_reuse` 1 (Riptide's default; Linux uses
2, loopback only) `connect()` reuses such a port and address pair when
both ends use timestamps (PAWS keeps old segments out) and at least a
second has passed, so programs that open many short connections to one
server do not run out of ephemeral ports. The test suite makes 3000
connections in a row to one port without an error.

**Keepalive.** `SO_KEEPALIVE` with `TCP_KEEPIDLE`, `TCP_KEEPINTVL` (seconds)
and `TCP_KEEPCNT` (BSD numbers 0x100, 0x200, 0x400; Linux uses 4, 5 and 6):
after IDLE seconds without traffic a probe goes out, then one every
INTERVAL seconds while unanswered; after COUNT unanswered probes the
connection fails with `ETIMEDOUT`. An answered probe restarts the idle
time (but the next one comes no sooner than INTERVAL). Home NAT routers
forget idle TCP connections after minutes to hours, so telnet, IRC or SSH
sessions of programs that never ask for keepalive die silently;
`keepalive_all 120` (with `keepintvl 30`, say) keeps them open. An
explicit `SO_KEEPALIVE` off wins over it.

**Nagle, `TCP_NODELAY`, `TCP_CORK`.** As in Linux: Nagle's algorithm holds
a small segment while an earlier small one is unacknowledged;
`TCP_NODELAY` (1) turns that off and sends what was held at once.
`TCP_CORK` (3, Linux) holds partial segments until it is cleared, at most
200 ms; full segments always go. `TCP_NOPUSH` (4, BSD) is the same without
the 200 ms timer (with nothing in flight the window probe timer sends it
after an RTO, as Linux does). `send()` with `MSG_MORE` (0x8000, Linux)
holds that call's tail the same way until a `send()` without it. A
`shutdown(SHUT_WR)` of a Fast Open socket whose handshake is still going
sends its FIN after the handshake (other sockets abort a connect, as BSD
and Linux do).

`netstat -s` counts Fast Open (data in the SYN, data not acknowledged,
cookie requests, lost SYNs, switch-offs), connects that timed out or
ended on ICMP unreachable, corked segments sent by the 200 ms limit,
keepalive probes and reused TIME_WAIT connections. `nc -F` connects with
Fast Open and `nc -K SECS` turns keepalive on.

### Path MTU black holes

A link with a smaller MTU on the way (PPPoE DSL 1492, a VPN, some cable
and mobile links) is normally found by path MTU discovery: the router in
front of it drops a full sized packet and says so with an ICMP
"fragmentation needed" (IPv6: "packet too big"), and Riptide lowers the
size for that destination (`ip route show cache`). Firewalls that drop
that ICMP make a *black hole*: the connection opens, small requests work,
and the first full sized segments vanish without a trace, so a download
or upload hangs until the program gives up. Riptide detects this as
Linux does (packetization layer path MTU discovery, RFC 4821):

| sysctl `net.inet.tcp.` | type, range | default | |
|---|---|---|---|
| `mtu_probing` | 0-2 | 1 | 0 off (only ICMP lowers the size); 1 on a suspected black hole; 2 always (every connection starts at `base_mss` and probes upwards) |
| `base_mss` | 48-65495 | 1024 | the MSS a black hole falls back to first |
| `mtu_probe_floor` | 48-65495 | 48 | the lowest MSS further halving goes to |
| `probe_threshold` | bytes, 1-65535 | 8 | the search for the largest size that gets through stops when the range is this narrow |
| `probe_interval` | seconds, 1-86400 | 600 | after this long a finished search starts again (the path may have improved), and a remembered black hole is forgotten |
| `mtu_cache` | text (read; writing clears) | | destinations with a black hole: `ADDR mtu N age S s` |

With `mtu_probing` 1, when the same segment has timed out
`tcp_retries1` (3) times, which is about 6 s with fast round trips, the
connection drops to segments of `base_mss` (1024 bytes of data; another
run of timeouts halves that) and goes on. It then probes upwards: now and
then one segment of a size halfway between the largest that got through
and the smallest that did not is sent among the normal ones; an
acknowledged probe raises the size, a lost one (seen by fast retransmit,
without counting as congestion) lowers the upper bound. After about six
probes the connection uses the largest size that gets through (probes
need several segments of unsent data queued, as in Linux: a program that
writes 2 KB at a time, like `nc`, keeps the fallback size). The
destination is remembered for `probe_interval` seconds (16 entries), so
the next connection to it starts at that size instead of stalling again.
This TCP memory does not change the path MTU of the IP layer: UDP is not
affected by a guess. IPv4 and IPv6 alike; when the ICMP does arrive,
normal path MTU discovery handles it first and no probing is needed.

`netstat -s` counts black hole reductions and MTU probes acknowledged
and lost (Linux: `TCPMTUPFail`, `TCPMTUPSuccess`). RiptidePrefs sets
`mtu_probing` on its "Advanced: Connecting" page ("MTU probing": Default,
Off, On black hole, Always). Measured against Linux over TUN with the
router's ICMP dropped (`tests/t_mtup_tun.c`): a 1 MB upload that stalls
for good with `mtu_probing` 0 completes in 6.4 s with 1 (6.4 s more than
with the ICMP getting through), almost all of it the three timeouts
before the black hole is suspected; a second connection to the same
address takes the normal time. On an emulated A1200 (FS-UAE, `nc`,
`tests/amiga/run_fsuae_pmtu.sh`) a 1 MB upload through a 1400 byte hole
takes about 10 s instead of 7 s.

### Time and .local names

**NTP.** `ntp pool.ntp.org` (or up to four names or addresses) starts the
SNTP client (RFC 4330, the client side of NTPv4): the first query goes out
two seconds after the stack starts, then once an hour (`ntp interval
SECONDS`, 64 to 604800). `ntp on` without servers uses the NTP servers of
the DHCP lease (option 42, requested automatically), or `pool.ntp.org`
when the lease has none. Names are looked up through the configured name
servers, A records first, then AAAA; each address gets two seconds to
answer, and the first valid reply wins. Replies are checked (our
originate stamp, mode, version, stratum 1-15, leap indicator, root
delay/dispersion under 16 s, a non-zero transmit stamp, a round trip under
10 s); a kiss-o'-death `RATE` doubles the interval, `DENY`/`RSTR` skip that
server. After a failed sync the client tries again after 30 s, 60 s, 120 s
... (at most the interval). `ntp sync` syncs now, `ntp off` stops.

The offset is computed from the four time stamps
(`((T2-T1) + (T3-T4)) / 2`, the delay `(T4-T1) - (T3-T2)`) without 64-bit
divisions, and NTP time stamps are converted with 32-bit wrap-around, so the
client keeps working through the NTP era change in February 2036 (up to
2106). When the clock is off by at least `ntp step` milliseconds (500 by
default) Riptide sets the system time with timer.device `TR_SETSYSTIME`;
AmigaOS cannot slew, so smaller offsets are only reported. With `ntp
battclock on` the battery-backed clock (battclock.resource) is set too.

**Time zone.** AmigaOS keeps local time. Riptide converts with the
offset from the Locale preferences (`loc_GMTOffset`, read when the stack
starts); `timezone +10:00` (or `-5`, `+5:30`) overrides it and `timezone
auto` goes back to the Locale value. `Riptide TIMEZONE` shows the offset in
use. Daylight saving time is not tracked: change the Locale preferences (or
the `timezone` line) when it starts and ends.

`Riptide NTP STATUS` and `Riptide STATUS` show the last sync:

```
NTP: pool.ntp.org, stratum 2, offset +3.214 s (clock set), delay 41 ms, 12 s ago, next in 3588 s
```

**mDNS.** Riptide answers multicast DNS (RFC 6762) for `HOSTNAME.local`
(the first label of `hostname`, `amiga` when none is set) on every
Ethernet-like interface with an address, over IPv4 (224.0.0.251) and IPv6
(ff02::fb) port 5353: A and AAAA records of that interface (120 s TTL) and
the reverse `in-addr.arpa`/`ip6.arpa` PTR names of its addresses. At start,
when an interface or address appears and when the host name changes, the
name is probed three times 250 ms apart and announced twice one second
apart; if another host already uses it Riptide picks `NAME-2.local`,
`NAME-3.local`, ... (the log says so, and `Riptide MDNS STATUS` shows the
name in use). Questions with the unicast-response bit are answered by
unicast, others by multicast at most once a second per interface, known
answers are suppressed, and queries from other ports than 5353 get the
"legacy unicast" answer of RFC 6762 6.7. On by default; `mdns off` in the
configuration (or `Riptide MDNS OFF`) turns it off and says goodbye (TTL 0).
DNS-SD service records are not published.

Programs can also resolve `.local` names: `gethostbyname()` and
`getaddrinfo()` send a one-shot mDNS query for names ending in `.local`
(to 224.0.0.251, two tries of one second) instead of asking the unicast
name servers; answers are cached for their TTL. The stack's own
`HOSTNAME.local` resolves to its interface addresses without traffic.
Link-local IPv6 answers are skipped (a hostent has no zone).

### Background transfers

A big download (an archive from Aminet, a Usenet binary) fills the queue
of the slowest link, usually the DSL or cable modem, and everything else,
a web page, IRC, a Telnet session, waits behind it: hundreds of
milliseconds per round trip. Riptide can run such transfers as
**background** ones that yield:

* **Downloads: rLEDBAT** (RFC 9840). The receiver limits the window it
  advertises so that the queue it causes stays near a target delay
  (`net.inet.tcp.ledbat_target`, 60 ms). It measures RTTs with TCP
  timestamps (a server without timestamps is not limited) and runs the
  LEDBAT++ controller: slow start with a reduced gain, a multiplicative
  decrease when the delay passes the target, and every now and then two
  RTTs at two segments to measure the base delay again (`ledbat_slowdown`).
  Alone, a background download still uses most of the link; next to a
  normal one it backs off to almost nothing. The window is never shrunk:
  the smaller limit is reached by not opening it further.
* **Uploads: LEDBAT++** (RFC 6817, draft-irtf-iccrg-ledbat-plus-plus), the
  congestion control `ledbat`, the same controller on the sender's
  congestion window.

LEDBAT yields to traffic that builds a queue (CUBIC and Reno servers, the
other computers' downloads). A server that keeps no standing queue itself
(BBR) does not push it back much.

Programs that know about it set the Riptide socket option
`setsockopt(s, IPPROTO_TCP, TCP_BACKGROUND, &v, 4)` (0x5200, in
`include/rt/rt_bsd.h`; v: 1 receive, 2 send, 3 both, 0 normal) or
`TCP_CONGESTION` "ledbat" (send only). For all the others there are rules:

```
background program=AmiFTP,aget,wget*   # by the program that opens the connection
background port=119,563,6881-6889       # by the remote port (Usenet, BitTorrent)
background lport=21                     # by the local port (a server here)
background mode=both                    # what the rules turn on: recv, send, both, off
background target=60                    # LEDBAT target delay, ms (5..1000)
background clear                        # no rules
background                              # show them
```

A program is matched by the command name a Shell runs (without its path)
or by its task name; `name*` matches a prefix, case does not matter. Each
line replaces the list it names (`port=` alone empties it). Rules decide
when a connection opens (the program when its socket is created); a
connection the program configured itself is left alone. At run time:
`Riptide BACKGROUND program=AmiFTP` (also `program AmiFTP`). The rules
are the sysctls `net.inet.tcp.bg_programs`, `bg_ports`, `bg_lports` and
`bg_mode`. `netstat -T` shows the background connections with the delay
LEDBAT measures:

```
tcp  10.0.0.15:49153 192.0.2.7:119 ESTABLISHED
  ledbat open age 42.1s  rtt 31.2/4.1 ms min 18.0 rto 232 ms backoff 0
  ...
  background receive send (rule)  rLEDBAT window 46336  delay 12.0 ms over base 18.0 ms, normal
```

| sysctl | default | |
|---|---|---|
| `net.inet.tcp.ledbat_target` | 60 | LEDBAT/rLEDBAT queueing delay target, ms (5..1000) |
| `net.inet.tcp.ledbat_slowdown` | 1 | LEDBAT++ periodic slowdowns (0 = off) |
| `net.inet.tcp.bg_mode` | 3 | what the rules turn on: 1 rLEDBAT, 2 LEDBAT, 3 both, 0 nothing |
| `net.inet.tcp.bg_programs` | (empty) | programs, `name,name*,...` |
| `net.inet.tcp.bg_ports` | (empty) | remote ports, `P,P-Q,...` |
| `net.inet.tcp.bg_lports` | (empty) | local ports |

### Socket memory limits

Every connection grows its buffers with the transfer (receive buffer
auto-tuning up to `net.inet.tcp.rmem_max`, the send buffer with the
congestion window). A web browser with a dozen connections could take
megabytes. Riptide counts the memory of all socket buffers (TCP receive,
out-of-order and send queues, UDP and raw receive queues) against three
limits, in KB, like Linux's `tcp_mem`:

* above `net.inet.tcp.mem_pressure` the stack is under pressure until the
  use falls below `mem_low`: buffers and windows stop growing and new
  connections get a window scale for 64 KB only;
* above `net.inet.tcp.mem_high` a connection that holds more than 8 KB in
  a direction gets no more: data that arrives is dropped (the sender
  sends it again later) and writers wait. Every connection keeps its 8
  KB, so nothing stalls for good (and the use can pass the limit by that
  much per connection).

The defaults follow the free memory when Riptide starts: a quarter of it
for `mem_high`, a sixth for `mem_pressure`, an eighth for `mem_low` (at
least 192, 128 and 96 KB). Without Fast RAM the buffers share Chip RAM
with the display, so 256 KB are left out first. `Riptide STATUS` shows the
use (`TCP memory: 212 KB in socket buffers (pressure at 1365 KB, limit
2048 KB)`), `sysctl net.inet.tcp.mem_used` and `mem_state` (0 normal, 1
under pressure, 2 at the limit) too, and `netstat -s` counts the periods
under pressure, their time and the dropped segments.

| sysctl | default | |
|---|---|---|
| `net.inet.tcp.mem_low` | free / 8 | pressure ends below (KB) |
| `net.inet.tcp.mem_pressure` | free / 6 | buffers stop growing above (KB) |
| `net.inet.tcp.mem_high` | free / 4 | hard limit (KB) |
| `net.inet.tcp.mem_used` | | in use now (KB, read only) |
| `net.inet.tcp.mem_state` | | 0 normal, 1 under pressure, 2 at the limit (read only) |

### Packet capture

Riptide can record the frames it sends and receives into a ring buffer in
RAM and save them as a pcap file for Wireshark or tcpdump on another
computer. This is the first thing to look at when transfers are slow: the
file shows whether the sender waits for window updates, loses packets and
retransmits, or is held back by late ACKs (Wireshark: Statistics > TCP
Stream Graphs > Stevens / tcptrace).

```
Riptide CAPTURE start                          ; every interface, 256 KB ring
ftp ftp://ftp.example.org/pub/big.lha          ; the transfer to look at
Riptide CAPTURE stop
Riptide CAPTURE save RAM:slow.pcap
```

`capture start` takes these options (all optional; they can also go in
the configuration file, to capture from start-up):

| Option | Meaning |
|---|---|
| `iface=NAME` | only this interface (default all, including `lo0`) |
| `proto=tcp\|udp\|icmp` | only this protocol (`icmp` includes ICMPv6) |
| `port=N` | only TCP/UDP with this source or destination port |
| `host=ADDR` | only IPv4 or IPv6 packets from or to this address |
| `snaplen=N` | bytes kept of each packet (default 128: all headers, a little payload; 20 to 65535) |
| `size=KB` | ring size (default 256, 16 to 16384); allocated at `start`, freed by `clear` |
| `wrap` | when the ring is full, overwrite the oldest packets (default: stop recording and count the rest as dropped) |
| `file=PATH` | save to PATH on `capture stop` and when Riptide quits |

`capture stop` stops recording and keeps the packets, `capture save FILE`
writes them (it can also run while recording: recording pauses while the
file is written, outside the stack lock), `capture status` shows the
state and counters, and `capture clear` frees the ring. A new `start`
discards the previous packets. With a filter, ARP is not recorded.

```
capture: running, 1210 frames (180 KB of 256 KB), 1210 seen, 0 dropped, 0 overwritten
  snaplen 128, stop when full, iface eth0 proto tcp
```

The file uses the Linux "cooked capture" link type (`LINKTYPE_LINUX_SLL`),
which every version of Wireshark and tcpdump reads: one file can hold
Ethernet, point-to-point and loopback interfaces, ARP as well as
IPv4/IPv6, and each packet says whether it was sent ("Out") or received,
broadcast or multicast, and the sender's MAC address. Packets are
recorded where they meet the driver: received ones as the driver hands
them over, sent ones as they go to it; loopback packets appear once, when
`lo0` delivers them. Timestamps come from the EClock (microseconds).

Memory: the ring is the only allocation (each packet takes 32 bytes plus
up to `snaplen` bytes, rounded up to 4: about 1,600 full-size TCP
segments in the default 256 KB), plus 4 KB while saving. When capture is
off the cost is one test per packet.

### Amiga keywords

```
interface NAME device=DEVICE [unit=N] [mtu=N] [reads=N] [tcpwindow=N] [raw]
      [ipv4=on|off] [ipv6=on|off] [qos=on|off]
interface NAME mni=DRIVER [unit=N] [options="TEXT"] [mtu=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off]
interface NAME reads=N             # at run time: change a SANA-II interface's IPv4 reads
interface NAME delete              # at run time: close the driver, drop the interface
interface NAME|all resetstats      # zero the interface and driver counters (ip link resetstats)
user NAME [uid=N] [gid=N] [home=DIR] [realname=TEXT]
compat aliases on|off              # publish the compat alias library (default on)
loglevel N
```

An `interface` line must come before the `ifconfig` or `dhcp` line that
uses its name.

**SANA-II devices.** `device=` names a SANA-II driver, for example
`device=DEVS:Networks/a2065.device`.

* Riptide opens the driver with buffer-management callbacks that copy
  straight into its packet buffers (summing each received frame in the
  same pass, so TCP and UDP do not read it again). Drivers that support
  the SANA-II R3 DMA callbacks (`S2_DMACopyToBuff32`,
  `S2_DMACopyFromBuff32`) get the packet buffer's address instead and move
  the frame themselves, without a CPU copy; Riptide's buffers are 32-bit
  aligned and hold the largest frame, and where one is not suitable the
  callback returns NULL and the driver copies. It then runs `S2_DEVICEQUERY` (MTU,
  hardware type, address size), `S2_GETSTATIONADDRESS`,
  `S2_CONFIGINTERFACE` with the factory address, and `S2_ONLINE`.
* On Ethernet-like devices (hardware type Ethernet or IEEE 802) Riptide
  keeps 48 `CMD_READ` requests queued for IPv4 (`reads=N`, 4 to 128), 4
  for ARP and 24 for IPv6. A frame that arrives while the driver holds no
  read of its type is dropped by the driver; `ip -s link` shows how low
  each queue got, and Riptide logs a warning (at most once a minute) when
  one ran out. Point-to-point devices (PPP, SLIP, and any other hardware
  type) get only the IPv4 and IPv6 reads and are configured with
  `ifconfig NAME A.B.C.D peer P.Q.R.S`. The IPv4 reads must cover a burst
  of the sender's window: `interface NAME reads=N` sent to the running
  stack (`ip link set NAME reads N`) changes the count at once; new reads
  are queued immediately, surplus ones leave service as they complete.
* Cooked mode is the default. `raw` makes Riptide build and strip the
  Ethernet header itself, using `SANA2IOF_RAW`. Use it only for drivers
  with a broken cooked mode.
* If a read fails because the device is offline, Riptide retries every
  second.

**MNI drivers.** `mni=` loads an MNI network driver (`.mni`, the driver
format of the historical stack) directly, for example `mni=z2-am7990.mni`.

* If the name has no path, Riptide tries `LIBS:`, `DEVS:Networks/` and
  then the historical stack's driver directory (`Miami:MNI/`).
* `unit=` is the board index. `options=` is the driver's option string,
  for example `options="FT=2 MEDIA=AUTO"`.
* Receive is zero copy: the driver writes frames straight into Riptide's
  buffers.
* The ABI was recovered from the historical stack; see `docs/RE-NOTES.md`,
  section 4.

**Using existing MNI drivers.**

* Prefer `mni=`: it is faster and has no further requirements.
* The historical stack's `sanamni.device` wrapper is a SANA-II device too,
  but it is not a good choice with Riptide:
  * it works only with a registered installation of that stack;
  * it rejects packet types 0x0800 (IP), 0x0806 (ARP) and 0x8035 (RARP),
    which are exactly the types Riptide needs;
  * it adds a copy.

**User.** `user` sets the single usergroup.library user. The default is
`user`, uid 0, gid 0, home `SYS:`.

**Compat alias.** `compat aliases on|off` decides whether `riptide.library`
is also published as the compat alias library (default `on`). Turn it off
if no program you use needs the historical API name. In the configuration
file it takes effect when the libraries are created; sent to the running
stack (RIPTIDE port) it adds or removes the alias at once, and `off` fails
with `EBUSY` while a program still has the alias open. The startup line
and `Riptide STATUS` show the state:

```
Libraries: bsdsocket.library 4, riptide.library 3, usergroup.library 4; compat alias: miami.library 21
```

### Example: Ethernet with DHCP

```
hostname amiga
interface eth0 device=DEVS:Networks/a2065.device unit=0
dhcp eth0
```

### Example: Ethernet with a static address

```
hostname amiga
domain example.net
nameserver 192.168.1.1
interface eth0 mni=z2-am7990.mni unit=0
ifconfig eth0 192.168.1.20/24 up
route add default 192.168.1.1
```

### Example: FS-UAE / WinUAE with uaenet.device

See `examples/riptide-uaenet.conf`. `uaenet.device` lives in the emulator's
expansion ROM, not in `DEVS:`. In FS-UAE it only exists with
`uae_sana2 = 1` in the emulator configuration, and needs `network_card =
a2065` for the built-in slirp NAT. Under slirp the Amiga gets 10.0.2.15 by
DHCP. The gateway (your host computer) is 10.0.2.2 and the DNS forwarder is
10.0.2.3. Incoming connections from the LAN need slirp port forwarding.

**FS-UAE drops frames: `tcpwindow=1440`.** FS-UAE 3.x (3.1.66 and 3.2.35)
passes the Amiga only the first of the frames that wait for it at one
interrupt and drops the others, although Riptide has dozens of reads
queued (`uaenet_int_handler()` in its `sana2.cpp` resets a per-opener flag
once per interrupt instead of once per frame). slirp sends two or three
frames for each ACK, so a download loses a segment in every burst; slirp's
TCP has no SACK and waits for its retransmission timer, 1.5 to 12 s each
time, and downloads crawl at well under 1 KB/s. `netstat -T` shows the
signs: out-of-order bytes (`ooo`), duplicate ACKs, almost no data.

The workaround is a one-segment receive window on that interface: slirp
then never has two frames queued at once. The Installer and
`examples/riptide-uaenet.conf` set it:

```
interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440
```

On a Mac with FS-UAE 3.2.35 an Aminet download went from about 0.3 KB/s
to 227 KB/s. Remove it under WinUAE, or once FS-UAE is fixed. At run time:
`ip link set eth0 tcpwindow 0` (or `1440`), for connections opened
afterwards.

**FS-UAE bridged to your network.** `tools/fs-uae/` has a patch for FS-UAE
3.2.35 and a build script (macOS with Homebrew, Linux): it fixes the frame
drops and adds a libpcap backend, so `uaenet.device` unit 2 is a host
interface (`uaenet_pcap = en0` in the `.fs-uae` file) and the Amiga is on
your LAN with its own MAC address: DHCP from your router, IPv6 from your
router's advertisements, no slirp, no NAT. Use
`interface eth0 device=uaenet.device unit=2` without `tcpwindow`. It needs
a wired interface and permission to capture; see `tools/fs-uae/README.md`.
The patched `uaenet.device` also honours Riptide's SANA-II DMA callbacks
(the emulator moves each frame straight into or out of Riptide's buffer,
no copy on the 68k): on an emulated A1200 that is 799 → 901 KB/s down
and 480 → 698 KB/s up (`docs/COMPARISON.md` 11). Builds from before
Riptide 0.26 need rebuilding with the current patch to get it.

**A stack that stops answering under FS-UAE with the AROS ROM.** On FS-UAE
3.2.35 with the AROS Kickstart replacement ROM, a busy host occasionally
makes exec lose a task that `Signal()` woke: the task is marked ready but
sits in neither of exec's task lists and never runs again. When that task
is Riptide's process, `Riptide STATUS` hangs, connections stop and
programs wait forever, while everything else keeps running. It happens
without Riptide too, under timer interrupt load
(`tests/amiga/run_fsuae_execrace.sh`; docs/DESIGN.md), so it is not a
Riptide fault; reboot the emulated Amiga. It is rare when the host has
spare CPU (about one bsdsocktest run in 50 to 100 while other emulators
ran) and common when the host is overloaded.

### DHCP

`dhcp IF` runs the DHCP client (RFC 2131) on IF. Besides the address,
netmask, router, DNS servers, domain name, host name, MTU and NTP servers
it asks for:

* **Classless static routes** (option 121, RFC 3442; Microsoft's option
  249 when a server sends only that). They are installed as DHCP routes
  (`ip route` shows `proto dhcp`; a router 0.0.0.0 makes an on-link route),
  replaced when a renewal brings other ones, and removed with the lease.
  When the option is there the router option (3) is ignored, as the RFC
  says: the default route is the option's 0.0.0.0/0 entry, and without
  one there is none. A malformed option is ignored as a whole. At most 16
  routes are used.
* **The domain search list** (option 119, RFC 3397). It becomes the
  resolver's search list (up to six domains; a name without a dot is
  tried with each, a name with one first as it is), unless `domain` or
  `search` set one in the configuration. The domain name (option 15)
  stays the domain; without option 119 it is the search list, as before.
  `ip dns` and `Riptide STATUS` show the list.

**Link-local address when DHCP fails.** When no lease has come 10 seconds
after DHCP started (`wait S`, or `sysctl net.inet.ip.linklocal_wait`),
the interface takes an IPv4 link-local address (RFC 3927, "ZeroConf"
or "APIPA"): 169.254.1.0 to 169.254.254.255, the first candidate from
its MAC address (so it is usually the same one again), then random ones.
Each is probed with three ARP requests first; when another host answers
or probes for it as well the next one is tried (after ten conflicts at
most one a minute). The address comes with a 169.254.0.0/16 on-link route
and no default route, so it reaches only the local link, which is what it
is for: two computers connected by a cable, or a network whose DHCP
server is down. mDNS announces `HOSTNAME.local` with it, so the Amiga is
reachable by name. A later conflict is defended once; a second one within
ten seconds gives the address up for a new one.

DHCP keeps trying meanwhile, and a lease replaces the link-local
address (a host has one or the other, never both: RFC 3927 lets the
link-local one go once a routable address exists). A static address
(`ifconfig`) is never replaced, and `ifconfig IF 169.254.X.Y/16` sets a
link-local address by hand.

The fallback is on by default, as on Windows and macOS: without a lease
the interface has no address at all otherwise, and a 169.254 address
never takes a route away from anything. `dhcp IF linklocal off` turns it
off for that interface (it takes effect at once and drops the address),
`sysctl net.inet.ip.linklocal=0` for every interface whose `dhcp` line
does not say. RiptidePrefs has a check box for it on the Address page.

`ip dhcp` shows the state:

```
1> ip dhcp
eth0: BOUND 192.168.1.20/24 server 192.168.1.1 router 192.168.1.1 lease 86400s valid_lft 86123sec classless-routes 2
1> ip dhcp
eth0: SELECTING
    link-local 169.254.200.17/16
```

### TCP window per interface

`tcpwindow=N` on the `interface` line, `ifconfig IF tcpwindow N` or `ip
link set IF tcpwindow N` limit the receive window that TCP offers on
connections routed over that interface (like Linux `ip route ... window
N`). Buffer auto-tuning still runs but never offers more. 0 removes the
limit; the value is 0 or 536..16777216 bytes and applies to connections
opened after the change. `ip link` and `Riptide STATUS` show it. Other
interfaces keep the full window, so the limit costs nothing on a real
network card.

### Several IPv4 addresses per interface

An interface can have up to nine IPv4 addresses: the primary one (the
`ifconfig IF A.B.C.D` or DHCP address) and eight more, on the same subnet
or on others:

```
ifconfig eth0 192.168.1.20/24 up
ifconfig eth0 alias 192.168.1.21/24        # same subnet
ifconfig eth0 alias 10.0.5.2/24            # another subnet on the same wire
```

* Each address answers ARP (and announces itself with a gratuitous ARP),
  ICMP, UDP and TCP; programs can `bind()` to any of them. Each prefix gets
  its connected route and its broadcast address.
* **Source address.** Packets to a destination (or through a gateway) on
  the subnet of an address leave from that address; anything else from
  the primary one. `ip route get` is not needed: `ip addr` shows which is
  which.
* **DHCP** keeps managing the primary address; additional static ones stay
  across renewals and when the lease ends.
* Removing the primary address (`ip addr del`, `ifconfig IF A.B.C.D
  -alias`) promotes the next one, as Linux does with
  `promote_secondaries`. `ifconfig IF delete` (and `ip addr flush dev IF`)
  removes all of them.
* Point-to-point interfaces keep one address and their peer.
* Tools: `ip addr add A.B.C.D/N dev IF` adds one when the interface has an
  address (`ip addr replace` changes the primary), `ip addr show` lists the
  others as `secondary`; `ifconfig IF:1 A.B.C.D` (the Linux label form) and
  BSD's `ifconfig IF alias A.B.C.D` / `-alias A.B.C.D` work too;
  `ifconfig` shows them as `(alias)` lines and `Riptide STATUS` as `also
  A.B.C.D/M`. The new table `net.riptide.addrs4` lists every address; the
  ioctls follow BSD: `SIOCAIFADDR` adds, `SIOCDIFADDR` removes the address
  given, `SIOCGIFCONF` has one entry per address, `SIOCGIFADDR` and
  `SIOCSIFADDR` work on the primary one.
* The mDNS responder answers `HOSTNAME.local` with all of them (four per
  interface).

Several network cards work at once as before: give each its own
`interface` line, address and routes.

### VLAN interfaces (802.1Q)

A VLAN interface is an interface of its own on the wire of an Ethernet
interface (its parent): frames it sends carry an IEEE 802.1Q tag with its
VLAN id, and tagged frames with that id that arrive on the parent go to it.
Each has its own IPv4 and IPv6 addresses, DHCP client, ARP and neighbour
tables, multicast memberships, mDNS answers, routes and counters:

```
interface eth0 device=uaenet.device unit=0
dhcp eth0
interface eth0.10 vlan=10 parent=eth0 dhcp                  # VLAN 10, DHCP
interface eth0.20 vlan=20 parent=eth0 address=10.20.0.2/24 pcp=5
interface eth0.30 vlan=30 parent=eth0 pcp=priority mtu=1500 up
route add 10.99.0.0/16 10.20.0.1
```

| Option | |
|---|---|
| `vlan=ID` | the VLAN id, 1-4094 (required) |
| `parent=IF` (or `link=IF`) | the Ethernet interface the VLAN runs on; it must be configured first. Several VLANs per parent, each id once |
| `pcp=0-7` | the priority code point of the frames sent (default 0) |
| `pcp=priority` | the PCP from the sender's `SO_PRIORITY` (0-7; packets without one: 0) |
| `mtu=N` | default: the parent's MTU less 4, so a tagged frame is no longer than a full untagged one; up to the parent's MTU when the driver and the network take 1518 byte frames |
| `dhcp`, `address=A.B.C.D/BITS`, `up` | start DHCP, set an address (and bring it up), or just bring it up (IPv6 link-local and SLAAC) |
| `tcpwindow=`, `ipv4=`, `ipv6=`, `qos=` | as on any `interface` line |

RiptidePrefs edits VLAN lines with all of these options (the VLANs
page); a VLAN line with another option, or named other than `IF.ID`,
stays as it is under "Other lines". Without `dhcp`, `address=` or `up`
the VLAN is made but not started (it comes up with `ip link set eth0.10
up`).

`interface eth0.10 delete` removes one (its addresses, routes, sockets'
bindings and DHCP lease go with it). A line that fails (a bad option, an
address in use) leaves no half-made interface behind. Any name works
(`office`, `vlan10`); `IF.ID` is the convention the tools and RiptidePrefs
use.

* **Parent.** The VLAN's link state follows the parent: down when the
  parent is down or has no carrier, up again with it; bringing a VLAN up
  brings its parent up. Removing the parent (`ip link del eth0`) removes
  its VLANs. A smaller parent MTU lowers the VLANs' MTUs; the VLAN's MTU
  cannot be set above the parent's.
* **Receive.** Frames tagged with an id that has no interface are dropped
  and counted as unknown protocol on the parent (`missed` in `ip -s
  link`); priority-tagged frames (id 0) are handled by the parent as if
  untagged. Stacked tags (QinQ, 0x88a8) are not supported.
* **SANA-II.** Riptide queues reads for packet type 0x8100 as long as the
  parent has a VLAN (as many as for IPv4, `reads=`). The driver must pass
  tagged frames to them: uaenet.device and drivers that accept any packet
  type do; a driver that filters by type, or a network card that strips
  tags, needs its own VLAN settings. MNI drivers deliver every frame.
* The VLAN uses the parent's MAC address; its multicast groups go to the
  parent's filter (shared groups are counted, so leaving on one interface
  does not cut the other off).
* Not on point-to-point (PPP/SLIP) interfaces.

At run time:

```
1> ip link add link eth0 name eth0.10 type vlan id 10 [pcp 4]
1> ip addr add 10.10.0.2/24 dev eth0.10
1> ip -d link show eth0.10
5: eth0.10@eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1496 state UP mode DEFAULT
    link/ether 52:54:00:12:34:56 brd ff:ff:ff:ff:ff:ff
    vlan protocol 802.1Q id 10 parent eth0 pcp 4
1> ip link del eth0.10
1> ifconfig eth0.20 create vlan 20 vlandev eth0 [vlanpcp 5]    # BSD style
1> ifconfig eth0.20 destroy
```

`ifconfig` shows `vlan: 20 vlanpcp: 5 parent interface: eth0`, `Riptide
STATUS` `VLAN 20 on eth0, 802.1Q, priority 5`; `netstat -i` lists VLANs
like any interface. The `net.riptide.ifaces` records have `vlan_parent`
(the parent's index, 0 when not a VLAN), `vlan_id`, `vlan_pcp`
(`RT_CTL_VLAN_PCP_PRIO` for `pcp=priority`) and `nvlan` (the number of
VLANs on the interface) at the end (0.16; check the record size).

### IPv4 and IPv6 on or off

`ipv4 off` and `ipv6 off` switch a protocol off for every interface (and
for interfaces added later); `ipv4 eth0 off` / `ipv6 eth0 off`, or
`ipv4=off` / `ipv6=off` on the `interface` line, for one. `on` switches it
back on. Both work in the configuration and at run time (RIPTIDE port,
RiptidePrefs' Apply).

* **IPv4 off** removes the interface's IPv4 addresses and routes, stops its
  DHCP client and ignores IPv4 and ARP frames: an IPv6-only interface.
  After `ipv4 eth0 on` configure an address or start DHCP again.
* **IPv6 off** removes the interface's IPv6 state: no link-local address,
  no router solicitations, no MLD, no IPv6 input.
* With IPv6 off everywhere (or no IPv6 route to the Internet),
  `getaddrinfo()` does not ask for AAAA records (see `aaaa_filter` under
  "Happy Eyeballs"), so programs do not wait for IPv6 addresses they
  cannot reach.

`ip link` shows the state, `Riptide STATUS` prints `IPv4 off`.

### Hosts file

`ENV:Riptide/hosts` uses the usual format, `A.B.C.D name [alias...]`, with
`#` comments. Its entries go into the static host table, which is checked
before DNS.

### Resolver: EDNS0 and TCP

Classic DNS over UDP carries at most 512 bytes; CDN names with many
addresses, long CNAME chains and AAAA answers often need more. Riptide's
resolver (`gethostbyname()`, `gethostbyaddr()`, `getaddrinfo()`, the
Happy Eyeballs lookups, `res_query()` & co.) handles larger answers in two
ways:

* **EDNS0** (RFC 6891): every query carries an OPT record that offers
  answers of up to **1232 bytes** over UDP (the DNS Flag Day 2020 value,
  which fits any path MTU of 1280 or more without fragments). The answer
  buffer is that size and taken only for the lookup. A name server that
  answers FORMERR or NOTIMP is asked once more without the OPT record; it
  and a server whose answers carry no OPT record are then asked without
  EDNS0 for 10 minutes (`dns reset` forgets that at once).
* **TCP fallback** (RFC 7766): an answer with the TC ("truncated") bit
  is asked again from the same server over TCP (5 s for connect, query
  and answer; answers up to 16 KB are read). If TCP fails, what came over
  UDP is used, as before. This works for A, AAAA (also the parallel A and
  AAAA queries of `getaddrinfo()`), PTR and the raw `res_*` calls.

```
dns edns_size 1232     # EDNS0 UDP size: 0 = no EDNS0, 512..4096 (default 1232)
dns edns on|off        # the same: 1232 or 0
dns tcp on|off         # TCP fallback (default on)
dns reset              # forget which servers lack EDNS0
dns [status]           # e.g. "EDNS0 1232 bytes, TCP fallback on; 0 server(s)
                       #  without EDNS; 3 TCP queries, 0 failed"
sysctl dns.edns_size=N   sysctl dns.tcp_fallback=0|1    # the same settings
```

`sysctl dns.stats.tcp`, `dns.stats.tcp_failed` and `dns.stats.noedns`
count the TCP queries, the failed ones and the servers found without
EDNS0. Run time: `Riptide DNS EDNS_SIZE 0`, `Riptide DNS TCP OFF`,
`Riptide DNS` for the status; RiptidePrefs has both on the Names page.

The raw BIND calls behave as BIND's without `RES_USE_EDNS0`: `res_send()`
sends the caller's message as it is (an OPT record in it is honoured: the
UDP buffer grows to its size, up to 4096), goes over TCP when the message
is longer than 512 bytes or the answer is truncated, and copies at most
`anslen` bytes back; a TCP answer longer than that comes back cut with TC
set. A large `anslen` alone does not make it use TCP (BIND does not
either).

The security properties are unchanged: random query IDs and source ports
for every query, and answers count only from the server's address and
port 53 with our ID and question. DNS 0x20 (random upper/lower case in
the query name) is not used: names are compared without regard to case
anyway, and some servers and middleboxes do not keep the case.

The resolver's cache keeps up to 8 addresses per name, as many as
`gethostbyname()`, `gethostbyname2()` and `getaddrinfo()` return (larger
answers are parsed in full, the first 8 distinct addresses are kept), and
follows CNAME chains of up to 7 links.

**Negative answers and failures** are cached too (32 entries of their
own, per fully qualified name, so each search list candidate counts on
its own):

* **NXDOMAIN** ("no such name", RFC 2308): for as long as the zone's SOA
  record in the answer allows (the smaller of its TTL and MINIMUM field),
  at most `dns negttl` seconds (default 3600). It answers every type of
  query for the name (RFC 8020). An answer without SOA is not cached.
* **NODATA** (the name exists without records of that type): the same,
  for that type only, so a name without IPv6 addresses gets its IPv4
  ones at once and the AAAA question is not asked again.
* **Failures** (RFC 9520): SERVFAIL, REFUSED, FORMERR and no answer from
  any server. The same question is not sent again for 1 s, then 2, 4, 8
  ... seconds while it keeps failing, at most `dns failttl` seconds
  (default 30); a program asking in a loop gets "try again" at once. A
  break (Ctrl-C), a lookup that could not send anything (no route) and a
  change of the name server list are not held against it.

```
dns negttl 3600        # longest time NXDOMAIN / NODATA is kept, 0..86400 (0 = off)
dns failttl 30         # longest time a failed lookup is not repeated, 0..300 (0 = off)
dns flush              # forget the cache, positive and negative
sysctl dns.cache.neg_maxttl=N   sysctl dns.cache.fail_max=N   sysctl dns.cache.flush=1
```

`dns status` also shows the cache (e.g. `cache on: 12 names, 3 negative
(up to 3600 s), 0 failed (up to 30 s); 40 hits, 7 negative, 2 failed`);
`dns.stats.cache_hits`, `dns.stats.neg_hits` and `dns.stats.fail_hits`
are the counters. `dns.cache.enabled=0` turns off both caches. The raw
BIND calls (`res_query()` & co.) do not use the cache, as in BIND.

## RiptidePrefs

`RiptidePrefs` is a preferences editor for `riptide.conf` in the style of
the system's prefs editors, on the Workbench screen or another public
screen, for AmigaOS 2.04/3.x and AROS. It has two faces with the same
pages and settings: with **MUI 3.8 or later** installed (muimaster.library
19; MUI is optional and not part of Riptide) it opens the MUI editor
described [below](#the-mui-editor), otherwise a GadTools window that needs
nothing but the ROM. Both follow the screen font. Start it from its icon
(`SYS:Prefs`) or from a Shell:

```
RiptidePrefs [FROM=file] [EDIT] [USE] [SAVE] [PUBSCREEN=name]
             [GADTOOLS] [PAGE=name] [CMD=n] [THEME=DARK|LIGHT] [DEBUG=file]
```

It edits `ENV:Riptide/riptide.conf` (`ENVARC:` when `ENV:` has none, or
the `FROM` file). `GADTOOLS` opens the GadTools editor even with MUI
installed; `PAGE` opens on a page (its name, e.g. `PAGE=Ports`, or its
number from 0), `CMD` runs a command of the MUI editor's menus once the
window is open (the numbers are in `src/amiga/prefs/mui_ids.h`, e.g.
`CMD=105` About), `THEME` picks the MUI editor's colours for this run, and
`DEBUG` writes a log of what the editor does. Started from Workbench,
the same tool types in RiptidePrefs' icon do the same: `PAGE=Names`,
`GADTOOLS`, `PUBSCREEN=name`, `THEME=DARK`, `DEBUG=file`.

One editor at a time: started again while one is open (from a Shell or
its icon, either editor), RiptidePrefs brings the open window to the
front, activates it and ends at once with return code 0. The open editor
owns the public port `RiptidePrefs` for this; the MUI editor is also a
single-task MUI application. `USE` and `SAVE` on the command line (no
window) still work while an editor is open. The GadTools window has a
**Page** cycle gadget, the MUI one a page list; these are the pages (the
MUI editor drops the "Advanced: " of the names and lists those pages
under a heading of their own):

| Page | Settings | Lines in riptide.conf |
|---|---|---|
| Status | none: what Riptide is doing now, read once a second while the page is shown (see [below](#the-status-page)). The MUI editor opens on it and lists it first; in the GadTools editor it is the last page | none |
| Interface | up to 4 network cards (see [below](#several-cards)), each with its interface name, device, unit, MTU, TCP window, reads; IPv4 and IPv6 on/off for all cards (two check boxes). MUI editor: the list of cards, **New card** (`e`) and **Remove card** (`o`), and the settings of the card chosen; GadTools editor: the first card | each card's `interface NAME device=...` line (other options such as `raw`, `options=`, `ipv4=`, `ipv6=` or `qos=` are kept), `ipv4 on\|off`, `ipv6 on\|off` |
| Address | per card: DHCP or static; address (`A.B.C.D` or `A.B.C.D/BITS`), netmask, gateway; with DHCP "Link-local if no DHCP" (a 169.254 address while there is no lease, on by default). MUI editor: the **Card** cycle (`d`) chooses whose address the page shows | `dhcp IF [linklocal off]`, or `ifconfig IF ADDR [netmask M] up` and `route add default GW` (first card) or `route add default GW dev IF metric 10*N` (the others) |
| Extra addresses | a list of up to 8 more IPv4 addresses on the cards and VLANs (`[IF\|]A.B.C.D/BITS`, or `A.B.C.D` with `netmask M` and/or `broadcast B`; without `IF\|` the first card), with DHCP or a static primary address. MUI editor: a form with Interface (a cycle of the cards and VLANs), IPv4 address, Netmask and Broadcast | `ifconfig IF alias SPEC` |
| VLANs | a list of up to 8 VLANs on the cards: `[CARD\|]ID [dhcp\|A.B.C.D/BITS\|up] [pcp=0-7\|pcp=priority] [mtu=N] [ipv4=on\|off] [ipv6=on\|off] [qos=on\|off] [tcpwindow=N]`, e.g. `10 dhcp`, `20 10.20.0.2/24 pcp=5 mtu=1400` or `eth1\|30 dhcp` (without `CARD\|`: the first card). MUI editor: a form with Network card (a cycle of the cards), VLAN ID (and the interface's name it gives), Address mode (DHCP, Static address, Up without an IPv4 address, Not started), IPv4 address, Priority (PCP: Default, 0-7 or From traffic priority), MTU, TCP window, IPv4, IPv6 and QoS (Default, On, Off) | `interface IF.ID vlan=ID parent=IF [dhcp\|address=A.B.C.D/BITS\|up] [pcp=...] [mtu=...] ...` |
| Names | host name, domain, up to three DNS servers; EDNS size (empty: default, 0 = off) and DNS over TCP (Default/On/Off) | `hostname`, `domain`, `nameserver`, `sysctl dns.edns_size=N`, `sysctl dns.tcp_fallback=0\|1` |
| Time | NTP on/off and servers (empty: from the DHCP lease, else pool.ntp.org), time zone (`auto` = the Locale preferences, or `+10:00`), mDNS responder (Default/On/Off) | `ntp SERVER...` / `ntp on` / `ntp off`, `timezone +HH:MM\|auto`, `mdns on\|off` |
| TCP | quick ACK (Default/Off/On), delayed ACK segments and time limit, ECN (Default/Off/On/Accept only) | `sysctl net.inet.tcp.quickack=`, `delack_segs=`, `delack_ms=`, `sysctl net.inet.tcp.ecn=` |
| Advanced: Connecting | Fast Open (Default/On/Off), Fast Open for every connect with a cookie, Fast Open backoff after lost SYNs (s, 0-86400), first and largest SYN RTO (ms, 200-3000 / 200-120000), SYN backoff (Linear/Doubling), MTU probing for path MTU black holes (Default/Off/On black hole/Always) | `sysctl net.inet.tcp.fastopen=`, `fastopen_all=`, `fastopen_blackhole=`, `syn_rto_initial_ms=`, `syn_rto_max_ms=`, `syn_backoff=`, `mtu_probing=` |
| Advanced: Ports | connect timeout (s, 0-600), ICMP unreachable during a connect (Keep trying / 2nd report / At once), TIME_WAIT reuse (Off/On/Loopback only), TIME_WAIT limit (0-65536), local port range (1024-65535) | `sysctl net.inet.tcp.connect_timeout=`, `syn_icmp_abort=`, `tw_reuse=`, `max_tw_buckets=`, `net.inet.ip.portrange.first=`, `.last=` |
| Advanced: Keepalive | keepalive for every TCP connection (idle seconds, 0 = off), idle time, probe interval (s, 1-32767), probe count (1-127) | `sysctl net.inet.tcp.keepalive_all=`, `keepidle=`, `keepintvl=`, `keepcnt=` |
| Advanced: Background | default congestion control (CUBIC/Reno/LEDBAT), background mode (Off/Receive/Send/Both), background programs (`AmiFTP,wget*`), remote and local ports (`20,21,6881-6889`), LEDBAT target (ms, 5-1000), LEDBAT slowdowns (On/Off) | `sysctl net.inet.tcp.cc=`, `bg_mode=`, `bg_programs=`, `bg_ports=`, `bg_lports=`, `ledbat_target=`, `ledbat_slowdown=` |
| Advanced: Memory | socket memory low water, pressure and high water (KB, 16-4194304; empty: from the free memory at start) | `sysctl net.inet.tcp.mem_low=`, `mem_pressure=`, `mem_high=` |
| Advanced: Happy Eyeballs | Happy Eyeballs (On/Off), attempt delay (ms, 10-2000), resolution delay (ms, 0-1000), prefer (RFC 6724 / IPv6 / IPv4), IPv6 lookup filter (On/Off), the longest time a negative DNS answer is cached (s; empty: 3600, 0 = off) | `sysctl net.inet.he.enable=`, `attempt_delay=`, `resolution_delay=`, `prefer=`, `aaaa_filter=`, `sysctl dns.cache.neg_maxttl=N` |
| Advanced: QoS | QoS bands (Default/On/Off), frames in flight (1-32), band limit (frames, 8-1024), priority sets DSCP (On/Off) | `qos on\|off`, `sysctl net.inet.ip.qos_inflight=`, `qos_limit=`, `qos_prio_dscp=` |
| Advanced: QoS rules | a list of up to 16 rules: `[proto=tcp\|udp] port=\|lport=\|rport=P,.. \| program=NAME` and `priority=interactive\|default\|bulk\|0-7` and/or `dscp=EF\|CS1\|AF41\|0-63`. MUI editor: a form with Protocol, Ports, Which end (either, this Amiga's, the other one's), Program, Priority and DSCP mark | `qos rule ...` |
| Advanced: Counters | none: every count Riptide keeps, read while the page is shown (MUI editor; see [below](#the-counters-page)). The GadTools editor has a note there (`netstat -s`, `ip -s link`) | none |
| Other lines | a list of the lines RiptidePrefs does not edit itself (e.g. `compat ...`, a fifth card, other routes, `background ...`); a line can be edited, replaced, added or deleted | as typed |

The "Advanced:" pages edit the settings of "Connecting, Fast Open,
keepalive, cork", "Path MTU black holes", "Background transfers", "Socket memory limits", "Happy
Eyeballs" and "Priorities and quality of service" in this document (the
extra addresses: "Several IPv4 addresses per interface"); Default (or an
empty field) leaves the line out, so Riptide's built-in value applies.

A screenshot of the first page shows the window titled "Riptide
Preferences": the Page cycle gadget at the top, the labelled fields of
the page below it (string gadgets, check boxes for IPv4 and IPv6), a hint line
("Interface changes need a restart of Riptide") and the buttons **Save**,
**Use**, **Apply** and **Cancel** along the bottom.

* **Save** writes `ENVARC:Riptide/riptide.conf` (the old one becomes
  `riptide.conf.bak`) and `ENV:Riptide/riptide.conf`; **Use** writes only
  `ENV:`. **Cancel** (or the close gadget, or Esc) changes nothing.
* **Apply** sends what a running Riptide can change at once through the
  RIPTIDE port: host name, domain, name servers (`nameserver flush` and
  `nameserver add`), NTP, time zone, mDNS, the sysctls, `qos on|off`, the
  QoS rules (`qos rules flush` and `qos rule ...`) and extra addresses
  (`ifconfig IF ADDR -alias`, `ifconfig IF alias SPEC`). It says when
  changes need a restart of Riptide: interface, address and IPv4/IPv6
  on/off, and a setting set back to Default or emptied (the running
  value stays until then). After Save or Use, RiptidePrefs offers the
  same when Riptide is running.
* The file is changed only where you changed a setting: comments, blank
  lines, unknown keywords, the order of lines, trailing `# comments` on
  edited lines and CRLF line ends stay as they were, so an unchanged file
  is written back byte for byte. New settings are added after related
  lines (a domain after the host name, name servers after `dhcp` or
  `ifconfig`) or at the end.
* Input is checked before anything is written; a requester names the
  field (e.g. "Address: enter an IPv4 address ...") and the cursor goes
  there. Empty number fields mean "Riptide's default" and leave the
  keyword out of the file.
* Keyboard: the underlined letter of a field activates it (or toggles a
  check box or cycle gadget; with Shift backwards), `p`/`P` switch pages,
  `s`, `u`, `a`, `c` are the buttons, Esc cancels. On the list pages
  (Extra addresses, VLANs, Advanced: QoS rules, Other lines) `l` edits
  the line, `d` adds it, `r` replaces and `e` deletes the selected one; an
  extra address, VLAN or QoS rule is checked before it goes into the
  list. Apply removes VLANs that were deleted or changed and creates the
  new ones at once (renaming the interface needs a restart).
* `USE` or `SAVE` on the command line copies the file without opening the
  window (`RiptidePrefs FROM=RAM:test.conf SAVE`, or `RiptidePrefs USE`
  to copy `ENVARC:` to `ENV:`).

### The MUI editor

The MUI editor is one window in a theme of its own (light or dark), after
the guide that Easel, Quill, Ledger and Folio were built by:

* **Left, the page list:** an icon and the name of each page, the
  settings pages under "Settings" and the others under "Advanced". The
  page shown has the accent bar; a page lights up under the pointer.
  `p` and `P` (or the cursor keys) go to the next and previous page.
* **Right, the page:** its head (a larger icon, the name and one or two
  sentences about it), then the settings, each with its label (the
  underlined letter is its key, as in the GadTools editor) and a note at
  the bottom. List pages have the list, a form with the settings of one
  entry and **Add**, **Replace**, **Delete**: the list shows each entry
  in words ("eth0.10: DHCP, PCP 4, MTU 1400"), clicking one fills the
  form, Add adds the form as a new entry and Replace puts it in place of
  the one chosen. A field that is wrong is named in a message and gets
  the cursor. The Other lines page edits a whole line instead. (The
  GadTools editor keeps one line to type on every list page.)
* **Below:** flat **Save**, **Use**, **Apply** and **Cancel** buttons
  (`s`, `u`, `a`, `c`; Esc cancels) and a status line (whether Riptide is
  running, and when there are changes not saved yet).
* **Help bubbles:** every setting, button and page explains itself when
  the pointer rests on it (its name in bold and what it does). They follow
  MUI's own bubble settings and can be turned off in the menu.
* **Menus** (every command; Amiga key in brackets): *Project*: Open...
  (O, a riptide.conf to edit), Save As... (A), Save (S), Use (U), Apply
  (Y), About... (?), Quit (Q). *Edit*: Reset to Defaults (D, every
  setting back to Riptide's default; lines the editor does not know stay),
  Last Saved (L, `ENVARC:`), Restore (R, what the editor started with).
  *Settings*: Dark Theme (T), Help Bubbles (H). Open and Save As need
  asl.library.
* **Theme:** the colours are a light and a dark table of named colours,
  taken as pens with `ObtainBestPen()` on the window's screen, so on a
  palette screen (AGA, 256 colours or fewer, even the 4-colour Workbench)
  they share or approach the screen's colours; lines and the chosen page
  keep a colour of their own there. The icons are drawn in code from
  short vector descriptions, at any size. The text uses the screen's font
  (an interface font of RiptidePrefs' own may follow; the drawing code
  asks for it in one place). The theme and the bubble switch are kept in
  `ENVARC:Riptide/RiptidePrefs.prefs` (`dark=1`, `bubbles=0`).
* **Screen sizes:** the window fits a 640 × 480 or 640 × 512 screen and
  grows with larger ones. Below 400 lines (the 640 × 256 PAL Workbench)
  it uses a compact layout: the page list without icons and headings, the
  head on one line, closer rows. When even that does not fit, RiptidePrefs
  opens the GadTools editor.

The MUI editor is in `src/amiga/prefs/mui_*.c`: `mui_main.c` (the window,
menus and the one `command()` switch every button, key and menu item goes
through), `mui_classes.c` (the page list, head, buttons, labels, status
line and separators, custom classes of MUI's Area class), `mui_theme.c`
(colours, pens, drawing, icons, bubbles), `mui_lang.c` (`tr()`: every
string is looked up there, so the editor can be translated) and `mui.h`
(the MUI definitions it uses, written for Riptide and checked against the
MUI SDK by `tests/amiga/check_layouts.py`). The pages and settings come
from `prefs_fields.c`, which the GadTools editor (`gt_prefs.c`) builds its
pages from too.

### The Status page

The first page of the MUI editor (and the last of the GadTools one) shows
what Riptide is doing, read again while the page is on screen (every
second, or as the **Update** setting under the tabs says: every 2 or 5
seconds, or Paused, which keeps what is shown; it is kept in
`ENVARC:Riptide/RiptidePrefs.prefs`); on other pages, or with the window
closed, nothing is read. It changes nothing. Each second RiptidePrefs asks Riptide for its tables
(`net.riptide.status`, `ifaces`, `tcpinfo`, `tcpstat`, `ipstat`; every
fifth second also `addrs6`, `routes` and `dns`) with one `RTCMD_TABLES`
message on the [RIPTIDE port](#the-riptide-port), so it opens no library
and never keeps Riptide from quitting (with Riptide 0.22 or older it
opens `riptide.library` for each read and closes it again). When Riptide
is not running the page says so, looks for it only every 3 seconds and
draws nothing until it starts; then it fills in by itself. A Riptide that
answers without figures (0.22 and older have no `net.riptide.status`) is
shown as running, without uptime, CPU and memory.
The graphs scroll by one column per read and only the text that changed
is drawn again. Still, drawing on a planar screen is work for a 68020:
on an emulated A1200 with a 640x480 256-colour screen, a download that
takes all the CPU ran 10 to 17 % slower while the Status page was on
screen updating every second than with RiptidePrefs on another page,
and 6 to 11 % slower updating every 5 seconds (three runs each; the
reads themselves take 20 to 60 ms). Set Update to every 5 seconds or
Paused, leave the page, or close the window, when you need the last bit
of speed.

* **At the top:** whether Riptide runs, its version and uptime, the share
  of the CPU its process used in the last second, and the memory sockets
  and packet buffers use (and whether they are under pressure or at the
  limit, see [Socket memory limits](#socket-memory-limits)).
* **Graphs** (the last minute, one column a second, the scale to the
  highest value): all traffic of every interface but loopback (received:
  columns, sent: a line) and the CPU share. The CPU share comes from
  `net.riptide.status`: reading it starts a meter in the Riptide process
  for 12 seconds (a vertical blank interrupt server counts the 50 or 60
  ticks a second in which the process was waiting for work), so it costs
  nothing when nobody reads it.
* **Tabs** (click, or keys `1` to `4`, cursor left and right):
  *Interfaces*: each interface (VLANs too) in three lines (state, driver
  or VLAN, MTU; IPv4 address, gateway, DHCP state and lease left or
  link-local; IPv6 addresses), its speeds in and out, and its own graph;
  when not all fit, a line says which are shown and a click on the list
  shows the next ones.
  *Connections*: every TCP connection in two lines, both ends in full
  first (`192.168.1.2:1025  to  93.184.216.34:80`, a listener only its
  own), then its state, round trip time, window in packets and ECN,
  speed, and the program that opened it (from 0.23), moving ones first; click one to see its round trip time (line)
  and window (columns) over the last minute below. *Names and time*: host
  name, name servers, name cache hits, and NTP (server, when it last
  synced, the clock's offset then, round trip, next sync, why the last
  try failed). *Counters*: connections open and made, segments resent
  (also in the last second), timeouts and recoveries, ECN, path MTU black
  holes, drops for memory and IP drops.
* **GadTools editor:** the same in seven lines of text (summary, memory,
  the first interface but loopback with its speeds, TCP, time), refreshed
  every second by a timer (every 3 s while Riptide is not running); only
  the lines that changed are drawn again.

The arithmetic and wording are in `src/amiga/prefs/prefs_status.c`
(host-tested by `tests/t_prefs_status.c`: rates over counters that wrap,
the CPU share, scales, history, connections matched between reads, every
line of text); reading the tables in `status_amiga.c`, the MUI page and
its classes (graph, tabs, table, interface list, text) in `mui_status.c`.

### Several cards

The first four `interface` lines with `device=`, each with its `dhcp`/
`ifconfig`/`route add default`/`ifconfig IF alias` lines and its VLANs
(`interface IF.ID vlan=ID parent=IF` with `dhcp`, `address=`, `up`,
`pcp=`, `mtu=`, `tcpwindow=`, `ipv4=`, `ipv6=` and `qos=`) are on the
Interface, Address, Extra addresses and VLANs pages; a fifth card, other
interfaces (VLAN lines with other options or names among them) and other
routes appear under Other lines.

* **Interface page (MUI):** the list of cards ("eth1: a2065.device unit
  0, DHCP"); clicking one shows its settings below, and the Address page
  then shows its address. **New card** adds one with the next free name
  (`eth1`) and puts the cursor in its Device field; **Remove card**
  removes the card chosen with its VLANs and their addresses (the first
  card stays). Renaming a card renames its VLANs and addresses too.
* **Gateways:** the first card's router is `route add default GW`; another
  card's is `route add default GW dev IF metric 10*N` (N its place in the
  list, or the metric the line had), so the first card's route is used
  first and the others take over when it goes.
* **Checks:** two cards with the same name, or the same device and unit,
  and a VLAN or extra address on a card that is not there are refused,
  and the editor shows the card or the entry.
* **GadTools editor:** it edits the first card (its 7 rows leave no room
  for a chooser); the other cards and their lines stay as they are, and
  an error on another card names it and points to the MUI editor.

### The Counters page

Advanced: Counters (MUI editor) shows every count Riptide keeps, grouped
and in plain words: for each card *Interface X* (packets, bytes, errors,
drops, multicasts), *Driver of X* (the SANA-II/MNI counters) and *QoS of
X* (the bands), then *IPv4*, *ICMP*, *IGMP (multicast)*, *UDP*, *TCP*,
*IPv6*, *ICMPv6, Neighbor Discovery, MLD*, *Names (DNS cache)* and
*Memory*: name, value and the change per second. These are the tables
`netstat -s` and `ip -s link` read. The list has a scroll bar, and only
lines that changed are drawn again.

* It reads at the Status page's **Update** setting while it is shown,
  and **Refresh** (`f`) reads at once.
* **Zero** (`z`) shows the counts since then ("Counts since Zero, 2 min
  ago."); Riptide's own counters stay as they are. **Since start** (`t`)
  goes back to the counts since Riptide started. Levels such as the
  memory in use are not zeroed.
* When Riptide is not running the page says so and looks again every 3
  seconds.

The rows, rates and Zero are in `src/amiga/prefs/prefs_counters.c`
(host-tested by `tests/t_prefs_status.c`), the page in `mui_counters.c`. The background rules are edited as their sysctls; a
`background ...` keyword line stays under Other lines (it sets the same
values, so keep only one of the two). List entries that stay keep their
line (and its comment) when others are added, replaced or deleted.

## Tools

Riptide comes with the usual Linux network commands, written for AmigaOS:
`ip`, `ifconfig`, `route`, `arp`, `netstat`, `traceroute`, `ping`, `host`,
`sysctl`, `hostname`, `nc`, `ftp`, `speedtest` and `rtdiag`. Copy them to `C:`. They take Linux style
arguments (`-n`, `-c 3`, ...), print what their Linux counterparts print,
stop on Ctrl-C and return 0 (ok), 5 (warning, e.g. no ping reply), 10
(error) or 20 (failure, e.g. no stack). Each has `-h` / `--help`. They
need little stack (buffers come from AllocVec()).
`ping6` and `traceroute6` are `ping -6` and `traceroute -6` (see "IPv6").

Where the data comes from:

* `ping`, `traceroute`, `host` (A/PTR), `nc`, `ftp` and `speedtest` use
  only the standard bsdsocket.library API, so they also work with other
  stacks.
* `ifconfig` (changes), `route add/del` and `arp -s/-d` use the BSD ioctls
  (SIOCAIFADDR, SIOCSIF*, SIOCADDRT/SIOCDELRT, SIOCSARP/SIOCDARP).
* Listings read Riptide's tables with `RiptideSysCtl()` (below), and `ip`,
  `sysctl` and `hostname NAME` send configuration lines to the RIPTIDE
  port.

### ip

iproute2 syntax and output; objects and commands can be abbreviated (`ip
a`, `ip r`, `ip n`, `ip l`). Options: `-s` statistics, `-d` details (shows
the driver), `-br` brief, `-4`. `ip maddr [show [dev IF]]` lists the
multicast memberships of each interface (link layer, IPv4 and IPv6
groups, `users N` when shared; `-d` adds the IGMP version spoken).

```
1> ip a
1: lo0: <LOOPBACK,MULTICAST,UP,LOWER_UP> mtu 16384 state UNKNOWN mode DEFAULT
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
    inet 127.0.0.1/8 scope host lo0
       valid_lft forever preferred_lft forever
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP mode DEFAULT
    link/ether 52:54:00:12:34:56 brd ff:ff:ff:ff:ff:ff
    inet 10.0.2.15/24 brd 10.0.2.255 scope global dynamic eth0
       valid_lft 86123sec preferred_lft 86123sec
1> ip r
default via 10.0.2.2 dev eth0 proto dhcp src 10.0.2.15
10.0.2.0/24 dev eth0 proto kernel scope link src 10.0.2.15
127.0.0.0/8 dev lo0 proto kernel scope link src 127.0.0.1
```

| Command | |
|---|---|
| `ip link [show [dev] IF]` | interfaces (`-s`: counters) |
| `ip link set [dev] IF up\|down\|mtu N\|promisc on\|off\|arp on\|off` | |
| `ip link set [dev] IF tcpwindow N` | largest TCP receive window on connections over IF (bytes, 0 = no limit; see "TCP window per interface") |
| `ip link set [dev] IF reads N` | IPv4 read requests queued with a SANA-II driver (4..128) |
| `ip link add NAME type sana2 device DEV [unit N] [mtu N] [reads N]` | open a SANA-II device at run time |
| `ip link add NAME type mni driver FILE [unit N] [options TEXT]` | load an MNI driver |
| `ip link add link IF name NAME type vlan id ID [pcp N\|priority] [mtu N]` | a VLAN interface on IF (see "VLAN interfaces"); `ip link` shows it as `NAME@IF`, `-d` adds `vlan protocol 802.1Q id ID` |
| `ip link del [dev] NAME` | close a driver interface or remove a VLAN |
| `ip link resetstats [[dev] IF]` | **Riptide extension:** zero the counters of IF (all interfaces without IF) |
| `ip addr [show [dev] IF]` | addresses; DHCP addresses are `dynamic` with the lease time left |
| `ip addr add\|replace A.B.C.D/BITS [peer P] [broadcast B] dev IF` | `add` adds a further address (shown as `secondary`, up to nine per interface), `replace` changes the primary one |
| `ip addr del A.B.C.D[/BITS] dev IF`, `ip addr flush dev IF` | |
| `ip route [show] [cache]` | routes with `proto kernel/static/dhcp/redirect`; `cache` lists path MTU entries |
| `ip route add\|del\|replace\|change\|append PREFIX\|default [via GW] [dev IF] [metric N] [mtu N]` | also `ip route add unreachable PREFIX` |
| `ip route get ADDR` | which route a packet would take |
| `ip route flush dev IF\|cache\|all` | |
| `ip neigh [show [dev IF]]` | ARP table: `10.0.2.2 dev eth0 lladdr 52:55:0a:00:02:02 REACHABLE` |
| `ip neigh add\|replace ADDR lladdr MAC [dev IF] [nud permanent]` | entries are always permanent |
| `ip neigh del ADDR [dev IF]`, `ip neigh flush [dev IF]` | |
| `ip dhcp [show] [dev IF]` | **Riptide extension:** DHCP state, server, router, lease, the number of classless static routes, the link-local address (see "DHCP") |
| `ip dhcp start\|stop\|renew [dev] IF` | **Riptide extension** |
| `ip dns [show]`, `ip dns add\|del ADDR`, `ip dns flush`, `ip dns search DOMAIN...`, `ip dns domain DOMAIN` | **Riptide extension:** resolver configuration (resolv.conf style output) |

**Driver counters.** For SANA-II and MNI interfaces `ip -s link` adds what
the driver side saw since the last `ip link resetstats`; `ip -s -s link`
(or `-ss`) adds the details:

```
1> ip -s -s link show eth0
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP mode DEFAULT
    ...
    driver sana2 uaenet.device unit 0, counters of the last 179 s
    RX reads: IPv4 48/48 lowest 41 ran out 0, ARP 4/4 lowest 4 ran out 0, IPv6 24/24 lowest 24 ran out 0
    RX: frames 5120 bytes 7421440 errors 0 nobuf 0, wakeups 830, at most 17 frames per wakeup
    TX: frames 2600 bytes 156000 errors 0 queue full 0, in flight 0 (most 3 of 32)
    driver stats: rx 5120 tx 2600 bad 0 overruns 0 unknown types 0
    RX frames per wakeup: 1: 410 2-3: 250 4-7: 120 8-15: 45 16-31: 5 32+: 0
    device query: hardware type 1, level 0, mtu 1500, raw mtu 0, 10000000 bit/s
    driver stats: reconfigurations 0, online since 0, 0.2 s old
    special stats: none
```

| Line | |
|---|---|
| `RX reads` | per packet type: reads queued now / configured, the fewest the driver still held when Riptide woke up to collect frames, and how often it held none. **`ran out` above 0 means the driver probably dropped frames**: raise `reads=N` on the `interface` line. For MNI drivers one buffer pool serves every type (`RX buffers`) |
| `RX` | frames and bytes the driver delivered, read errors, receive buffers Riptide could not allocate (`nobuf`), wakeups that collected frames and the largest batch |
| `TX` | frames handed to the driver, errors, packets dropped because every write request was busy (`queue full`), writes outstanding now and at most |
| `driver stats` | the driver's own `S2_GETGLOBALSTATS` (`overruns` are frames the hardware missed); MNI drivers report `driver dropped` (frames refused for lack of a buffer) instead |
| `RX frames per wakeup` | how many frames each wakeup collected: large batches mean frames arrive faster than Riptide gets to run |
| `RX/TX errors` | error codes and counts (`IOERR_*`, `S2ERR_*`, MNI status) |
| `special stats` | `S2_GETSPECIALSTATS` (retries, FIFO underruns, ... as the driver names them) |

Riptide asks the driver for its statistics when they are read (outside the
stack lock), so `ip -s link` waits 0.2 s for fresh ones when the last
query is older than half a second. Driver statistics are not reset.

### ifconfig

net-tools output (`ifconfig` lists the interfaces that are up, `-a` all,
`-s` the short table). The extra `device` line names the driver; `RX ...
overruns` are the frames the driver says it missed. `-v` adds two lines of
driver counters (IPv4 reads queued/configured, the lowest, how often they
ran out, wakeups, the largest batch, writes in flight); see `ip -s link`.

```
1> ifconfig eth0
eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500
        inet 10.0.2.15  netmask 255.255.255.0  broadcast 10.0.2.255
        ether 52:54:00:12:34:56  txqueuelen 1000  (Ethernet)
        RX packets 1210  bytes 123414 (120.5 KiB)
        ...
        device uaenet.device unit 0
1> ifconfig eth0 10.0.2.20 netmask 255.255.255.0 up
1> ifconfig eth0 mtu 1400 promisc
```

Settings: `A.B.C.D[/BITS]`, `netmask`, `broadcast`, `pointopoint`, `mtu`,
`metric`, `[-]promisc`, `[-]arp`, `[-]allmulti`, `del A.B.C.D`, `up`,
`down`. A new address without a netmask gets the classful one, as on Linux.
`ifconfig NAME create vlan ID vlandev IF [vlanpcp N]` and `ifconfig NAME
destroy` add and remove VLAN interfaces (FreeBSD syntax).

### route

```
1> route -n
Kernel IP routing table
Destination     Gateway         Genmask         Flags Metric Ref    Use Iface
0.0.0.0         10.0.2.2        0.0.0.0         UG    0      0       12 eth0
10.0.2.0        0.0.0.0         255.255.255.0   U     0      0        3 eth0
127.0.0.0       0.0.0.0         255.0.0.0       U     0      0        0 lo0
1> route add default gw 10.0.2.2
1> route add -net 172.16.0.0 netmask 255.255.0.0 gw 10.0.2.2
1> route add -host 172.17.0.1 gw 10.0.2.2
1> route del -net 172.16.0.0 netmask 255.255.0.0
```

Without `-n` the gateway is looked up by name. `-e` shows the MSS column,
`-C` the path MTU cache. `metric N` is passed through the RIPTIDE port
(struct ortentry has no metric).

### arp

```
1> arp -n
Address                  HWtype  HWaddress           Flags Mask            Iface
10.0.2.2                 ether   52:55:0a:00:02:02   C                     eth0
1> arp -an
? (10.0.2.2) at 52:55:0a:00:02:02 [ether] on eth0
1> arp -s 10.0.2.4 52:55:0a:00:02:04
1> arp -d 10.0.2.4
```

Flags: `C` complete, `M` permanent. Proxy ARP (`pub`) is not supported.

### netstat

`netstat [-a] [-n] [-t] [-u] [-w] [-l] [-e] [-c]` lists sockets (default:
connected ones; `-a` all, `-l` listening; `-e` adds RTT, cwnd and
retransmissions of TCP), `-r` the routing table, `-i` the interface
counters, `-s` the protocol statistics (with an `Igmp:` section), `-g`
(`--groups`) the multicast group memberships, `-T` (`--tcp-info`) the details
of each TCP connection; `-c` repeats every second until Ctrl-C.

```
1> netstat -an
Active Internet connections (servers and established)
Proto Recv-Q Send-Q Local Address           Foreign Address         State
tcp        0      0 0.0.0.0:7778            0.0.0.0:*               LISTEN
tcp        0      0 127.0.0.1:7777          127.0.0.1:53584         ESTABLISHED
udp        0      0 0.0.0.0:68              0.0.0.0:*
raw        0      0 0.0.0.0:1               0.0.0.0:*
```

As on Linux, the "port" of a raw socket is its protocol (1 = ICMP).

`netstat -T [-4|-6] [-a|-l] [-n] [-c]` prints what TCP knows about each
connection, in the spirit of Linux `ss -ti`. It is the first thing to
look at when a transfer is slow:

```
1> netstat -Tn
TCP connection details (w/o servers)
tcp  10.0.2.15:1027 10.0.2.2:21 ESTABLISHED
  cubic open age 12.4s  rtt 3.2/1.1 ms min 1.0 rto 204 ms backoff 0
  opts ts sack ws wscale 7/2 mss 1460/1460 advmss 1460 cwnd 10 ssthresh -
  wnd snd 65535 rcv 32768 (limit 32768)  buf rcv 32768 snd 16384
  queue recv 0 send 0 ooo 0/0 seg  inflight 0 sacked 0 lost 0
  rx 1048576 bytes, 731 segs (720 with data), 84 KB/s average
  tx 120 bytes acked, 402 segs (6 with data)
  acks out 396 (delayed 12, dup 0, sack blocks 0)  dupacks in 0
  retrans 0  fast recovery 0  rto 0  tlp 0  zero wnd out 0 in 0
  time busy 120ms rwnd-limited 0ms sndbuf-limited 0ms
       unread 2340ms zero-wnd 0ms  last rx 10ms tx 1200ms ack 10ms ago
```

* **rtt** smoothed RTT / variation, the lowest RTT seen, the
  retransmission timeout; **mss** send/receive.
* **wnd snd** is the peer's window, **rcv** the one we advertise (bytes,
  after scaling) and **limit** how far it may grow; **buf** the
  (auto-tuned) socket buffers.
* **queue**: bytes not yet read by the program, not yet acknowledged by
  the peer, and held out of order waiting for a lost segment.
* **rx/tx**: bytes and segments; the average rate is over the life of the
  connection.
* **acks out**: pure ACKs we sent; *delayed* ones came from the delayed
  ACK timer, *dup* ones were sent while data was missing (loss), *sack
  blocks* counts the SACK blocks in them. **dupacks in**: duplicate ACKs
  from the peer (our data was lost).
* **retrans**: segments resent; **fast recovery** and **rto** count
  recoveries from loss (an RTO means a stall of at least 200 ms); **tlp**
  tail loss probes; **zero wnd** windows of 0 we advertised / received.
* **time busy**: time with data of ours in flight; of that,
  *rwnd-limited* is time the peer's window held us back and
  *sndbuf-limited* time the program waited for buffer space (as in
  Linux, the later condition wins). **unread**: time received data sat
  waiting for the program to read it; **zero-wnd**: time our window was
  0. A download whose `unread` time grows while `zero-wnd` stays 0 is
  limited by the program, not the network. **last rx/tx/ack**: when data
  last arrived, data last left, and the peer last acknowledged (`-`:
  never).

The same numbers are in the `net.riptide.tcpinfo` table and, for one
socket, `getsockopt(IPPROTO_TCP, TCP_INFO)`: `struct rt_tcp_info` in
`include/rt/rt_bsd.h` grew the Riptide counters at its end (a caller with
the shorter 0.10 structure gets exactly what it asked for).

### traceroute

`traceroute [-n] [-I] [-m max_ttl] [-q nqueries] [-w waittime] [-p port]
[-f first_ttl] HOST [packetlen]`. UDP probes to port 33434 and up by
default, ICMP echo with `-I`; hop names are looked up unless `-n`.
Probes go one after the other (Linux sends them in parallel), and `!H`,
`!N`, `!P`, `!X`... mark unreachable answers.

```
1> traceroute 127.0.0.1
traceroute to 127.0.0.1 (127.0.0.1), 30 hops max, 60 byte packets
 1  localhost (127.0.0.1)  11.853 ms  11.825 ms  12.319 ms
```

### ping

```
ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=ms] [TIMEOUT=ms] [QUIET]
ping [-c count] [-s size] [-i seconds] [-W seconds] [-t ttl] [-q] [-n] HOST
```

Both forms can be mixed. The default count is 4; `-c 0` (`COUNT=0`) pings
until Ctrl-C. Output follows Linux ping, including `(DUP!)`, ICMP errors
(`From 10.0.2.15 icmp_seq=1 Destination Host Unreachable`) and the
`rtt min/avg/max/mdev` summary.

### host

`host NAME` and `host A.B.C.D` resolve through the stack (static table,
cache, DNS). With `-t TYPE` (A, NS, CNAME, SOA, PTR, MX, TXT, AAAA, ANY)
or a server argument (`host -t mx amiga.org 10.0.2.3`) host sends its own
DNS query and prints the answer records; `-v` shows the header, `-W` the
timeout in seconds. Its query offers EDNS0 (1232 bytes; without it again
when the server answers FORMERR or NOTIMP), and a truncated answer is
asked again over TCP (`;; Truncated, retrying in TCP mode.`).

### sysctl, hostname, nc

* `sysctl -a`, `sysctl [-n] NAME...` (a prefix such as `net.inet.tcp`
  lists everything below it), `sysctl [-w] NAME=VALUE`.
* `hostname [-s|-f|-d|-i|-I] [NAME]` shows or sets the host name.
* `nc [-d] [-v] [-w secs] HOST PORT` and `nc -l [-p] PORT` (accept one
  connection): a minimal netcat for testing. Standard input is sent first,
  then the reply is copied to standard output (AmigaDOS input cannot be
  waited on together with a socket); `-d` sends nothing; `-u` sends stdin
  as one UDP datagram. `nc -u -l [-v] [-w secs] [-c count] PORT` prints
  received datagrams (until `-w` seconds of silence or `count` of them);
  `-m GROUP` joins an IPv4 multicast group first (on the interface with
  address `-I ADDR`). When sending, `-T ttl`, `-I ADDR` and `-L` set
  `IP_MULTICAST_TTL`, `IP_MULTICAST_IF` and `IP_MULTICAST_LOOP` 0:
  `nc -u -T 4 239.1.2.3 5000 <file`. `-F` connects with TCP Fast Open
  (`TCP_FASTOPEN_CONNECT`), `-K SECS` sets `SO_KEEPALIVE` with
  `TCP_KEEPIDLE` and `TCP_KEEPINTVL` SECS.

### ftp

An FTP client in the style of the BSD/Linux `ftp`:

```
ftp [-n] [-v] [-p|-A] [-i] [-d] [-t secs] [host [port]]
ftp [-o localfile] ftp://[user[:pass]@]host[:port]/path
ftp -u ftp://[user[:pass]@]host[:port]/[path] localfile...
```

`-n` no automatic login, `-v` verbose (the default when input is a
console), `-p` passive (default) / `-A` active mode, `-i` no prompting in
`mget`/`mput`, `-d` show the protocol, `-t` the network timeout (default
60 s). Commands are read from standard input, so `ftp -n host 2121
<S:ftpscript` runs a script (lines starting with `#` are comments; with
`-v` each command is echoed). The URL forms fetch one file (to the file
name of the URL, or `-o`; `-o -` writes to standard output; a path ending
in `/` lists the directory) or upload files, log in as `anonymous` unless
the URL names a user, print the transfer statistics and exit with 0 or 10.

```
1> ftp 10.0.2.2 2121
Connected to 10.0.2.2 (10.0.2.2).
220 pyftpdlib 1.5.10 ready.
Name (10.0.2.2:anonymous):
331 Username ok, send password.
Password:
230 Login successful.
Remote system type is UNIX.
Using binary mode to transfer files.
ftp> get big.lha
229 Entering extended passive mode (|||49731|).
150 File status okay. About to open data connection.
226 Transfer complete.
1048576 bytes received in 3.21 secs (319.0 KB/s)
```

| Command | |
|---|---|
| `open host [port]`, `user name [pass]`, `close`/`disconnect`, `bye`/`quit`/`exit` | connection; the password is read without echo (console raw mode) |
| `ls`/`dir [path [localfile]]`, `nlist [path [localfile]]` | LIST / NLST |
| `cd dir`, `cdup`, `pwd`, `lcd [dir]`, `lpwd` | remote and local (AmigaDOS) directory |
| `get`/`recv remote [local]`, `put`/`send local [remote]`, `append local [remote]` | the local name defaults to the last part of the remote one (and vice versa); `-` is standard output |
| `mget pattern...`, `mput pattern...` | `mget` expands patterns on the server (NLST), `mput` with AmigaDOS patterns (`#?`, `*` and `?`); `prompt` toggles the questions (y/n/a/q) |
| `restart offset`, `reget remote [local]` | REST: the next `get`/`put` starts at offset; `reget` resumes from the size of the local file |
| `delete`, `mkdir`, `rmdir`, `rename from to`, `size`, `modtime` | |
| `binary`/`image`, `ascii`, `type [ascii\|binary]` | binary is the default after login; ascii converts CRLF ⇄ LF |
| `passive [on\|off]`, `epsv [on\|off]` | passive mode (default on): EPSV, then PASV |
| `hash [on\|off\|bytes]` | a `#` per 1024 bytes (or the given size) |
| `verbose`, `debug`, `prompt`, `timeout [secs]`, `status`, `help`/`?` | |
| `quote`/`literal line`, `site args`, `system` | raw commands; the reply is always shown |

Commands can be abbreviated. Notes:

* **Passive mode and NAT.** The address in a `227` (PASV) reply is used
  only when it is the control connection's peer; otherwise the data
  connection goes to the peer address (as curl and modern clients do), so
  servers behind NAT or advertising a private address work. EPSV (RFC
  2428) carries no address at all; a server that refuses it gets PASV from
  then on. Active mode (`passive off`, `-A`) sends `PORT` with the local
  address and needs the server to reach it: it does not work through
  slirp/NAT.
* **Ctrl-C** during a transfer closes the data connection, sends `ABOR`
  and reads the server's replies, so the session goes on (`receive
  aborted. waiting for remote to finish abort.`); the partial file stays
  for `reget`. At the prompt Ctrl-C prints a hint; press it twice to
  quit. When input is a script, Ctrl-C between commands ends it.
* Transfers use a 32 KB buffer (AllocVec) and AmigaDOS Read/Write; write
  errors (disk full) abort the transfer. Timing comes from timer.device.
* IPv6 servers work with EPSV and EPRT (RFC 2428). There is no
  `.netrc`, proxy, FXP, LPSV, TLS or `!` shell escape; offsets are limited to 2 GB.

**Testing transfers against your computer.** Under FS-UAE with slirp NAT
(Amiga 10.0.2.15, host 10.0.2.2) run an FTP server on the host, for
example pyftpdlib:

```
python3 -m pip install pyftpdlib
mkdir -p ~/ftptest
python3 -m pyftpdlib -p 2121 -w -d ~/ftptest      # anonymous, writable
```

then on the Amiga `ftp 10.0.2.2 2121`, log in as `anonymous` (any
password) and `put`/`get` files; passive mode works through slirp. The same
as a script: `ftp -v -n 10.0.2.2 2121 <S:ftptest` with `user anonymous
x`, `put RAM:test.bin`, `get test.bin RAM:back.bin`, `bye` in the file.
Without a server on the host, `ftpd-test` (below) serves an AmigaDOS
directory over loopback.

### ftpd-test

`ftpd-test [-p port] [-1] [-v] ROOT` (built in `build/amiga/ftpd-test` from
`tests/amiga/ftpd_test.c`) is a minimal FTP server **for testing only**: one
session at a time, any user and password are accepted, no access control.
It serves the AmigaDOS directory ROOT (default port 2121) with USER, PASS,
SYST, TYPE, PWD, CWD, CDUP, PASV, EPSV, PORT, LIST, NLST, RETR, STOR, APPE,
SIZE, MDTM, DELE, MKD, RMD, RNFR/RNTO, REST, FEAT, HELP, ABOR and QUIT, plus
`SITE NOEPSV` and `SITE PASVIP a.b.c.d` to test the client's EPSV fallback
and NAT handling. `-1` exits after one session, `-v` logs the protocol.

```
1> run >NIL: ftpd-test RAM:
1> ftp 127.0.0.1 2121
```

### speedtest

A speedtest.net bandwidth test with the options and output of
[speedtest-cli](https://github.com/sivel/speedtest-cli) (Matt Martz,
Apache-2.0), the reference for its behaviour; an original C
implementation of the legacy HTTP protocol (`src/apps/speedtest`).

```
speedtest [--simple|--json|--csv [--csv-delimiter C]] [--bytes] [--single]
          [--no-download] [--no-upload] [--server ID]... [--exclude ID]...
          [--timeout SECS] [--list] [--csv-header] [--version]
speedtest --server-url http://HOST:PORT/speedtest/upload.php [options]
speedtest --mirror http://HOST[:PORT] [options]
```

```
Retrieving speedtest.net configuration...
Testing from Example ISP (203.0.113.7)...
Retrieving speedtest.net server list...
Selecting best server based on ping...
Hosted by Example Sponsor (Sydney) [12.34 km]: 23.456 ms
Testing download speed................................
Download: 12.34 Mbit/s (1506.3 KB/s)
Testing upload speed....................
Upload: 1.23 Mbit/s (150.1 KB/s)
```

The figure in brackets is the same rate in KB/s (1024 bytes per second,
as `ftp` shows it); `--json` and `--csv` report bits per second as
speedtest-cli does.

It reads `speedtest-config.php` and the server list (read as it arrives;
distances in fixed point, no FPU needed), times three `latency.txt`
requests to each of the 5 nearest servers, then downloads
`random350x350.jpg` ... `random4000x4000.jpg` over 4 connections and
uploads 128 KB to 2 MB bodies over 2 (the configuration's `threads`, at
most 4), 10 seconds each (its `testlength`). All connections are
non-blocking keep-alive HTTP/1.1, served by one WaitSelect() loop: Ctrl-C
works at any time (exit code 5). Riptide extras: `--server-url URL` (test
against that server without the configuration and list), `--mirror URL`
(read those from another http:// base), `--duration SECS`, `--threads N`,
`--count N` (requests per size), `--download-sizes 350,...`,
`--upload-sizes KB,...` and `--debug`.

**HTTPS.** There is no TLS, so no `--secure` or `--share`. speedtest.net
may serve the configuration and list only over HTTPS; speedtest then says
so and stops. The test servers (`http://host:8080/speedtest/`) speak http:
use `--server-url` with a server's `url` from a list fetched elsewhere
(`curl -s https://www.speedtest.net/speedtest-servers-static.php`).

**Testing against your computer.** `tools/speedtest_mock.py` (python3)
plays speedtest.net and a test server, pointing its server list back at
itself; `/__stats` shows what it counted. Options: `--chunked`,
`--redirect`, `--https-redirect`, `--delay MS`, `--rate KB/s`, `--close`,
`--size-div N`, `--servers N`. Under FS-UAE with slirp:

```
python3 tools/speedtest_mock.py --port 8080         # on the host
1> speedtest --mirror http://10.0.2.2:8080
1> speedtest --server-url http://10.0.2.2:8080/speedtest/upload.php
```

`httpd-test [-p port] [-d div] [-c] [-r] [-s] [-k] [-v]` (in
`build/amiga`, not in the package) does the same on the Amiga, and
`make` builds `build/host/speedtest` for Linux.

**Speed.** On a 68020 the result is CPU bound. In FS-UAE (A1200, cycle
exact) with speedtest, `httpd-test` and Riptide on one CPU over loopback:
about 0.5-1.2 Mbit/s down, 1.2-1.3 Mbit/s up and 100 ms ping. Timestamps use
the Amiga clock (local time).

### rtcheck

`rtcheck [-v|VERBOSE] [START=cmd] [STOP=cmd] [CONNECT=a.b.c.d:port] [NOCHECK]` is the
library test client. Without arguments it runs about 80 checks against the
running stack over loopback. `CONNECT=` runs a TCP echo test through a
real interface, against an echo server on the other end. `START=` starts a
command in the background and waits until bsdsocket.library appears;
`NOCHECK` then exits without running the checks. `-v` (or `VERBOSE`) prints
every check on its own numbered line (`ok` or `FAIL`), grouped by section;
without it only failures and the summary are shown.

### stackprobe

`stackprobe [QUICK] [PERF] [PEER=host] [ONLY=area,...] [JSON=file] [CSV=file] [NAME=label] [VERBOSE]`
tests which TCP/IP features the running stack offers and how they behave,
through the public Amiga APIs only, so the same program runs under any
stack: AmiTCP, Miami, Genesis, Roadshow, AmiTCP_NG, PrismNet, tolunnet
or Riptide. It is plain 68000 code and needs nothing but a running stack.

Every test ends as `PASS`, `FAIL` (with the error), `--`
(unsupported: the call is not in the library's jump table, or the stack
answers ENOPROTOOPT/EOPNOTSUPP), `skip` (needs a network or `PEER=`) or
`info` (a value, not a verdict). The areas:

| Area | What it tries |
|---|---|
| `libs` | which libraries and ports exist: bsdsocket, usergroup, miami, the AS225 socket and inet libraries, miamibpf/miamipcap, AmiTCP 4's ifconfig.library, riptide |
| `api` | every call of the AmiTCP bsdsocket.library table once (socket to GetSocketEvents) |
| `tags` | every `SocketBaseTagList()` code of the AmiTCP SDK, the set-and-read-back ones, the errno pointers of 1, 2 and 4 bytes |
| `netdb` | hosts and reverse lookups, services, protocols, networks |
| `tcp` | loopback transfer with the data checked; half close; resets (SO_LINGER 0, close with unread data, send after a reset); refused; non-blocking connect, accept and recv; backlog; EADDRINUSE and SO_REUSEADDR in TIME_WAIT; ephemeral ports; MSG_PEEK, MSG_DONTWAIT, MSG_WAITALL; urgent data (MSG_OOB, the except set, SIGURG, SIOCATMARK, SO_OOBINLINE); FIOASYNC |
| `udp` | loopback datagrams; truncation; 1.5 to 64 KB datagrams; connected UDP and its ECONNREFUSED; broadcast with and without SO_BROADCAST; multicast join, loop and leave; a raw ICMP ping of 127.0.0.1 |
| `options` | socket, TCP and IP options set and read back, an unknown option, SO_RCVTIMEO and SO_SNDTIMEO |
| `select` | WaitSelect() timeouts (20, 100, 500 ms, measured), polling, a signal in the mask, Ctrl-C ending a blocking accept(), readable and writable, a descriptor above 64 |
| `misc` | how many sockets a program gets, a socket handed to another process (ReleaseSocket/ObtainSocket), the errors for bad descriptors, families and protocols |
| `ipv6` | AF_INET6 TCP and UDP over ::1, IPV6_V6ONLY, an IPv6 listener taking an IPv4 client, `gethostbyname2()` |
| `ext` | the Roadshow extensions (interface, route and name server lists, the *ent() calls, inet_aton/pton/ntop, In_LocalAddr, ProcessIsServer, BPF), miami.library (getaddrinfo, getnameinfo, if_nameindex ...) and usergroup.library |
| `perf` | (`PERF`) loopback TCP throughput, connections a second, UDP datagrams a second, the cost of a WaitSelect() poll: stack and CPU together |
| `peer` | (`PEER=host`, with `tests/amiga/stackprobe_peer.py` on that host) TCP echo round trip, upload, download, a refused connection, UDP echo |

A call is only made when the library's jump table reaches its vector, and
each test runs under a watchdog: a call that blocks for ever on some stack
(for example `recv(MSG_DONTWAIT)` where the flag is unknown) is ended by a
Ctrl-C after 20 s and counts as a failure, so a run always finishes. `JSON=`
writes the report for `tools/stackprobe_compare.py`, which prints a
Markdown table of several reports side by side (see `docs/COMPARISON.md`).
`tests/amiga/run_fsuae_stackprobe.sh` runs it under one stack in FS-UAE.

## Profiling and debug log

Two diagnostics are built into every Riptide. Both cost next to nothing
while they are off (one test of a variable per probe), and neither writes
to the console while it runs. While on they do cost something: on an
emulated A1200 (68020, loopback ftp, 1 MB gets) throughput went from
about 245 KB/s to 225 KB/s with the profiler on, and to 180 KB/s with the
profiler plus `debug tcp,socket,timer`, which logs every ACK decision.
Use them from the Shell (`Riptide PROFILE ...`, `Riptide DEBUG ...`), in
the configuration file (`profile start`, `debug tcp`), or through the
RIPTIDE port.

### Profiler

```
Riptide PROFILE START      ; zero the counters and start
  ... run the download ...
Riptide PROFILE SHOW       ; the table (also while it runs)
Riptide PROFILE STOP       ; stop; SHOW still shows the run
Riptide PROFILE RESET      ; zero the counters, keep running or stopped
```

The profiler times the stack's code paths with the EClock (about 1.4 µs
resolution on PAL machines, 709379 Hz):

```
profile: running, 14247.1 ms wall clock, 2064 KB received (all interfaces), clock 709379 Hz
section           calls   total ms  %wall  us/call   max us   us/KB
idle                221     5112.8   35.8    23135   811288    2477
tcp output          313     2595.8   18.2     8293    18877    1257
tcp input           317     2202.9   15.4     6949    64574    1067
...
not in Riptide              2213.0   15.5
```

| Section | What it covers |
|---|---|
| `stack loop` | Riptide process under the lock, outside the sections below (driver replies, loopback) |
| `timers` | timer callbacks: retransmissions, delayed ACKs, ARP, DHCP |
| `ip input`, `ip6 input` | IPv4/IPv6 input up to the transport |
| `tcp input`, `udp input` | transport input (the ACKs and data it sends count as `tcp output`) |
| `tcp output` | building segments and IP output (the driver call excluded) |
| `checksum` | checksums that are not combined with a copy (headers are summed inline and not counted) |
| `copy to app`, `copy from app` | copying between the application's buffer and Riptide |
| `driver send` | the `SendIO()` / `MNI_Send()` call |
| `socket send` | the protocol's send operation outside the sections it calls (TCP: queueing the data, `tcp_write_xmit()`) |
| `driver rx copy` | the SANA-II driver's `CopyToBuff` callback, which also sums the frame (driver context; nothing with DMA) |
| `lock wait app`, `lock wait rt` | time applications / the Riptide process waited for the stack lock |
| `idle` | the Riptide process waiting for work in `Wait()` |

* Sections nest. Their times are exclusive: `tcp input` does not include
  the `checksum` or `tcp output` it calls. `max us` is the longest single
  call including what it called.
* `driver rx copy`, the lock waits and `idle` are counters measured outside
  the lock. They overlap the sections (a driver copying in an interrupt
  is also charged to whatever was running).
* `%wall` is the share of the wall-clock time since `PROFILE START`;
  `us/KB` divides by the bytes received on all interfaces in that time
  (the cost of a download per KB). `not in Riptide` is the wall-clock time
  left after the sections and `idle`: applications, the OS, other tasks.
* Times are wall-clock times of the task that runs the code, so a task
  switch in the middle (to a higher priority task) is counted too.
* Below the table, event counters per received KB and per TCP segment
  sent: clock reads (hardware and cached), outermost lock acquisitions
  and those that waited, `rt_malloc()` calls, buffer allocations and
  cache misses, kicks of the Riptide process, application wakeups,
  frames to and from the drivers, and rounds of the Riptide process's
  loop. They show what a change saves without a stopwatch.
* The table is also available to programs as `net.riptide.profile`
  (`{4, 255, 19}`, `struct rt_ctl_prof`, see "Interfaces for tools").

### Debug log

```
Riptide DEBUG tcp,driver           ; log these categories (16 KB ring)
Riptide DEBUG +dns -driver         ; add / remove categories
Riptide DEBUG all SIZE=64          ; everything, in a 64 KB ring
Riptide DEBUG SHOW 40              ; the last 40 lines
Riptide DEBUG DUMP RAM:riptide.dbg ; the whole ring to a file
Riptide DEBUG CLEAR                ; empty the ring
Riptide DEBUG NONE                 ; stop logging (the ring keeps its lines)
Riptide DEBUG STATUS               ; categories, ring size, lines, bytes lost
```

Lines go to a ring buffer in memory, with a time stamp in seconds since
the ring was created or cleared. When the ring is full the oldest lines
are overwritten (`DEBUG STATUS` says how many bytes). `DUMP` copies the
ring and writes the file from the Riptide process without holding up the
network.

| Category | Lines |
|---|---|
| `tcp` | state changes, every ACK decision (now or delayed, and why), out-of-order segments, SACK blocks, fast recovery, retransmission timeouts, zero windows (ours and the peer's), window probes |
| `ip`, `ip6` | dropped datagrams (header errors, not for us, discarded), reassembly |
| `driver` (`sana2`, `mni`) | each received frame, read errors, waits for buffers |
| `timer` | timer runs, delayed ACK timers |
| `socket` (`udp`) | `socket()`, `connect()`, `close()` |
| `dns` | queries, servers, timeouts, results |
| `dhcp` | DHCP state changes and messages sent |
| `arp` (`nd`) | ARP requests and replies, failed resolutions, IPv6 neighbour states |
| `log` | a copy of the log messages, at every level |
| `tcpstat` | every `interval=MS` (default 500, 20..60000): one line per TCP connection with the receive and send rate over the interval, RTT and variance, RTO, cwnd, ssthresh, both windows, unread and out-of-order bytes, segments in flight, retransmissions, RTOs, ACKs sent (of them delayed) and zero windows sent/received |

A `tcpstat` line from a loopback ftp download on an emulated A1200:

```
   3.328938 tcpstat: 64855>63152 st 4 rx 219 KB/s tx 0 KB/s rtt 20/10 ms rto 221 cwnd 10 ssth 65535 swnd 32664 rwnd 33184 unread 65016 ofo 0 infl 0 retr 0 rtos 0 acks 15/3 zw 2/0
```

`st` is the TCP state (4 = ESTABLISHED); `rx` and `tx` are the bytes
received and acknowledged per second since the last line. The time series
shows when a download stalls and why: the window (`rwnd`, `unread`, `zw`),
losses (`retr`, `rtos`) or the ACK clock (`acks`). Here 65016 bytes wait
unread and two zero windows were sent: the program reading the data, not
the network, sets the pace.

The ring takes memory (`SIZE=1..1024` KB): on a machine with Chip RAM
only, keep it small, or the Shell may not be able to load the `Riptide`
command again to read it.

### rtdiag: one command for a bug report

```
rtdiag ftp -o NIL: ftp://ftp.example.org/pub/big.lha   ; wrap one download
rtdiag -t 60                                           ; or record 60 s (Ctrl-C stops)
                                                       ;   while you download in another Shell
```

`rtdiag [-o DIR] [-t SECONDS] [-i IF] [-s KB] [-v] [COMMAND ...]` zeroes the
interface and driver counters and starts the profiler, the debug log
(`log,tcpstat` at 500 ms; `-v` adds `tcp,driver`, which logs every ACK
decision and frame and slows the stack down) and a packet capture
(`snaplen=128`, a `-s` KB ring, default 512; `-i` limits it to one
interface). Then it runs COMMAND, or waits. Afterwards it switches the
diagnostics off and writes to DIR (default `RAM:rtdiag`):

| File | Contents |
|---|---|
| `report.txt` | CPU, Kickstart and free memory, `Riptide STATUS` before and after, `sysctl -a`, the run time and the command's return code, `profile show`, `capture status`, `ip -s -s link`, `ifconfig -v`, addresses and routes, `netstat -s`, `netstat -T -a -n` |
| `debug.txt` | the debug log, with a `tcpstat` line per connection every 500 ms |
| `capture.pcap` | the packet capture, for Wireshark or tcpdump |

`report.txt` runs the tools (`ip`, `ifconfig`, `netstat`, `sysctl`) from
the path, so install them too. Memory: the capture ring (512 KB) and the
debug ring (64 KB, 256 KB with `-v`); on a machine with Chip RAM only use
`-s 64`.

## Interfaces for tools

### Tables: `RiptideSysCtl()` and `net.riptide.*`

Riptide publishes its tables as read-only sysctls under the MIB
`{CTL_NET (4), 255, n}` (names `net.riptide.*`). Every table is a `struct
rt_ctl_hdr` (version, record size, count) followed by fixed-layout
records; `include/rt/rt_ctl.h` defines them. Addresses are in network
byte order, everything else in host order; there are no pointers.

| n | name | records |
|---|---|---|
| 1 | `net.riptide.ifaces` | `struct rt_ctl_iface`: name, index, type, flags, MTU, hardware address, address/netmask/broadcast/peer, metric, all packet counters, DHCP state/server/router/lease, driver description, `tcpwindow` (0.12); link-local address, state and conflicts, classless route count (0.18) |
| 2 | `net.riptide.routes` | `struct rt_ctl_route`: destination, mask, gateway, interface, flags, proto (kernel/static/dhcp/redirect), metric, MTU, use count, expiry, linkdown |
| 3 | `net.riptide.neigh` | `struct rt_ctl_neigh`: ARP entries with state (INCOMPLETE/REACHABLE/STALE/PERMANENT) |
| 4 | `net.riptide.sockets` | `struct rt_ctl_sock`: TCP, UDP and raw sockets with state, queues, RTT, cwnd |
| 5 | `net.riptide.dns` | one `struct rt_ctl_dns`: name servers, search domain, host name |
| 6..9 | `net.riptide.{ip,icmp,udp,tcp}stat` | one `struct rt_{ip,icmp,udp,tcp}_stats` |
| 18 | `net.riptide.tcpinfo` | `struct rt_ctl_tcpinfo`: every TCP socket of both families with RTT, windows, buffers, queues, loss and ACK counters, 64-bit byte counts and time spent busy, window/buffer limited, with unread data and with a zero window (`netstat -T`); from 0.23 also the name of the program that made the socket (`prog`, check `recsize`) |
| 19 | `net.riptide.profile` | `struct rt_ctl_prof`: the profiler (`profile start`): record 0 the run (wall-clock time, bytes received), then one per section and counter (calls, total and longest time) |
| 20 | `net.riptide.drivers` | `struct rt_ctl_driver`, one per driver interface (SANA-II, MNI): receive queues (reads configured/queued, lowest, times empty), frames per wakeup, buffer shortages, errors by code, transmits in flight, the `S2_DEVICEQUERY` values and the driver's own `S2_GETGLOBALSTATS`/`S2_GETSPECIALSTATS` |
| 30 | `net.riptide.mcast` | `struct rt_ctl_mcast`: multicast memberships of both families: interface, group, link layer address, users, IGMP version and reporter flag (`ip maddr`, `netstat -g`) |
| 31 | `net.riptide.igmpstat` | one `struct rt_igmp_stats` |
| 40 | `net.riptide.addrs4` | `struct rt_ctl_addr4`: every IPv4 address of every interface: interface, address, netmask, broadcast, prefix length, primary/DHCP flags (0.15) |
| 41 | `net.riptide.qos` | `struct rt_ctl_qos`: the transmit bands of each interface with QoS: frames queued now and at most, sent, bytes, dropped, sent straight to the driver (0.15) |
| 42 | `net.riptide.status` | one `struct rt_ctl_status`: version, uptime, socket and buffer memory, NTP (state, last sync, offset, round trip, server, error), name cache hits, and a CPU meter: reading it makes the Riptide process count the time it waits for work for the next 5 s (`cpu_idle` in `cpu_hz` ticks, `cpu_now`), so two reads give its busy share (0.23; RiptidePrefs' Status page) |

Use BSD sysctl semantics: call with `oldp = NULL` to get the size,
allocate a bit more (a table can grow in between) and call again. If it
still does not fit, the call fails with `ENOMEM`, the buffer holds the
records that fitted (with the header count adjusted) and `*oldlenp` the
size needed. Readers must step through records by `recsize`, so later
versions can append fields.

### BSD ioctls

Besides the interface ioctls (`SIOCGIFCONF`, `SIOC[GS]IF*`), Riptide
supports `SIOCAIFADDR` (adds an address, see "Several IPv4 addresses per
interface"), `SIOCDIFADDR` (removes the one given), `SIOCADDRT`/`SIOCDELRT` (4.3BSD `struct ortentry`)
and `SIOCSARP`/`SIOCGARP`/`SIOCDARP` (AmiTCP's 38 byte `struct arpreq`;
the 4.4BSD 36 byte form with group `'i'` works too). Two extensions for
`struct ortentry`, harmless for programs that leave the fields zero: with
`RTF_MASK` (0x80) in `rt_flags` the netmask is taken from `rt_hash`
(otherwise host routes are /32, default /0 and networks get their classful
mask), and `rt_ifp` may hold an interface index to bind the route to.

### The RIPTIDE port

The public message port `RIPTIDE` takes a `struct RiptideMsg`
(`include/rt/riptide_port.h`) with one of these commands:

| `cmd` | |
|---|---|
| `RTCMD_STATUS` (1) | status text, as `Riptide STATUS` prints it |
| `RTCMD_QUIT` (2) | quit if no program has the libraries open, else `result` = EBUSY |
| `RTCMD_COMMAND` (3) | `text` holds one line of the configuration language (all keywords above, including `interface ...` and `interface NAME delete`), or a query: `sysctl -a`, `sysctl [-n] NAME...`, `sysctl [-w] NAME=VALUE`. `capture ...` (packet capture) answers with its status line; `capture save FILE` writes the file from Riptide's process before it replies. `profile ...` and `debug ...` answer in the reply text (the table, the ring) |
| `RTCMD_TABLES` (4) | 0.23: tables of [`net.riptide.*`](#tables-riptidesysctl-and-netriptide) without opening a library: `text` holds their numbers (e.g. `42 1 18`); a reply buffer is needed and gets, for each table, a ULONG number, a ULONG length and the dump as `RiptideSysCtl()` gives it, the next one 4-byte aligned (length 0 and `RTMSGF_TRUNCATED` for a table that failed or did not fit; `replylen` is the bytes used). RiptidePrefs' Status page reads this way once a second: opening and closing `riptide.library` costs more than the reads on a 68020, and an open library would keep Riptide from quitting. Older versions answer EINVAL |

The client allocates the message in public memory, sets `mn_ReplyPort`
and `mn_Length = sizeof(struct RiptideMsg)`, and may point `reply` /
`replysize` at its own buffer for long answers (otherwise the answer goes
to `text`, 1024 bytes). It finds the port and `PutMsg()`s under
`Forbid()`, then waits for the reply. `result` is 0 or an errno (BSD
numbers); the text reply holds the output or the error message,
`replylen` its length, and `flags` has `RTMSGF_TRUNCATED` when it did not
fit.

Riptide runs the command in its own process under the stack lock and
replies at once; it never waits for the client, so a client that crashes
cannot hang the stack. Only `interface` lines take a moment while the
driver opens. Messages still queued at shutdown are replied with
`ESHUTDOWN`. Version 1 messages (`mn_Length` without the reply buffer
fields, as sent by Riptide 0.9) are still understood.

## Happy Eyeballs

Riptide connects to host names the way RFC 8305 ("Happy Eyeballs version
2") describes, so a broken IPv6 path (a dead tunnel, a router that
advertises IPv6 but does not forward it, an unreachable AAAA address)
costs a quarter of a second instead of a connect timeout:

1. **Lookups.** The AAAA and A queries go out together. When the A answer
   comes first, Riptide waits at most `resolution_delay` (50 ms) for the
   AAAA one and goes on without it; a late A answer is waited for (the
   IPv4 fallback needs it). Cached, hosts file and `.local` answers need no
   network.
2. **Order.** The addresses are sorted by RFC 6724 (which puts IPv4 before
   ULA IPv6 addresses `fc00::/7`, and global IPv6 before IPv4), or with
   `prefer ipv6` / `prefer ipv4` that family first; then the families
   alternate.
3. **Racing.** Riptide connects to the first address, and every `delay`
   ms (250) or as soon as an attempt fails to the next one, keeping
   several attempts going; the first that connects wins, the others are
   closed.

With `happy_eyeballs off` the lookups are one after the other and the
addresses are tried in turn, each with an equal share of the timeout.

| keyword | sysctl | default | |
|---|---|---|---|
| `happy_eyeballs on\|off` | `net.inet.he.enable` | on | parallel lookups and racing |
| `happy_eyeballs delay MS` | `net.inet.he.attempt_delay` | 250 | 10..2000: ms before the next attempt starts |
| `happy_eyeballs resolution_delay MS` | `net.inet.he.resolution_delay` | 50 | 0..1000: ms an A answer waits for the AAAA one |
| `happy_eyeballs prefer auto\|ipv6\|ipv4` | `net.inet.he.prefer` | auto (0) | first family (6 or 4), auto = RFC 6724 |
| `happy_eyeballs aaaa_filter on\|off` | `net.inet.he.aaaa_filter` | on | no AAAA lookups (getaddrinfo with AF_UNSPEC, connecting by name) while IPv6 has no route to global addresses |

**Who uses it.** The `nc` and `ftp` tools connect by name this way (with
`-4`/`-6` only that family). Programs get it from riptide.library 2:

```c
LONG RiptideConnectName(CONST_STRPTR host, ULONG port, struct TagItem *tags);
```

returns a connected TCP socket (`RTCN_Type`, SOCK_DGRAM: a connected UDP
socket) in the caller's descriptor table (shared with bsdsocket.library),
in blocking mode, or -1 with `Errno()` set (`EHOSTUNREACH` when the name
does not resolve). Tags: `RTCN_Family` (AF_UNSPEC, AF_INET, AF_INET6),
`RTCN_Timeout` (ms, default 75000) and `RTCN_Result` (a `struct
RiptideConnectResult`: the address that answered, the time it took, how
many addresses there were and attempts started). Open riptide.library
version 2 for it.

**Programs that use gethostbyname()** only ever get IPv4 addresses, so
they are not affected by IPv6 problems. Programs that use
`getaddrinfo()` and try the first result only get IPv4 first whenever
IPv6 cannot reach the Internet (`aaaa_filter`), and RFC 6724 order
otherwise; `getaddrinfo()` itself does the lookups in parallel.

## Priorities and quality of service

On a slow link, or with a fast upload, the frames a program sends queue up
in front of the network driver: a SANA-II driver takes up to 32, which at
the speed of an emulated or real Amiga is a few hundred milliseconds of
data in front of every keystroke, ACK or DNS query. With **QoS** on
(`qos on`, or `qos=on` on an `interface` line; SANA-II and MNI
interfaces) Riptide gives the driver at most `inflight` frames (4) at a
time and keeps the rest in three bands, sent strictly in order:

| band | what |
|---|---|
| interactive | ARP, ICMP and ICMPv6, DNS, NTP, mDNS and DHCP, TCP segments without data (ACKs, SYN, FIN), TCP segments of up to 64 bytes (keystrokes), DSCP EF, CS4..CS7 and AF4x, TOS "low delay", socket priority 6 and 7 |
| default | everything else |
| bulk | DSCP CS1, LE and AF1x, TOS "throughput", socket priority 1, 2, 3 and 5 |

So a telnet or IRC keystroke waits behind at most four frames, not behind
a whole upload, and the ACKs of a download leave at once while an upload
fills the link. `ip -s link` shows each band (`QoS: direct N, interactive
N (queued, most, dropped) ...`); `qos status` too. A band holds `limit`
frames (128); beyond that TCP backs off as on any full queue.

QoS is off by default: when the link is faster than the Amiga can fill
(a fast emulated network, a 100 Mbit/s card) the frames that wait for
the driver to finish one cost a few percent of upload speed and gain
nothing. Turn it on for slow links (serial, PPP, 10 Mbit/s shared with
other traffic, a slow upstream) where uploads and interactive use mix.
Socket priority, DSCP marking and rules below work with QoS off too:
they set the DSCP that the router sees.

**Socket priority.** `setsockopt(s, SOL_SOCKET, SO_PRIORITY, &n, 4)` with
n 0..7 (Linux semantics; `SO_PRIORITY` is 0x1100 in Riptide's headers,
`SO_RIPTIDE_PRIORITY` in the SDK, and Linux's number 12 is accepted too)
picks the band, and, when the program has not set `IP_TOS` /
`IPV6_TCLASS` itself, the DSCP of its packets, so a home router with QoS
can tell them apart too: 0 none, 1 CS1, 2 AF11, 3 AF21, 4 AF31, 5 AF41, 6
EF, 7 CS6 (`qos priodscp off` keeps the DSCP at 0). Accepted sockets
inherit their listener's priority. `IP_TOS` and `IPV6_TCLASS` are honoured
on every packet of TCP (SYN and ACKs included), UDP and raw sockets.

**Rules for programs that set nothing.** Most Amiga programs never set a
priority. `qos rule` lines give one to their sockets, by port (`port=`
either end, `lport=` local, `rport=` remote), protocol and program (the
command name a Shell runs, or the task name, without its path):

```
qos rule proto=tcp port=22,23,6667 priority=interactive dscp=EF   # ssh, telnet, IRC
qos rule program=AmiFTP priority=bulk                              # downloads give way
qos rule rport=6881,6882 priority=bulk dscp=CS1
```

A rule never overrides what a program sets itself (`SO_PRIORITY`,
`IP_TOS`); program rules apply when the socket is created, port rules
when its ports are known (bind, connect, accept, sendto). At most 16
rules; `qos rules flush` removes them, `qos status` lists them.

QoS decides the order in which frames leave this Amiga. How fast a TCP
transfer sends at all is TCP's congestion control: a transfer can also
be made to yield to others in the network with the background
congestion control (`tcp background ...`, LEDBAT), which takes the same
kind of rules.

| keyword | sysctl | default | |
|---|---|---|---|
| `qos on\|off [IF]`, `interface ... qos=on\|off` | `net.inet.ip.qos_default` | off | bands on driver interfaces |
| `qos inflight N` | `net.inet.ip.qos_inflight` | 4 | 1..32 frames in the driver |
| `qos limit N` | `net.inet.ip.qos_limit` | 128 | 8..1024 frames per band |
| `qos priodscp on\|off` | `net.inet.ip.qos_prio_dscp` | on | DSCP from SO_PRIORITY |

## Sending UDP fast: pacing, batches, no checksum

For programs that stream datagrams (video, audio, multicast senders)
Riptide 0.17 has three things Linux programs know.

**Pacing: waiting for send space.** A UDP socket is writable only while
the interface its last datagram left by has room: the SANA-II driver
holds fewer than 32 frames (MNI: its pool), or, with QoS on, the
socket's band holds fewer than `qos limit` frames. When it has no room:

* a **blocking** socket's `send()`/`sendto()`/`sendmsg()` waits until
  the driver has finished a frame, then sends (`SO_SNDTIMEO` limits the
  wait: then `EWOULDBLOCK`, as for TCP). No datagram is dropped and
  none fails with `ENOBUFS` because the driver is busy.
* a **non-blocking** socket's send fails with `EWOULDBLOCK` (35), not
  `ENOBUFS`, and nothing is sent. `WaitSelect()` with the socket in the
  write set returns as soon as there is room again (also
  `SO_EVENTMASK` `FD_WRITE`), so a sender loops "send until
  EWOULDBLOCK, WaitSelect for writing".

The first datagram, and one to another interface, is always tried. A
driver that holds frames but completes none for a second counts as
stalled (a SANA-II device that stopped replying): senders stop waiting
for it and its datagrams are dropped, as before 0.17. On a
68030 or 68040 with an emulated or 100 Mbit/s card the CPU is the limit
and the queue does not fill (`netstat -s`: "sends that waited for
transmit room" stays 0); it fills on 10 Mbit/s cards, with QoS
(`qos inflight 1` keeps a single frame in the driver) and with batches.
Before 0.17 such a datagram was dropped (an output error of the
interface) while the send reported success.

**Batches: `RiptideSendMMsg()`, `RiptideRecvMMsg()`.** Linux
`sendmmsg()`/`recvmmsg()` in riptide.library version 3 (LVO -534 and
-540, `sdk/`):

```c
#include <libraries/riptide.h>   /* struct RiptideMMsgHdr */
#include <proto/riptide.h>

LONG RiptideSendMMsg(LONG fd, struct RiptideMMsgHdr *vec, ULONG vlen, ULONG flags);
LONG RiptideRecvMMsg(LONG fd, struct RiptideMMsgHdr *vec, ULONG vlen, ULONG flags);
```

`struct RiptideMMsgHdr` is a `struct msghdr` (name, iov, flags; control
is ignored) followed by `rmm_Len`, Linux's `struct mmsghdr`. The fd is
the bsdsocket.library socket of the same task (riptide.library must be
opened by that task, as for the other socket calls of riptide.library).
`RiptideSendMMsg()` sends the messages in order and returns how many it
sent, each one's byte count in `rmm_Len`; it stops at the first that
fails, and returns -1 with `Errno()` set only when the first one fails.
A blocking socket waits for room between messages as above; a
non-blocking one returns the count so far when one would block. `flags`
are `send()`'s (`MSG_DONTWAIT`, ...). `RiptideRecvMMsg()` receives the
first message as `flags` say (it may block) and then only what is already
queued; it sets `rmm_Len`, `msg_namelen` and `msg_flags`
(`MSG_TRUNC`). There is no timeout argument. Open riptide.library with
version 3 to use them.

**No checksum: `SO_NO_CHECK`.** `setsockopt(s, SOL_SOCKET,
SO_RIPTIDE_NO_CHECK, &one, 4)` (0x1101, from the SDK's
`libraries/riptide.h`; Linux's `SO_NO_CHECK` number 11 is accepted too)
sends this socket's IPv4 UDP datagrams without a checksum
(the field is 0, RFC 768); `getsockopt()` reads it back. Only datagram
sockets take it (others: `ENOPROTOOPT`). IPv6 datagrams always carry a
checksum (RFC 8200), whatever the option says. Received datagrams are
checked as before. The global `sysctl net.inet.udp.checksum=0` still
turns IPv4 UDP checksums off for every socket.

**What it costs: per-call measurements.** `tests/amiga/udpblast.c`
against a host sink through the bridged FS-UAE (cycle-exact A1200/030 at
28 MHz and A4000/040 at 25 MHz, uaenet.device; 3000 datagrams, time on
the Amiga's clock; `tests/amiga/run_fsuae_udpblast.sh`):

| mode | A1200/030 0.16 | A1200/030 0.17 | A4000/040 0.16 | A4000/040 0.17 |
|---|---|---|---|---|
| `sendto()` blocking, 1400 B | 474/s, 2108 us | 462/s, 2161 us | 835/s, 1196 us | 811/s, 1232 us |
| non-blocking + WaitSelect, 1400 B | 475/s, 2101 us | 460/s, 2169 us | 805/s, 1240 us | 810/s, 1233 us |
| `SO_NO_CHECK`, 1400 B | (no option) | 479/s, 2084 us | (no option) | 854/s, 1170 us |
| `RiptideSendMMsg()` x16, 1400 B | | 772/s, 1294 us | | 1405/s, 711 us |
| multicast, 1400 B | 479/s | 474/s | 815/s | 810/s |
| `sendto()`, 64 B | 557/s, 1795 us | 553/s, 1807 us | 903/s, 1106 us | 898/s, 1113 us |
| `RiptideSendMMsg()` x16, 64 B | | 1025/s, 974 us | | 1761/s, 567 us |
| QoS `inflight 1`, `limit 8`: blocking | 451/s, **42 lost** | 428/s, none lost | 802/s, **186 lost** | 756/s, none lost |
| the same, non-blocking | 455/s, **83 lost** | 436/s, none lost, 34 waits | 791/s, **121 lost** | 757/s, none lost, 2 waits |

(datagrams per second, microseconds per datagram; "lost": accepted by
the stack but never on the wire.) Run to run the rates vary by a few
per cent. What this says:

* A single `sendto()` costs about 1.8 ms on the 030 and 1.1 ms on the 040
  even for 64 bytes: the call, the stack lock, the driver request and
  the switch to the stack task for its completion. Copying and
  checksumming 1400 bytes adds only about 0.3 ms and 0.1 ms.
* A batch of 16 pays the per-call part once: 1.6x (1400 B) to 1.9x
  (64 B) the datagrams per second on both CPUs.
* `SO_NO_CHECK` saves about 75 us (030) and 60 us (040) per 1400-byte
  datagram, 3-5%.
* The pacing check costs a few microseconds per datagram (within the
  run-to-run spread). Where it matters, the queue full, 0.16 threw
  datagrams away while reporting success; 0.17 makes the sender wait.

## IPv4 multicast

Riptide is a multicast host (RFC 1112): it joins groups, reports them
with IGMP and sends and receives multicast datagrams; it does not route
multicast.

**IGMP.** IGMPv3 (RFC 3376) by default, in its simplest form: every group
is in EXCLUDE mode with an empty source list. Joins and leaves go out as
state change reports at once and once more within a second (Robustness 2,
or the querier's QRV), general queries are answered with one report of all
groups after a random delay up to the Maximum Response Time,
group-and-source queries with the queried sources. When an IGMPv2 or v1
querier is heard the interface falls back to that version (v1/v2 reports
to the group with report suppression, v2 leaves to 224.0.0.2) until the
older querier has been silent for 260 s (400 s for v1).
`net.inet.igmp.version` (0 = automatic, 1..3) caps the version. Messages
carry the Router Alert option, TTL 1 and TOS 0xc0. 224.0.0.1 is joined on
every multicast interface and never reported. Groups are announced again
when an interface comes up or gets a new address.

**Sockets.** The 4.4BSD/AmiTCP option numbers at level `IPPROTO_IP`:

| option | value | argument |
|---|---|---|
| `IP_MULTICAST_IF` | 9 | `struct in_addr`: the interface with that address, `INADDR_ANY` = the route to the group (else the first multicast interface; no default route needed). Also `struct ip_mreqn` (12 bytes, index wins) |
| `IP_MULTICAST_TTL` | 10 | `u_char` (or `int`), default 1; 0 delivers only to members on this host |
| `IP_MULTICAST_LOOP` | 11 | `u_char` (or `int`), default 1: members on this host get a copy |
| `IP_ADD_MEMBERSHIP` | 12 | `struct ip_mreq` (group, interface address or `INADDR_ANY`); also `struct ip_mreqn`. At most 20 per socket; `EADDRINUSE` when already joined |
| `IP_DROP_MEMBERSHIP` | 13 | as above; `EADDRNOTAVAIL` when not joined |

UDP and raw sockets take them. As in BSD, a UDP datagram for a group is
delivered to every socket bound to the port (several with `SO_REUSEADDR`
or `SO_REUSEPORT`) that joined the group on the receiving interface;
224.0.0.1 goes to every socket on the port, and sockets may bind to the
group address. Closing a socket leaves its groups. Source-specific
options (`IP_ADD_SOURCE_MEMBERSHIP`, `IP_BLOCK_SOURCE`) are not offered:
their numbers differ between systems and the AmiTCP SDK has none.

**Drivers.** A group is programmed into the driver's filter as
01:00:5e plus the low 23 bits of the group (`S2_ADDMULTICASTADDRESS` on
SANA-II, `AddMulticast()` on MNI), once per link layer address however
many groups and sockets share it. A SANA-II driver without a multicast
filter may not deliver the groups at all.

**Inside the stack.** Other modules join with `rt_ip_mc_join(netif,
group)` and `rt_ip_mc_leave()` (reference counted, lock held,
`include/rt/rt_ip.h`); `rt_ip_mc_member()` is what IP input accepts and
`rt_ip_mc_default_if(group)` picks the interface. Such a join makes the
interface accept the group; sockets still receive its datagrams only
after `IP_ADD_MEMBERSHIP`, so the mDNS responder joins 224.0.0.251
through its kernel socket.

## IPv6

Riptide is a dual-stack host: IPv6 runs next to IPv4 on every interface
without configuration, and IPv4 behaves exactly as before.

**What happens on its own.** When an interface comes up Riptide gives it
a link-local address (`fe80::/64`, interface identifier per RFC 7217:
stable for the network but not derived from the MAC, unless
`net.inet6.ip6.addr_gen_mode=0` asks for EUI-64), joins the all-nodes
and solicited-node groups (MLDv2, MLDv1 with old routers), runs duplicate
address detection and sends router solicitations. Router advertisements
bring global addresses (SLAAC), the default router and more specific
routes (RFC 4191), the link MTU, hop limit and DNS servers and search
domain (RDNSS/DNSSL, RFC 8106). When the router sets the O (or M) flag
Riptide also asks a DHCPv6 server for DNS servers and the search domain
(stateless DHCPv6, RFC 8415). Once a new global address has passed DAD
Riptide announces it to the routers (three unsolicited neighbour
advertisements to ff02::2, a second apart, RFC 9131), so the first reply
sent to it is not held up or lost while the router resolves it; `netstat
-s -6` counts them as "address announcements". `lo0` has `::1`.

**Configuration** (riptide.conf or `ip`/`ifconfig`/`route` at run time):

```
ipv6 off                           # no IPv6 on interfaces that come up later
ipv6 eth0 on|off                   # one interface, now
ifconfig eth0 inet6 2001:db8::5/64 [nodad]     # static address (+ prefix route)
ifconfig eth0 inet6 2001:db8::5 delete
route add -inet6 default via fe80::1%eth0      # or: route add default via fe80::1 dev eth0
route add -inet6 2001:db8:99::/48 via 2001:db8::1
nameserver 2001:db8::53            # IPv6 name servers (link-local: fe80::1%eth0)
host myhost 2001:db8::7            # static host table, AAAA
arp add 2001:db8::9 02:00:00:00:00:09 dev eth0 # static neighbour
sysctl net.inet6.ip6.use_tempaddr=2            # RFC 8981 temporary addresses
```

IPv6 sysctls: `net.inet6.ip6.enable` (1), `accept_rtadv` (1),
`use_tempaddr` (0; 1 create, 2 also prefer temporary addresses),
`addr_gen_mode` (1), `stable_secret` (empty: derived from the MAC),
`dad_count` (1), `gratuitous_na` (1; RFC 9131 announcements), `hlim` (64), `forwarding` (0), `mtu_expires` (600 s),
`v6only` (0), and `net.inet6.icmp6.ratelimit` (1000 ms per destination),
`errppslimit` (100/s), `echo_ignore_multicast` (0).

**Tools.** `ip a` and `ifconfig` show `inet6` lines; `ip -6 addr|route|neigh`
(with `add`, `del`, `flush`, `get`), `ping -6` / `ping6`, `traceroute -6`
/ `traceroute6`, `netstat` (`tcp6`/`udp6`/`raw6` rows, `-6`, `-s -6`),
`host` (AAAA, `ip6.arpa` reverse lookups, IPv6 name servers), `nc -6` and
`ftp` (EPSV/EPRT, `ftp://[2001:db8::1]/`) work over IPv6. Without `-6`,
`ping`, `traceroute`, `nc` and `ftp` use IPv6 for IPv6 literals and IPv4
for host names. `ip dns` lists every name server with where it came from.
`Riptide STATUS` lists the IPv6 addresses and name servers.

**Programming.** The ABI is the BSD (KAME) one that the Miami SDK also
used:

| | |
|---|---|
| `AF_INET6` / `PF_INET6` | 28 |
| `struct sockaddr_in6` | 28 bytes: len, family, port, flowinfo, addr[16], scope_id (RFC 3493). 24-byte RFC 2133 addresses (no scope_id, as in the Miami SDK) are accepted, and a 24-byte buffer gets 24 bytes back |
| `IPPROTO_IPV6` / `IPPROTO_ICMPV6` | 41 / 58 |
| `IPV6_UNICAST_HOPS` 4, `IPV6_MULTICAST_IF` 9, `_HOPS` 10, `_LOOP` 11, `IPV6_JOIN_GROUP` 12, `IPV6_LEAVE_GROUP` 13, `IPV6_CHECKSUM` 26, `IPV6_V6ONLY` 27, `IPV6_TCLASS` 61, `IPV6_DONTFRAG` 62 | KAME values |
| `ICMP6_FILTER` | 18 at level `IPPROTO_ICMPV6`, KAME semantics (a set bit passes the type) |
| `AI_NUMERICSERV` 8, `AI_ALL` 0x100, `AI_V4MAPPED_CFG` 0x200, `AI_ADDRCONFIG` 0x400, `AI_V4MAPPED` 0x800 | KAME values |

* An `AF_INET6` socket bound to `::` also takes IPv4 peers, which appear
  as `::ffff:a.b.c.d` (RFC 3493; `IPV6_V6ONLY` or the `v6only` sysctl
  turn this off). `AF_INET` sockets are unchanged.
* `RiptideSupportsIPV6()` (and the compat alias) returns TRUE.
  `inet_pton()`/`inet_ntop()` take `AF_INET6` (RFC 5952 output),
  `gethostbyname2(name, AF_INET6)` returns AAAA records,
  `getaddrinfo()` takes `AF_INET6` and `AF_UNSPEC` (sorted by RFC 6724
  destination selection, so IPv4 comes first when there is no usable
  IPv6 route), `AI_V4MAPPED`, `AI_ALL`, `AI_ADDRCONFIG` and zone ids
  (`fe80::1%eth0`), and `getnameinfo()` takes `sockaddr_in6`.
  bsdsocket.library's `gethostbyaddr()` resolves IPv6 addresses
  (`type` 28, `len` 16); `gethostbyname()` stays IPv4.
* Raw `AF_INET6` sockets receive the payload without the IPv6 header
  (RFC 3542); for ICMPv6 the stack computes the checksum.
* Table readers: `net.riptide.addrs6`, `routes6`, `neigh6`, `sockets6`,
  `dns6`, `ip6stat`, `icmp6stat` and `ifaces6` (numbers 10 to 17,
  records in `include/rt/rt_ctl.h`). The IPv4 tables are unchanged and
  do not list IPv6 sockets.

**Drivers.** SANA-II devices get CMD_READs for type 0x86DD, and the
multicast groups IPv6 needs (and the IPv4 ones) are programmed with
`S2_ADDMULTICASTADDRESS`/`S2_DELMULTICASTADDRESS`. A driver without a
multicast filter (it answers `S2ERR_NOT_SUPPORTED`, logged once) may not
deliver neighbour solicitations, and then IPv6 cannot resolve neighbours
on that link. MNI drivers get packet type 0x86DD and `AddMulticast()`.
On point-to-point (PPP/SLIP) devices Riptide reads 0x86DD too, but there
is no IPV6CP, so IPv6 over PPP depends on the driver.

**Not implemented:** stateful DHCPv6 (address and prefix leases),
ancillary data (`IPV6_PKTINFO`, `IPV6_RECVHOPLIMIT`: `ping6` cannot show
the reply's hop limit), routing headers (a host: one with segments left
is refused with a parameter problem), mobility, IPsec, SEND, and IPv6 over PPP (IPV6CP).

## Roadshow extensions (bsdsocket.library 4)

`bsdsocket.library` (version 4) has the extensions of the Roadshow
API after the AmiTCP 4 vectors. The vector offsets are those of the
Roadshow SDK's `bsdsocket_lib.sfd`, from `bpf_open()` at -366 to
`getnameinfo()` at -822. The reserved slots return 0.
`tests/amiga/check_layouts.sh` checks the offsets, registers, structures
and constants against that SDK (`ROADSHOW_SDK`, or the copy in NDK 3.2R4
under `SANA+RoadshowTCP-IP`). The code is in `src/amiga/roadshow.c`,
`src/core/bpf.c` and `src/core/bsdinfo.c`.

Every call runs in the calling task. Queries read the stack's state
directly. Changes that a `riptide.conf` line can make are sent to the
Riptide process as RIPTIDE port commands, so they act exactly like
`route`, `ifconfig`, `dhcp` and `interface` in the configuration file and
the tools. These are routes, interface addresses and state, DHCP, and
adding or removing interfaces. A program calling them needs Riptide
running, which it does anyway when it has the library open.

| Area | Calls (LVO) | In Riptide |
|---|---|---|
| Packet filter | `bpf_open` -366 … `bpf_data_waiting` -408 | 16 channels on any interface, classic BPF filter programs, the BSD double buffer, `bpf_hdr` records, read timeout, immediate mode, notify and interrupt signals, `bpf_write()`. Ethernet interfaces give `DLT_EN10MB` frames (the 14 byte header is rebuilt from the addresses the driver reported); point-to-point interfaces and `lo0` give `DLT_NULL`. `BIOCPROMISC` sets the interface promiscuous until the last channel that asked closes. |
| Routes | `AddRouteTagList` -414, `DeleteRouteTagList` -420, `ChangeRouteTagList` -426 (private), `FreeRouteInfo` -432, `GetRouteInfo` -438 | `RTA_*` tags become `route add/del/change` lines. A destination without a host part is a classful network. `GetRouteInfo()` returns the IPv4 routing table as version 3 `rt_msghdr` records with `RTA_DST`, `RTA_GATEWAY`, `RTA_NETMASK` and `RTA_IFA` addresses, filtered by flags. |
| Interfaces | `AddInterfaceTagList` -444, `ConfigureInterfaceTagList` -450, `Release/ObtainInterfaceList` -456/-462, `QueryInterfaceTagList` -468, `RemoveInterface` -732 | `AddInterfaceTagList()` is `interface NAME device=D unit=N` (`IFA_NumReadRequests` becomes `reads=`, `IFA_LimitMTU` `mtu=`; the other SANA-II tuning tags are ignored, `IFA_HardwareAddress` is refused). `IFC_*` set addresses, aliases, metric, MTU and state (`SM_Online`/`SM_Offline` also switch the SANA-II device), and `IFC_ReleaseAddress` releases a DHCP lease. `IFC_GetPeerAddress` and `IFC_GetDNS` need SANA-II R4 address queries and fail with `EOPNOTSUPP`. Every `IFQ_*` tag is answered (the driver's counters where there are some, 0 for copy statistics and debug mode). Unknown query tags are left alone. |
| Address allocation | `CreateAddrAllocMessageA` -474, `DeleteAddrAllocMessage` -480, `BeginInterfaceConfig` -486, `AbortInterfaceConfig` -492 | Riptide's DHCP client does the work. `AAMP_BOOTP` uses it too (DHCP servers answer both), and `AAMP_SLOWAUTO`/`AAMP_FASTAUTO` use it with an immediate 169.254/16 link-local fallback. The message is replied with the address, mask, server, router, DNS servers, domain and lease expiry once bound, or with `AAMR_Timeout` (the client is stopped again if this request started it), `AAMR_InterfaceWrongType` (not Ethernet), `AAMR_AddressKnown` (a static address), `AAMR_Busy` and so on. |
| Monitoring | `AddNetMonitorHookTagList` -498, `RemoveNetMonitorHook` -504 | All seven hook types. `MHT_Connect`, `MHT_Bind` and `MHT_Send` run in the caller of `connect()`, `bind()`, `send()`/`sendto()`/`sendmsg()`, and a positive result is the error that call returns. `MHT_ICMP`, `MHT_UDP`, `MHT_TCP_Connect` (a SYN without ACK) and `MHT_Packet` run in the Riptide process: `MA_Drop` drops as if the checksum was wrong, `MA_Ignore` skips ICMP processing, and `MA_DropWithReset` answers a connection request with a reset. The hooks of a program that quits without removing them are removed when it closes the library. |
| Statistics | `GetNetworkStatistics` -510 | All ten `NETSTATUS_*` tables in the 4.4BSD layouts (`icmpstat`, `igmpstat`, `ipstat`, `mbstat`, `mrtstat`, `rtstat`, `tcpstat`, `udpstat`, and `protocol_connection_data` for TCP and UDP sockets). Riptide counts what the Linux SNMP MIB counts, so the BSD counters are filled from the nearest Riptide counter, and the ones it does not keep are 0 (`mbstat` and `mrtstat` entirely: no mbufs, no multicast routing). IPv6 sockets are not listed. |
| DNS | `AddDomainNameServer` -516, `RemoveDomainNameServer` -522, `Release/ObtainDomainNameServerList` -528/-534, `Get/SetDefaultDomainName` -702/-708 | Additions nest. In the list, `dnsn_UseCount` is the nesting count, or -1 for servers from the configuration or DHCP. The default domain is `domain` in `riptide.conf`. |
| Network database | `set/end/getnetent` -540…-552, `…protoent` -558…-570, `…servent` -576…-588 | Walk the built-in tables. The networks table has only "loopback". |
| Address conversion | `inet_aton` -594, `inet_ntop` -600, `inet_pton` -606, `In_LocalAddr` -612, `In_CanForward` -618 | IPv4 and IPv6. `In_LocalAddr()` is true for our addresses and every address on a connected subnet. |
| Kernel memory | `mbuf_copym` -624 … `mbuf_pullup` -684 | Not provided: there are no mbufs. `SBTC_HAVE_KERNEL_MEMORY_API` says FALSE, and the calls return NULL or -1 (`ENOSYS`). |
| Internet superserver | `ProcessIsServer` -690, `ObtainServerSocket` -696 | Riptide has no superserver: FALSE and -1. |
| Configuration data | `ObtainRoadshowData` -714, `ReleaseRoadshowData` -720, `ChangeRoadshowData` -726 | 17 options mapped to sysctls: `bpf.bufsize`, `icmp.maskrepl`, `icmp.processecho`, `icmp.procesststamp`, `ip.defttl`, `ip.forwarding`, `ip.sendredirects`, `task.controller.priority` (the Riptide process's priority), `tcp.do_rfc1323`, `tcp.do_timestamps`, `tcp.do_win_scale`, `tcp.mssdflt`, `tcp.recvspace`, `tcp.sendspace`, `udp.cksum`, `udp.recvspace` and `udp.sendspace` (`net.inet.udp.maxdgram`). One writer at a time. |
| Reentrant resolver | `gethostbyname_r` -738, `gethostbyaddr_r` -744 | IPv4. The result goes into the caller's buffer (`ERANGE` if it is too small). |
| IP filter | `ipf_*` -762 … -798 (private) | Not provided: -1 with `ENXIO`; `SBTC_IPF_API_VERSION` is 0. |
| getaddrinfo() | `freeaddrinfo` -804, `getaddrinfo` -810, `gai_strerror` -816, `getnameinfo` -822 | The Roadshow flavour: `struct addrinfo` with `ai_addr` before `ai_canonname`, negative `EAI_*` codes and Roadshow's `AI_*`/`NI_*` values. `gai_strerror()` takes its code in a0. riptide.library's implementation does the lookup. |

**SocketBaseTagList() codes.** Besides the AmiTCP codes:

* `SBTC_NUM_PACKET_FILTER_CHANNELS` (16).
* Every `SBTC_HAVE_*_API` code answers TRUE, except
  `SBTC_HAVE_KERNEL_MEMORY_API`, which answers FALSE.
* `SBTC_UDP_CHECKSUM`, `SBTC_IP_FORWARDING`, `SBTC_IP_DEFAULT_TTL`,
  `SBTC_ICMP_MASK_REPLY`, `SBTC_ICMP_SEND_REDIRECTS`,
  `SBTC_ICMP_PROCESS_ECHO` and `SBTC_ICMP_PROCESS_TSTAMP` read and set
  the stack-wide sysctls. Riptide answers ICMP time stamp requests
  now. The new sysctls `net.inet.icmp.process_echo` and
  `net.inet.icmp.process_tstamp` take `IR_Process`/`IR_Ignore`/`IR_Drop`
  as 0/1/2.
* `SBTC_SYSTEM_STATUS`, and `SBTC_GET_BYTES_RECEIVED`/`SENT` (by
  reference, an `SBQUAD_T`).
* `SBTC_SIG_ADDRESS_CHANGE_MASK` is signalled whenever an interface
  address is set, added or removed.
* `SBTC_ERROR_HOOK` is called on every `errno` and `h_errno` change.
  `SBTC_LOG_HOOK` takes over `vsyslog()` output.
* `SBTC_CAN_SHARE_LIBRARY_BASES` is accepted.
* `SBTC_COMPAT43` and `SBTC_IDN_DEFAULT_CHARACTER_SET` read 0, and only
  0 can be set (4.4BSD socket addresses; no IDN translation).
* `SBTC_LOG_FILE_NAME` reads "" and cannot be set: Riptide logs to its
  own output.
* `SBTC_IP_FILTER_HOOK` is NULL, and only NULL can be set.

**SDK.** `sdk/fd/bsdsocket_lib.fd`, `sdk/include/clib/bsdsocket_protos.h`,
the vbcc inlines, pragmas and `proto/bsdsocket.h` cover the whole table,
AmiTCP part included. The private vectors are `##private`, as in the
Roadshow SFD. `sdk/include/libraries/bsdsocket.h` has the tags,
constants and structures above, written from the Roadshow documentation.
The socket structures themselves come from the usual network headers.
`tests/amiga/rs_test.c`, built from the SDK, calls every vector and is run
by `tests/amiga/run_fsuae_roadshow.sh`.

**Roadshow programs.** NetMon 0.34 (Aminet `comm/net/netmon.lha`), an
interface monitor written for Roadshow, runs under Riptide. It finds the
interfaces and shows their traffic, and its `WRITESTATS` option writes
the same counters to `ENV:`.

## Compatibility notes

**Per-opener library bases.** As in the historical stack, every task that
opens `bsdsocket.library`, `riptide.library` or the compat alias gets its
own library base.

* A second `OpenLibrary()` from the same task returns the same base with a
  higher open count.
* `bsdsocket`, `riptide` and the alias share one per-task context: the descriptor
  table, `errno` and the signal masks.
* Each opener has its own socket descriptor table. The default size is 64;
  change it with `SBTC_DTABLESIZE`.
* Close every library from the task that opened it.

**WaitSelect() on a socket that was never connected.** A TCP socket that
has not been connected (nor listened on) is neither readable nor
writable, as in AmiTCP and BSD (`soreadable()`/`sowriteable()`), so
`WaitSelect()` waits or returns 0 for it. Linux would report it as
writable and hung up; 0.10 did. A socket whose connection ended is
readable (end of file), one whose connect failed reports the error.

**Errors.**

* `errno` is kept per task. Read it with `Errno()`, or link your own
  variable to it with `SocketBaseTags(SBTC_ERRNOLONGPTR...)` or
  `SetErrnoPtr()` (sizes 1, 2 and 4).
* `SocketBaseTagList()` supports every `SBTC_*` code of the AmiTCP 4 SDK.
  It returns 0, or the 1-based position of the first tag it rejected.

**Signals.**

* The default break mask is Ctrl-C (`SBTC_BREAKMASK`). Blocking calls
  return `EINTR` when a break signal arrives.
* `WaitSelect()` takes an extra signal mask in d1. On return it holds the
  signals that arrived.
* `SBTC_SIGEVENTMASK` together with `SO_EVENTMASK` delivers events for
  `GetSocketEvents()`. `FIOASYNC` delivers the `SBTC_SIGIOMASK` and
  `SBTC_SIGURGMASK` signals.

**Name service.**

* `gethostbyname()` and `gethostbyaddr()` use the static table first,
  then DNS.
* `getnetbyname()`, `getnetbyaddr()` and `getnetent()` know only
  "loopback" (net 127).
* `getservbyname()`, `getservbyport()`, `getprotobyname()`,
  `getprotobynumber()`, `getservent()` and `getprotoent()` use built-in
  tables.

**riptide.library.**

* Implemented: `RiptideSysCtl()` (by MIB), `RiptideDisallowDNS()`,
  `RiptideGetPid()`, `RiptideIsOnline()` and `RiptideOnOffline()` (by
  interface name; "mi0" or NULL means any interface),
  `RiptideGetHardwareLen()`, `RiptideResGetOptions()`/`RiptideResSetOptions()`,
  `RiptideSupportedCPUs()`, `inet_ntop()`, `inet_pton()`, `inet_aton()`,
  `gethostbyname2()`, `getaddrinfo()`, `freeaddrinfo()`,
  `getnameinfo()`, `gai_strerror()`, the `if_*` functions, `sockatmark()`,
  `RiptideGetFdCallback()` and `RiptideSetFdCallback()`.
* Version 2 (Riptide 0.15) adds `RiptideConnectName()` at LVO -528, after
  the historical table (see "Happy Eyeballs").
* Version 3 (Riptide 0.17) adds `RiptideSendMMsg()` and `RiptideRecvMMsg()`
  at LVO -534 and -540 (see "Sending UDP fast").
* Fd callbacks must be 68k register callbacks (`RIPTIDECPU_M68KREG`); PPC
  callbacks are refused.
* Return fixed values:
  * `RiptideSupportsIPV6()` returns TRUE (see "IPv6").
  * `RiptideOpenSSL()` returns NULL (there is no SSL).
  * `RiptidePFAddHook()` returns NULL (no packet monitoring).
  * `RiptideSetSocksConn()` returns FALSE (no SOCKS).
* The reserved vectors (`RiptideReserved*`, up to LVO -522) return 0. In
  the historical stack these were private calls (resolver internals used
  by its own DNS tools, kvm access for its NetStat, its settings
  database), so programs that depend on them, such as that stack's NetStat,
  DiG and NSLookup, do not work.
* **Compat alias.** The alias (see "Compat alias" above) is the same
  library: same vectors, per-task bases and context. A program that opens
  it calls the same functions under their historical names, which
  `tests/amiga/check_layouts.sh` checks against that API's SDK.

**riptide.library SDK.** `sdk/` holds what a program needs to call
riptide.library (and bsdsocket.library, see "Roadshow extensions"), generated from `src/amiga/lib_lvos.txt` by
`tools/gen_lvos.py` (`make -f Makefile.amiga sdk`; `check` fails when it
is out of date):

| File | |
|---|---|
| `sdk/fd/riptide_lib.fd` | FD file (base `_RiptideBase`; the reserved vectors are `##private`), for `fd2pragma`, `fd2sfd` and the like |
| `sdk/include/libraries/riptide.h` | `RIPTIDE_LIB_NAME`, `RIPTIDE_LIB_VERSION`, `RIPTIDECPU_*`, the `net.riptide.*` MIB numbers, the `RTCN_*` tags and `struct RiptideConnectResult`, `struct RiptideMMsgHdr`, `SO_RIPTIDE_PRIORITY`, `SO_RIPTIDE_NO_CHECK` |
| `sdk/include/clib/riptide_protos.h` | prototypes |
| `sdk/include/inline/riptide_protos.h` | vbcc inline calls |
| `sdk/include/pragmas/riptide_pragmas.h` | SAS/C and DICE `#pragma libcall` |
| `sdk/include/proto/riptide.h` | includes the above for the compiler in use |

```c
#include <proto/exec.h>
#include <proto/riptide.h>      /* vc +aos68k -Isdk/include ... */

struct Library *RiptideBase;
...
RiptideBase = OpenLibrary(RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
```

Struct pointers are declared as `APTR`. For the record layouts of the
`net.riptide.*` tables include `include/rt/rt_ctl.h` from the Riptide
sources. `tests/amiga/sdk_test.c` is a small example that is built from
the SDK alone and run in the FS-UAE tools test. GCC users can generate
inlines from the FD file with their usual tools.

**syslog.** `vsyslog()` messages go to Riptide's output, prefixed with the
`SBTC_LOGTAGPTR` tag and filtered by `SBTC_LOGMASK`. Arguments are 32-bit
(AmiTCP `va_list`). `%m` is supported.

**usergroup.library.**

* There is a single user and a single group (`users`).
* Credentials changes (`setuid()`, `setgid()`, `setgroups()`, `umask()`)
  are kept per opener task.
* **`crypt()` is not DES.** It is a placeholder hash in the classic
  13-character format, so passwords hashed by another stack do not verify.
* `getpass()` reads from the caller's console.
* There is no utmp or lastlog: `getutent()` and `getlastlog()` return NULL.

**Not supported:** `socketpair()`, ancillary data in `recvmsg()`
(`msg_controllen` is always 0), and ARexx.

## CPU support

One Riptide binary runs on the 68020, 68030, 68040 and 68060. At startup
it reads the CPU type from `SysBase->AttnFlags` and picks its checksum and
copy routines (hand-written 680x0 assembly in `src/arch/m68k/rt_m68k.s`;
no FPU, Blitter or Copper code):

| CPU | Checksum | Copy (user/driver memory) | Copy between packet buffers |
|---|---|---|---|
| 68020, 68030 | `csum_020` | `copy_020` | `copy_020` |
| 68040, 68060 | `csum_020` | `copy_020` | `copy_040` (MOVE16) |

* `csum_020` is an unrolled `addx.l` loop (16 longs per iteration, entered
  by a computed jump for the remainder). `copy_020` aligns the destination
  and runs 16 `move.l` per iteration. `csum_copy_020` copies and sums in
  one pass; TCP uses it to build each segment it sends, UDP to take
  the payload from the program, and the SANA-II `CopyToBuff` callback to
  take each received frame.
* There is no separate 68060 checksum: the emulators give no 68060 timing
  that could justify one, so `csum_020` serves every CPU. On the 68060
  plain `move.l` copies are used for any memory, as on the 68020.
* The choice is logged at info level (`DEBUG=2`) and shown by `Riptide
  STATUS`:

  ```
  cpu: 68060, checksum: csum_020, copy: copy_020, buffer copy: copy_040 (move16) (routines auto)
  ```

  and by the read-only sysctls `riptide.cpu.model` and
  `riptide.cpu.routines`.
* `cpu routines generic|020|040|060` in the configuration file (or sent to
  the running stack) overrides the choice, for troubleshooting: `generic`
  is the portable C code. `040`/`060` are refused on a CPU without MOVE16.

**MOVE16.** On the 68040 and 68060, copies between Riptide's own packet
buffers use MOVE16 for 16-byte lines when both sides have the same
alignment and the copy is at least 80 bytes long. MOVE16 bursts whole
cache lines and is known to fail on some boards and with some memory
(Chip RAM, some Zorro memory). Riptide therefore:

* never uses it on memory it does not own (programs' buffers, SANA-II
  driver buffers);
* allocates its buffers in Fast RAM, and turns MOVE16 off for good (with a
  warning) as soon as one of its allocations lands outside Fast RAM;
* has a kill switch: `sysctl riptide.cpu.move16=0` (configuration file or
  the `sysctl` tool at run time). Use it if the stack misbehaves on a
  68040/68060 accelerator; `cpu routines 020` has the same effect.

**68060 and 64-bit arithmetic.** The 68060 lacks the 64-bit forms of
`mulu.l`/`divu.l`; `68060.library` emulates them, slowly. The assembly
uses none, and the per-packet clock and RTO code uses 32-bit arithmetic.
The vbcc build still calls vbcc's 64-bit helpers (which use those forms)
in CUBIC's window update (per ACK in congestion avoidance) and, less
often, in receive-buffer tuning, PRR, DHCP and number formatting. The GCC
build is compiled with `-mtune=68020-60`, so GCC does not inline the
64-bit forms either; its libgcc helpers remain in the same places.

**Benchmark.** `Riptide BENCH` prints KB/s for the C and the assembly
routines on 64 to 8192 byte buffers (`tests/amiga/run_fsuae_bench.sh`
runs it in FS-UAE; `FSUAE_MODEL`/`FSUAE_EXTRA` select other CPUs). On the
emulated A1200 (68020, cycle exact, Fast RAM), 1460-byte buffers:

| Routine | C | Assembly |
|---|---|---|
| checksum | 2168 KB/s | 5857 KB/s |
| copy | 3559 KB/s (vbcc `memcpy`) | 4961 KB/s |
| copy + checksum | 1348 KB/s (two passes) | 4018 KB/s (one pass) |

Emulator timing is not real hardware timing; the 68030/40/60 models of
FS-UAE are not cycle exact at all. Measure on the real machine.

`BENCH` then times single calls (µs per call, outside the stack lock).
On the emulated A1200 with its AROS Kickstart:

| Call | µs |
|---|---|
| `ReadEClock()` | 67 |
| `rt_now_ms()` / `rt_now_us()` (EClock and divisions) | 111 / 138 |
| `GetSysTime()` | 118 |
| `rt_lock()` + `rt_unlock()` (uncontended semaphore) | 155 |
| `AllocVec()` + `FreeVec()`, 1792 bytes | 1972 |
| `rt_buf_alloc()` + `rt_buf_free()` (cached), with the lock | 232 |
| timer.device `SendIO()` + `AbortIO()` + `WaitIO()` | 593 |

`BENCH TX` measures the send path the same way: about 2220 µs per segment on
the emulated A1200, of which about 350 µs is the copy and sum from the
application (see `docs/DESIGN.md`, "Send path").

A received segment used to read the clock about nine times; the holder of
the stack lock now reads it once per hold. Commodore's Kickstart may be
cheaper per call than AROS's; the counts matter either way.

## Building

The primary toolchain is vbcc with the NDK 3.2 headers. They are a build
dependency and are not part of the repository.

```
make -f Makefile.amiga            # VBCC=/opt/vbcc NDK=/opt/ndk3.2 by default
                                  # -> build/amiga/Riptide, build/amiga/rtcheck
make -f Makefile.amiga check      # SDK up to date, hunk checks + vamos self test
make -f Makefile.amiga sdk        # regenerate sdk/ from src/amiga/lib_lvos.txt
make -f Makefile.amiga package    # Installer package, see below
```

`make -f Makefile.amiga package` (`tools/mkpackage.sh`) assembles
`build/package/Riptide/`: `Install_Riptide` (from `install/`), `ReadMe`,
the Aminet readme `Riptide.readme`, `RiptidePrefs`, `C/` with all
programs, `Docs/` and `SDK/`, with icons made by `tools/mkicon.py` (classic OS 2.x/3.x
`.info` files; `tools/mkicon.py dump FILE.info` prints one). From that it
makes `build/riptide-<version>.lha` with an LHA archiver that can write
archives ("LHa for UNIX" `lha`, or `jlha` from Debian/Ubuntu's
`jlha-utils`; Debian's `lhasa` only extracts) or else a `.zip`, the Aminet
readme `build/riptide-<version>.readme`, and with amitools' `xdftool` an
880 KB FFS floppy image `build/riptide-<version>.adf`.

An alternative build uses Debian's `m68k-linux-gnu-gcc` in freestanding
mode. It needs no NDK:

```
make -f Makefile.amiga TOOLCHAIN=gcc        # -> build/amiga-gcc/...
```

* It uses Riptide's own minimal OS headers (`src/amiga/os`) and a small
  libc (`src/amiga/libc.c`).
* It links one image at address 0 with `--emit-relocs`.
  `tools/elf2hunk.py` turns that image into a hunk executable with a single
  `HUNK_CODE` and `HUNK_RELOC32`.
* Its stack is faster: about a fifth less 68020 time per TCP segment than
  the vbcc build (`BENCH TX`), 15 % more download and 9 % more upload on
  the emulated A1200, 20 % and 14 % on the A4000/040 (`docs/DESIGN.md`,
  "Compiler"). `make -f Makefile.amiga TOOLCHAIN=gcc package` packages it.

Both builds generate their library glue from the LVO tables
`src/amiga/lib_lvos.txt`, `src/amiga/os/os_lvos.txt` and
`src/amiga/mni_lvos.txt` with `tools/gen_lvos.py`.

* vbcc points the vectors straight at C functions that take `__reg()`
  arguments.
* GCC gets register-to-stack trampolines. For calls, GCC gets inline
  `jsr` stubs that load a6 inside the asm, because a6 is GCC's frame
  pointer on m68k-linux.

## Testing

| Test | What it checks |
|---|---|
| `tests/amiga/check_layouts.sh` | Riptide's own structure layouts, constants and vector tables against the NDK 3.2, the AmiTCP SDK 4.3 and the SDK 2.11 of the historical API (`COMPAT_SDK`): the compat alias must keep its vector layout (riptide.library's names are mapped to the historical ones), and `sdk/fd/riptide_lib.fd` must match the table. With `MUI_SDK=` (the `Developer` drawer of Aminet `dev/mui/mui38dev.lha`) also RiptidePrefs' MUI definitions (`src/amiga/prefs/mui.h`: structures, attribute and method numbers) and its muimaster.library vectors. Set `NDK=`, `COMPAT_SDK=` and `AMITCP_SDK=` to the unpacked archives. The Roadshow extensions of bsdsocket.library are checked against the Roadshow SDK (`ROADSHOW_SDK=`, default `$NDK/SANA+RoadshowTCP-IP`, part of NDK 3.2R4: its `sfd/` and `netinclude/`): the whole vector table to -822, `src/amiga/roadshow.h`, the packet filter and routing message layouts, the statistics structure sizes and about 290 constants; `sdk/fd/bsdsocket_lib.fd` must match the table too. |
| `tests/amiga/run_fsuae_roadshow.sh build/amiga` | Boots FS-UAE (AROS ROM, or `KICKSTART_ROM=`) with Riptide on loopback and runs `tests/amiga/rs_test` (built from `sdk/` only), which calls every Roadshow vector of bsdsocket.library and checks the results: packet filter on lo0 (filter program, notify signal, read timeout and interrupt, `bpf_write()` delivered to a socket), interface queries and changes (an alias and the address change signal), routes, DNS servers and the default domain, statistics, configuration data, the netdb enumeration, address conversion, `gethostbyname_r()`/`gethostbyaddr_r()`, the Roadshow `getaddrinfo()` layout and codes, all seven monitoring hook types, address allocation messages and the SocketBaseTagList() codes. |
| `tests/amiga/test_hunk.py FILES` | Loads the executables with amitools and checks their relocations. It also has elf2hunk unit tests: absolute, PC-relative and absolute-symbol relocations, rejecting 16-bit relocations, bss in the hunk size, and a run under vamos. |
| `tests/amiga/run_vamos.sh build/amiga/Riptide` | `Riptide SELFTEST` under vamos: MakeLibrary/AddLibrary, the Open/Close vectors, and about 75 library-level checks through the real jump tables. It drives TCP and UDP over loopback (127.0.0.1 and ::1, 24- and 28-byte `sockaddr_in6`, a dual-stack listener, the IPv6 resolver calls) with non-blocking sockets while the test pumps the stack itself. Needs `machine68k==0.3.0`. |
| `tests/amiga/run_fsuae_tools.sh build/amiga` | Boots FS-UAE with a uaenet.device interface (`uae_sana2`, `network_card = a2065`) and runs about 80 tool invocations (every tool, both argument styles, error cases, `ip link add/del` at run time). Their outputs are saved on DH0:out/ and checked with regular expressions. This FS-UAE has no slirp backend (nothing is received on eth0), so eth0 is configured statically and pings, traceroutes and netstat run over loopback. IPv6: `ip -6` addr/route/neigh changes and listings, `ifconfig inet6`, `ping6`/`ping -6`, `traceroute6`, `host` AAAA and `ip6.arpa`, `nc -6` (also an IPv4 client of a dual-stack listener), `netstat -6`. It also runs `sdk_test` (built from `sdk/` only) and checks the library line of the startup log and of `STATUS`. Background transfers: `Riptide BACKGROUND program=nc` makes the next `nc` client LEDBAT and rLEDBAT (`netstat -T`), the socket memory sysctls and the `STATUS` line. |
| `STACK=name tests/amiga/run_fsuae_stackprobe.sh OUT.json [ARGS]` | Runs `stackprobe` under one TCP/IP stack on FS-UAE and keeps its JSON report (`tools/stackprobe_compare.py` puts several side by side; `tests/test_stackprobe_compare.py` tests that script). `STACK=riptide` (a Riptide build directory in `STACK_DIR`), `amitcp` (the unpacked AmiTCP-3.0b2 drawer), `amitcp_ng` (an AmiTCP_NG tree with `build/` and `install/`), or any other name with `STACK_DIR` holding files to copy over System: and a `start` file. With `KICKSTART_ROM` and `WB_DIR` (an installed Workbench; neither is in this repository) the stack is installed on a copy of that Workbench; without them only Riptide runs, on the AROS ROM. Every stack gets `uaenet.device` unit 0 at 10.0.2.15/24. `FSUAE_EXTRA="uae_cpu_model = 68000"` runs it on a plain 68000. `tests/amiga/stackprobe_peer.py` is the far end for `PEER=`. |
| `tests/amiga/run_fsuae_ftp.sh build/amiga` | The ftp client against `ftpd-test` over loopback on FS-UAE: a script logs in, lists, uploads and downloads 1 MB files (also to NIL:), checks ascii conversion in both directions (the wire format is checked by switching the server to TYPE I with `quote`), `reget`/`restart`, `mget`/`mput`, mkdir/rmdir/rename/delete, 550 errors, multi-line replies, EPSV refused → PASV with a bogus PASV address, active mode (PORT), aborted transfers (a test hook raises Ctrl-C after N bytes) followed by more commands, a failed login and the URL forms; then IPv6 sessions with a dual-stack `ftpd-test -6` (EPSV, EPRT, `ftp://[::1]:2122/`) and an IPv4 client of the same server. The files land on the host directory DH0: and are compared byte for byte; the transfer rates are printed. |
| `tests/t_arch_m68k.c` (host, `make CROSS=m68k-linux-gnu-`) | Every assembly routine against the C versions: all lengths 0..160 with every source/destination alignment 0..15, 3000 random lengths up to 4096, 64-128 KB and 4 MB buffers, initial sums 0, 0xffffffff and others, random/all-ones/zero data, guard bytes around every copy; odd-offset chaining, the TCP `rt_buf` copy + checksum, routine selection and the sysctls. Run it on each CPU: `RT_CPU=68040 qemu-m68k -cpu m68040 build/m68k-linux-gnu/tests/t_arch_m68k` (also `m68020`, `m68030`, `m68060`). qemu's 68060 model lacks MOVE16, so there the test turns MOVE16 off through the kill switch. It skips in other builds. |
| `tests/amiga/run_fsuae_speedtest.sh build/amiga` | speedtest against three `httpd-test` servers on FS-UAE (plain, chunked with redirects, redirecting to https://): `--server-url`, `--mirror`, all output formats, byte counts against the servers' counters, errors and Ctrl-C. |
| `tests/t_speedtest_*.c` (host) | speedtest's XML, URL and HTTP parsers (every split point, random input), distances; end to end against `tools/speedtest_mock.py` over Linux sockets and over Riptide through TUN. |
| `tests/amiga/run_fsuae_bench.sh build/amiga` | `Riptide BENCH` on FS-UAE (A1200; `FSUAE_MODEL=A4000/040 FSUAE_EXTRA="cpu = 68060"` and so on for other CPUs). |
| `tests/amiga/run_fsuae_netbench.sh build/amiga` | The speed lab: TCP download and upload throughput (2 MB, median of 3) on bridged FS-UAE (`FSUAE_PCAP=`, root; veth `rtnb0`/`rtnb1`, 192.168.79.0/24, or `NB_IF`, `NB_PEER`, `NB_NET`) for four cycle-exact CPU presets (A1200 68020/14, 68030/25, A4000 68040/25, 68060/50; `CPUS=`), with `tests/amiga/netbench.c` on the Amiga (data discarded or generated, no disk) and `tests/amiga/netbench_host.py` on the host; a profiled transfer each way (`Riptide PROFILE`; netbench `-I` adds the CPU time left idle, measured by a priority -127 task, and for uploads the time spent in `send()`) and ping. Writes `results.md` and `results.json`; `BASELINE=tests/amiga/netbench-baseline.json` fails a run more than 10 % (`TOLERANCE=`) slower. `AMITCP_DIR=` adds AmiTCP 3.0b2 (see `docs/COMPARISON.md` 11). `KICKSTART=` boots a real Kickstart instead of FS-UAE's AROS ROM: with Kickstart 3.1.4 the A1200 68020 preset measured 790.7 KB/s down and 450.4 up for 0.23 (gcc build), against 697.5 and 431.0 on the AROS ROM in the same session, so the AROS numbers understate a real A1200 a little. |
| `tests/amiga/test_installer.sh` | Runs `Install_Riptide` with a Linux build of InstallerLG (`INSTALLERLG=`; Aminet `util/sys/InstallerLG.src.lha`, `make` in `build/`) against a fake Amiga tree, with the script's `C:`, `S:`, `ENV:`, `ENVARC:`, `LIBS:` and `SYS:` mapped to directories: Novice (programs, docs, RiptidePrefs in `SYS:Prefs`, riptide.conf, hosts, User-Startup block), Novice again (configuration kept, one block), Pretend (nothing written), Expert with a leftover `LIBS:bsdsocket.library` and another stack in memory, Average with another SANA-II device and a static address, and uninstall. That build answers every question with its default (askbool: the first choice). |
| `tests/t_prefs_conf.c` (host) | RiptidePrefs' configuration parser and writer (`src/amiga/prefs/conf_edit.c`): byte-for-byte round trips (comments, quotes, trailing comments, CRLF, no final newline, empty file), parsing, each kind of edit (in place, inserted, removed, DHCP ↔ static), the Other lines list, input validation and the live commands for Apply; the settings table of both editors (`prefs_fields.c`): a help text and a key for every setting, no key twice on a page or taken by P/S/U/A/C, at most seven rows a page, number and string input, validation and list entries. |
| `tests/amiga/run_vamos_prefs.sh build/amiga/RiptidePrefs` | (in `make -f Makefile.amiga check`) `RiptidePrefs FROM ... SAVE` / `USE` without a window under vamos: the files in `ENVARC:`/`ENV:` equal the input, the second Save leaves `riptide.conf.bak`, error return codes. |
| `MUI_DIR=... tests/amiga/run_fsuae_prefs_single.sh build/amiga` | RiptidePrefs on FS-UAE (AROS) with MUI 3.8: one editor at a time (with the MUI and then the GadTools editor open under a covering window, starting it again as either editor, or as from Workbench, ends that copy with rc 0 and no window, and brings the open one to the front; `USE` still works; Save afterwards), and starting from Workbench: a launcher the script builds (`twb`) starts RiptidePrefs as Workbench does (a process without a CLI and a WBStartup message) and waits for the reply, with a stand-in for Riptide's `RIPTIDE` port that answers and logs every command: Save without changes, Save with a change answering Apply (the command reaches the port) and Later (saved, not applied), and Cancel; each start must be replied and its process gone, and a window and the editor must still open afterwards. Then two copies with icons (`tools/mkicon.py`): tool types `PAGE=Names` and `DEBUG=` must open the MUI editor on Names, and `GADTOOLS` the GadTools editor. |
| `FSUAE_PCAP=... MUI_DIR=... tests/amiga/run_fsuae_prefs_status.sh build/amiga` | RiptidePrefs' Status page on bridged FS-UAE (AROS, MUI 3.8; root; veth `rts0`/`rts1`, 192.168.171.0/24, or `ST_IF`, `ST_PEER`, `ST_NET`) with the real stack: `tests/amiga/status_peer.py` on the host gives a steady download, a slow upload and timed downloads; screenshots of every tab (640 × 480, 256 colours), dark on 640 × 512 with 8 colours, the compact layout, the GadTools editor's Status page, and both editors with Riptide not running; the `DEBUG` log must show reads with traffic in and out and a CPU share. Riptide has two VLANs besides eth0 (`ST_VLANS`), and every window must fit its screen (the window's size from the log). It also times the same download (`DL_SIZE`, 4 MB) with RiptidePrefs closed, on another page and with the Status page open; `ST_UPDATE=0`, `1` or `2` sets the page's Update setting (1, 2 or 5 s). |
| `MUI_DIR=... tests/amiga/run_fsuae_prefs_mui.sh build/amiga` | RiptidePrefs' MUI editor on FS-UAE (AROS) with MUI 3.8 (`MUI_DIR`: the `MUI` drawer of Aminet `util/libs/mui38usr.lha`; the AROS ROM lacks diskfont, iffparse, locale, commodities, asl and datatypes.library, so `tests/amiga/mkstublib.py` makes stand-ins that fail every call, and MUI runs without disk fonts, saved MUI settings, catalogs or file requesters, and shows no help bubbles). The keyboard edits of the GadTools test must give the same `riptide.conf`, the list pages through their forms (an extra address after a refused one, a VLAN with priority 4, a second one made static with an MTU through Replace, a QoS rule); screenshots of every page on a 640 × 480 256-colour screen (light) and a 640 × 512 8-colour one (dark), and the 640 × 256 Workbench (compact layout, a page lit under the pointer; the AROS ROM opens no screen wider than 640, so larger ones are not tested here); About through `CMD=`, Dark Theme saved and used by the next start, Reset to Defaults with Use, Cancel changing nothing, `GADTOOLS`, the three forms in the compact layout, and the bubble texts (`DEBUG` log). Run it for `build/amiga-gcc` too. |
| `tests/amiga/run_fsuae_prefs.sh build/amiga` | RiptidePrefs on FS-UAE (AROS; no MUI there, so the GadTools editor): opens the window, walks every page (screenshots in OUTDIR), switches to a static address and saves to get the validation requester, edits host name, domain, DNS, NTP, mDNS, an extra address, a VLAN and an Other line with the keyboard (xdotool), saves, and compares `ENVARC:` (exact expected file), `ENV:` and the `.bak`. Run it for `build/amiga-gcc` too. |
| `tests/amiga/check_icons.sh` | The icon layout of `tools/mkicon.py` against the NDK's `workbench/workbench.h` (compiled for m68k), and every `.info` file of `build/package` parsed back. |
| `tests/amiga/run_fsuae_installer.sh` | Installs with the real Installer 43.3 (`INSTALLER43=`, Aminet `util/misc/Installer-43_3.lha`) on FS-UAE, at Novice level, driving the GUI with xdotool (`tests/amiga/fsuae_pointer.py`). With `LHA68K=` (Amiga LhA 2.15, Aminet `util/arc/lha_68k.lha`) it first unpacks `build/riptide-<version>.lha` with LhA on the Amiga. Checks the installed files, the configuration and the User-Startup block, then boots again and checks that Riptide starts from `S:User-Startup` with the generated configuration and that `rtcheck -v` passes. |
| `tests/amiga/run_fsuae.sh build/amiga` | Full system test. It boots FS-UAE (A1200 with the built-in AROS Kickstart, headless through Xvfb) from a directory hard drive whose Startup-Sequence runs `rtcheck`. `rtcheck` starts Riptide, runs the checks against the live stack with real multitasking, and then checks that `QUIT` is refused while the libraries are open and accepted after they are closed. The live checks include blocking connect, WaitSelect timeouts through timer.device, Ctrl-C → EINTR, and extra signals. It also opens riptide.library and the compat alias (own base, shared per-task context), checks that `compat aliases off` is refused while the alias is open and removes it once closed, and starts Riptide a second time with `compat aliases off` in the configuration. |
| `tests/t_tcpopt_tun.c` (host, root) | Against the Linux kernel over TUN, the Linux end in a network namespace with `tcp_fastopen=3` and iptables rules as middleboxes: Fast Open (cookie request, data in the SYN with `TCP_FASTOPEN_CONNECT` and `sendto(MSG_FASTOPEN)` as seen by Linux's `TCP_INFO`, a deferred connect that reads or waits first, a server without TFO, SYN-data dropped once and twice, three lost at three servers switching TFO off, `fastopen 0`), the connect timeout and the SYN backoff (times and SYN counts), ICMP host/port unreachable with each `syn_icmp_abort`, `TCP_KEEPINIT`, `SO_SNDTIMEO`, TIME_WAIT with 20 ports (`EADDRNOTAVAIL` without reuse, 20 reused with it) and 3000 short connections, keepalive probes and a silently dead peer, `keepalive_all` and an explicit `SO_KEEPALIVE` 0, and segment counts for Nagle, `TCP_NODELAY`, `TCP_CORK` with its 200 ms limit, `TCP_NOPUSH` and `MSG_MORE` (bytes checked at Linux). |
| `tests/t_mtup_wire.c` | Path MTU black holes between two Riptide stacks over the simulated wire, packets over 1400 bytes silently dropped: 1 MB uploads and downloads over IPv4 and IPv6 complete with `mtu_probing` 1 (black hole after about 6 s, probes up to MSS 1347/1327), the next connection to the same address starts at the remembered size without a timeout (`mtu_cache`), `mtu_probing` 0 stalls until `TCP_USER_TIMEOUT`, and `mtu_probing` 2 probes from `base_mss` up to the full 1448 on a clean path and to 1347 through the hole. |
| `tests/t_mtup_tun.c` (host, root) | The same against Linux over TUN through a Linux router namespace with a 1400 byte link: the router's ICMP "fragmentation needed" dropped by iptables (upload, a Linux client downloading from a Riptide listener, `mtu_probing` 0 stalling), an iptables length filter, and normal path MTU discovery when the ICMP passes; IPv6 too where the host kernel has it. |
| `tests/amiga/run_fsuae_pmtu.sh build/amiga` | Through bsdsocket.library on bridged FS-UAE (`FSUAE_PCAP=`, root; veth `rtpm0`/`rtpm1`, 192.168.170.0/24, or `PM_IF`, `PM_PEER`, `PM_NET`; the Linux end in a namespace with MTU 1400, advmss 1460 and ICMP dropped, `tests/amiga/pmtu_peer.py`): a 1 MB `nc` upload, a 1 MB download and a 1 MB pull from `nc -l` all complete, `mtu_cache` lists the peer, and `netstat -s` counts the black holes. `PM_MTU=1500` runs it without the hole. |
| `tests/amiga/run_fsuae_tcpopt.sh build/amiga` | The same through bsdsocket.library on bridged FS-UAE (`FSUAE_PCAP=`, root; veth `rtf0`/`rtf1`, 192.168.101.0/24, or `TF_IF`, `TF_PEER`, `TF_NET`; the Linux end in a namespace with `tests/amiga/tcpopt_peer.py`): `nc -F` three times (the second and third requests arrive in the SYN), `net.inet.tcp.fastopen_cache`, a connect timed out by `connect_timeout`, one ended by ICMP host unreachable, `nc -K 1` keepalive probes, and the `netstat -s` counters. |
| `tests/amiga/run_fsuae_bridge.sh build/amiga` | Riptide on FS-UAE built with `tools/fs-uae` (`FSUAE_PCAP=` the binary), bridged to a veth pair (needs root; `BR_IF`, `BR_PEER`, `BR_NET` choose other veth names and another /24 than `rtbr0`, `rtbr1`, `192.168.77`): IPv4 ping and two 1 MB HTTP downloads from the host end with the full window (no retransmissions, nothing out of order, bytes compared), IPv6 address autoconfiguration, the RA default route and `ping6` against `tests/amiga/v6responder.py`. |
| `tests/t_vlan_unit.c`, `tests/t_vlan_wire.c`, `tests/t_vlan_tap.c` (host; TAP needs root) | VLANs: the `interface ... vlan=` keyword and its errors, tags and PCP (fixed and from `SO_PRIORITY`) on transmit, receive by id, priority-tagged, unknown-id, QinQ and runt frames, the MTU clamp, parent down/up/removal, link multicast reference counts, the `ifaces` record, a failed line leaving nothing; two stacks on the simulated wire with VLANs 10 and 20 (UDP up to the MTU and fragmented, 1 MB TCP each way, isolation of a VLAN the peer lacks, removal at run time); against Linux over TAP with an `AF_PACKET` peer (the container kernel may lack 8021q): tagged ND, ARP with PCP 5, ping with 1518 byte frames, frames of another VLAN ignored. |
| `tests/amiga/run_fsuae_vlan.sh build/amiga` | VLANs on bridged FS-UAE (`FSUAE_PCAP=`, root; veth `rtv0`/`rtv1`, 192.168.130.0/24 untagged and 192.168.131.0/24 on VLAN 10, or `VL_IF`, `VL_PEER`, `VL_NET`, `VL_NET2`) against `tests/amiga/vlan_peer.py`: ping untagged and on VLAN 10 (also 1496 bytes: 1518 byte tagged frames through uaenet.device), UDP on VLAN 10, PCP 4 on the wire, `ip -d link`, `ifconfig`, `netstat -i`, `STATUS`, VLANs added and removed with `ip` and `ifconfig`, a duplicate id refused. |
| `tests/t_udptx_unit.c` (host) | UDP sending (0.17) against a driver that holds its frames (4 slots): a non-blocking socket is writable while the driver has room, then not writable and `EWOULDBLOCK` (the interface marked for a wakeup), writable again after a completion; a blocking send waits for the completion; one blocked when the interface goes down, or when the driver stops completing frames (stalled for 1 s), wakes up and is not left sleeping; `SO_NO_CHECK` (and Linux's 11) leaves the IPv4 checksum 0, refused on TCP; `rt_api_sendmmsg()` whole, partial and `EWOULDBLOCK`, `rt_api_recvmmsg()` over loopback with lengths and addresses. |
| `tests/amiga/run_fsuae_udpblast.sh build/amiga` | UDP send rate on bridged FS-UAE (`FSUAE_PCAP=`, root; veth `rtub0`/`rtub1`, 192.168.150.0/24; `CPUS`, default a1200-030 and a4000-040, cycle-exact): `tests/amiga/udpblast.c` sends 3000 datagrams per mode (blocking, non-blocking with WaitSelect, `SO_NO_CHECK`, `RiptideSendMMsg()` 16 at a time, multicast, 64-byte datagrams) to `tests/amiga/udpsink.py`, which counts them on the wire and those without a checksum; a second boot with `qos inflight 1`, `qos limit 8` makes the senders wait for room. Prints a table of rates and per-call times; `CHECK=1` fails on a lost or failed datagram, a checksum where there should be none (or the reverse), or a non-blocking sender that never had to wait. |
| `tests/amiga/run_fsuae_mcast.sh build/amiga` | IPv4 multicast on the same bridged FS-UAE (veth `rtmc0`/`rtmc1`, 192.168.82.0/24, no default route) against `tests/amiga/mcast_peer.py`: `nc -u -l -m` joins a group (IGMPv3 report with Router Alert on the wire, a general query from Linux answered, Linux's datagrams received, the leave reported), `ip maddr` and `netstat -g` while joined and after, and `nc -u` to a group Linux joined (source and TTL 1 checked). Host side, `tests/t_igmp_unit.c` covers the IGMP state machines and socket options and `tests/t_mcast_tap.c` repeats the exchange with the Linux kernel over TAP. |
| `tests/amiga/run_fsuae_dns.sh build/amiga` | The resolver's EDNS0 and TCP fallback on FS-UAE bridged to the veth pair `rtd0`/`rtd1` (`FSUAE_PCAP=`, root; `BR_IF`, `BR_PEER`, `BR_NET` choose others) against `tests/amiga/dns_server.py`, which cuts UDP answers at 512 bytes or the query's EDNS0 size and answers in full over TCP: `host NAME` (gethostbyname) of a 40-record name gets its answer over UDP with EDNS0 and no TCP, a 100-record name (A and AAAA) over TCP; `host -t` (its own query) the same; `Riptide DNS` status and `Riptide DNS EDNS_SIZE 0` at run time. |
| `tests/t_dhcp_ll.c` (host), `tests/t_dhcp_tap.c` (host, root) | DHCP options 121/249 and 119 and IPv4 link-local addresses: the option decoders with malformed input (bad widths, truncation, forward and looping compression pointers, bad labels, too many names); a scripted server on the simulated wire: 121 split over two instances with option 3 ignored, routes replaced on renewal, option 3 again, everything removed on stop, a static search list kept; RFC 3927: the MAC-derived first candidate, probe timing, a conflict by an ARP reply and by another host's probe, two announcements, defence and a second conflict, `dhcp IF linklocal off\|on`, a lease replacing the address, no probing next to a static address. Against Linux over TAP with `tests/interop/dhcp_server.py`: the routes carry a datagram to 10.99.0.1, Linux pings the lease, then the link-local address, a conflict when Linux claims it (gratuitous ARP; `arping -U` when installed), a lease replacing it. |
| `tests/amiga/run_fsuae_linklocal.sh build/amiga` | IPv4 link-local on bridged FS-UAE without a DHCP server (`FSUAE_PCAP=`, root; veth `rtl0`/`rtl1`, 192.168.180.0/24, or `LL_IF`, `LL_PEER`, `LL_NET`) against `tests/amiga/linklocal_peer.py`: ARP probes and announcements, the mDNS announcement of `amiga.local`, Linux resolving it and pinging the address, the Amiga pinging 169.254.1.1, `ip dhcp`, `ip route` (no default route), `ip addr`, `STATUS`. |
| `tests/amiga/run_fsuae_netsvc.sh build/amiga` | NTP and mDNS on FS-UAE bridged to the veth pair `rtmd0`/`rtmd1` (`FSUAE_PCAP=`, root): `tests/amiga/netsvc_peer.py` on the host end is an SNTP server and an mDNS peer. The first sync sets the Amiga clock (timer.device, battclock.resource), the next one finds it within 2 s (emulated time drifts); `host` resolves `linuxpeer.local` and the stack's own name over mDNS; the peer sees the probes and announcements and gets answers to QU, QM, legacy unicast and reverse PTR questions. |
| `tests/t_netsvc_unit.c`, `tests/t_netsvc_wire.c`, `tests/t_ntp_tun.c` (host) | The NTP offset/delay arithmetic, NTP time stamps across the 2036 era change, the `ntp`/`timezone`/`mdns` keywords and the mDNS answer parser; two stacks on the simulated wire (`.local` lookups, a reverse PTR question, a kiss-o'-death, the clock step, a name conflict ending in `beta-2.local`); and a sync against a Linux-side SNTP server whose name the client looks up itself (TUN, root). |
| `tests/t_ecn_unit.c` (host) | ECN against a scripted peer on a fake point-to-point interface: the ECN-setup SYN (not ECT), a SYN-ACK with ECE only turns ECN on, one without ECE or with ECE and CWR does not; `ecn` 2 and 0 send no ECN-setup SYN; a retransmitted SYN drops ECE/CWR (and keeps them with `ecn_fallback` 0); ECT(0) on new data but not on ACKs or retransmissions; CE received is echoed with ECE until the peer's CWR; an ECE received reduces cwnd once per window and the next new data carries CWR; `IP_TOS` keeps the ECN bits; the `netstat -s` counters. |
| `tests/t_cubic_unit.c` (host) | CUBIC's slow start exit on a bare connection, round by round with synthetic RTTs: HyStart++ keeps slow start at a steady RTT, enters CSS (a quarter of the growth) on a rise, resumes slow start when the RTT falls, ends in congestion avoidance after 5 CSS rounds; at most 8 segments per stretch ACK; no HyStart++ after a loss; classic HyStart and `hystart=0`. |
| `tests/t_ledbat_unit.c` (host) | Background transfers and memory limits: the LEDBAT++ controller in a fluid model of a 12 Mbit/s link with 20 ms base RTT and 500 ms of buffer (alone: about 50 ms of queue at 97 % utilisation and periodic slowdowns; against Reno: under 1 % of the link; Reno alone: 400 ms of queue), loss handling; rLEDBAT's window on a bare connection; the `background` keyword in every form AmigaDOS hands it over, the sysctls, rules by program name (`rt_host_set_program()`) and local port, `TCP_BACKGROUND`, an explicit `TCP_CONGESTION` winning; socket memory: 12 connections nobody reads stay at the 1 MB limit (2 MB without), the data is intact once read, the defaults from the free memory. |
| `tests/t_ledbat_tun.c` (host, root) | Background transfers against Linux CUBIC over TUN. Downloads: Riptide reads the TUN device at 8 Mbit/s (`rt_host_tun_rx_rate()`), so the kernel's tun queue (300 packets) is the bottleneck; a normal download fills it (about 270 ms RTT), a background one (rLEDBAT by a `lport` rule) keeps the RTT near 65 ms at 96 % of the link, and next to a normal one it gets about 1 %. Uploads: Riptide's transmit rate limit; LEDBAT alone 63 ms RTT (CUBIC 235 ms), against CUBIC about 1 %. |
| `tests/amiga/run_fsuae_bsdsocktest.sh build/amiga` | [bsdsocktest](https://github.com/tbdye/bsdsocktest), an independent bsdsocket.library conformance suite (`BSDSOCKTEST=` its Amiga binary from the release archive), on FS-UAE: its loopback tests (socket lifecycle, send/recv, options, WaitSelect, signals, DNS, utility calls, descriptor transfer, errno, ICMP, throughput). Passes when every test that ran passed; 0.25: 130 of 130, 12 skipped (they need a host helper or a network). On a real Kickstart 3.1.4 (`KICKSTART=`) also 130 of 130, and its "TCP sustained loopback" figure is about 3.4 times the AROS ROM's (667 against 196 to 200 KB/s on the A1200 preset): the stack's calls into exec and timer.device cost much more on the AROS ROM. With vbcc it also runs `tests/amiga/taskprobe.c`, and a run without a result says whether the emulated system lost the stack process (see the next row). |
| `tests/amiga/run_fsuae_execrace.sh [BOOTS] [SECS] [DIR] [quiet]` | Not a Riptide test: two tasks wake each other with `Signal()`/`Wait()` (`tests/amiga/execrace.c`, no TCP/IP stack) on fresh FS-UAE boots, watched by `tests/amiga/taskprobe.c`. On FS-UAE 3.2.35 with the AROS Kickstart ROM, under host load, a task that `Signal()` made ready sometimes ends up in neither exec's `TaskReady` nor `TaskWait` list and never runs again; the rare bsdsocktest/tools runs that stopped with no result (Riptide's stack process lost the same way, in state ready, its links still pointing at the list it was taken out of) are this. Reports how many boots stalled. |
| `MUI_DIR=... tests/amiga/run_fsuae_ks314_wb.sh [PKG]` | On a real Kickstart 3.1.4 with an installed Workbench 3.1.4 (not part of Riptide: `KS314_ROM`, `KS314_SYS`, see `tests/amiga/ks314.sh`) and MUI 3.8, Riptide installed from the package as `Install_Riptide` does and running: RiptidePrefs started by double-clicking its icon on the Workbench window (a real Workbench start), in rounds: host name changed, Save, "Apply" to Riptide; the same answered "Later"; a change, then Esc; started a second time while open (the second copy must hand over and quit); a minute on the Status page, then Save; the GadTools editor (an icon with the `GADTOOLS` tool type), Save, Apply; a help bubble, Project > Save As... and Open... with the ASL requester. After each round `tests/amiga/wbprobe.c` must show no RiptidePrefs left and the Workbench screen's layers free, and Workbench must still select an icon, drag one, open and close a drawer and show its menus. `ROUNDS=` picks rounds. |
| `FSUAE_PCAP=tests/amiga/fsuae_ks314.sh FSUAE_REAL=.../fs-uae ...` | Runs another FS-UAE test on a real Kickstart 3.1.4 instead of the AROS ROM: the wrapper adds `kickstart_file` to the configuration the test writes. The tests boot their own disks with ROM shell commands, so no Workbench is needed. `run_fsuae_bsdsocktest.sh` and `run_fsuae_netbench.sh` also take `KICKSTART=`. |

`rtcheck CONNECT=a.b.c.d:port` also runs a TCP echo test through a real
interface.
