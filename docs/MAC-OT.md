# Open Transport and Riptide on System 7.1 (68k): a design

Written 19 November 2026, when Internet Explorer 4.01 for 68k Macs failed on Riptide with "A
read/write error has occurred (-28)" and the question was whether Riptide needs Open Transport.
What follows is the evidence, how Open Transport programs bind to it, the three ways Riptide could
serve them, and a recommendation. It uses only Apple's and Microsoft's published documentation and
Riptide's own code; none of Apple's Open Transport software or headers are copied or needed for
anything here.

## Internet Explorer 4.01 does not need Open Transport

**IE 4.01 for 68k runs on MacTCP.** Microsoft's own requirements for Internet Explorer 4 for the
Macintosh (Knowledge Base article 181767, "How to Configure a Macintosh Computer for Internet
Access") list "a Macintosh with 68030 or later processor" and "MacTCP or Open Transport 1.1.1 or
later". Archive listings of the 4.01 68k release give the same: 68030 or later, System 7.1 or later,
Open Transport 1.1.1 or MacTCP 2.0.6, 8 MB of memory. 4.01 was the last 68k version; 4.5 is
PowerPC only.

**The -28 was Riptide's resolver file, not missing Open Transport.** -28 is notOpenErr. Apple's DNR
glue (dnr.c, compiled into every MacTCP program) returns it from StrToAddr when OpenResolver could
not load a `'dnrp'` resource, before it looks at the address at all, which is why even
`http://192.168.0.142` failed and why Riptide's log showed nothing. The glue looks for the file that
holds `'dnrp'` like this (Apple's published dnr.c, 1988 with the 1991 changes):

1. the Control Panels folder, type `'cdev'` creator `'ztcp'`;
2. the System Folder, `'cdev'` `'mtcp'`;
3. the Control Panels folder, `'cdev'` `'mtcp'`.

The oldest glue (1988) only scans the System Folder for `'cdev'` `'mtcp'` and takes the first. The
Riptide DNR file was `'cdev'` `'ztcp'` in the System Folder, which no version of the glue looks for.
Since 19 November 2026 it is `'cdev'` `'mtcp'` (docs/MAC.md, "TCP, UDP and the resolver"), and the
driver changes an older Riptide DNR's creator at startup. The same day's other MacTCP fixes
(TCPStatus's layout, TCPNoCopyRcv with a one-entry RDS, A5 in notification routines, stream reuse,
TCPTerminate after a graceful close, Gestalt `'mtcp'`) are in docs/MAC.md too.

So for IE 4.01 the MacTCP path is the answer, and on the user's Mac (Raspberry Pi 3B, System 7.1,
19 November 2026) it is confirmed: with the resolver found, Internet Explorer 4.01 loaded
google.com and frogfind.com through Riptide. Open Transport support is a separate question: which
programs need it, and what it would take.

## Which 68k programs need Open Transport

Few, on System 7.1. Open Transport reached 68k Macs (68030 and later) only in 1996, with version
1.1; the 68k Internet programs of 1994 to 1998 were written for MacTCP, because most 68k Macs never
had Open Transport, and those that also use Open Transport (Internet Explorer 4.01 among them) test
Gestalt `'otan'` and fall back to MacTCP. Programs that require Open Transport are mostly later
PowerPC-only ones (Internet Explorer 4.5 is one). That bounds what Open Transport support is worth
on a 68k Mac with System 7.1.

## How a 68k program binds to Open Transport

From *Inside Macintosh: Networking With Open Transport* (1996) and Apple's Technical Q&As NW18,
NW19, NW34, NW41 and TN1173:

- **Linking.** A 68k application links Apple's static libraries `OpenTransport.o` and
  `OpenTransportApp.o` (`OpenTransportExtn.o` for code that is not an application), and
  `OpenTptInet.o` for TCP/IP (`OpenTptATalk.o` for AppleTalk). These are not the implementation:
  "OT uses ASLM as its underlying shared library system, with the ASLM bits wrapped inside
  standard .o (for 68K) and XCOFF/PEF (for PPC) files" (NW34). On a 68k Mac the functions live in
  ASLM (Apple Shared Library Manager) libraries that Open Transport's installer puts in the
  Extensions folder; the static glue finds them by ASLM class and function-set IDs and calls
  through ASLM's dispatch. CFM-68K programs bind to CFM shared libraries instead (Apple's "Open Tpt
  CFM68K Dev. Note"); Open Transport modules could not be built with CFM-68K (NW19).
- **Presence.** Gestalt `'otan'` (gestaltOpenTpt): bit 0 present, bit 1 loaded, bits 2/3
  AppleTalk present/loaded, bits 4/5 TCP present/loaded (NW41). `'otvr'` gives the version.
  Programs that can use either stack test `'otan'` first and fall back to MacTCP when it is absent.
- **Starting.** `InitOpenTransport()` (or `InitOpenTransportUtilities` for the memory and timer
  calls alone), `CloseOpenTransport()` at the end; for pre-Carbon clients OT keeps the client's
  assets through a global in the A5 world (TN1173).
- **Endpoints.** `OTOpenEndpoint(OTCreateConfiguration("tcp"), 0, &info, &err)`, or the
  asynchronous `OTAsyncOpenEndpoint` with a notifier; `OTBind`, `OTConnect`, `OTSnd`, `OTRcv`,
  `OTSndOrderlyDisconnect`, `OTRcvOrderlyDisconnect`, `OTSndDisconnect`, `OTUnbind`,
  `OTCloseProvider`; `OTLook` for the pending event; `OTGetEndpointState` for the XTI state.
- **Modes.** Each provider is synchronous or asynchronous (`OTSetSynchronous`,
  `OTSetAsynchronous`) and blocking or non-blocking (`OTSetBlocking`, `OTSetNonBlocking`). An
  asynchronous provider reports through its notifier: `T_OPENCOMPLETE`, `T_BINDCOMPLETE`,
  `T_CONNECT`, `T_DATA`, `T_GODATA`, `T_ORDREL`, `T_DISCONNECT`, `T_DISCONNECTCOMPLETE`... A
  synchronous blocking call waits, calling the program's yield (`OTUseSyncIdleEvents`, `kOTSyncIdleEvent`).
- **XTI/TPI semantics.** The endpoint states (`T_UNBND`, `T_IDLE`, `T_OUTCON`, `T_INCON`,
  `T_DATAXFER`, `T_OUTREL`, `T_INREL`), the errors (`kOTLookErr` when an event is pending,
  `kOTFlowErr` when the send would block, `kOTNoDataErr`, `kOTOutStateErr`), `T_MORE` on partial
  records, the `TCall`, `TBind`, `TUnitData` and `TDiscon` structures with `TNetbuf` buffers, and
  `InetAddress` (`kOTInetAddressType`, port, host).
- **Notifiers.** Called at deferred task time (or from the call itself), never re-entered for one
  provider; a notifier may call OT asynchronously but must not block; `OTEnterNotifier` /
  `OTLeaveNotifier`, `OTScheduleDeferredTask`, `OTAtomicSetBit` and the like for synchronisation.
- **Memory.** `OTAllocMem` / `OTFreeMem` work at interrupt and deferred-task time (OT's own pool,
  grown at system task time); a notifier must not call the Memory Manager.
- **Names.** `OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err)` (or the asynchronous
  form) gives an `InetSvcRef`; `OTInetStringToAddress`, `OTInetAddressToName`,
  `OTInetMailExchange`, `OTInetSysInfo`; `OTInetHostToString` and `OTInetStringToHost` need no
  provider.

## Three ways Riptide could serve Open Transport programs

### (a) Riptide's own Open Transport libraries, no Apple Open Transport installed

Riptide implements the client API on its core: endpoints are core sockets, as MacTCP streams are
in src/mac/mactcp.c, run from the stack's deferred task; InetServices is the driver's lookups
(RTMAC_DNS: the core resolver's asynchronous ones since 23 November 2026). Gestalt `'otan'` answers "present, loaded, TCP present and loaded" (no AppleTalk).

- **For:** one stack for every program (MacTCP and OT ones share Riptide's TCP, its DHCP, IPv6,
  RiptidePrefs); nothing of Apple's to install; works on a 68020 and on System 7.0/7.1, where
  Apple's OT does not go.
- **Against:** the binding. Existing 68k binaries call OT through Apple's static glue, which
  loads ASLM libraries by class ID: to serve them unchanged, Riptide has to look like ASLM to that
  glue (its Gestalt entry, its library and function-set registration, its dispatch and per-client
  globals) and register libraries with OT's class IDs and function sets in OT's order. ASLM's own
  documentation (in the OT 1.1 SDK) describes the library side; the exact IDs and vtable orders
  OT's glue expects are not in *Inside Macintosh* and would have to be established from published
  SDK documentation, which is the risky part. CFM-68K programs would need CFM shared libraries
  with OT's names on top of Apple's CFM-68K Runtime Enabler. The API surface is large (roughly 150
  client calls and utilities, and some 20 Internet services calls) and XTI's states must be exact.
- **Size:** about 8,000 to 12,000 lines of C and assembly for the client API, InetServices and the
  ASLM-compatible binding; a month or two of work with testing.
- **Apple software needed:** none (CFM-68K programs: Apple's CFM-68K Runtime Enabler).

### (b) Apple's Open Transport 1.1.x, with Riptide as its TCP/IP provider

Apple's OT stays the client API and framework; Riptide becomes a STREAMS module that OT's
configuration names for "tcp", "udp" and "ip" in place of Apple's Open Tpt Internet Library.

- **For:** binary compatibility is Apple's own; every OT program works as it does with Apple's
  stack.
- **Against:** writing TPI providers for STREAMS to OT's module rules (Apple's Open Transport
  Module Developer Note: `streamtab`, `put`/`srv` procedures, message types, OT's port scanner and
  configuration database); replacing Apple's TCP/IP inside OT is not a documented configuration,
  so the TCP/IP control panel, the `'otan'` bits and the MacTCP compatibility layer (OT's own
  `.IPP`) would all assume Apple's provider; Riptide's own `.IPP` cannot run beside OT's. Apple's
  OT asks for a 68030 or later.
- **Size:** about 5,000 to 8,000 lines for the TPI/STREAMS layer, plus much testing against OT's
  undocumented expectations.
- **Apple software needed:** Open Transport 1.1.1 to 1.1.2 for 68k, with ASLM; the user supplies it.

### (c) Apple's Open Transport with its own TCP/IP, and Aurora68's card under it

Riptide is not involved for Open Transport programs. Open Transport drives Ethernet through DLPI
drivers. Whether Open Transport 1.1 can also use a card that only has a classic `.ENET` driver
(Aurora68's slot 9 card is one) through a compatibility layer has to be checked against Apple's
Open Transport Ethernet documentation and with one boot; if it can, no code is needed, otherwise
Aurora68 needs an OT DLPI driver for its card (Apple's Open Transport Ethernet driver developer
documentation; roughly 2,000 to 3,000 lines in the card's own driver set).

- **For:** the least work, possibly none; Apple's full OT, its TCP/IP control panel and DHCP.
- **Against:** two stacks cannot share the card: with OT installed, OT's `.IPP` is present and
  Riptide's INIT stays off (src/mac/init.S), so the user loses Riptide (its preferences, IPv6,
  mDNS, the Command Terminal) whenever OT is on; 68030 and later only.
- **Apple software needed:** Open Transport 1.1.x for 68k.

## Recommendation

1. **Now: the MacTCP path for Internet Explorer 4.01** (done 19 November 2026, docs/MAC.md): the
   Riptide DNR's creator, the MacTCP fixes, the browser test; IE 4.01 confirmed working on real
   hardware the same day. Nothing below is needed for it.
2. **If Open Transport programs are wanted, route (a)**, because it keeps Riptide the one stack on
   the Mac and needs nothing of Apple's, in stages, each one useful and tested on its own:
   - **Stage 1, the client API on Riptide's core** (src/mac/ot/): providers and endpoints over
     core sockets, the XTI state machine, synchronous and asynchronous modes, blocking and
     non-blocking, notifiers from the stack's deferred task, `OTLook`, `OTAllocMem` from a pool,
     `OTInetStringToAddress` and `OTInetAddressToName` through the driver's lookups. Our own
     declarations, written from *Inside Macintosh: Networking With Open Transport*; reached by
     our own programs through a private `.IPP` call that returns a function table (as RTMAC_API
     does for the Command Terminal). Tested by an OT test program of our own that fetches pages
     from pinet's HTTP server the way a browser does (asynchronous endpoints, notifiers, orderly
     release, several at once).
   - **Stage 2, Gestalt `'otan'`** answered only when the binding of stage 3 is present, so that
     programs never take the OT path into libraries that are not there.
   - **Stage 3, the binding for existing 68k programs**: the ASLM-compatible library layer with
     OT's class IDs, from the OT SDK's published ASLM documentation; then CFM-68K libraries with
     OT's names for CFM-68K programs. Tested with programs of our own built the way OT programs
     are built, against declarations of our own.
   - **Stage 4, what OT programs ask for beyond TCP**: UDP endpoints (`"udp"`), `OTInetMailExchange`,
     `OTInetSysInfo`, raw IP, and the configuration calls RiptidePrefs would answer.
3. **Route (c) stays an experiment for whoever has Open Transport 1.1.x at hand**: whether OT's
   classic-driver path accepts Aurora68's `.ENET` is worth one boot, but it means running without
   Riptide.

## Sources

- Microsoft Knowledge Base 181767, "How to Configure a Macintosh Computer for Internet Access"
  (Internet Explorer 4 for the Macintosh: 68030, MacTCP or Open Transport 1.1.1).
- Apple, MacTCP Programmer's Guide (1993); Apple sample code dnr.c (UDPSample and TCP samples,
  1988 and 1991).
- Apple Technical Q&A NW41 (Gestalt `'mtcp'` and `'otan'`), NW18 (OT libraries for 68k), NW19,
  NW34 (OT on ASLM), TN1173.
- Apple, *Inside Macintosh: Networking With Open Transport* (1996).
