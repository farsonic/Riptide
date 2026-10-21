# Riptide on AmigaOS

Riptide is a TCP/IP stack for AmigaOS 3.x on a 68020 or better. It is a
drop-in replacement for a historical commercial Amiga stack (see
`docs/RE-NOTES.md`) and keeps that stack's binary interfaces. It publishes
these libraries:

| Library | Version | API |
|---|---|---|
| `bsdsocket.library` | 4.1 | AmiTCP 4: all 46 functions, `socket()` through `GetSocketEvents()` |
| `riptide.library` | 1.0 | Riptide's own API: `RiptideSysCtl()`, interface state, `getaddrinfo()` & co., `if_*`; 53 documented vectors, reserved vectors up to LVO -522 return 0. See "riptide.library" below. |
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
   to `Riptide/SDK`.
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

The install locations are kept in `ENVARC:Riptide/BinDir`, `DocDir` and
`SDKDir`. Run the installer again to update Riptide (your configuration is
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
Riptide [CONFIG=file] [DEBUG=level] [QUIT] [STATUS] [SELFTEST] [BENCH]
Riptide CAPTURE start|stop|status|save FILE|clear [OPTIONS]
Riptide PROFILE START|STOP|RESET|SHOW
Riptide DEBUG CATEGORIES|NONE|STATUS|SHOW [N]|CLEAR|DUMP FILE [SIZE=KB]
```

| Argument | Meaning |
|---|---|
| `CONFIG=file` | Use this configuration file instead of `ENV:Riptide/riptide.conf`. |
| `DEBUG=n` | Log level: 0 = errors, 1 = warnings (the default), 2 = info, 3 = debug. |
| `STATUS` | Ask the running Riptide for a summary: interfaces, addresses, packet counters, driver and DHCP state, the routing table, DNS servers and library users. |
| `QUIT` | Ask the running Riptide to quit. It refuses while any program still has one of its libraries open. |
| `SELFTEST` | Run the built-in library self test in one task, without starting the stack. It is used under the vamos emulator. |
| `BENCH` | Time the C and assembly checksum and copy routines (see "CPU support") and exit. Needs timer.device, so not under vamos. |
| `CAPTURE ...` | Packet capture in the running stack; the rest of the line is the command (see "Packet capture"). |
| `PROFILE ...` | Control the running stack's profiler (see "Profiling and debug log"). |
| `DEBUG ...` | With a word instead of a number: control the running stack's debug log (see "Profiling and debug log"). A number is still the log level of a new instance. |

`QUIT`, `STATUS`, `CAPTURE`, `PROFILE` and `DEBUG` talk to the running
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
nameserver [add] A.B.C.D           # up to the core's limit, in order
nameserver del A.B.C.D | nameserver flush
host NAME A.B.C.D                  # static host table entry
route add|del|replace|change DEST[/BITS]|default [netmask M] [via|gw] GATEWAY
      [dev IF] [metric N] [mtu N] [unreachable]
route flush [dev IF] | route flush cache
sysctl NAME=VALUE                  # e.g. sysctl net.inet.tcp.sack=1
ifconfig IFNAME [A.B.C.D[/BITS]] [netmask M] [broadcast B] [peer A.B.C.D]
      [mtu N] [metric N] [tcpwindow N] [[-]promisc] [[-]arp] [delete] [up|down]
dhcp IFNAME [start|stop|renew]     # run (stop, renew) the DHCP client on IFNAME
arp add A.B.C.D MAC [dev IF] | arp del A.B.C.D [dev IF] | arp flush [dev IF]
cpu routines auto|generic|020|040|060   # checksum/copy routines (troubleshooting)
capture start [OPTIONS] | capture stop | capture status | capture save FILE | capture clear
                                   # packet capture, see "Packet capture"
profile start|stop|reset|show      # the profiler (see "Profiling and debug log")
debug CATEGORY[,...]|all|none [size=KB] | debug show [N] | debug clear | debug dump FILE
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
| `net.inet.tcp.sack`, `timestamps`, `window_scaling`, `ecn` | 1, 1, 1, 2 | the TCP options (`rfc1323` sets timestamps and window scaling together) |
| `net.inet.tcp.recvspace`, `sendspace` | 32768, 32768 | initial buffer sizes |
| `net.inet.tcp.rmem_max`, `moderate_rcvbuf` | 65536, 1 | receive buffer auto-tuning and its limit (host test builds: 262144) |
| `net.inet.tcp.delack_segs` | 2 | send an ACK every N full-sized segments (1 = every segment) |
| `net.inet.tcp.delack_ms` | 200 | longest an ACK is delayed (ms) |
| `net.inet.tcp.quickack` | 0 | 1 = never delay ACKs |
| `net.inet.tcp.cc` | cubic | congestion control (`cubic`, `reno`) |

`delack_segs` counts like Linux: an ACK goes out once more than N-1
segments are unacknowledged, but only if the window it advertises is not
smaller than the last one; otherwise the ACK waits until the application
reads (or for the delayed ACK timer). The SANA-II read count is changed
with `ip link set NAME reads N` (see below). `netstat -T` shows the effect
on each connection.

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
interface NAME mni=DRIVER [unit=N] [options="TEXT"] [mtu=N]
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
  straight into its packet buffers. It then runs `S2_DEVICEQUERY` (MTU,
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
Libraries: bsdsocket.library 4, riptide.library 1, usergroup.library 4; compat alias: miami.library 21
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

### TCP window per interface

`tcpwindow=N` on the `interface` line, `ifconfig IF tcpwindow N` or `ip
link set IF tcpwindow N` limit the receive window that TCP offers on
connections routed over that interface (like Linux `ip route ... window
N`). Buffer auto-tuning still runs but never offers more. 0 removes the
limit; the value is 0 or 536..16777216 bytes and applies to connections
opened after the change. `ip link` and `Riptide STATUS` show it. Other
interfaces keep the full window, so the limit costs nothing on a real
network card.

### Hosts file

`ENV:Riptide/hosts` uses the usual format, `A.B.C.D name [alias...]`, with
`#` comments. Its entries go into the static host table, which is checked
before DNS.

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
the driver), `-br` brief, `-4`.

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
| `ip link del [dev] NAME` | close a driver interface |
| `ip link resetstats [[dev] IF]` | **Riptide extension:** zero the counters of IF (all interfaces without IF) |
| `ip addr [show [dev] IF]` | addresses; DHCP addresses are `dynamic` with the lease time left |
| `ip addr add\|replace A.B.C.D/BITS [peer P] [broadcast B] dev IF` | Riptide keeps one IPv4 address per interface: `add` refuses a second one, `replace` changes it |
| `ip addr del A.B.C.D[/BITS] dev IF`, `ip addr flush dev IF` | |
| `ip route [show] [cache]` | routes with `proto kernel/static/dhcp/redirect`; `cache` lists path MTU entries |
| `ip route add\|del\|replace\|change\|append PREFIX\|default [via GW] [dev IF] [metric N] [mtu N]` | also `ip route add unreachable PREFIX` |
| `ip route get ADDR` | which route a packet would take |
| `ip route flush dev IF\|cache\|all` | |
| `ip neigh [show [dev IF]]` | ARP table: `10.0.2.2 dev eth0 lladdr 52:55:0a:00:02:02 REACHABLE` |
| `ip neigh add\|replace ADDR lladdr MAC [dev IF] [nud permanent]` | entries are always permanent |
| `ip neigh del ADDR [dev IF]`, `ip neigh flush [dev IF]` | |
| `ip dhcp [show] [dev IF]` | **Riptide extension:** DHCP state, server, router, lease |
| `ip dhcp start\|stop\|renew [dev] IF` | **Riptide extension** |
| `ip dns [show]`, `ip dns add\|del ADDR`, `ip dns flush`, `ip dns search DOMAIN` | **Riptide extension:** resolver configuration (resolv.conf style output) |

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
counters, `-s` the protocol statistics, `-T` (`--tcp-info`) the details
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
timeout in seconds.

### sysctl, hostname, nc

* `sysctl -a`, `sysctl [-n] NAME...` (a prefix such as `net.inet.tcp`
  lists everything below it), `sysctl [-w] NAME=VALUE`.
* `hostname [-s|-f|-d|-i|-I] [NAME]` shows or sets the host name.
* `nc [-d] [-v] [-w secs] HOST PORT` and `nc -l [-p] PORT` (accept one
  connection): a minimal netcat for testing. Standard input is sent first,
  then the reply is copied to standard output (AmigaDOS input cannot be
  waited on together with a socket); `-d` sends nothing; `-u` sends stdin
  as one UDP datagram.

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
Download: 12.34 Mbit/s
Testing upload speed....................
Upload: 1.23 Mbit/s
```

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
| `checksum` | checksums that are not combined with a copy |
| `copy to app`, `copy from app` | copying between the application's buffer and Riptide |
| `driver send` | the `SendIO()` / `MNI_Send()` call |
| `driver rx copy` | the SANA-II driver's `CopyToBuff` callback (driver context) |
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
| 1 | `net.riptide.ifaces` | `struct rt_ctl_iface`: name, index, type, flags, MTU, hardware address, address/netmask/broadcast/peer, metric, all packet counters, DHCP state/server/router/lease, driver description, `tcpwindow` (0.12) |
| 2 | `net.riptide.routes` | `struct rt_ctl_route`: destination, mask, gateway, interface, flags, proto (kernel/static/dhcp/redirect), metric, MTU, use count, expiry, linkdown |
| 3 | `net.riptide.neigh` | `struct rt_ctl_neigh`: ARP entries with state (INCOMPLETE/REACHABLE/STALE/PERMANENT) |
| 4 | `net.riptide.sockets` | `struct rt_ctl_sock`: TCP, UDP and raw sockets with state, queues, RTT, cwnd |
| 5 | `net.riptide.dns` | one `struct rt_ctl_dns`: name servers, search domain, host name |
| 6..9 | `net.riptide.{ip,icmp,udp,tcp}stat` | one `struct rt_{ip,icmp,udp,tcp}_stats` |
| 18 | `net.riptide.tcpinfo` | `struct rt_ctl_tcpinfo`: every TCP socket of both families with RTT, windows, buffers, queues, loss and ACK counters, 64-bit byte counts and time spent busy, window/buffer limited, with unread data and with a zero window (`netstat -T`) |
| 19 | `net.riptide.profile` | `struct rt_ctl_prof`: the profiler (`profile start`): record 0 the run (wall-clock time, bytes received), then one per section and counter (calls, total and longest time) |
| 20 | `net.riptide.drivers` | `struct rt_ctl_driver`, one per driver interface (SANA-II, MNI): receive queues (reads configured/queued, lowest, times empty), frames per wakeup, buffer shortages, errors by code, transmits in flight, the `S2_DEVICEQUERY` values and the driver's own `S2_GETGLOBALSTATS`/`S2_GETSPECIALSTATS` |

Use BSD sysctl semantics: call with `oldp = NULL` to get the size,
allocate a bit more (a table can grow in between) and call again. If it
still does not fit, the call fails with `ENOMEM`, the buffer holds the
records that fitted (with the header count adjusted) and `*oldlenp` the
size needed. Readers must step through records by `recsize`, so later
versions can append fields.

### BSD ioctls

Besides the interface ioctls (`SIOCGIFCONF`, `SIOC[GS]IF*`), Riptide
supports `SIOCAIFADDR`, `SIOCADDRT`/`SIOCDELRT` (4.3BSD `struct ortentry`)
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
(stateless DHCPv6, RFC 8415). `lo0` has `::1`.

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
`dad_count` (1), `hlim` (64), `forwarding` (0), `mtu_expires` (600 s),
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
multicast groups IPv6 needs are programmed with
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
* `getnetbyname()` and `getnetbyaddr()` know only "loopback" (net 127).
* `getservbyname()`, `getservbyport()`, `getprotobyname()` and
  `getprotobynumber()` use built-in tables.

**riptide.library.**

* Implemented: `RiptideSysCtl()` (by MIB), `RiptideDisallowDNS()`,
  `RiptideGetPid()`, `RiptideIsOnline()` and `RiptideOnOffline()` (by
  interface name; "mi0" or NULL means any interface),
  `RiptideGetHardwareLen()`, `RiptideResGetOptions()`/`RiptideResSetOptions()`,
  `RiptideSupportedCPUs()`, `inet_ntop()`, `inet_pton()`, `inet_aton()`,
  `gethostbyname2()`, `getaddrinfo()`, `freeaddrinfo()`,
  `getnameinfo()`, `gai_strerror()`, the `if_*` functions, `sockatmark()`,
  `RiptideGetFdCallback()` and `RiptideSetFdCallback()`.
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
riptide.library, generated from `src/amiga/lib_lvos.txt` by
`tools/gen_lvos.py` (`make -f Makefile.amiga sdk`; `check` fails when it
is out of date):

| File | |
|---|---|
| `sdk/fd/riptide_lib.fd` | FD file (base `_RiptideBase`; the reserved vectors are `##private`), for `fd2pragma`, `fd2sfd` and the like |
| `sdk/include/libraries/riptide.h` | `RIPTIDE_LIB_NAME`, `RIPTIDE_LIB_VERSION`, `RIPTIDECPU_*`, the `net.riptide.*` MIB numbers |
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
  one pass; TCP uses it to build each segment it sends and UDP to take
  the payload from the program.
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
the Aminet readme `Riptide.readme`, `C/` with all programs, `Docs/` and
`SDK/`, with icons made by `tools/mkicon.py` (classic OS 2.x/3.x
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
| `tests/amiga/check_layouts.sh` | Riptide's own structure layouts, constants and vector tables against the NDK 3.2, the AmiTCP SDK 4.3 and the SDK 2.11 of the historical API (`COMPAT_SDK`): the compat alias must keep its vector layout (riptide.library's names are mapped to the historical ones), and `sdk/fd/riptide_lib.fd` must match the table. Set `NDK=`, `COMPAT_SDK=` and `AMITCP_SDK=` to the unpacked archives. |
| `tests/amiga/test_hunk.py FILES` | Loads the executables with amitools and checks their relocations. It also has elf2hunk unit tests: absolute, PC-relative and absolute-symbol relocations, rejecting 16-bit relocations, bss in the hunk size, and a run under vamos. |
| `tests/amiga/run_vamos.sh build/amiga/Riptide` | `Riptide SELFTEST` under vamos: MakeLibrary/AddLibrary, the Open/Close vectors, and about 75 library-level checks through the real jump tables. It drives TCP and UDP over loopback (127.0.0.1 and ::1, 24- and 28-byte `sockaddr_in6`, a dual-stack listener, the IPv6 resolver calls) with non-blocking sockets while the test pumps the stack itself. Needs `machine68k==0.3.0`. |
| `tests/amiga/run_fsuae_tools.sh build/amiga` | Boots FS-UAE with a uaenet.device interface (`uae_sana2`, `network_card = a2065`) and runs about 80 tool invocations (every tool, both argument styles, error cases, `ip link add/del` at run time). Their outputs are saved on DH0:out/ and checked with regular expressions. This FS-UAE has no slirp backend (nothing is received on eth0), so eth0 is configured statically and pings, traceroutes and netstat run over loopback. IPv6: `ip -6` addr/route/neigh changes and listings, `ifconfig inet6`, `ping6`/`ping -6`, `traceroute6`, `host` AAAA and `ip6.arpa`, `nc -6` (also an IPv4 client of a dual-stack listener), `netstat -6`. It also runs `sdk_test` (built from `sdk/` only) and checks the library line of the startup log and of `STATUS`. |
| `tests/amiga/run_fsuae_ftp.sh build/amiga` | The ftp client against `ftpd-test` over loopback on FS-UAE: a script logs in, lists, uploads and downloads 1 MB files (also to NIL:), checks ascii conversion in both directions (the wire format is checked by switching the server to TYPE I with `quote`), `reget`/`restart`, `mget`/`mput`, mkdir/rmdir/rename/delete, 550 errors, multi-line replies, EPSV refused → PASV with a bogus PASV address, active mode (PORT), aborted transfers (a test hook raises Ctrl-C after N bytes) followed by more commands, a failed login and the URL forms; then IPv6 sessions with a dual-stack `ftpd-test -6` (EPSV, EPRT, `ftp://[::1]:2122/`) and an IPv4 client of the same server. The files land on the host directory DH0: and are compared byte for byte; the transfer rates are printed. |
| `tests/t_arch_m68k.c` (host, `make CROSS=m68k-linux-gnu-`) | Every assembly routine against the C versions: all lengths 0..160 with every source/destination alignment 0..15, 3000 random lengths up to 4096, 64-128 KB and 4 MB buffers, initial sums 0, 0xffffffff and others, random/all-ones/zero data, guard bytes around every copy; odd-offset chaining, the TCP `rt_buf` copy + checksum, routine selection and the sysctls. Run it on each CPU: `RT_CPU=68040 qemu-m68k -cpu m68040 build/m68k-linux-gnu/tests/t_arch_m68k` (also `m68020`, `m68030`, `m68060`). qemu's 68060 model lacks MOVE16, so there the test turns MOVE16 off through the kill switch. It skips in other builds. |
| `tests/amiga/run_fsuae_speedtest.sh build/amiga` | speedtest against three `httpd-test` servers on FS-UAE (plain, chunked with redirects, redirecting to https://): `--server-url`, `--mirror`, all output formats, byte counts against the servers' counters, errors and Ctrl-C. |
| `tests/t_speedtest_*.c` (host) | speedtest's XML, URL and HTTP parsers (every split point, random input), distances; end to end against `tools/speedtest_mock.py` over Linux sockets and over Riptide through TUN. |
| `tests/amiga/run_fsuae_bench.sh build/amiga` | `Riptide BENCH` on FS-UAE (A1200; `FSUAE_MODEL=A4000/040 FSUAE_EXTRA="cpu = 68060"` and so on for other CPUs). |
| `tests/amiga/test_installer.sh` | Runs `Install_Riptide` with a Linux build of InstallerLG (`INSTALLERLG=`; Aminet `util/sys/InstallerLG.src.lha`, `make` in `build/`) against a fake Amiga tree, with the script's `C:`, `S:`, `ENV:`, `ENVARC:`, `LIBS:` and `SYS:` mapped to directories: Novice (programs, docs, riptide.conf, hosts, User-Startup block), Novice again (configuration kept, one block), Pretend (nothing written), Expert with a leftover `LIBS:bsdsocket.library` and another stack in memory, Average with another SANA-II device and a static address, and uninstall. That build answers every question with its default (askbool: the first choice). |
| `tests/amiga/check_icons.sh` | The icon layout of `tools/mkicon.py` against the NDK's `workbench/workbench.h` (compiled for m68k), and every `.info` file of `build/package` parsed back. |
| `tests/amiga/run_fsuae_installer.sh` | Installs with the real Installer 43.3 (`INSTALLER43=`, Aminet `util/misc/Installer-43_3.lha`) on FS-UAE, at Novice level, driving the GUI with xdotool (`tests/amiga/fsuae_pointer.py`). With `LHA68K=` (Amiga LhA 2.15, Aminet `util/arc/lha_68k.lha`) it first unpacks `build/riptide-<version>.lha` with LhA on the Amiga. Checks the installed files, the configuration and the User-Startup block, then boots again and checks that Riptide starts from `S:User-Startup` with the generated configuration and that `rtcheck -v` passes. |
| `tests/amiga/run_fsuae.sh build/amiga` | Full system test. It boots FS-UAE (A1200 with the built-in AROS Kickstart, headless through Xvfb) from a directory hard drive whose Startup-Sequence runs `rtcheck`. `rtcheck` starts Riptide, runs the checks against the live stack with real multitasking, and then checks that `QUIT` is refused while the libraries are open and accepted after they are closed. The live checks include blocking connect, WaitSelect timeouts through timer.device, Ctrl-C → EINTR, and extra signals. It also opens riptide.library and the compat alias (own base, shared per-task context), checks that `compat aliases off` is refused while the alias is open and removes it once closed, and starts Riptide a second time with `compat aliases off` in the configuration. |
| `tests/amiga/run_fsuae_bridge.sh build/amiga` | Riptide on FS-UAE built with `tools/fs-uae` (`FSUAE_PCAP=` the binary), bridged to a veth pair (needs root): IPv4 ping and two 1 MB HTTP downloads from the host end with the full window (no retransmissions, nothing out of order, bytes compared), IPv6 address autoconfiguration, the RA default route and `ping6` against `tests/amiga/v6responder.py`. |
| `tests/amiga/run_fsuae_bsdsocktest.sh build/amiga` | [bsdsocktest](https://github.com/tbdye/bsdsocktest), an independent bsdsocket.library conformance suite (`BSDSOCKTEST=` its Amiga binary from the release archive), on FS-UAE: its loopback tests (socket lifecycle, send/recv, options, WaitSelect, signals, DNS, utility calls, descriptor transfer, errno, ICMP, throughput). Passes when every test that ran passed; 0.12: 130 of 130, 12 skipped (they need a host helper or a network). |

`rtcheck CONNECT=a.b.c.d:port` also runs a TCP echo test through a real
interface.
