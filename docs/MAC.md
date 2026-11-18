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
make -f Makefile.mac                 # build/mac/Riptide.bin, a MacBinary extension file
make -f Makefile.mac CONF=my.conf    # with my.conf as its riptide.conf ('TEXT' 128)
make -f Makefile.mac AURORA68=1      # development on Aurora68: the log also goes to the Pi's log
```

Needs `gcc-m68k-linux-gnu` and `binutils-m68k-linux-gnu` (Debian, Ubuntu) and Python 3. Copy
`Riptide.bin` into the System Folder's Extensions folder (`hcopy -m` for an HFS image, or any
MacBinary tool), take MacTCP out of the Control Panels folder, and restart.

The configuration is riptide.conf, the same generic keywords as on the Amiga (AMIGA.md,
"Configuration file"), on the interface `en0`. Riptide reads the one RiptidePrefs saves (the
Riptide folder in the Preferences folder), else the Riptide file's `'TEXT'` 128 resource (make
`CONF=`), else `dhcp en0`. The card line is the Amiga's, with the Mac's driver:
`interface en0 device=.ENET unit=0` (without one the card is en0). A static setup:

```
ifconfig en0 192.168.1.20/24
route add default gw 192.168.1.1
nameserver 192.168.1.1
```

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

## Notes for the next stages

- Real programs (Internet Explorer, Fetch, NCSA Telnet) will show what is missing: TCPPassiveOpen
  with a remote filter, urgent data, TCPExtendedStat, UDPMulti*, HInfo and MXInfo.
- Unknown csCodes are logged ("csCode N is not supported yet"), which shows what a real program
  asks for first.
