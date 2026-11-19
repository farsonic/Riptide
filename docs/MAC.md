# Riptide on the classic Mac OS

Riptide as a MacTCP replacement: the `.IPP` driver that every classic Mac Internet program talks
to, with Riptide's core behind it and the Mac's own `.ENET` Ethernet driver underneath, exactly
where MacTCP sits. A program written for MacTCP opens `.IPP` and makes the same Control calls;
the Ethernet card's driver sees the same protocol handlers and EWrite calls MacTCP would make.
The target is System 7 (6.0.7 should work but is untested) on a 68020 or later, without Open
Transport, which installs a MacTCP layer of its own.

Developed and tested on Aurora68 (a Raspberry Pi in a Macintosh's 68000 socket running the
Macintosh IIci ROM; github.com/farsonic/Aurora68), whose testbed boots System 7.1 under QEMU with
an Ethernet card and a host-side peer, but nothing in `src/mac/` depends on it except the
optional development log.

## Status

| Stage | What | State |
|---|---|---|
| 1 | The driver installs and starts at boot, the core runs, Ethernet through `.ENET`, `ipctlGetAddr`, `ipctlEchoICMP` | done: PiPing (a MacTCP ping) gets replies through Riptide |
| 2 | TCP and UDP: TCPCreate ... TCPRelease, the UDP set, ASR notifications, TCPStatus, TCPGlobalInfo | done in the testbed (src/mac/mactcp.c): tests/mac/mactcp_test.c passes 21 of 21 against the Aurora68 peer |
| 3 | The resolver: a `'dnrp'` code resource the DNR glue finds (StrToAddr, AddrToName; HInfo and MXInfo answer noAnsErr) | done in the testbed: the Riptide DNR file (src/mac/dnr), an asynchronous DNS client in the driver |
| 4 | Preferences | done: RiptidePrefs, the Amiga's MUI editor built for the Mac (MAC-GUI.md) |
| 5 | The command-line tools | done: the Riptide Terminal, the Amiga's ip, ifconfig, route, arp, netstat, ping, ping6, traceroute, traceroute6, host, hostname, nc, ftp, sysctl and speedtest in a Mac window ("The Riptide Terminal" below) |

Also working in the testbed (18 November): DHCP (`dhcp en0`: bound, lease kept), IPv6 (a
link-local address and one from the router advertisement's prefix, each passing duplicate
address detection and answering ICMPv6 echo; the IPv6 multicast groups are registered with
`.ENET`'s EAddMulti), riptide.conf from the Preferences folder with the Amiga's `interface`
line, and the Status page of RiptidePrefs reading the running stack.

![RiptidePrefs on the Mac: the Status page with DHCP](mac-gui-status.png)

![PiPing through Riptide](mac-piping.png)

## How it fits together

```
 application (MacTCP API: PBControl on .IPP)
   |  drvr.S: Control entry; the call leaves the Device Manager's queue (a concurrent driver)
   v
 ipp.c ------ queued, handled by the stack's deferred task, completed with ioResult + ioCompletion
   |  Riptide's descriptor socket API (non-blocking)
   v
 src/core (unchanged: the same files as the Amiga build)
   |  rt_netif link_output / rt_netif_input
   v
 net_mac.c -- EWrite (async, transmit slots)  /  receive ring <- mac_enet_ph (drvr.S, EAttachPH)
   v
 .ENET (the card's driver: found with SGetTypeSRsrc, opened with OpenSlot)
```

**One context.** The Mac has one processor and no threads, so every call into the core happens
in the stack's deferred task (Deferred Task Manager: interrupt level 0 with interrupts enabled,
after the interrupt handlers). The Time Manager task (the core's next timer), the `.ENET`
protocol handler (a frame arrived) and the Control routine (a new call) only queue work and ask
for that task. The core's lock is therefore a nesting counter, and its wait queues never block:
the MacTCP interface is asynchronous anyway, and a synchronous caller spins on `ioResult` exactly
as it does with MacTCP. The task runs on the stack it interrupted. A private stack in the system
heap trips the stack sniffer (System Error 28, "stack collision with heap"), and the core keeps
its frames small (DESIGN.md).

**Memory.** The Memory Manager may not be called at interrupt time, so the OS layer allocates a
1 MB pool at Open and runs a first-fit allocator in it (`rt_malloc`). The Ethernet rings and the
driver itself are allocated at Open too. The extension's `'sysz'` resource asks System 7 to grow
the system heap for all of it before the INIT runs. Without it the 415 KB driver did not load.

**A concurrent driver.** MacTCP completes calls out of order: a TCPRcv can wait for minutes while
TCPSend calls complete. The Device Manager hands a driver one call at a time and starts the next
one at IODone. So the Control entry takes each call off the driver's queue, clears `drvrActive`
and returns (Inside Macintosh: Devices, "Writing a Device Driver"), and `mac_complete` later sets
`ioResult` and calls `ioCompletion` (A0 = the parameter block, D0 = the result). MacTCP's
notification routines are pascal procedures: `mac_call_pascal1` pushes the argument, also sets
A0 (PiPing's routine reads A0), and restores the stack pointer afterwards either way.

**The image.** The driver is C compiled with an ordinary m68k ELF GCC (`-fno-pic`), linked at 0
with `--emit-relocs`. `tools/elf2mac.py` turns it into a `'DRVR'` resource: the code, the data,
the zero-filled BSS, then a table of every 32-bit absolute field. The driver's first Open adds
its load address to each field and flushes the instruction cache before any C runs (drvr.S). The
INIT (init.S, position independent) loads `'DRVR'` 128 into the system heap, installs it in a
free unit table entry, fills in the device control entry (`_DrvrInstall` makes the entry but
leaves the driver pointer and flags to the caller), and opens `.IPP`. If a `.IPP` driver is
already installed, MacTCP's for instance, it does nothing.

## Building and installing

```
make -f Makefile.mac                 # build/mac/Riptide.bin, a MacBinary extension file, and the
                                     # programs: RiptidePrefs.bin, RiptideTerminal.bin, RiptideDNR.bin
make -f Makefile.mac CONF=my.conf    # with my.conf as its riptide.conf ('TEXT' 128)
make -f Makefile.mac AURORA68=1      # development on Aurora68: the log also goes to the Pi's log
```

Needs `gcc-m68k-linux-gnu` and `binutils-m68k-linux-gnu` (Debian, Ubuntu) and Python 3. Copy
`Riptide.bin` into the System Folder's Extensions folder (`hcopy -m` for an HFS image, or any
MacBinary tool), take MacTCP out of the Control Panels folder, and restart.

The configuration is riptide.conf, the same generic keywords as on the Amiga (AMIGA.md,
"Configuration file"). Riptide reads the one RiptidePrefs saves (the Riptide folder in the
Preferences folder), else the Riptide file's `'TEXT'` 128 resource (make `CONF=`), else it opens
every Ethernet card with DHCP on each. The card line is the Amiga's, with the Mac's driver:
`interface en0 device=.ENET unit=0` (without any, every card is opened: en0, en1 ... in the Slot
Manager's order; "Several cards" below). A static setup:

```
ifconfig en0 192.168.1.20/24
route add default gw 192.168.1.1
nameserver 192.168.1.1
```

## Several cards

Every card the Slot Manager lists as catNetwork/typeEtherNet can be an interface of its own
(src/mac/net_mac.c, up to four): its own refnum from OpenSlot on its slot and sResource id, its
own protocol handlers (drvr.S has an entry point per card, `mac_enet_ph0` to `3`, because a
driver does not tell its handler which card it is), its own receive ring and transmit slots. One
NuBus card may hold several: Aurora68's slot-9 card carries its wired port (sResource $81,
`.Network_EtherNet_PiStorm`) and its WiFi ($83, `.Network_WiFi_PiStorm`), two drivers with two
unit-table entries. So a card is named by its slot and sResource id, by its kind, by part of its
driver's name, or by its place in the list:

```
interface en0 device=.ENET card=ethernet      # or slot=9 id=0x81, or unit=0
interface en1 device=.ENET card=wifi          # or slot=9 id=0x83, or unit=1, or card=Network_WiFi
dhcp en0
dhcp en1
```

`card=wifi` is a card whose driver's name has "WiFi", "Wireless" or "AirPort" in it;
`card=ethernet` any other. With `card=`, `slot=` or `id=`, a `unit=` on the same line (RiptidePrefs
always writes one) is ignored. A line that names a card the Mac does not have is logged and
skipped, so the same riptide.conf serves a Mac with only one of them. RiptidePrefs' first run
writes exactly the four lines above (with `unit=`).

**Which way out.** Each interface has a metric, 10 for a wired card and 20 for a WiFi card
(`metric=N` on the interface line changes it), and DHCP gives its routes that metric, so with both
cards on DHCP the default route goes through the wired card. The core keeps both default routes
even when both DHCP servers name the same router (src/core/route.c: the same route through
another interface is another route). A driver that can say whether its link is up is asked once a
second: Aurora68's answer csLinkStatus (230), a Control call of their own, with 'A68L' in the
answer so that it cannot be mistaken; other drivers are taken as always up. When the wired link
goes down the core stops using it (`rt_netif_link_change`) and the WiFi's route takes over; when
it comes back, DHCP confirms the lease and the wired route wins again. MacTCP's calls
(ipctlGetAddr, the MTU) report the interface the default route uses, which changes with it.

**mDNS** answers on every interface with that interface's addresses (src/core/mdns.c already
did); the host name is one, and the Mac's own answers heard back through the other card are
recognised as its own, so "macse.local" does not conflict with itself when both cards are on one
network.

**The Amiga build** shares only the two core changes (DHCP's routes take the interface's metric,
which is 0 there unless set; the same route through two interfaces is allowed); its own files are
unchanged.

**Tested** (19 November) on Aurora68's solo testbed, the wired card being the kernel's file bridge
or QEMU's USB network device, the WiFi a simulated radio, each with its own host peer: both cards
listed, opened with refnums -53 and -54 and their own addresses, DHCP bound on both (two networks),
pings out of each, two default routes with metrics 10 and 20; with the wired cable "pulled"
(`mac.netdown=`) a running ping moved to the WiFi within a second and back when the link returned,
one echo lost at the change (Aurora68's docs/08 item 49 and its docs/img/system71-riptide-*.png).
Once, after a 100-second outage, the Mac stopped with a system error just after the link came
back; a second run with a 40-second outage did not, and the cause is not known yet. Not tested:
a Mac with two real cards in two slots, and a driver without csLinkStatus (it is taken as always
up, so its route never yields).

## Testing on Aurora68

The Aurora68 repository's `tools/run-solo.sh` boots a System 7.1 disk image under QEMU with the
Pi kernel as the whole logic board, and `tools/pinet.py` is an Ethernet peer at 192.168.68.1 on
the host (ARP, ICMP echo, UDP echo, BOOTP). With Riptide in the image's Extensions folder and
MacTCP removed:

```
python3 tools/pinet.py /tmp/psmac-net peer &
HD=test.image tools/run-solo.sh 200 mac.net=/tmp/psmac-net
```

Built with `AURORA68=1`, every Riptide log line and the INIT's milestones show in the kernel log
as `[MAC] Riptide: ...`, written through the Aurora68 card's mailbox.

## TCP, UDP and the resolver (stages 2 and 3)

`src/mac/mactcp.c` makes each MacTCP stream a socket of the core. A call that cannot finish at
once (an open, a receive with no data, a send with the send buffer full) stays on its stream and
is looked at again whenever the stack runs; it completes like every other call, through
`mac_complete`. The notification routines are called from the stack's context as MacTCP calls
them: TCPDataArrival when data comes and no receive waits for it, TCPClosing at the other end's
FIN, TCPTerminate when the connection dies, UDPDataArrival. TCPNoCopyRcv and UDPRead hand out
the stream's own receive buffer (given back with TCPRcvBfrReturn and UDPBfrReturn); TCPSend
completes when its data is in the core's send buffer.

The resolver is the Riptide DNR file: type `'cdev'`, creator `'ztcp'`, in the System Folder,
holding `'dnrp'` 0 (src/mac/dnr). Apple's DNR glue, which every MacTCP program carries, finds it
as it finds MacTCP's (MacTCP must be out of the Control Panels folder and the System Folder).
Dotted addresses are answered at once; names and addresses go to the driver as a private call
(`RTMAC_DNS`), and the answer reaches the program's result procedure from the call's completion
routine, StrToAddr having returned cacheFault meanwhile. The lookups are the driver's own small
DNS client (A and PTR queries over UDP, the configured name servers, the search domain), because
the core's resolver waits in select, which the deferred task must never do.

Install: Riptide in the Extensions folder, Riptide DNR in the System Folder, MacTCP out of the
Control Panels folder; RiptidePrefs anywhere. `make -f Makefile.mac test` builds the test program
(MacTCPTest.bin), which runs against Aurora68's `tools/pinet.py` (TCP and UDP echo on port 7, DNS
for `*.testbed`).

## The Riptide Terminal

Riptide for the Amiga comes with command-line tools (src/amiga/cmd). The Mac has no command line,
so the Riptide Terminal (src/mac/term, `build/mac/RiptideTerminal.bin`) is one: a window you can
move, resize and zoom, a scrolling console in Monaco 9, and a `riptide>` prompt. A line typed
there runs the tool it names, from the same source files as the Amiga's tools, unchanged
(compiled as RiptidePrefs' files are, with the Amiga's types and plain prototypes, docs/MAC-GUI.md):
`ip addr`, `ip route`, `ip neigh`, `ifconfig`, `route -n`, `arp -a`, `netstat -s`, `ping -c 3 HOST`,
`ping6`, `traceroute`, `host NAME`, `nc HOST PORT`, `ftp`, `sysctl -a`, `speedtest`. Output goes to
the console, Command-period stops the running tool (the Amiga's Ctrl-C), nc and ftp read lines
typed while they run (Command-D or Control-D ends that input, as Ctrl-\ does on the Amiga), Up
and Down bring back earlier lines, File > Clear Window (Command-K) empties it. Built in are
`help`, `history`, `clear`, `quit` and `conf LINE`, which hands a riptide.conf line to the
running stack (as the RIPTIDE port's COMMAND: `conf nameserver 192.168.1.1`,
`conf host printer 192.168.1.9`). The lines of the text file "Terminal Startup" in
Preferences:Riptide:, if there is one, run at launch as if typed.

**The tools wait, and on the Mac nothing else does.** The tools use the BSD socket interface: a
recv waits for data, select for a socket or a timeout, a name lookup for its answer. MacTCP's
interface (ipp.c) never waits: it queues the call for the stack's deferred task. So the driver
has a second way in for programs: the private Control call `RTMAC_API` answers with the address
of a table of plain C functions (src/mac/api_mac.c, struct rtmac_api in
src/mac/prefs/mac_ipp_private.h): the core's socket calls (`rt_api_*`), the resolver
(`rt_resolve_name46`, `rt_resolve_addr`, `rt_resolve_addr6`), sysctl (the `net.riptide` tables
ip, route, arp and netstat print), the configuration commands, interface names. The program calls
them directly, in its own context, the way a bsdsocket.library call runs in the calling task on
the Amiga. While such a call runs the core is the program's: the deferred task, should an
interrupt start it, puts its work off, and the call asks for it again when it leaves
(os_mac.c, `mac_core_enter`/`mac_core_leave`). A call that must wait sleeps on its client's
sleeper: it gives the core back, calls the program's yield function (WaitNextEvent: the Mac, and
the deferred task that brings the frames in, go on; the window stays alive), takes the core again
and looks once more, until a waker sets the sleeper, the time is up or Command-period was
pressed (`-EINTR`, or `SIGBREAKF_CTRL_C` for a select that asked for it, as WaitSelect does).
src/mac/term/term_bsd.c turns the table into bsdsocket.library's and riptide.library's calls,
term_os.c gives the tools Output(), Input(), Delay(), the EClock (the Mac's Microseconds) and
the rest of what they use, on RiptidePrefs' exec and dos (mm_os.c).

On the Amiga each run of a tool is a fresh program. Here the tools stay loaded, so their static
data is put back before every run: the linker script (src/mac/term/term.ld) gathers their
`.data` and `.bss` in one place, and the terminal keeps a copy of the first.

Left out: `rtdiag`, which runs other commands through SystemTagList and writes a packet capture
(the Mac driver has no capture files). `speedtest` is in, but needs the Internet.

![The Riptide Terminal](mac-terminal.png)

## Notes for the next stages

- Real programs (Internet Explorer, Fetch, NCSA Telnet) will show what is missing: TCPPassiveOpen
  with a remote filter, urgent data, TCPExtendedStat, UDPMulti*, HInfo and MXInfo.
- Unknown csCodes are logged ("csCode N is not supported yet"), which shows what a real program
  asks for first.
