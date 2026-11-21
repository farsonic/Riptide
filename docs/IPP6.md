# .IPP6: IPv6 for classic Mac programs, the MacTCP way

`.IPP6` is a driver that Riptide installs beside `.IPP` (docs/MAC.md). It has MacTCP's programming
model (PBControl calls with parameter blocks, synchronous or asynchronous with ioCompletion,
notification routines for TCP and UDP streams, the same csCodes, the same result codes) with
addresses that hold IPv6: a 20-byte `IP6Address` takes the place of MacTCP's 32-bit `ip_addr` and its
port. IPv4 hosts are IPv4-mapped addresses (`::ffff:a.b.c.d`, RFC 4291 2.5.5.2), so one stream type,
one call and one address type cover both families, as RFC 3493's sockets do. Names are resolved by the
driver itself (A and AAAA, ordered by RFC 6724), with no `'dnrp'` code resource and no DNR glue.

This document is the specification, written for anyone writing a Mac program. The C header is
`sdk/mac/IPP6.h` (standalone: no Riptide headers, MacTCP-style types); an example program is
`sdk/mac/examples/ipp6get.c`. The first program written for it is Aurora68 Play, the internet radio
of Aurora68 (a Raspberry Pi in a Macintosh SE), whose stream is read with TCP calls whose completion
routines fill an audio ring at interrupt time; the interface is made for exactly that style.

Contents: 1 Why; 2 Detection and falling back to MacTCP; 3 The model (calls, completion,
notification, interrupt rules); 4 Addresses; 5 Parameter blocks; 6 TCP calls; 7 UDP calls; 8 The
interface's own calls (information, addresses, names); 9 Notification routines; 10 Result codes;
11 Versions; 12 Porting a MacTCP program; 13 Examples; 14 What Riptide's version 1 does and does not do.

## 1 Why

MacTCP's interface knows only 32-bit IPv4 addresses (`ip_addr` in every call), so a MacTCP program
reaches an IPv6 host only through the stand-in IPv4 addresses Riptide makes up for it (docs/MAC.md,
"IPv6 for MacTCP programs", RFC 6535). That is the right answer for the programs of the 1990s, which
cannot change. A program written or changed today can do better: name the IPv6 address, see it in
its window and its log, choose between the families, and connect to a link-local address. Riptide's
own tools (ping6, traceroute6, nc) use a private, synchronous interface (`RTMAC_API`) that cannot be
called at interrupt time. `.IPP6` is the public one: documented, asynchronous, interrupt-safe, and
the same shape as MacTCP's, so that a MacTCP program is ported by changing the driver name and the
address fields.

## 2 Detection and falling back to MacTCP

Two ways, either is enough:

1. `Gestalt('ipp6', &version)` answers `noErr` and the interface version (1 for this document) when
   `.IPP6` is installed. A program that needs calls of a later version checks the number (section 11).
2. `OpenDriver("\p.IPP6", &refNum)` answers `noErr` and the driver reference number. Every call
   below goes to that reference number (`ioCRefNum`).

Ask Gestalt first: an OpenDriver of a name that is not in the unit table makes the Device Manager look
for a `'DRVR'` resource of that name in the open resource files, which is slower and answers an error
of its own (`dInstErr`, -26, or `resNotFound`). When `.IPP6` is not there (Apple's MacTCP, Open
Transport's MacTCP layer, an older Riptide), open `.IPP` and use MacTCP's calls with IPv4 addresses
and Apple's DNR glue for names, as before:

```c
short refnum;
long version;
int ipp6 = Gestalt('ipp6', &version) == noErr && version >= 1 &&
           OpenDriver("\p.IPP6", &refnum) == noErr;
if (!ipp6 && OpenDriver("\p.IPP", &refnum) != noErr)
    /* no TCP/IP on this Mac */;
```

Close: like `.IPP`, `.IPP6` stays open for every program; a Close is refused with `closErr` (-24) and
leaves the reference number working, so a program may close it on quitting or not.

Riptide installs `.IPP6` when the stack starts (the first Open of `.IPP`, which Riptide's INIT makes at
startup), in a free unit table entry from 48 up, as a second driver of the same stack: the two drivers
share the stack, its interfaces and its deferred task, not their streams (a `.IPP` stream pointer
given to `.IPP6` is `invalidStreamPtr`, and the other way round). If a driver named `.IPP6` is
already installed, Riptide leaves it.

## 3 The model

**Calls.** Every call is `PBControl` on the `.IPP6` reference number with a parameter block whose
header is the Device Manager's (`CntrlParam`: `ioCompletion` at 12, `ioResult` at 16, `ioCRefNum` at
24, `csCode` at 26). The fields after the header depend on the call (section 5). A synchronous call
(`PBControlSync`, `PBControl(pb, false)`) returns when the call is complete; an asynchronous one
(`PBControlAsync`, `PBControl(pb, true)`) returns at once with `ioResult` = 1 (inProgress) and the
call completes later. `.IPP6` is a concurrent driver, as MacTCP's `.IPP` is: calls complete in any
order (a TCPRcv may wait minutes while TCPSends complete), and every call has its own block, which
must stay where it is, unmoved and unchanged, until the call is complete. Immediate calls (the
`immed` bit of `ioTrap`) are refused with `controlErr`.

**Completion.** When a call is complete the driver sets `ioResult`, then calls `ioCompletion` if it
is not NULL, exactly as MacTCP does: with the parameter block in A0 and the result in D0 (the Device
Manager's convention) and also as the one argument on the stack, because MacTCP.h declares the
completion routines as C functions of the block (`void MyCompletion(TCP6iopb *pb)`). The stack
comes back as it was, whether the routine pops its argument or not. `ioResult` is always set before
the routine runs. A5 is not set for a completion routine: a program that uses its globals there sets
A5 itself (`SetA5`/`SetCurrentA5` with a value it saved, THINK C's and MPW's usual glue), as with
MacTCP and every Device Manager completion routine.

**Notification routines (ASRs).** A TCP or UDP stream may have a notification routine, given at
TCPCreate/UDPCreate. It is called for the events of section 9, as MacTCP calls it: a pascal procedure
with the arguments on the stack, and A5 = the A5 of the program that created the stream (taken when
the program made the create call), D0 = the event, D1 = the termination reason, A1 = the ICMP
report, A2 = the user data pointer. A notification routine is never called from inside another one,
nor from inside a completion routine.

**Interrupt time.** Completion and notification routines run in the stack's context: Riptide's
deferred task (Deferred Task Manager: interrupt level 0, but interrupt time, on whatever stack was
interrupted). There, as in any MacTCP completion routine:

- do not call the Memory Manager or anything that may move or purge memory, nor the Toolbox; keep
  the routine short; set A5 before using globals (completion routines);
- **any `.IPP6` call may be made asynchronously** (`PBControlAsync`, with its own block and, if
  wanted, a completion routine). It is queued and started in the same round of the stack, before
  the stack returns from the deferred task. Issuing the next TCPRcv (or TCPNoCopyRcv) from the
  completion routine of the last one is the intended way to read a stream continuously at interrupt
  time;
- **synchronous calls only of the immediate kind**: TCPStatus, TCPGlobalInfo, TCPRcvBfrReturn,
  UDPBfrReturn, UDPMaxMTUSize, ip6GetInfo, ip6GetAddrs, ip6AddrToStr, ip6StrToAddr. Made
  synchronously from a `.IPP6` (or `.IPP`) completion or notification routine they are answered on
  the spot (MacTCP answers them at once, and programs rely on it). Any other synchronous call there
  would wait for the stack's task, which is the caller: never do it;
- from other interrupt code (a VBL task, a Time Manager task, a Sound Manager callback, another
  driver's completion routine) make `.IPP6` calls asynchronously only. The Control entry only queues
  the block, with interrupts off for a few instructions, so it may be entered at any level;
- `ip6GetInfo`, `ip6AddrToStr` and `ip6StrToAddr` never touch the stack's state: made
  synchronously they are answered in the Control call itself, so they may be made synchronously
  anywhere, at any time (made asynchronously they are queued like the other calls).

A call answered on the spot (a synchronous call of the kinds above, or a call refused at once, such
as an immediate call) completes through the Device Manager's IODone, which gives a completion routine
the block in A0 and the result in D0 but not on the stack; every call that is queued, which includes
every asynchronous call, gets both.

**Order.** Calls on one stream are taken in the order they are made: sends go out in order,
receives are satisfied in order. A TCPSend completes when its data is in the stack's send buffer
(its WDS may be reused from then on, earlier than MacTCP allows); TCPClose completes at once, the FIN
following the queued sends.

**Programs that quit.** A program must release its streams (TCPRelease, UDPRelease) and withdraw
its lookups (ip6ResolveCancel) before it quits: a stream's buffers and routines lie in the program's
partition. Riptide checks every half second for streams and lookups of programs whose partition has
gone and releases them without completing their calls (logged), as it does for `.IPP`; a completion
that falls due before that check would still run in the freed memory.

## 4 Addresses

```c
typedef struct IP6Address {     /* 20 bytes */
    unsigned char  addr[16];    /*  0: the address, network byte order */
    unsigned short port;        /* 16: the port (big-endian, as the 68k's own order) */
    unsigned short scope;       /* 18: the interface (an index), for scoped addresses */
} IP6Address;
```

- `addr` is an IPv6 address in network byte order. An IPv4 address `a.b.c.d` is the IPv4-mapped
  address `::ffff:a.b.c.d`: ten zero bytes, two 0xFF bytes, then the four bytes of the IPv4 address
  (`addr[12..15]` is MacTCP's `ip_addr`, in the same byte order). `IP6_SET_V4(a, ip)` and
  `IP6_GET_V4(a)` in the header convert. All-zero (`::`) is the unspecified address: "any" or "the
  stack's choice" where a call allows it.
- `port` is a TCP or UDP port number, 0 meaning "any" or "the stack's choice" where allowed.
- `scope` is the interface index of a link-local address (`fe80::/10`, RFC 4007's zone), as
  ip6GetAddrs reports it (en0 is usually 2: 1 is the loopback interface). 0 means the default: the
  interface IPv4's default route uses. The stack ignores it for other addresses and returns 0 for
  them; a scope that names no interface is `ip6ScopeErr`.
- The structure holds only bytes and 16-bit words: it is laid out the same by every 68k compiler,
  with 2-byte or 4-byte alignment, and has no padding. It is 2-byte aligned within the parameter
  blocks.

The text form (ip6AddrToStr, ip6StrToAddr): RFC 5952 for IPv6 (`fd68::1`), dotted decimal for an
IPv4-mapped address (`192.168.68.1`), `fe80::1%en0` or `fe80::1%2` for a scoped one.

## 5 Parameter blocks

There are three: `TCP6iopb` (130 bytes) for the TCP calls, `UDP6iopb` (86 bytes) for UDP and
`IP6iopb` (80 bytes) for the interface's own calls. Each call uses its own fixed part of its block;
the driver reads and writes only the fields the call documents (a program may keep a block of the
right size and reuse it). All three start with the Device Manager's header:

| Offset | Size | Field | |
|---|---|---|---|
| 0 | 4 | qLink | the Device Manager's (the driver links its waiting calls through it) |
| 4 | 2 | qType | |
| 6 | 2 | ioTrap | |
| 8 | 4 | ioCmdAddr | |
| 12 | 4 | ioCompletion | in: the completion routine, or NULL |
| 16 | 2 | ioResult | out: 1 while the call is in progress, then the result |
| 18 | 4 | ioNamePtr | |
| 22 | 2 | ioVRefNum | |
| 24 | 2 | ioCRefNum | in: `.IPP6`'s reference number |
| 26 | 2 | csCode | in: the call |

In TCP6iopb and UDP6iopb, offset 28 holds the stream (`tcpStream`, `udpStream`: a long, out of the
create call, in for every other call) and the call's own fields start at 32. In IP6iopb they start at
28. Byte means an unsigned 8-bit field, word an unsigned 16-bit one, long a 32-bit one, Ptr a
32-bit pointer, IP6Address the 20 bytes of section 4. "in" fields are the caller's, "out" fields the
driver's; fields not listed for a call are not touched.

**Alignment.** The blocks are laid out as MacTCP.h's are: 68k alignment (2-byte for words, longs and
pointers), which MPW C, THINK C, Symantec C, CodeWarrior 68k (with `#pragma options align=mac68k`,
its 68k default) and GCC for the m68k use for Toolbox structures. The header sets that alignment with
the compilers' pragmas and checks the sizes at compile time. Programs that build their blocks by hand
(in assembly, or as byte arrays) use the offsets below, which are the definition.

The calls and their csCodes:

| csCode | Call | Block | Immediate | Section |
|---|---|---|---|---|
| 20 | UDPCreate | UDP6iopb | | 7 |
| 21 | UDPRead | UDP6iopb | | 7 |
| 22 | UDPBfrReturn | UDP6iopb | yes | 7 |
| 23 | UDPWrite | UDP6iopb | | 7 |
| 24 | UDPRelease | UDP6iopb | | 7 |
| 25 | UDPMaxMTUSize | UDP6iopb | yes | 7 |
| 30 | TCPCreate | TCP6iopb | | 6 |
| 31 | TCPPassiveOpen | TCP6iopb | | 6 |
| 32 | TCPActiveOpen | TCP6iopb | | 6 |
| 34 | TCPSend | TCP6iopb | | 6 |
| 35 | TCPNoCopyRcv | TCP6iopb | | 6 |
| 36 | TCPRcvBfrReturn | TCP6iopb | yes | 6 |
| 37 | TCPRcv | TCP6iopb | | 6 |
| 38 | TCPClose | TCP6iopb | | 6 |
| 39 | TCPAbort | TCP6iopb | | 6 |
| 40 | TCPStatus | TCP6iopb | yes | 6 |
| 42 | TCPRelease | TCP6iopb | | 6 |
| 43 | TCPGlobalInfo | TCP6iopb | yes | 6 |
| 70 | ip6GetInfo | IP6iopb | anywhere | 8 |
| 71 | ip6GetAddrs | IP6iopb | yes | 8 |
| 72 | ip6Resolve | IP6iopb | | 8 |
| 73 | ip6ResolveAddr | IP6iopb | | 8 |
| 74 | ip6ResolveCancel | IP6iopb | | 8 |
| 75 | ip6AddrToStr | IP6iopb | anywhere | 8 |
| 76 | ip6StrToAddr | IP6iopb | anywhere | 8 |

"Immediate": may be made synchronously from a completion or notification routine (section 3);
"anywhere": made synchronously, answered in the Control call, from any context. Every other csCode answers `controlErr`
(-17), and the driver's log names it once.

## 6 TCP calls (TCP6iopb)

The TCP calls are MacTCP's, with the same meanings, the same validity bits and the same results, so
the MacTCP Programmer's Guide describes them. Where a block holds no address it is MacTCP's block,
byte for byte. Below are the layouts and what `.IPP6` does differently from MacTCP or adds.

**TCPCreate (30)** creates a stream; its pointer comes back in `tcpStream`.

| Offset | Size | Field | |
|---|---|---|---|
| 28 | 4 | tcpStream | out |
| 32 | 4 | rcvBuff | in: the stream's receive buffer (Ptr), locked, until TCPRelease |
| 36 | 4 | rcvBuffLen | in: its size, at least 4096 |
| 40 | 4 | notifyProc | in: the notification routine, or NULL |
| 44 | 4 | userDataPtr | in: given to the notification routine |

Results: noErr, invalidBufPtr, invalidLength, streamAlreadyOpen (the buffer is another stream's),
insufficientResources (32 streams of TCP and UDP together).

**TCPPassiveOpen (31), TCPActiveOpen (32)** open a connection: passive waits for one from
`remoteAddr` (`::` and port 0: anyone), active makes one to `remoteAddr`.

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 1 | ulpTimeoutValue | in: seconds (validityFlags bit 0x80); 0 = 2 minutes |
| 33 | 1 | ulpTimeoutAction | in: 0 report, 1 abort (validityFlags bit 0x40); default abort |
| 34 | 1 | validityFlags | in |
| 35 | 1 | commandTimeoutValue | in: passive only, seconds, 0 = for ever |
| 36 | 20 | remoteAddr | in: active: the host and port (not `::`, not multicast); passive: the one to accept (`::`/0: any). out: the other end |
| 56 | 20 | localAddr | in: `::` and 0 for the stack's choice, or an address of this Mac and/or a port. out: this end |
| 76 | 1 | tosFlags | in: ignored in version 1 |
| 77 | 1 | precedence | in: ignored |
| 78 | 1 | dontFrag | in: ignored |
| 79 | 1 | timeToLive | in: ignored (the hop limit) |
| 80 | 1 | security | in: ignored |
| 81 | 1 | optionCnt | in: ignored (no IP options) |
| 82 | 40 | options | in: ignored |
| 122 | 4 | userDataPtr | the caller's |

The fields are MacTCP's, in MacTCP's order; each 6-byte host and port pair is a 20-byte IP6Address,
so every field after `remoteAddr` is 28 bytes further on than in MacTCP's TCPOpenPB.

An IPv4-mapped `remoteAddr` opens over IPv4, any other over IPv6. A passive open with a `::` local
address listens on both families; with an IPv4-mapped local address (`::ffff:0.0.0.0` for any) on
IPv4 only, with an IPv6 one on that address. An active open fails within the ULP timeout (openFailed;
ipDestDeadErr when there is no route), a passive one when the command timeout runs out
(commandTimeout). When the open completes, `remoteAddr` and `localAddr` hold the two ends (the local
address is the one the stack chose: of the remote's family).

Results: noErr, invalidStreamPtr, connectionExists, invalidLength (active: port 0), ip6BadAddrErr
(active to `::` or a multicast address; local and remote of different families), ip6ScopeErr,
duplicateSocket (the local port is in use), ip6NoSourceErr (no address of that family to send from),
ipDestDeadErr, openFailed, commandTimeout, insufficientResources, connectionTerminated (a TCPAbort
or TCPRelease during the open).

**TCPSend (34)**: MacTCP's TCPSendPB.

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 1 | ulpTimeoutValue | in (validityFlags 0x80) |
| 33 | 1 | ulpTimeoutAction | in (validityFlags 0x40) |
| 34 | 1 | validityFlags | in |
| 35 | 1 | pushFlag | in: nonzero: send now (Nagle off for the stream from then on) |
| 36 | 1 | urgentFlag | in: ignored (sent as ordinary data) |
| 37 | 1 | filler | |
| 38 | 4 | wdsPtr | in: the write data structure: {word length, Ptr data} ..., ending with length 0 |
| 42 | 4 | sendFree | not used |
| 46 | 2 | sendLength | not used |
| 48 | 4 | userDataPtr | the caller's |

The WDS total must be 1 to 65535 bytes. Results: noErr, invalidStreamPtr, connectionDoesntExist,
connectionClosing (after TCPClose), invalidWDS, invalidLength, connectionTerminated.

**TCPNoCopyRcv (35), TCPRcvBfrReturn (36), TCPRcv (37)**: MacTCP's TCPReceivePB.

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 1 | commandTimeoutValue | in: seconds, 0 = for ever |
| 33 | 1 | filler | |
| 34 | 1 | markFlag | out: 0 |
| 35 | 1 | urgentFlag | out: 0 |
| 36 | 4 | rcvBuff | TCPRcv in: where the data goes |
| 40 | 2 | rcvBuffLen | TCPRcv in: its size; out: the bytes received |
| 42 | 4 | rdsPtr | TCPNoCopyRcv in, TCPRcvBfrReturn in: the read data structure |
| 46 | 2 | rdsLength | TCPNoCopyRcv in: its entries (one is enough) |
| 48 | 2 | secondTimeStamp | |
| 50 | 4 | userDataPtr | the caller's |

TCPRcv copies into the caller's buffer and completes as soon as there is data (not when the buffer is
full). TCPNoCopyRcv lends the stream's own receive buffer: one RDS entry {length, pointer} and the
terminating zero; the program gives it back with TCPRcvBfrReturn (the same RDS) before the next
TCPNoCopyRcv completes. When the other end has closed and everything it sent has been received, a
receive completes with connectionClosing. Results: noErr, connectionClosing, commandTimeout,
invalidStreamPtr, connectionDoesntExist, invalidBufPtr, invalidLength, invalidRDS (a buffer given
back twice), connectionTerminated.

**TCPClose (38)**: MacTCP's TCPClosePB: 32 ulpTimeoutValue, 33 ulpTimeoutAction, 34 validityFlags,
35 filler, 36 userDataPtr. Sends the FIN after the queued sends and completes at once (MacTCP waits
for the FIN's acknowledgement); receives go on until the other end closes. Results: noErr,
connectionClosing (closed already), connectionDoesntExist, invalidStreamPtr.

**TCPAbort (39)**: 32 userDataPtr. Resets the connection; waiting calls complete with
connectionTerminated, the notification routine hears TCPTerminate with TCPULPAbort. The stream
stays and may be opened again. Results: noErr, connectionDoesntExist, invalidStreamPtr.

**TCPStatus (40)**: MacTCP's TCPStatusPB with the two addresses.

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 1 | ulpTimeoutValue | out |
| 33 | 1 | ulpTimeoutAction | out |
| 34 | 4 | unused | |
| 38 | 20 | remoteAddr | out |
| 58 | 20 | localAddr | out |
| 78 | 1 | tosFlags | out: 0 |
| 79 | 1 | precedence | out: 0 |
| 80 | 1 | connectionState | out: 0 closed, 2 listen, 6 SYN sent, 8 established, 10 FIN wait 1, 12 FIN wait 2, 14 close wait, 16 closing (MacTCP's numbers) |
| 81 | 1 | filler | |
| 82 | 2 | sendWindow | out |
| 84 | 2 | rcvWindow | out |
| 86 | 2 | amtUnackedData | out: 0 |
| 88 | 2 | amtUnreadData | out: bytes waiting to be received |
| 90 | 4 | securityLevelPtr | out: NULL |
| 94 | 4 | sendUnacked | out: 0 |
| 98 | 4 | sendNext | out: 0 |
| 102 | 4 | congestionWindow | out: 0 |
| 106 | 4 | rcvNext | out: 0 |
| 110 | 4 | srtt | out: ms |
| 114 | 4 | lastRTT | out: ms |
| 118 | 4 | sendMaxSegSize | out |
| 122 | 4 | connStatPtr | out: a TCPConnectionStats (zero) |
| 126 | 4 | userDataPtr | the caller's |

Every field after `remoteAddr` is 28 bytes further on than in MacTCP's TCPStatusPB. Results: noErr,
connectionDoesntExist (no connection: the other fields are still filled), invalidStreamPtr.

**TCPRelease (42)**: the stream goes; a connection is aborted first. `rcvBuff` (32) and `rcvBuffLen`
(36) come back with the buffer given at TCPCreate, now the program's again. Results: noErr,
invalidStreamPtr.

**TCPGlobalInfo (43)**: MacTCP's TCPGlobalInfoPB: 32 tcpParamPtr (out: MacTCP's TCPParam: the RTO
algorithm's name, min and max RTO, the MSS, the number of streams, the largest window), 36
tcpStatsPtr (out), 40 tcpCDBTable (out: the table of `.IPP6`'s TCP streams, NULL entries free), 44
userDataPtr, 48 maxTCPConnections (out, word). The tables are the driver's, read only.

## 7 UDP calls (UDP6iopb)

**UDPCreate (20)**: MacTCP's UDPCreatePB, unchanged.

| Offset | Size | Field | |
|---|---|---|---|
| 28 | 4 | udpStream | out |
| 32 | 4 | rcvBuff | in: the stream's receive buffer, at least 2048 bytes |
| 36 | 4 | rcvBuffLen | in |
| 40 | 4 | notifyProc | in: the notification routine, or NULL |
| 44 | 2 | localPort | in: the port, 0 for the stack's choice; out: the port |
| 46 | 4 | userDataPtr | in: given to the notification routine |
| 50 | 2 | endingPort | not used |

The stream receives datagrams to its port over both IPv4 and IPv6 (one dual-stack socket) and sends
over either. Results: noErr, invalidBufPtr, invalidLength, duplicateSocket, insufficientResources.

**UDPRead (21), UDPBfrReturn (22)**

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 2 | timeOut | UDPRead in: seconds, 0 = for ever |
| 34 | 20 | remoteAddr | UDPRead out: the sender |
| 54 | 4 | rcvBuff | UDPRead out: the datagram, in the stream's buffer; UDPBfrReturn in: the same |
| 58 | 2 | rcvBuffLen | UDPRead out: its length |
| 60 | 2 | secondTimeStamp | |
| 62 | 4 | userDataPtr | the caller's |
| 66 | 20 | destAddr | UDPRead out: the address it was sent to (this Mac's address of that family) and the stream's port |

A datagram is lent in the stream's buffer, as MacTCP lends it, until UDPBfrReturn. Every field after
`remoteAddr` is 14 bytes further on than in MacTCP's UDPReceivePB. Results: noErr, commandTimeout,
invalidStreamPtr, invalidBufPtr, connectionTerminated (UDPRelease while waiting).

**UDPWrite (23)**

| Offset | Size | Field | |
|---|---|---|---|
| 32 | 2 | reserved | |
| 34 | 20 | remoteAddr | in: the destination (IPv4-mapped or IPv6, not `::`) |
| 54 | 4 | wdsPtr | in: the datagram's pieces (a WDS) |
| 58 | 1 | checkSum | in: ignored (the checksum is always computed; IPv6 requires it) |
| 59 | 1 | filler | |
| 60 | 2 | sendLength | not used |
| 62 | 4 | userDataPtr | the caller's |
| 66 | 2 | localPort | not used |

The datagram is sent before the call completes. At most 1600 bytes (the link's MTU less the headers
is what goes without fragments: UDPMaxMTUSize). Results: noErr, invalidStreamPtr, invalidWDS,
invalidLength, ip6BadAddrErr, ip6ScopeErr, ipDestDeadErr (no route, or nothing answered the
neighbour or ARP query), ip6NoSourceErr.

**UDPRelease (24)**: `rcvBuff` (32) and `rcvBuffLen` (36) come back. Results: noErr, invalidStreamPtr.

**UDPMaxMTUSize (25)**: 32 mtuSize (word, out), 34 remoteAddr (IP6Address, in), 54 userDataPtr. The
largest datagram that goes to `remoteAddr` without fragments: the link MTU less 28 bytes (IPv4 and
UDP headers) for an IPv4-mapped address, less 48 (IPv6 and UDP) for an IPv6 one, the smaller of
the two for `::`. No stream is needed.

## 8 The interface's own calls (IP6iopb)

**ip6GetInfo (70)**: what the interface is. Answered in the Control call when made synchronously.

| Offset | Size | Field | |
|---|---|---|---|
| 28 | 4 | version | out: the interface version (1); the same as Gestalt `'ipp6'` |
| 32 | 4 | features | out: bits, below |
| 36 | 2 | maxTCPStreams | out: streams (TCP and UDP share them: 32) |
| 38 | 2 | maxUDPStreams | out: the same pool |
| 40 | 2 | maxLookups | out: ip6Resolve and ip6ResolveAddr calls at once (8) |
| 42 | 2 | order | out: ip6Resolve's default order (kIP6OrderRFC6724, kIP6OrderIPv6First, kIP6OrderIPv4First) |
| 44 | 4 | implementation | out: a C string, the driver's (read only): "Riptide .IPP6 1" and the build |
| 48 | 4 | userDataPtr | the caller's |

`features`: kIP6HasIPv4 (0x01) an interface has an IPv4 address; kIP6HasIPv6 (0x02) an interface
has an IPv6 address beyond link-local (global or unique local); kIP6HasLinkLocal (0x04) a link-local
IPv6 address; kIP6HasIPv6Route (0x08) IPv6 can reach global addresses (a default route and a
source); kIP6HasNameServer (0x10) a name server is configured. They describe the moment of the call:
DHCP and router advertisements change them.

**ip6GetAddrs (71)**: the interfaces' addresses, the IPv6 equivalent of MacTCP's ipctlGetAddr.

| Offset | Size | Field | |
|---|---|---|---|
| 28 | 4 | buffer | in: an array of IP6AddrInfo |
| 32 | 4 | bufferLen | in: its size in bytes |
| 36 | 2 | count | out: entries written |
| 38 | 2 | total | out: entries the stack has (more than count when the buffer was too small) |
| 40 | 4 | reserved | in: 0 |
| 44 | 4 | userDataPtr | the caller's |

```c
typedef struct IP6AddrInfo {    /* 32 bytes */
    IP6Address     addr;        /*  0: port 0; scope: the interface, for a link-local address */
    unsigned char  prefixLen;   /* 20: the on-link prefix (IPv4: the netmask's bits) */
    unsigned char  flags;       /* 21: kIP6Addr... */
    unsigned short ifIndex;     /* 22: the interface's index (the scope of its link-local addresses) */
    char           ifName[8];   /* 24: its name, a C string: "en0" */
} IP6AddrInfo;
```

Flags: kIP6AddrIPv4 (0x01) an IPv4 address (IPv4-mapped); kIP6AddrLinkLocal (0x02); kIP6AddrTemporary
(0x04, RFC 8981); kIP6AddrDeprecated (0x08); kIP6AddrTentative (0x10: duplicate address detection
still running, not usable yet); kIP6AddrPrimary (0x20: on the interface IPv4's default route uses,
whose IPv4 address ipctlGetAddr reports); kIP6AddrAutoconf (0x40, SLAAC). The IPv4 addresses come
first on each interface, then its IPv6 ones, interface by interface; the loopback interface is
left out. Results: noErr,
invalidBufPtr (NULL buffer with a nonzero length).

**ip6Resolve (72)**: a name to its addresses, IPv6 and IPv4, into the caller's array.

| Offset | Size | Field | |
|---|---|---|---|
| 28 | 4 | hostName | in: the name, a C string: a host name, or an address literal |
| 32 | 2 | hostNameLen | not used |
| 34 | 2 | flags | in: order and families, below |
| 36 | 20 | addr | not used |
| 56 | 4 | results | in: an array of IP6Address (the caller's) |
| 60 | 2 | maxResults | in: its entries (1 or more) |
| 62 | 2 | count | out: the addresses written |
| 64 | 2 | port | in: written into every result's port, so the results can be opened directly |
| 66 | 2 | timeout | in: seconds for the whole lookup, 0 = 10 |
| 68 | 4 | canonName | in: a buffer for the canonical name, or NULL |
| 72 | 2 | canonNameLen | in: its size |
| 74 | 2 | reserved | in: 0 |
| 76 | 4 | userDataPtr | the caller's |

How: an IPv6 literal (`fd68::1`, `fe80::1%en0`) or a dotted IPv4 address is answered at once
(noErr, one result, completed through ioCompletion like any call). Otherwise the hosts table
(riptide.conf `host` lines, IPv4 and IPv6) and the stack's cache are asked first, then the name
servers (IPv4 or IPv6 ones: riptide.conf, DHCP, DHCPv6, router advertisements) with an AAAA and an A
query at once, each sent again to the next server after 2.5 seconds; a name without a dot is also
tried with the search domain when the name servers do not know it; a name ending in a dot is taken as
it is. The call completes when both answers are in (or a query has been sent four times without
one), or when its timeout runs out, with what has come by then: an answer of one family is not held
up by the other's for longer than the timeout.

`flags`: the order, in the low two bits: kIP6OrderDefault (0) the stack's setting (riptide.conf
`ipp6 order rfc6724|ipv6|ipv4`, RFC 6724 by default), kIP6OrderRFC6724 (1) RFC 6724's destination
address selection (section 6 of it: rules 1, 2, 5, 6, 8, 9, 10, with the stack's addresses and
routes: a global IPv6 address that this Mac can reach from a global address of its own comes before
IPv4, but RFC 6724's policy table puts a unique local IPv6 address, fd00::/8, after IPv4, and an
IPv6 address this Mac has no source for after everything), kIP6OrderIPv6First (2) every IPv6 address before every IPv4 one, kIP6OrderIPv4First (3); the
families: kIP6OnlyIPv6 (0x04) no A query, kIP6OnlyIPv4 (0x08) no AAAA query; kIP6NoSearch (0x10) the
name as it is, without the search domain. A program connects to the results in order, going on to the
next when one fails (section 13): that is the fallback from IPv6 to IPv4.

Results: noErr (count 1 or more; more than maxResults addresses are cut to maxResults), authNameErr
(no such name), noAnsErr (the name has no address of the families asked for, or no name server
answered in time), noNameServer (none configured), nameSyntaxErr (an empty name, a label longer than
63, a bad literal), invalidBufPtr (no results array or no name), invalidLength (maxResults 0),
outOfMemory (more than maxLookups lookups at once), ip6CancelledErr (withdrawn by ip6ResolveCancel).

**ip6ResolveAddr (73)**: an address to its name (PTR in ip6.arpa, or in-addr.arpa for an IPv4-mapped
address). In: `addr` (36, its port and scope ignored), `hostName` (28: the buffer the name goes into),
`hostNameLen` (32: its size), `timeout` (66). Out: the name, a C string. Results: noErr,
authNameErr (no name), noAnsErr, noNameServer, ip6TruncatedErr (the buffer is too small: the start
of the name is in it), invalidBufPtr, outOfMemory, ip6CancelledErr.

**ip6ResolveCancel (74)**: 28 target (Ptr: the IP6iopb of an ip6Resolve or ip6ResolveAddr in
progress), 32 userDataPtr. The lookup completes with ip6CancelledErr (its completion routine runs)
before this call completes with noErr; ip6NotPendingErr when the target is not a lookup in progress
(it may have completed meanwhile). A program quitting cancels its lookups this way.

**ip6AddrToStr (75)**: in: `addr` (36), `hostName` (28: the buffer), `hostNameLen` (32). Out: the
text (section 4), a C string; at most 46 bytes with the NUL (`IP6_ADDRSTRLEN`), more with a scope.
Results: noErr, ip6TruncatedErr, invalidBufPtr. Answered in the Control call when made synchronously.

**ip6StrToAddr (76)**: in: `hostName` (28: the text: an IPv6 literal, with `%interface` or `%index`
for a scope, or a dotted IPv4 address; surrounding brackets `[...]` are allowed), `port` (64). Out:
`addr` (36), with `port` in its port field. Results: noErr, nameSyntaxErr, ip6ScopeErr,
invalidBufPtr. Answered in the Control call when made synchronously. No name lookups: that is
ip6Resolve.

## 9 Notification routines

MacTCP's, with `.IPP6`'s stream pointers:

```c
pascal void MyTCPNotify(unsigned long tcpStream, unsigned short eventCode, Ptr userDataPtr,
                        unsigned short terminReason, ICMP6Report *icmpMsg);
pascal void MyUDPNotify(unsigned long udpStream, unsigned short eventCode, Ptr userDataPtr,
                        ICMP6Report *icmpMsg);
```

TCP events: TCPClosing (1) the other end has closed and everything it sent has been delivered (or
is waiting in the stream for a receive); TCPULPTimeout (2) a stream whose ULP action is "report" has
had sends waiting without progress for the ULP timeout; TCPTerminate (3) the connection is over,
exactly once per connection, with `terminReason`: TCPRemoteAbort (2) the other end reset it,
TCPNetworkFailure (3), TCPULPTimeoutTerminate (5), TCPULPAbort (6) the program's TCPAbort or
TCPRelease, TCPULPClose (7) both ends closed; TCPDataArrival (4) data has arrived and no receive is
waiting for it (once, until a receive completes); TCPUrgent (5) and TCPICMPReceived (6) are never sent
by version 1. UDP: UDPDataArrival (1) a datagram is waiting and no UDPRead is; UDPICMPReceived (2) is
never sent by version 1 (so `icmpMsg` is always NULL; ICMP6Report is defined in the header for a
later version).

The routine runs at interrupt time with A5 set (section 3). It may make asynchronous calls and the
immediate synchronous ones. After TCPTerminate the stream is as TCPCreate left it and may be opened
again.

GCC has no `pascal` keyword: a GCC program gives a small assembly routine that takes the arguments
off the stack (or reads D0, D1, A2), as Riptide's tests/mac/ipp6_test.c does.

## 10 Result codes

MacTCP's codes keep their meanings:

| Code | Name | Meaning here |
|---|---|---|
| 0 | noErr | |
| 1 | inProgress | in ioResult while an asynchronous call is in progress |
| -17 | controlErr | an unknown csCode, or an immediate (ioTrap) call |
| -24 | closErr | Close: `.IPP6` stays open |
| -23005 | connectionClosing | the other end has closed (receives), or TCPClose was made already |
| -23006 | invalidLength | a buffer or WDS too short or too long, port 0, maxResults 0 |
| -23007 | connectionExists | an open on a stream with a connection or an open waiting |
| -23008 | connectionDoesntExist | no connection on the stream |
| -23009 | insufficientResources | no stream left, or the stack could not make a socket |
| -23010 | invalidStreamPtr | not a stream of `.IPP6` (of that kind) |
| -23011 | streamAlreadyOpen | the receive buffer is another stream's |
| -23012 | connectionTerminated | the connection ended (abort, release) while the call waited |
| -23013 | invalidBufPtr | a NULL buffer or pointer that must be given |
| -23014 | invalidRDS, invalidWDS | a bad RDS or WDS, or a buffer given back twice |
| -23015 | openFailed | the connection could not be made (refused, or the ULP timeout) |
| -23016 | commandTimeout | the command timeout ran out |
| -23017 | duplicateSocket | the local port is in use |
| -23033 | ipDestDeadErr | no route to the host, or it does not answer at the link (ARP, neighbour discovery) |
| -23041 | nameSyntaxErr | not a name or address literal |
| -23044 | noNameServer | no name server is configured |
| -23045 | authNameErr | the name does not exist (ip6Resolve), or the address has no name (ip6ResolveAddr) |
| -23046 | noAnsErr | no address of the families asked for, or no name server answered |
| -23048 | outOfMemory | too many lookups at once |

New codes, for what MacTCP has no word for, from -23060 down (MacTCP's own end at -23048):

| Code | Name | Meaning |
|---|---|---|
| -23060 | ip6BadAddrErr | an address the call cannot use: `::` or multicast where a host is needed, local and remote of different families |
| -23061 | ip6ScopeErr | a scope (or `%interface`) that names no interface |
| -23062 | ip6TruncatedErr | the text or name did not fit the buffer (what fitted is there, NUL-terminated) |
| -23063 | ip6CancelledErr | a lookup withdrawn by ip6ResolveCancel |
| -23064 | ip6NotPendingErr | ip6ResolveCancel: the target is not a lookup in progress |
| -23065 | ip6NoSourceErr | this Mac has no address of the destination's family that can reach it (an IPv6 destination with only a link-local address, or IPv4 turned off) |

Codes -23066 to -23079 are kept for later versions of `.IPP6`.

## 11 Versions

The interface version is the number Gestalt `'ipp6'` and ip6GetInfo answer. This document is
version 1. The rules for later versions:

- a block's layout never changes: no field moves or changes its size or meaning, and fields a version
  does not use stay as they are documented (reserved fields: 0 in, not touched);
- a new feature is a new csCode (from 77 up for the interface's own calls, at MacTCP's numbers for a
  MacTCP call not yet provided, such as UDPMultiCreate or TCPExtendedStat), or a flag bit that was
  reserved and had to be 0, and raises the version by one;
- a program that uses a call or flag of version N checks `version >= N`; a driver of an older version
  answers an unknown csCode with controlErr, so a program may also just try;
- a csCode is never given a second meaning, and a result code never changes its meaning.

## 12 Porting a MacTCP program

1. Look for `.IPP6` (section 2) and keep `.IPP` as the fallback.
2. Names: replace OpenResolver/StrToAddr/AddrToName/CloseResolver with ip6Resolve and ip6ResolveAddr
   (asynchronous, completion through ioCompletion; withdraw them with ip6ResolveCancel). No DNR file
   or glue is needed.
3. Addresses: every `ip_addr` and port pair becomes an `IP6Address`. In TCPPassiveOpen/TCPActiveOpen
   and TCPStatus the fields after the remote address move 28 bytes; in UDPRead, UDPWrite and
   UDPMaxMTUSize the fields after the remote address move 14 bytes. TCPCreate, TCPSend, TCPRcv,
   TCPNoCopyRcv, TCPRcvBfrReturn, TCPClose, TCPAbort, TCPRelease, TCPGlobalInfo, UDPCreate and
   UDPRelease are unchanged, byte for byte. An IPv4 address goes in as `::ffff:a.b.c.d`
   (`IP6_SET_V4`).
4. Connect to the addresses ip6Resolve gave in order; on openFailed, ipDestDeadErr, ip6NoSourceErr
   or ip6BadAddrErr go on to the next (the stream may be opened again after a failed open).
5. Show addresses with ip6AddrToStr; accept typed ones with ip6StrToAddr (an IPv6 literal in a URL
   is `[fd68::1]`).
6. ipctlGetAddr becomes ip6GetAddrs. Completion routines, notification routines, the receive
   buffer, WDS and RDS handling, TCPRcvBfrReturn and UDPBfrReturn stay as they are.

Aurora68 Play (mac/piplay/radio.c in the Aurora68 repository) is the case in point: it opens `.IPP`,
looks the station up through the DNR glue, then TCPCreate, TCPActiveOpen (remoteHost at 36,
remotePort at 40), TCPSend, and TCPRcv calls chained from their own completion routines into the audio
ring, TCPAbort and TCPRelease. With `.IPP6` it opens `.IPP6`, looks the station up with ip6Resolve
(asynchronously, results with the port already in them), puts each result into `remoteAddr` at 36 in
turn until a TCPActiveOpen succeeds, and keeps everything else, the interrupt-time receive chain
included; on a Mac without `.IPP6` it goes on using `.IPP` as now.

## 13 Examples

Detection, a name to addresses, then the first address that connects, with synchronous calls for
brevity (`sdk/mac/examples/ipp6get.c` is the whole program, with the MacTCP fallback):

```c
#include "IPP6.h"

static short ipp6;                                  /* .IPP6's reference number */

static OSErr call(void *pb, short cs)
{
    ((IP6iopb *)pb)->ioCRefNum = ipp6;
    ((IP6iopb *)pb)->csCode = cs;
    return PBControlSync((ParmBlkPtr)pb);
}

IP6Address addrs[8];
IP6iopb r;
TCP6iopb t;
unsigned long stream;
short i, err;

memset(&r, 0, sizeof r);
r.csParam.resolve.hostName = "peer.testbed";
r.csParam.resolve.results = addrs;
r.csParam.resolve.maxResults = 8;
r.csParam.resolve.port = 80;
r.csParam.resolve.flags = kIP6OrderIPv6First;      /* AAAA and A: IPv6 first, then IPv4 */
err = call(&r, ip6Resolve);

memset(&t, 0, sizeof t);
t.csParam.create.rcvBuff = NewPtr(16384);
t.csParam.create.rcvBuffLen = 16384;
err = call(&t, TCPCreate);
stream = t.tcpStream;
for (i = 0; i < r.csParam.resolve.count; i++) {
    memset(&t.csParam, 0, sizeof t.csParam);
    t.tcpStream = stream;
    t.csParam.open.ulpTimeoutValue = 20;            /* seconds per address ... */
    t.csParam.open.validityFlags = timeoutValue;    /* ... valid */
    t.csParam.open.remoteAddr = addrs[i];
    if (call(&t, TCPActiveOpen) == noErr)
        break;                                      /* connected; else the next address (IPv4) */
}
```

An asynchronous receive chain at interrupt time, as an audio player reads a stream:

```c
static TCP6iopb rcv;                                /* one block, reused by the chain */
static char chunk[8192];

static void rcv_done(TCP6iopb *pb)                  /* interrupt time: A0 = pb as well */
{
    if (pb->ioResult == noErr) {
        ring_put(chunk, pb->csParam.receive.rcvBuffLen);   /* no Memory Manager here */
        pb->csParam.receive.rcvBuff = chunk;
        pb->csParam.receive.rcvBuffLen = sizeof chunk;
        PBControlAsync((ParmBlkPtr)pb);             /* the next receive, from the completion */
    } else
        chain_ended = pb->ioResult;                 /* connectionClosing at the end of the stream */
}

memset(&rcv, 0, sizeof rcv);
rcv.ioCompletion = (TCP6IOCompletionUPP)rcv_done;
rcv.ioCRefNum = ipp6;
rcv.csCode = TCPRcv;
rcv.tcpStream = stream;
rcv.csParam.receive.commandTimeoutValue = 0;        /* wait as long as it takes */
rcv.csParam.receive.rcvBuff = chunk;
rcv.csParam.receive.rcvBuffLen = sizeof chunk;
PBControlAsync((ParmBlkPtr)&rcv);
```

(A THINK C or MPW completion routine that touches globals sets A5 first; the block's address is
also in A0.)

Name lookup asynchronously, the result in the completion routine:

```c
static IP6iopb look;
static IP6Address found[4];
static volatile short look_done;

static void look_completion(IP6iopb *pb) { look_done = 1; }

memset(&look, 0, sizeof look);
look.ioCompletion = (IP6IOCompletionUPP)look_completion;
look.ioCRefNum = ipp6;
look.csCode = ip6Resolve;
look.csParam.resolve.hostName = "v6only.testbed";
look.csParam.resolve.results = found;
look.csParam.resolve.maxResults = 4;
look.csParam.resolve.port = 80;
PBControlAsync((ParmBlkPtr)&look);
while (!look_done)
    WaitNextEvent(everyEvent, &ev, 1, NULL);        /* the program goes on meanwhile */
```

## 14 What Riptide's version 1 does and does not do

Riptide (src/mac/ipp6.c) implements every call above. TCP and UDP streams are the core's dual-stack
sockets (`AF_INET6` with IPV6_V6ONLY off: an IPv4-mapped address goes over IPv4), the lookups a small
asynchronous DNS client in the driver (the core's own resolver waits in select, which the deferred
task must never do). Not in version 1: urgent data (sent as ordinary data, never reported), IP
options, type of service, traffic class and hop limit (the fields are ignored), ICMP reports to the
notification routines, UDPStatus, UDPMultiCreate/Send/Read, TCPExtendedStat, multicast, an ICMPv6
echo call. They are left for later versions (section 11).

Riptide's log (PiLog's Mac tab on Aurora68, built with AURORA68=1) names the first call of each kind
(`.IPP6: first TCPActiveOpen (32)`), every call that ends in an error, once per call and result, and
every csCode that is not supported, once.

Tested on Aurora68's testbed (System 7.1 under QEMU with `tools/pinet.py` as the peer: 95 checks of
tests/mac/ipp6_test.c, and the example program, which also fell back to `.IPP` correctly on Apple's
MacTCP; docs/MAC.md, "The .IPP6 test"); not yet on real
hardware, nor with a program compiled by MPW C, THINK C or CodeWarrior (the header's layout is
checked by tests/t_ipp6_layout.c on the host and for the m68k).
