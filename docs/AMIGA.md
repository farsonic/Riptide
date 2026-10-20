# Riptide on AmigaOS

Riptide is a TCP/IP stack for AmigaOS 3.x on a 68020 or better. It replaces
MiamiDx 1.0c and is a drop-in for applications. It publishes three
libraries with the same vector layouts as MiamiDx:

| Library | Version | API |
|---|---|---|
| `bsdsocket.library` | 4.1 | AmiTCP 4: all 46 functions, `socket()` through `GetSocketEvents()` |
| `miami.library` | 21.1 | Miami SDK 2.11: the 53 documented vectors. The MiamiDx-private vectors up to LVO -522 exist and return 0. |
| `usergroup.library` | 4.1 | AmiTCP usergroup, 39 functions. Single user only. |

Riptide runs as one ordinary CLI process. That process owns the timers and
the network drivers. Socket calls run in the calling program's own task,
under one exec semaphore (the stack lock).

## Installation

1. Copy `Riptide` to `C:` or anywhere else in your path.
2. Create the configuration file `ENV:Riptide/riptide.conf` (keep a copy in
   `ENVARC:Riptide/`). Riptide also reads `ENV:Riptide/hosts` if that file
   exists.
3. Start the stack from `S:User-Startup` or from a shell:

   ```
   Run >NIL: Riptide
   ```

   To see the log, redirect it to a file or a console window instead of
   `NIL:`, for example `Run >CON:0/0/640/100/Riptide Riptide`.

Riptide refuses to start if `bsdsocket.library`, `miami.library` or
`usergroup.library` is already in the system library list. It then prints
"A TCP/IP stack is already running:" followed by the id string of each
library it found, so you can tell which stack owns them. Quit MiamiDx,
AmiTCP or Roadshow first. Also turn off any emulator bsdsocket emulation,
and remove another stack's `LIBS:bsdsocket.library`, which loads by itself
when a program opens it.

You do not need SetPatch tricks or `LIBS:` files: the libraries exist only
while Riptide runs.

## Command line

```
Riptide [CONFIG=file] [DEBUG=level] [QUIT] [STATUS] [SELFTEST]
```

| Argument | Meaning |
|---|---|
| `CONFIG=file` | Use this configuration file instead of `ENV:Riptide/riptide.conf`. |
| `DEBUG=n` | Log level: 0 = errors, 1 = warnings (the default), 2 = info, 3 = debug. |
| `STATUS` | Ask the running Riptide for a summary: interfaces, addresses, packet counters, driver and DHCP state, the routing table, DNS servers and library users. |
| `QUIT` | Ask the running Riptide to quit. It refuses while any program still has one of its libraries open. |
| `SELFTEST` | Run the built-in library self test in one task, without starting the stack. It is used under the vamos emulator. |

`QUIT` and `STATUS` talk to the running stack through the public message
port `RIPTIDE`. You can also signal the Riptide process directly:

* **Ctrl-C** works like `QUIT`. If programs still have the libraries open,
  Riptide sends each of them Ctrl-C, as MiamiDx does. It then quits once
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
      [mtu N] [metric N] [[-]promisc] [[-]arp] [delete] [up|down]
dhcp IFNAME [start|stop|renew]     # run (stop, renew) the DHCP client on IFNAME
arp add A.B.C.D MAC [dev IF] | arp del A.B.C.D [dev IF] | arp flush [dev IF]
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

### Amiga keywords

```
interface NAME device=DEVICE [unit=N] [mtu=N] [raw]
interface NAME mni=DRIVER [unit=N] [options="TEXT"] [mtu=N]
interface NAME delete              # at run time: close the driver, drop the interface
user NAME [uid=N] [gid=N] [home=DIR] [realname=TEXT]
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
  keeps 8 `CMD_READ` requests queued for IPv4 and 4 for ARP. Point-to-point
  devices (PPP, SLIP, and any other hardware type) get only the IPv4
  reads and are configured with `ifconfig NAME A.B.C.D peer P.Q.R.S`.
* Cooked mode is the default. `raw` makes Riptide build and strip the
  Ethernet header itself, using `SANA2IOF_RAW`. Use it only for drivers
  with a broken cooked mode.
* If a read fails because the device is offline, Riptide retries every
  second.

**MNI drivers.** `mni=` loads a Miami Network Interface driver (`.mni`)
directly, for example `mni=z2-am7990.mni`.

* If the name has no path, Riptide tries `LIBS:` and then `Miami:MNI/`.
* `unit=` is the board index. `options=` is the driver's option string,
  for example `options="FT=2 MEDIA=AUTO"`.
* Receive is zero copy: the driver writes frames straight into Riptide's
  buffers.
* The ABI was recovered from MiamiDx; see `docs/RE-NOTES.md`, section 4.

**Using Miami's own drivers.**

* Prefer `mni=`: it is faster and has no further requirements.
* Miami's `sanamni.device` wrapper is a SANA-II device too, but it is not a
  good choice with Riptide:
  * it works only with a registered MiamiDx installation;
  * it rejects packet types 0x0800 (IP), 0x0806 (ARP) and 0x8035 (RARP),
    which are exactly the types Riptide needs;
  * it adds a copy.

**User.** `user` sets the single usergroup.library user. The default is
`user`, uid 0, gid 0, home `SYS:`.

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

### Hosts file

`ENV:Riptide/hosts` uses the usual format, `A.B.C.D name [alias...]`, with
`#` comments. Its entries go into the static host table, which is checked
before DNS.

## Tools

Riptide comes with the usual Linux network commands, written for AmigaOS:
`ip`, `ifconfig`, `route`, `arp`, `netstat`, `traceroute`, `ping`, `host`,
`sysctl`, `hostname` and `nc`. Copy them to `C:`. They take Linux style
arguments (`-n`, `-c 3`, ...), print what their Linux counterparts print,
stop on Ctrl-C and return 0 (ok), 5 (warning, e.g. no ping reply), 10
(error) or 20 (failure, e.g. no stack). Each has `-h` / `--help`. They
need little stack (buffers come from AllocVec()).

Where the data comes from:

* `ping`, `traceroute`, `host` (A/PTR) and `nc` use only the standard
  bsdsocket.library API, so they also work with other stacks.
* `ifconfig` (changes), `route add/del` and `arp -s/-d` use the BSD ioctls
  (SIOCAIFADDR, SIOCSIF*, SIOCADDRT/SIOCDELRT, SIOCSARP/SIOCDARP).
* Listings read Riptide's tables with `MiamiSysCtl()` (below), and `ip`,
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
| `ip link add NAME type sana2 device DEV [unit N] [mtu N]` | open a SANA-II device at run time |
| `ip link add NAME type mni driver FILE [unit N] [options TEXT]` | load an MNI driver |
| `ip link del [dev] NAME` | close a driver interface |
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

### ifconfig

net-tools output (`ifconfig` lists the interfaces that are up, `-a` all,
`-s` the short table). The extra `device` line names the driver.

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
counters, `-s` the protocol statistics; `-c` repeats every second until
Ctrl-C.

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

### rtcheck

`rtcheck [START=cmd] [STOP=cmd] [CONNECT=a.b.c.d:port] [NOCHECK]` is the
library test client. Without arguments it runs about 80 checks against the
running stack over loopback. `CONNECT=` runs a TCP echo test through a
real interface, against an echo server on the other end. `START=` starts a
command in the background and waits until bsdsocket.library appears;
`NOCHECK` then exits without running the checks.

## Interfaces for tools

### Tables: `MiamiSysCtl()` and `net.riptide.*`

Riptide publishes its tables as read-only sysctls under the MIB
`{CTL_NET (4), 255, n}` (names `net.riptide.*`). Every table is a `struct
rt_ctl_hdr` (version, record size, count) followed by fixed-layout
records; `include/rt/rt_ctl.h` defines them. Addresses are in network
byte order, everything else in host order; there are no pointers.

| n | name | records |
|---|---|---|
| 1 | `net.riptide.ifaces` | `struct rt_ctl_iface`: name, index, type, flags, MTU, hardware address, address/netmask/broadcast/peer, metric, all packet counters, DHCP state/server/router/lease, driver description |
| 2 | `net.riptide.routes` | `struct rt_ctl_route`: destination, mask, gateway, interface, flags, proto (kernel/static/dhcp/redirect), metric, MTU, use count, expiry, linkdown |
| 3 | `net.riptide.neigh` | `struct rt_ctl_neigh`: ARP entries with state (INCOMPLETE/REACHABLE/STALE/PERMANENT) |
| 4 | `net.riptide.sockets` | `struct rt_ctl_sock`: TCP, UDP and raw sockets with state, queues, RTT, cwnd |
| 5 | `net.riptide.dns` | one `struct rt_ctl_dns`: name servers, search domain, host name |
| 6..9 | `net.riptide.{ip,icmp,udp,tcp}stat` | one `struct rt_{ip,icmp,udp,tcp}_stats` |

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
| `RTCMD_COMMAND` (3) | `text` holds one line of the configuration language (all keywords above, including `interface ...` and `interface NAME delete`), or a query: `sysctl -a`, `sysctl [-n] NAME...`, `sysctl [-w] NAME=VALUE` |

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

## Compatibility notes

**Per-opener library bases.** Like MiamiDx, every task that opens
`bsdsocket.library` or `miami.library` gets its own library base.

* A second `OpenLibrary()` from the same task returns the same base with a
  higher open count.
* `bsdsocket` and `miami` share one per-task context: the descriptor
  table, `errno` and the signal masks.
* Each opener has its own socket descriptor table. The default size is 64;
  change it with `SBTC_DTABLESIZE`.
* Close every library from the task that opened it.

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

**miami.library.**

* Implemented: `MiamiSysCtl()` (by MIB), `MiamiDisallowDNS()`,
  `MiamiGetPid()`, `MiamiIsOnline()` and `MiamiOnOffline()` (by interface
  name; "mi0" or NULL means any interface), `inet_ntop()`, `inet_pton()`,
  `inet_aton()`, `gethostbyname2()` (AF_INET only), `getaddrinfo()`,
  `freeaddrinfo()`, `getnameinfo()`, `gai_strerror()`, the `if_*`
  functions, `sockatmark()`, `MiamiGetFdCallback()` and
  `MiamiSetFdCallback()`.
* Fd callbacks must be 68k register callbacks (`MIAMICPU_M68KREG`); PPC
  callbacks are refused.
* Return fixed values:
  * `MiamiSupportsIPV6()` returns 0.
  * `MiamiOpenSSL()` returns NULL (there is no SSL).
  * `MiamiPFAddHook()` returns NULL (no packet monitoring).
  * `MiamiSetSocksConn()` returns FALSE (no SOCKS).
* The MiamiDx-private vectors (the resolver internals used by MiamiDx's
  own DNS tools, kvm access for its NetStat, and its settings database)
  return 0. Programs that depend on them, such as Miami's NetStat, DiG and
  NSLookup, do not work.

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

**Not supported:** IPv6, `socketpair()`, ancillary data in `recvmsg()`
(`msg_controllen` is always 0), and ARexx.

## Building

The primary toolchain is vbcc with the NDK 3.2 headers. They are a build
dependency and are not part of the repository.

```
make -f Makefile.amiga            # VBCC=/opt/vbcc NDK=/opt/ndk3.2 by default
                                  # -> build/amiga/Riptide, build/amiga/rtcheck
make -f Makefile.amiga check      # hunk checks + vamos self test
```

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
| `tests/amiga/check_layouts.sh` | Riptide's own structure layouts, constants and vector tables against the NDK 3.2, the AmiTCP SDK 4.3 and the Miami SDK 2.11. Set `NDK=`, `MIAMI_SDK=` and `AMITCP_SDK=` to the unpacked archives. |
| `tests/amiga/test_hunk.py FILES` | Loads the executables with amitools and checks their relocations. It also has elf2hunk unit tests: absolute, PC-relative and absolute-symbol relocations, rejecting 16-bit relocations, bss in the hunk size, and a run under vamos. |
| `tests/amiga/run_vamos.sh build/amiga/Riptide` | `Riptide SELFTEST` under vamos: MakeLibrary/AddLibrary, the Open/Close vectors, and about 75 library-level checks through the real jump tables. It drives TCP and UDP over loopback with non-blocking sockets while the test pumps the stack itself. Needs `machine68k==0.3.0`. |
| `tests/amiga/run_fsuae_tools.sh build/amiga` | Boots FS-UAE with a uaenet.device interface (`uae_sana2`, `network_card = a2065`) and runs about 80 tool invocations (every tool, both argument styles, error cases, `ip link add/del` at run time). Their outputs are saved on DH0:out/ and checked with regular expressions. This FS-UAE has no slirp backend (nothing is received on eth0), so eth0 is configured statically and pings, traceroutes and netstat run over loopback. |
| `tests/amiga/run_fsuae.sh build/amiga` | Full system test. It boots FS-UAE (A1200 with the built-in AROS Kickstart, headless through Xvfb) from a directory hard drive whose Startup-Sequence runs `rtcheck`. `rtcheck` starts Riptide, runs the checks against the live stack with real multitasking, and then checks that `QUIT` is refused while the libraries are open and accepted after they are closed. The live checks include blocking connect, WaitSelect timeouts through timer.device, Ctrl-C → EINTR, and extra signals. |

`rtcheck CONNECT=a.b.c.d:port` also runs a TCP echo test through a real
interface.
