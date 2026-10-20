# MiamiDx 1.0c: reverse-engineering notes

> **Naming note.** This is the factual record of analysing the original MiamiDx binary, so it uses the original names throughout. Riptide's own code and documentation use Riptide names: the API that MiamiDx publishes as `miami.library` is implemented by `riptide.library` (functions `Riptide*` instead of `Miami*`, e.g. `RiptideSysCtl()`), which Riptide additionally publishes under the old name as a switchable compat alias (`compat aliases on|off`). See `docs/AMIGA.md`.

This document covers interfaces and internals only, for interoperability and a clean-room reimplementation. It contains no Miami code. Official SDK prototypes are referred to by name only. Facts such as offsets, LVOs, register conventions and string identifiers are given where they are needed.

The registration and key-file mechanism exists (strings `Miami:MiamiDx.key`, `Miami.key1` and `$MiamiKey$` at h0+0x526..0x544, plus the code around h0+0x1700..0x1800). It is **not analysed** here. Where an interface touches it, that is noted and the analysis stops.

## 0. Conventions and evidence tags

* `h<N>+0xOFF` is an offset into hunk N of the binary being discussed. For the main binary `MiamiDx`: h0 = code (457144 bytes), h1 = data (348), h2 = BSS (4096), h3 = data (1444, library init blocks), h4 = data (21860 initialised + BSS, 59440 bytes total).
* Small-data model (SAS/C). Every entry point does `lea $8000.l,a4`, relocated to h4, so `d16(a4)` means **h4+0x8000+d16**. All globals below are written as `h4+0x....`.
* SAS/C emits `jmp abs.l` stubs (for example the block at h0+0x60a08..0x60aa4). A call to a stub is written `stub=>target`.
* `LVO -n` is a library vector offset. Exec calls go through `-$2a94(a4)` (h4+0x556c = SysBase copy) and dos calls through h4+0x5568 (DOSBase). utility.library is at h4+0x1316.
* Evidence tags:
  * **[read]**: seen directly in the disassembly.
  * **[conf]**: cross-confirmed by a second, independent source, such as a data table, the guide, the SDK or a client binary that calls it.
  * **[inf]**: inferred from patterns; the confidence is given where it matters.
  * **[guide]**: stated in `MiamiDx.guide`.
* Tools written for this work (scratch, not shipped) are in `scratchpad/work/`:
  * `adis.py`: capstone disassembler with reloc, a4 and string annotation.
  * `ann.py`: resolves jump stubs.
  * `digest.py`: per-function strings, globals and callees.
  * `libuse.py`: finds which library LVOs a client binary calls.
  * `mnisigs.py`: MNI RomTag and vector table plus register signatures.
  * `strs.py`: strings with hunk offsets.
  * `funcs.py`, `gref.py`: global-reference maps.
  * Large outputs: `MiamiDx.ann` (annotated full disassembly), `kdigest.txt`, `libuse_all.txt`, `mni.sigs`, `lvotables.md`.

---

## 1. Package inventory

The hunk layout uses C = CODE, D = DATA and B = BSS, with sizes in bytes. Descriptions come from `MiamiDx.guide` (NODE_UTILITY_*) [guide] unless marked otherwise.

| File | Size | Hunks | Version | What it is |
|---|---|---|---|---|
| `MiamiDx` | 497416 | C457144 D348 B4096 D1444 D21860 | MiamiDeluxe 1.0c (10.04.2000) | The stack, dialer, PPP, MNI/SANA-II/serial drivers, inetd, NAT, firewall and GUI host, all in one executable. Publishes usergroup, bsdsocket and miami.library from its own code (section 2). |
| `MiamiAddr` | 10060 | C9144 D712 | 1.1 | Converts IP addresses between representations. |
| `MiamiArp` | 17936 | C16984 D772 | 3.0 | Displays and controls the ARP table (uses miami LVO -30 sysctl and -78). |
| `MiamiDNSQuery` | 25180 | C22988 D1592 | 1.2 | DNS query for host records (uses the undocumented resolver LVOs). |
| `MiamiDiG` | 40060 | C37636 D1740 | 1.2 | `dig`. Heaviest user of the undocumented resolver-state LVOs -396..-462. |
| `MiamiDoD` | 9732 | C8780 D760 | 3.2d | Dial-on-demand helper (miami -84/-90/-96/-102/-216). |
| `MiamiFinger` | 10308 | C9420 D708 | 3.0 | finger client. |
| `MiamiFtp` | 79508 | C72892 B65536 D4788 | 1.4 | Command-line FTP client. |
| `MiamiHost` | 35468 | C33276 D1596 | 1.2 | `host`. |
| `MiamiIPFW` | 28096 | C26716 B102400 D956 | 1.2 | ipfw-style firewall rule tool. |
| `MiamiIPNatD` | 21960 | C20296 B65536 D1160 | 1.3 | External IP-NAT daemon (divert based). |
| `MiamiIfConfig` | 12744 | C11348 D1024 | 2.2 | ifconfig. |
| `MiamiInit` | 155948 | C144932 D384 D6724 | 3.2h | Interactive setup wizard. |
| `MiamiMRInfo` / `MiamiMRouteD` / `MiamiMTrace` / `MiamiMapMBone` | | | 3.0 | Multicast routing tools (mrinfo, mrouted, mtrace, map-mbone). |
| `MiamiNSLookup` | 63504 | C60516 D2128 | 1.2 | nslookup. |
| `MiamiNetStat` | 42296 | C40132 B65536 D1496 | 3.2d | 4.4BSD netstat. Reads kernel structures via the kvm-like miami LVOs -36..-60. |
| `MiamiPing` / `MiamiTraceRoute` | | | 3.0b | ping and traceroute (raw ICMP). |
| `MiamiRegister` | 130124 | C79632 D44188 D3524 | 3.2d | Registration tool. Out of scope. |
| `MiamiRemind` | 11736 | C10668 D868 | 3.1 | Online-time reminder (miami -96/-102/-204/-210/-216). |
| `MiamiResolve` | 23100 | C8456 D692 | 3.2h | Name and address resolution test. |
| `MiamiRoute` | 21296 | C19452 D1452 | 3.0 | BSD `route` over a routing socket. |
| `MiamiSecureShellKeygen` / `MiamiSecureShellKill` | | | 0.9c / 0.9b | SSH key generator and session killer. |
| `MiamiSysCtl` | 15196 | C12404 D2116 | 3.2c | sysctl front end (deprecated). |
| `MiamiTCPDump` | 81784 | C70292 B60416 D7304 | 3.2d | tcpdump (uses miamibpf/miamipcap). |
| `MiamiTelnet` / `MiamiTelnetD` | | | 0.9n / 0.9a | telnet/rlogin/SSH client and telnet server. |
| `Libs/miamibpf.library` | 9704 | C9448 D52 | 3.0 (RomTag v3) | BPF packet filter (API in MiamiSDK `miamibpf_lib.fd`). |
| `Libs/miamipcap.library` | 38716 | | 5.1 | libpcap port. |
| `Libs/miamiipnat.library` | 22296 | | 1.6 | IP-NAT engine. Calls miami -78, -168..-192 and -366/-372 (section 3). |
| `Libs/miamisocks.library` / `miamisocksd.library` | | | 2.8 / 1.2 | SOCKS client shim (opened per task by bsdsocket Open when SOCKS is enabled; h0+0x59266) and SOCKS server. |
| `Libs/miamisecureshell.library` | 31308 | | 2.1 | SSH transport. |
| `Libs/plain.MiamiPanel` | 12708 | | 4.2 | Control-panel module (loaded from `Miami:Libs/plain.MiamiPanel`, h0+0x44c). |
| `Devs/miamipppoe.device`, `miamipptp.device`, `miamil2tp.device` | | | 1.5 / 1.0 / 1.0 | Pseudo serial devices (RomTag type 3) that tunnel PPP over PPPoE, PPTP or L2TP. MiamiDx opens them like serial devices (names at h0+0x26512..0x26536). |
| `MNI/*.mni` (6 files) | 10–19 KB | C + small D | 3.0 / 3.1 | Miami Network Interface Ethernet drivers (section 4). |
| `SANA2/sanamni.device` | 9604 | C9116 D344 | sanamni 1.0 (29.11.98) | SANA-II device wrapping an MNI driver (section 4.6). |
| `MiamiDx.guide` | | | | AmigaGuide manual (the source of every [guide] tag). |
| `PC/MDMCISCO.INF`, `Icons/*` | | | | Windows modem INF and icons. Not relevant. |

GUI engines are external modules named `Miami:Libs/<name>.MiamiDxGui`. h0+0x323ce builds the path from `Miami:Libs/` and `.MiamiDxGui` [read]. No GUI engine is included in this archive.

---

## 2. Process and library architecture

### 2.1 Start-up and command line [read]

* The entry point is h0+0x0. It clears the BSS and saves SysBase to h4+0x556c.
* `ReadArgs` (dos LVO -798) runs at h0+0x1d5c with the template at h0+0x580: `PACKETDEBUG/K/N,DONTCONNECT/S,SETTINGS,AREXX/K,PUBSCREEN/K,GUI/K,NOGUI/S`. Results are stored as follows:
  * PACKETDEBUG: h4+0x1a2
  * DONTCONNECT: h4+0x10
  * SETTINGS: copied to h4+0x7b10
  * AREXX: copied to h4+0x1ca
  * PUBSCREEN: copied to h4+0xfbe
  * GUI: h4+0x151
  * NOGUI: h4+0x1a6
* The same keywords exist as Workbench tooltypes (strings at h0+0x5fc..0x632). Their meanings are in NODE_REFERENCE_TOOLTYPES [guide]. PACKETDEBUG=10/20 writes `MiamiDx.debug.<if>` hex dumps; AREXX=script runs that script at start-up.
* **Single-stack guard** (h0+0x1de6..0x1e4c): under Forbid it calls `FindName(SysBase->LibList (SysBase+0x17a), …)` for `usergroup.library`. If that library is present, it first tries to flush stale libraries (`AllocMem(-1, MEMF_PUBLIC)`, then `Delay(30)`). It then checks for `usergroup.library`, `bsdsocket.library` and `miami.library`, and if any of them still exists, MiamiDx refuses to start. Your stack must therefore not coexist with MiamiDx, and the reverse also holds.

### 2.2 How the three libraries are published [read]

All three are built with `MakeLibrary` (exec -84) from init blocks in h3. None of them is backed by a SegList. Each one uses a **two-level base scheme**:

1. A *master* base is added to the system list with `AddLibrary` (exec -396). Its only real vectors are Open and Expunge.
2. `OpenLibrary` lands in the master's Open. Open builds (or reuses) a **separate per-opener-task Library** with a second vector table, and returns that. Every later call and `CloseLibrary` goes to the per-task base.

| Library | Master MakeLibrary / AddLibrary | Master vectors (Open, -, Expunge, -) | Per-task vectors (-, Close, -, -, funcs…) | InitStruct table | dataSize (per base) | Name / idString / version |
|---|---|---|---|---|---|---|
| usergroup | h0+0x16bc8 / h0+0x16c14 | h3+0x5c: 0x16a2a, 0, 0x16af2, 0 | h3+0x70: 0, 0x16b46, 0, 0; funcs from h3+0x80 (39) | h3+0x38 | 0x72a | h3+0x0 / h3+0x12 "usergroup.library 4.1 (15.12.96)", v4.1 |
| bsdsocket | h0+0x595dc / h0+0x59782..0x597be | h3+0x4c4: 0x591d0, 0, 0x592d0, 0 | h3+0x4d8: 0, 0x5950a, 0, 0; funcs from h3+0x4e8 (46) | h3+0x4a0 | 0x26 | h3+0x468 / h3+0x47a "bsdsocket.library 4.1 (15.12.96)", v4.1 |
| miami | h0+0x5f034 / h0+0x5f0cc | h3+0x2f4: 0x5ed04, 0, 0x5ef32, 0 | h3+0x308: 0, 0x5ef86, 0, 0; funcs from h3+0x318 (83) | h3+0x2d0 | 0x2e | h3+0x29f / h3+0x2ae "miami.library 21.0 (11.03.2000)", v21.0 |

Each InitStruct table sets ln_Type = NT_LIBRARY, lib_Flags = SUMUSED|CHANGED, the version and revision, the name and the idString [read]. Do not call the master's Close; it is a null vector.

**Per-task context ("ctx").** bsdsocket and miami share one per-task context.

* `ctx_lookup` is stub 0x5bb3a=>0x46036. It walks the list at h4+0x8ac4 under Forbid and matches ctx+0x1c against FindTask(NULL).
* If no context is found, h0+0x59058 creates one with `AllocVec(0xb68, MEMF_PUBLIC|MEMF_CLEAR)`.
* Each per-task Library stores the ctx pointer at **base+0x22**.
* The ctx stores the bsdsocket base at ctx+0x40 and the miami base at ctx+0x44. A second OpenLibrary from the same task only increments lib_OpenCnt (h0+0x591ec, h0+0x5ed1a).

usergroup keeps its per-task state inline in its 0x72a-byte base. h0+0x16a2a matches base+0x22 against the owner task and keeps its list at h4+0x7ea6.

Known ctx fields [read, inf]:

| Offset | Meaning |
|---|---|
| +0x1c | Owner task. |
| +0x3f | Flag byte. bit2 is set by miami -354; bit3 by miami -486 and changes shutdown behaviour. |
| +0x40 / +0x44 | bsdsocket / miami per-task base. |
| +0x48 / +0x4c | Descriptor table: +0x4c is an array of socket pointers, followed by an in-use bitmap; +0x48 is its size. See socketpair at 0x6026a. |
| +0x50 / +0x54 | fd callback and its CPU type (MiamiSet/GetFdCallback). |
| +0x74 | Open reference count. |
| +0xa68 | Per-opener resolver context pointer (allocated lazily by 0x69724). |
| +0xa70 | DisallowDNS flag. |
| +0xa7c | Per-opener miamisocks.library base. |
| +0xa80 / +0xa84 | SSL library base / SSL open count. |
| +0xa8c | Online-change signal mask (miami -204). |
| +0xa90 | Memory pool. |
| +0xa94 | PPC bridge object. |
| +0xa9c | UserGroupCredentials copy (0xb8 bytes, miami -348). |
| +0xb54 | SSL callback block. |

miami per-task base layout: Library (0x22), +0x22 ctx, +0x26 kvm-lock nesting count, +0x2a saved spl level (see -42/-48).

**Shutdown** [read]. h0+0x59608 (bsdsocket) and h0+0x5f060 (miami):

* Sets the "closing" flag (h4+0x4b60 / h4+0x4c9c) and marks the master for expunge.
* For every live ctx: if ctx+0x3f bit3 is set, it invokes the opener's fd-callback with (fd = -1, code 0x101) via h0+0x5a2c4. Otherwise it sends `Signal(task, SIGBREAKF_CTRL_C)` (0x1000).
* It then polls with `Delay(10)` until every per-task base is closed.

**Kernel concurrency model** [read]. Library calls execute **on the caller's task**:

* There is no central stack task for the socket API. Calls are serialised by a recursive "big kernel lock": `kern_enter` at h0+0x467e2 (ObtainSemaphore h4+0x8ad8, owner h4+0x8ad4, depth h4+0x3ce8) and `kern_leave` at h0+0x46818.
* There is also a BSD `spl` emulation: `splX(level)` at h0+0x15336 (semaphore h4+0x7cd0, owner h4+0x2960, level h4+0x296c) and `splx` at h0+0x15380.
* Practically every protocol path is bracketed by these calls.

### 2.3 Internal processes [read]

Created with `CreateNewProc` (dos -498):

| Name string | Created at | Role |
|---|---|---|
| `Miami Airport <%s>` (h0+0x229a2) | h0+0x22dfa | One per hardware device (serial, MNI or SANA-II). Accepts command messages through h0+0x22a8c/0x22b5a (array h4+0x2ef8, count h4+0x2efc). [inf] |
| `Miami Heat <%s>` (h0+0x291fe) | h0+0x2a67e | One per configured interface (interface table h4+0xeea); the link and dialer worker. [inf] |
| `Miami Downtown` (h0+0x24b50) | h0+0x24c9e | Opens bsdsocket. Role not determined. |
| `Miami Beach` / `MiamiDx-INetD` (h0+0x2c346) | h0+0x2c6de | The internal inetd (built-in echo, discard, daytime, chargen, time, finger and auth services; strings at h0+0xefb2..). |
| `Miami IPNatD` | h0+0x43770 | Internal NAT daemon (loads `Miami:Libs/miamiipnat.library`). |
| `MiamiDx GetTime` | h0+0x445a4 | Time synchronisation (IFACETCPIP_GETTIME [guide]). |
| `MiamiPPC` | h0+0x68c38 | Optional PowerPC kernel through ppc.library (PPCGetMessage etc. are imported). |

### 2.4 ARexx [read + guide]

* Port name: `MIAMI.1` (h0+0x56a).
* The command parser is at about h0+0x2f00..0x3400. It uses a case-insensitive prefix compare (stub=>0x6d628) against the strings at h0+0x67c..0x7d0.
* A built-in help text at h4+0x1e10 lists the commands and templates:

| Command | Template | Notes [guide] |
|---|---|---|
| ACTIVATE, DEACTIVATE, HIDE, SHOW, QUIT | – | MUI-style standard commands. |
| HELP | FILE/A | |
| INFO | ITEM/A | Items: TITLE, AUTHOR, COPYRIGHT, DESCRIPTION, VERSION, BASE (h0+0x72e..0x794). BASE = "MIAMI". |
| CHANGEDB | – | Re-reads `ENVARC:MiamiChangeDB` (lines like `ADD services ntalk 518/udp`). The previous file is kept as `ENVARC:MiamiChangeDB.old` (h0+0x12e3a). |
| GETCONNECT | INTERFACE | Modem CONNECT string. |
| GETCONNECTTIME | INTERFACE | Seconds since CONNECT. |
| GETONLINETIME | INTERFACE | Seconds online. |
| GETSETTINGSNAME | – | |
| GETTRANSFEREDBYTES | INTERFACE | Returned as two 32-bit halves, format `"%lu %lu"` (h0+0x7e4). |
| HIDEPANEL / SHOWPANEL | – | |
| ISONLINE | INTERFACES | Answer is returned in RC. |
| KILLGUI, LOCKGUI, UNLOCKGUI | – | LOCKGUI nests. |
| LOADSETTINGS | FILE/A | |
| ONLINE / OFFLINE | INTERFACE | |
| ONLINEGUI | INTERFACE | |
| QUITFORCE | – | |

### 2.5 Configuration files and settings format

* The default settings file is `Miami:MiamiDx.default` (h0+0x658, h0+0x12f3c). SETTINGS= overrides it.
* The settings are an **IFF FORM `MIAM`** file written and read through iffparse.library (opened at h0+0x6ec12). The guide calls the format "intentionally undocumented" and says it is shared with Miami [guide]. Chunk IDs appear as immediates at three sites (writer about h0+0x1c83e.., table about h0+0x1e7e8.., reader about h0+0x1ed0a..) [read]:
  * `VERS`: version
  * `GENE`: general
  * `IFAC`: interfaces
  * `PPP `
  * `DIAL`: dialer
  * `EVEN`: events
  * `TCPI`: TCP/IP page
  * `LOG `, `GUI `, `MUI `, `ICON`
  * `SANA`: SANA-II/MNI hardware
  * `SOCK`: SOCKS
  * Database tables `DBxx`: `DBPR` protocols, `DBSE` services, `DBHO` hosts, `DBNE` networks, `DBDO` domains, `DBDN` DNS servers, `DBUS` users, `DBGR` groups, `DBIN` inetd, `DBAR` arp, `DBSO` socks, `DBHA` host aliases, `DBSY` sysctl, `DBFI` ipfilter, `DBNA` ipnat
  * MiamiDx-specific: `DXVR`, `DXTC`, `DXHW`, `DXDI`, `DXIF`
* A second form, **FORM `MII3`** with chunks `VERS DEVI MODM PHON USER PROT DNSS`, is handled at h0+0x21b86.. (MiamiInit settings import) [read]. ENV:Sys/Serial.prefs (FORM `PREF`/`SERL`) is parsed at h0+0x4229c [read].
* Other files and variables:
  * `ENV:MIAMI/GUI`: default GUI engine.
  * `ENV:MIAMI/SSLLIB`: SSL library path, read by MiamiOpenSSL at h0+0x5f6c2.
  * `ENV:Sys/def_MiamiApp.info` / `def_MiamiAppOnline.info`: AppIcons.
  * `Miami:MiamiDx.QuickReconnect-<if>`: PPP quick-reconnect state (h0+0x3b950).
  * `Miami:MNI/` and `DEVS:Networks/`: driver search paths (h0+0x32a2e/0x32a3a).
  * Environment variables set when online: HOST(NAME), DOMAIN(NAME), USERNAME, REALNAME, HOME, SOCKETCONFIG [guide].

---

## 3. Library LVO tables

The code addresses are h0 offsets taken from the vector tables in h3. Argument registers for documented functions follow the official FD files and were checked against the code prologues (`sigs2.py`, `scratchpad/work/*.sigs2`).

### 3.1 bsdsocket.library (AmiTCP 4 API, 46 vectors, h3+0x4e8)

| LVO | name | args (regs) | code |
|---|---|---|---|
| -30 | socket | d,t,p (d0,d1,d2) | h0+0x5a022 |
| -36 | bind | s,n,nl (d0,a0,d1) | h0+0x5c7b6 |
| -42 | listen | s,b (d0,d1) | h0+0x5c866 |
| -48 | accept | s,a,al (d0,a0,a1) | h0+0x5c8ee |
| -54 | connect | s,n,nl (d0,a0,d1) | h0+0x5caf2 |
| -60 | sendto | s,m,l,f,t,tl (d0,a0,d1,d2,a1,d3) | h0+0x5c654 |
| -66 | send | s,m,l,f (d0,a0,d1,d2) | h0+0x5c494 |
| -72 | recvfrom | s,b,l,f,fr,frl (d0,a0,d1,d2,a1,a2) | h0+0x5d05c |
| -78 | recv | s,b,l,f (d0,a0,d1,d2) | h0+0x5cf00 |
| -84 | shutdown | s,h (d0,d1) | h0+0x5cc8e |
| -90 | setsockopt | s,l,o,ov,ol (d0,d1,d2,a0,d3) | h0+0x5d1d2 |
| -96 | getsockopt | s,l,o,ov,ol (d0,d1,d2,a0,a1) | h0+0x5d290 |
| -102 | getsockname | s,h,n (d0,a0,a1) | h0+0x5d444 |
| -108 | getpeername | s,h,n (d0,a0,a1) | h0+0x5d53e |
| -114 | IoctlSocket | d,r,a (d0,d1,a0) | h0+0x5beac |
| -120 | CloseSocket | d (d0) | h0+0x5a126 |
| -126 | WaitSelect | n,r,w,e,t,m (d0,a0,a1,a2,a3,d1) | h0+0x5b296 |
| -132 | SetSocketSignals | sin,sio,su (d0,d1,d2) | h0+0x5a362 |
| -138 | getdtablesize |  () | h0+0x5a23e |
| -144 | ObtainSocket | i,d,t,p (d0,d1,d2,d3) | h0+0x5b0ea |
| -150 | ReleaseSocket | f,i (d0,d1) | h0+0x5aed8 |
| -156 | ReleaseCopyOfSocket | f,i (d0,d1) | h0+0x5b012 |
| -162 | Errno |  () | h0+0x5a258 |
| -168 | SetErrnoPtr | e,s (a0,d0) | h0+0x5a27a |
| -174 | Inet_NtoA | i (d0) | h0+0x5a3b0 |
| -180 | inet_addr | c (a0) | h0+0x5bcd6 |
| -186 | Inet_LnaOf | i (d0) | h0+0x5a3fe |
| -192 | Inet_NetOf | i (d0) | h0+0x5a444 |
| -198 | Inet_MakeAddr | n,h (d0,d1) | h0+0x5a490 |
| -204 | inet_network | c (a0) | h0+0x5a612 |
| -210 | gethostbyname | n (a0) | h0+0x5ad16 |
| -216 | gethostbyaddr | a,l,t (a0,d0,d1) | h0+0x5ab28 |
| -222 | getnetbyname | n (a0) | h0+0x5a966 |
| -228 | getnetbyaddr | n,t (d0,d1) | h0+0x5a8f0 |
| -234 | getservbyname | n,p (a0,a1) | h0+0x5a8c0 |
| -240 | getservbyport | p,pr (d0,a0) | h0+0x5a80c |
| -246 | getprotobyname | n (a0) | h0+0x5a6f2 |
| -252 | getprotobynumber | p (d0) | h0+0x5a686 |
| -258 | vsyslog | l,f,a (d0,a0,a1) | h0+0x5b620 |
| -264 | Dup2Socket | fa,fb (d0,d1) | h0+0x5ad38 |
| -270 | sendmsg | s,m,f (d0,a0,d1) | h0+0x5c72e |
| -276 | recvmsg | s,m,f (d0,a0,d1) | h0+0x5d146 |
| -282 | gethostname | h,s (a0,d0) | h0+0x5a62e |
| -288 | gethostid |  () | h0+0x5a670 |
| -294 | SocketBaseTagList | t (a0) | h0+0x59c0e |
| -300 | GetSocketEvents | e (a0) | h0+0x5b8e4 |

Notes [read]:

* WaitSelect (-126, 0x5b296) uses the select core at h0+0x5d9cc (its sleep message string is "select" at h0+0x5bcce).
* socket (-30) goes to `socreate` stub 0x5bb5e=>0x4c5c2.
* SocketBaseTagList (-294) is at 0x59c0e.

### 3.2 usergroup.library (39 vectors, h3+0x80)

| LVO | name | args (regs) | code |
|---|---|---|---|
| -30 | ug_SetupContextTagList | p,t (a0,a1) | h0+0x16e88 |
| -36 | ug_GetErr |  () | h0+0x16e46 |
| -42 | ug_StrError | c (d1) | h0+0x16da8 |
| -48 | getuid |  () | h0+0x16f7e |
| -54 | geteuid |  () | h0+0x16f8e |
| -60 | setreuid | r,e (d0,d1) | h0+0x17034 |
| -66 | setuid | i (d0) | h0+0x17084 |
| -72 | getgid |  () | h0+0x16f9e |
| -78 | getegid |  () | h0+0x16fae |
| -84 | setregid | r,e (d0,d1) | h0+0x17094 |
| -90 | setgid | i (d0) | h0+0x170fc |
| -96 | getgroups | n,g (d0,a1) | h0+0x16fcc |
| -102 | setgroups | n,g (d0,a1) | h0+0x1710c |
| -108 | initgroups | n,b (a1,d0) | h0+0x17306 |
| -114 | getpwnam | n (a1) | h0+0x17522 |
| -120 | getpwuid | u (d0) | h0+0x174de |
| -126 | setpwent |  () | h0+0x1748e |
| -132 | getpwent |  () | h0+0x1749e |
| -138 | endpwent |  () | h0+0x1747e |
| -144 | getgrnam | n (a1) | h0+0x17430 |
| -150 | getgrgid | g (d0) | h0+0x173ea |
| -156 | setgrent |  () | h0+0x1739a |
| -162 | getgrent |  () | h0+0x173aa |
| -168 | endgrent |  () | h0+0x1738a |
| -174 | crypt | k,s (a0,a1) | h0+0x168c2 |
| -180 | ug_GetSalt | u,b,s (a0,a1,d0) | h0+0x169a6 |
| -186 | getpass | p (a1) | h0+0x17706 |
| -192 | umask | m (d0) | h0+0x17180 |
| -198 | getumask |  () | h0+0x1719e |
| -204 | setsid |  () | h0+0x16f70 |
| -210 | getpgrp |  () | h0+0x16fbe |
| -216 | getlogin |  () | h0+0x17250 |
| -222 | setlogin | b (a1) | h0+0x1727a |
| -228 | setutent |  () | h0+0x1757e |
| -234 | getutent |  () | h0+0x1758e |
| -240 | endutent |  () | h0+0x1756e |
| -246 | getlastlog | u (d0) | h0+0x175f2 |
| -252 | setlastlog | u,n,h (d0,a0,a1) | h0+0x17630 |
| -258 | getcredentials | t (a0) | h0+0x171ae |

### 3.3 miami.library (83 vectors, h3+0x318)

LVOs -30..-342 correspond one-to-one to MiamiSDK 2.11 `miami_lib.fd`, but the FD lists 23 of them as `MiamiReservedN`. LVOs -348..-522 (30 functions) are not in any SDK. Both groups are analysed below. "Used by" comes from scanning every binary in the package for `movea.l <MiamiBase>,a6; jsr -n(a6)` (`libuse.py`, output in `libuse_all.txt`) [conf].

**Documented functions** (names per SDK): -30 MiamiSysCtl 0x5f200 · -66 MiamiDisallowDNS 0x5f35e (writes ctx+0xa70) · -78 MiamiGetPid 0x5f38e (returns the ctx pointer) · -96 MiamiPFAddHook 0x5f3ce · -102 MiamiPFRemoveHook 0x5f4e4 · -108 MiamiGetHardwareLen 0x5f5bc · -150 MiamiOpenSSL 0x5f66e (reads `MIAMI/SSLLIB` with GetVar and passes a callback block at ctx+0xb54) · -156 MiamiCloseSSL 0x5f752 · -198 MiamiSetSocksConn 0x5f992 (forwards to miamisocks LVO -234) · -210 MiamiIsOnline 0x5fa28 (`"mi0"` or NULL means any interface) · -216 MiamiOnOffline 0x5fa6c · -228 inet_ntop 0x60de6 · -234 inet_aton 0x60e18 · -240 inet_pton 0x60e2e · -246 gethostbyname2 0x5fb3a · -252 gai_strerror 0x60e5e · -258 freeaddrinfo 0x60edc · -264 getaddrinfo 0x60f30 · -270 getnameinfo 0x61516 · -276 if_nametoindex 0x616c0 · -282 if_indextoname 0x616d4 · -288 if_nameindex 0x616ea · -294 if_freenameindex 0x616fa · -300 MiamiSupportsIPV6 0x5fb56 (**always returns 0**) · -306/-312 MiamiResGet/SetOptions 0x5fb64/0x5fba0 · -318 sockatmark 0x6170e · -324 MiamiSupportedCPUs 0x5fbe0 (stores 1, 3, 1) · -330/-336 MiamiGet/SetFdCallback 0x5fbf6/0x5fc1a (CPU type must be 1 or 2; stored at ctx+0x50/+0x54).

**Reserved and undocumented functions.** Confidence: H = high, M = medium, L = low.

| LVO | Code | Registers (observed) | Behaviour seen in code | Inferred purpose | Conf. | Used by |
|---|---|---|---|---|---|---|
| -36 (R1) | 0x5f270 | a0 = array of {char *name; LONG value} (8 bytes each, ends with NULL name) | Looks up each name in a symbol table and stores the address. Uses the full table h4+0x4d0c/0x4ca4 or the reduced table h4+0x4d50/0x4ce4 depending on flag h4+0x2954. Returns the number found. Symbols: `_mbstat _ipstat _tcb _tcpstat _udb _udpstat _ifnet _icmpstat _rtstat _rt_tables _igmpstat _ip_mrtproto _mrtstat _mfctable _viftable _locallist`. | `kvm_nlist()` | H | NetStat |
| -42 (R2) | 0x5f2de | – | On the first nested call: kern_enter, then `splX(9)` with the old level saved in base+0x2a; increments base+0x26. | Begin a consistent kernel-memory read (lock) | H | NetStat |
| -48 (R3) | 0x5f312 | – | Reverse of -42 (splx, kern_leave). | End the kernel-memory read | H | NetStat |
| -54 (R4) | 0x5f338 | – | Returns 13. | Constant (interface or kvm revision?) | L | – |
| -60 (R5) | 0x5f346 | a0, a1 = ULONG * | `*a0 = 128; *a1 = 2048`. | Get MSIZE and MCLBYTES (for netstat -m) | H | NetStat |
| -72 (R6) | 0x5f372 | – | `OpenLibrary("bsdsocket.library", 0)`. | Convenience open of bsdsocket for the caller | H | – |
| -84 (R7) | 0x5f39e | – | Returns (h4+0x2954 != 0). | Feature-state query. The same flag gates T/TCP sysctl, CC option parsing, the full kvm table, SSL and PF hooks. It may relate to the registration state, which is not analysed. | M (mechanics H) | IfConfig, Route, SysCtl, TCPDump, DoD, MRInfo, MRouteD, MTrace, MapMBone, NetStat, Remind |
| -90 (R8) | 0x5f3b6 | – | Calls the function pointer at h4+0x12fe (default h0+0x15f42) with (0, 0); returns 0 with the default target. | Unknown hook (not analysed) | L | Same tools as -84 |
| -114 (R9) | 0x5f8a2 | – | Ensures the per-opener resolver context exists (0x69724), then calls 0x6c794. | `res_init()` | M-H | DNSQuery, DiG, Host, NSLookup |
| -120 (R10) | 0x5f85c | d0 op, a0 dname, d1 class, d2 type, a1 data, d3 datalen, a2 newrr, a3 buf, d4 buflen | Forwards nine arguments to 0x6c008. | `res_mkquery()` | H | DiG, Host, NSLookup |
| -126 (R11) | 0x5f8de | a0 msg, d0 msglen, a1 answer, d1 anslen | Calls 0x6b6c0 (contains the debug strings `;; res_send()` etc.). | `res_send()` | H | DiG, Host |
| -132 (R12) | 0x5f79c | a0 = caller-owned 0x4a0-byte buffer | 0x697f0 copies the current resolver state into a0, then makes a0 the active per-opener state. | Attach a caller-supplied `struct __res_state` | M | – |
| -138 (R13) | 0x5f7c2 | – | Returns h4+0x4c18, a table of pointers to "QUERY", "IQUERY", … | `_res_opcodes[]` | H | DNS tools |
| -144 (R14) | 0x5f7d4 | – | Returns h4+0x4c58, a table: "NOERROR", "FORMERR", "SERVFAIL", … | `_res_resultcodes[]` | H | DNS tools |
| -162 (R15) | 0x5f914 | – | Returns ctx+0xa7c. | Get the per-opener miamisocks.library base (NULL means no SOCKS) | M-H | Ping, TraceRoute |
| -168 (R16) | 0x5f928 | a0 = table name | 0x11f28 searches the 17 settings-database table names (`protocols` first, list at h4+0x23f8). Returns the index or -1. | DB: find table | M-H | miamiipnat.library |
| -174 (R17) | 0x5f93c | d0 = index | ObtainSemaphore on the table. | DB: lock table | M-H | miamiipnat |
| -180 (R18) | 0x5f950 | d0 | ReleaseSemaphore. | DB: unlock table | M-H | miamiipnat |
| -186 (R19) | 0x5f964 | d0 | Returns table+0x2e. | DB: row count | M-H | miamiipnat |
| -192 (R20) | 0x5f978 | d0 index, d1 row, a0 ULONG * | If the row is valid: `*a0 = table+0x3a[row]`, returns `table+0x36[row]` (the row text). | DB: get row | M-H | miamiipnat |
| -204 (R21) | 0x5f9ce | d0 = signal mask | Stores the mask at ctx+0xa8c. h0+0x5f9e2 signals every opener with its mask when the online state changes. | Request an online/offline change signal | M-H | Remind |
| -222 (R22) | 0x5fab0 | – | Returns global h4+0x2f30 (via 0x23d7e). | Unknown | L | – |
| -342 (R23) | 0x5fc50 | a0..a3 | 0x6905c builds a type-4 message and posts it to the `MiamiPPC` kernel through ppc.library. | PPC bridge call | L-M | – |
| -348 | 0x5fcb8 | – | Refreshes ctx+0xa9c from the global credentials (0x17204, same layout as UserGroupCredentials: 0xb8 bytes) and returns a pointer to it. | Get current credentials (like `getcredentials`) | M | – |
| -354 | 0x5fd1a | d0 = bool | Sets or clears ctx+0x3f bit2. | Per-opener "internal/system client" marker? | L | MiamiDx itself, miamil2tp, miamipptp |
| -360 | 0x5fd2e | – | Returns 1. | Capability probe ("Deluxe API present"?) | L | NetStat, Telnet, TelnetD, l2tp, pptp |
| -366 | 0x5fd4c | a0 | Stores h4+0x4d7c. | Register the IP-NAT module state pointer | M | miamiipnat.library |
| -372 | 0x5fd3c | – | Returns h4+0x4d7c. | Get the IP-NAT module pointer | M | NetStat, miamiipnat |
| -378 | 0x61774 | d0 net, a0 dst, d1 size | Prints significant octets as `%u` joined by "."; prints "0.0.0.0" when the value is zero. | `inet_neta()` | H | – |
| -384 | 0x61810 | d0 af (must be 2), a0 src, d1 bits (0..32), a1 dst, d2 size | Writes "a.b.c.d/bits". | `inet_net_ntop()` | H | – |
| -390 | 0x6193e | d0 af, a0 src, a1 dst, d1 size | Parses dotted or `0x` hex. | `inet_net_pton()` | M-H | – |
| -396 | 0x5fd5c | – | `AllocVec(0x4a0, MEMF_PUBLIC|MEMF_CLEAR)`. | Allocate a `struct __res_state` | H | DiG |
| -402 | 0x5fd80 | a0 | FreeVec. | Free a res_state | H | DiG |
| -408 | 0x5fd9e | a0 dst, a1 src (NULL means current) | Copies 0x4a0 bytes. | Copy a res_state | H | DiG |
| -414 | 0x5ff4c | a0 res (NULL means current), a1 taglist | Walks the tags (utility NextTagItem). Accepts tags 0x80570100..0x8057011a and gets or sets fields such as options, retrans, retry, nscount, nsaddr_list[i] (16-byte sockaddr each, index set by a tag), ndots and the dnsrch pointers. | Get or set resolver-state fields | M | DNSQuery, DiG, Host, NSLookup |
| -420 | 0x5fdf6 | a0 res (NULL means current), a1 filename | Writes the magic "MiamiResState\0" (14 bytes), a version word of 1 and 0x4a0 bytes. | Save a res_state to a file | H | DiG |
| -426 | 0x5fe88 | a0 res, a1 filename | Reads and checks the magic and version, then loads 0x4a0 bytes. Quirk: both save and load open with mode 1006 (MODE_NEWFILE) [read]. | Load a res_state | H | DiG |
| -432 | 0x5f7e6 | a0 dname, d0 class, d1 type, a1 answer, d2 anslen | Calls 0x6c210. | `res_query()` | H | DNSQuery |
| -438 | 0x5f808 | a0 name, a1 domain, d0 class, d1 type, a2 answer, d2 anslen | Calls 0x6c300 (format "%s.%s"). | `res_querydomain()` | H | – |
| -444 | 0x5f83a | a0 dname, d0 class, d1 type, a1 answer, d2 anslen | Calls 0x6c3f4. | `res_search()` | H | DNSQuery |
| -450 | 0x60126 | a0 msg, a1 eom, a2 comp_dn, a3 exp_dn, d0 length | Calls 0x6a94c. | `dn_expand()` | M-H | DNSQuery, DiG, Host, NSLookup |
| -456 | 0x60182 | a0 comp_dn, a1 eom | Calls 0x6a98a. | `dn_skipname()` | M | same |
| -462 | 0x601c6 | a0, a1, a2 | Stores three pointers at resolver context +0x208e, +0x2086 and +0x208a. | Install resolver hooks (query, response, debug output?) | L-M | DiG |
| -468 | 0x60212 | – | Returns h4+0x2954 raw. | Same flag as -84 | M | Telnet, TelnetD |
| -474 | 0x60222 | – | Empty body (d0 is returned unchanged). | Legacy no-op | H (mechanics) | Addr, DNSQuery, DiG, Ftp, Host, IPFW, IPNatD, NSLookup |
| -480 | 0x6022e | – | Returns the address h4+0x5d98, a structure used by the licensing code at h0+0x170a..0x17fa. | Not analysed (out of scope) | – | Telnet |
| -486 | 0x60240 | d0 = bool | Sets or clears ctx+0x3f bit3. When set, stack shutdown calls the opener's fd-callback (fd -1, code 0x101) instead of sending CTRL-C. | Shutdown notification by callback | M | – |
| -492 | 0x6026a | d0 domain, d1 type, d2 protocol, a0 int sv[2] | Two `socreate` (0x4c5c2) calls and `soconnect2` (0x4ca48); stores both fds in sv. | `socketpair()` | H | – |
| -498 | 0x6090e | d0 fd | If SOCKS is active, calls miamisocks LVO -240(fd). Otherwise performs a close/re-create/connect sequence on the socket (soclose 0x4c846, socreate 0x4c5c2, soconnect 0x4c9d6). | Unknown socket operation, possibly SOCKS-related | L | – |
| -504 | 0x5fc84 | a0..a3 | Calls 0x69108, which is a bare `rts`. | No-op (counterpart of -342?) | H (mechanics) | – |
| -510 | 0x6046c | a0 ifname, a1 ULONG *, a2 char *, a3 ULONG *, **a4** char * | Resolves the name to an index (0x5288c; "lo0" is rejected). Copies the kernel-interface record (array h4+0x4468) fields +0x16a, +0x172 (string), +0x16e and +0x1c2 (string). Returns 0 or a negative value. **a4 is used as an argument register.** | Per-interface status, probably times and connect/description strings | L | – |
| -516 | 0x60986 | a0 ifname, a1 buf | Copies the interface string at +0x2c2 (up to 32 bytes). | Per-interface name or alias string | L | – |
| -522 | 0x60926 | a0 = struct utsname * (five 64-byte fields) | sysname "AmigaOS", nodename (hostname h4+0x2988), release "V<exec version>", version "MiamiDeluxe 1.0c (10.04.2000)", machine "68k". | `uname()` | H | – |

Summary: the undocumented range is mostly **resolver internals** (BIND 4.9-style res_* and dn_*, plus resolver-state management), **kvm-style kernel inspection** for netstat, the **settings-database access** used by the NAT library, a **NAT module hook**, the **PPC bridge** and **BSD extras** (socketpair, uname, inet_net_*).

---

## 4. MNI (Miami Network Interface) driver ABI

### 4.1 Container [read, `mnisigs.py`, `mni.sigs`]

* Each `.mni` file is a normal Amiga hunk executable with two hunks (code and data) and a **RomTag at h0+0x4** (h0+0x0 holds `moveq #0,d0; rts`):
  * rt_Flags = RTF_AUTOINIT (0x80), rt_Version = **3**, rt_Type = NT_LIBRARY (9), rt_Pri = 0.
  * rt_Name is the file name; rt_IdString is something like "z2-smc91c90.mni 3.0"; rt_Init points at h1+0x4.
* The autoinit table at h1+0x4 holds:
  * dataSize: 0x9c (smc91c90), 0xa4 (am7990, mb86950), 0xc8 (z2-dp8390), 0xdc (3c509), 0x124 (gg2-dp8390)
  * funcTable (absolute pointers)
  * dataTable = 0
  * initRoutine = h0+0x20
* All six drivers share an identical front end. The vector addresses are the same in every driver (h0+0xc4..0x464), and only the chip code differs.
* **Load method:** `OpenLibrary(name, 0)`. If that fails, the caller retries with `"Miami:MNI/" + name` (sanamni h0+0x16e2..0x171e; MiamiDx h0+0x2ebe8 uses the configured path).
* Library-base private area: the driver sets **a4 = base+0x30** on every entry. a4+0x24 holds the library base, a4+0x28 SysBase, a4+0x2c a SignalSemaphore and a4+0x5c the board list.

### 4.2 Calling convention [read]

* Calls use standard library register arguments with a6 = MNI base. Every call that operates on a board takes an **API version in d0**. The caller passes the version obtained from GetInfo, and the driver rejects anything outside 1..3 with -1.
* Results come back in d0: 0 = OK, negative = error.
* Observed error codes: -1 bad argument or version, -2 no board, -3 out of memory, -5 already present, -7 aborted (written into a packet when the handle goes offline), -9 handle not online.

### 4.3 Function table (identical in all six drivers)

| LVO | Code | Registers | Name (inferred) | Behaviour [read in z2-smc91c90] |
|---|---|---|---|---|
| -6 / -12 / -18 / -24 | 0xc4 / 0xd6 / 0x100 / 0x0 | | Open / Close / Expunge / Reserved | Standard. Reserved returns 0. |
| -30 | 0x1a0 | a0 = MNIInfo * | **GetInfo** | In: [0] = the caller's API version, must be nonzero (sanamni passes 2, MiamiDx 3). Out: [1] = the driver's API version (3), [2] name, [3] description, [4] copyright, [5] `$VER` string, [6] supported-boards text, [7] options text (or "(none)"). |
| -36 | 0x1ca | d0 ver, a0 out[3] | **DeviceQuery** | out = {address size in bits = 48, MTU = 1500, wire type = 1 (Ethernet)}. MiamiDx h0+0x2ed46 uses it. |
| -42 | 0x238 | d0 ver, d1 flags (passed through), a0 = BoardList ** | **FindBoards** | Scans with expansion `FindConfigDev`. Allocates {ULONG count; struct {ULONG reserved; char *desc; ULONG unit;} board[count]}. Returns -2 if none found. |
| -48 | 0x272 | d0 ver, a0 list | **FreeBoards** | Frees the list and its strings. |
| -54 | 0x2a4 | d0 ver, a0 = MNIOpen * | **Open handle** | See 4.4. Returns 0 and stores the handle in MNIOpen+0x1c. Several handles per board are allowed. |
| -60 | 0x2d8 | a0 handle | **Close handle** | Goes offline first if needed. |
| -66 | 0x2f2 | a0 h, a1 out6, a2 out6 | **GetStationAddress** | a1 = factory address (board+0x126), a2 = current address (board+0x12c). NULL is allowed for either. |
| -72 | 0x312 | a0 h, a1 addr6 | **SetStationAddress** | Reprograms the chip if it is running. |
| -78 / -84 | 0x3b8 / 0x3d4 | a0 h, a1 addr6 | **Add / Remove multicast address** | Uses a 64-bit hash filter (handle+0x40, board+0x740). |
| -90 / -96 | 0x3f0 / 0x410 | a0 h, a1 lo6, a2 hi6 | **Add / Remove multicast range** | MiamiDx h0+0x2f3e2 / 0x2f41a. |
| -102 | 0x44a | a0 h, a1 MNIPacket * | **Send** | Returns 1 if the driver took the packet (completion arrives through callback 3), 0 if the hardware refused it (the caller keeps ownership), -9 if the handle is not online. Also loops a copy back to other online handles on the same board that have flag bit2 set (see -138). |
| -108 | 0x32e | a0 h, a1 buf | **GetStats** | Copies 0x70 bytes of board statistics (board+0xb2). |
| -114 | 0x430 | a0 h | **Online** | Starts the hardware on the first online handle and applies the handle's multicast and flag state. Nonzero means failure. |
| -120 | 0x39e | a0 h | **Offline** | Aborts the handle's pending transmits with status -7 through callback 3. |
| -126 / -132 | 0x34a / 0x366 | a0 h, d0 type | **Add / Remove packet type** | Per-handle ethertype filter (array at handle+0x10, count +0x14, capacity +0x18). Adding a duplicate returns -5. |
| -138 | 0x382 | a0 h, d0 flags | **SetFlags** | bit0 = promiscuous (smc: sets PRMS in RCR at h0+0x1e9e) [read/inf]. bit1 = all-multicast [inf]. bit2 = receive copies of frames other handles send on this board [inf]. |

### 4.4 Structures [read; field names are mine]

```
MNIOpen (stack struct, about 0x20 bytes)
 +0x00 ULONG unit              board index from FindBoards
 +0x04 ULONG reserved (0)
 +0x08 APTR  cb_getbuf         int  cb(MNIPacket *)  rx: supply a buffer
 +0x0c APTR  cb_rxdone         void cb(MNIPacket *)  rx: frame delivered
 +0x10 APTR  cb_txdone         void cb(MNIPacket *)  tx: packet returned
 +0x14 APTR  userdata          copied into every MNIPacket+0x34 the driver hands back
 +0x18 STRPTR options          e.g. "FT=2 MEDIA=AUTO"
 +0x1c APTR  handle (out)

MNIPacket (descriptor; sanamni uses 0x50 bytes header + 1500 data)
 +0x10 MNIFrag *frags          fragment chain (see below)
 +0x14 UBYTE addr[6]           tx: destination; rx: source
 +0x24 ULONG flags             bit1 (=2): tx broadcast (driver fills ff:ff:ff:ff:ff:ff); rx: broadcast/multicast indication
 +0x28 ULONG type              ethertype
 +0x2c LONG  status            driver-written error, e.g. -7 aborted
 +0x30 ULONG length            payload length
 +0x34 APTR  userdata          set by driver from MNIOpen+0x14 before each callback
 +0x38 ...   stack-private     (MiamiDx keeps its buffer cookie here)
 +0x3c APTR  owner handle      written by driver on Send
 +0x40 UBYTE srcaddr[6]        tx: driver writes station address

MNIFrag
 +0x00 MNIFrag *next   +0x04 reserved   +0x08 APTR data   +0x0c ULONG len
```

**Callback contract** [read]:

* Callbacks are ordinary C functions with **one stack argument** (`cb(pkt)`, argument at 4(sp)). Registers are not guaranteed, so a4 must be reloaded (MiamiDx's callbacks at h0+0x2e966/0x2e9ce/0x2ea52 each start with `lea $8000.l,a4`).
* They may run in the driver's interrupt or soft-interrupt context. Both clients only do list operations under Disable or Forbid plus `Signal()` inside them.
* `cb_getbuf`: the client must set pkt+0x10 to a fragment that has room for pkt+0x30 bytes and return 0, or return <0 to drop the frame (sanamni returns -3 and counts an overrun).
* `cb_rxdone`: the frame is complete; read pkt+0x14/0x24/0x28/0x30.
* `cb_txdone`: ownership of a sent descriptor returns to the client.

### 4.5 How MiamiDx drives MNI [read, h0+0x2eab0..0x2f960]

1. Pre-allocates 32 receive buffers (descriptor 0x2e bytes, `AllocVec(0x800)` buffer) at h0+0x2eb80.
2. `OpenLibrary`, then GetInfo with version 3 (h0+0x2ec0a). The returned API version is kept at +0x484a.
3. Open(-54) with the three callbacks h0+0x2e966/0x2e9ce/0x2ea52, userdata = the per-hardware struct, options at +0x46e2. The handle is kept at +0x4856.
4. Call sites for every other entry point are at h0+0x2ecc0..0x2f452 [read]: GetStationAddress (-66), SetStationAddress (-72), Online (-114), Offline (-120), Send (-102, at 0x2ef64 and 0x2f8aa), Add/Remove packet type (-126/-132, presumably for IP, ARP and RARP [inf]), multicast (-78/-84/-90/-96), SetFlags (-138, presumably promiscuous mode for TCPDump [inf]). No call to GetStats (-108) was found in this code range.
5. In "direct" mode (+0x48b9), the receive callback allocates an mbuf-cluster fragment straight from the stack (h0+0x2f8d8), so frames land in mbufs without a copy. The receive-done callback calls the link input h0+0x2f26e(type, buffer, length) and signals the hardware process.

### 4.6 sanamni.device [read + guide]

* RomTag type NT_DEVICE, v1. It refuses to run unless MiamiDx is installed with key files (that check is not analysed).
* Configuration: `ENV:SANA2/sanamni<unit>.config`, parsed with the template `MNINAME/A,MNIUNIT/N,MNIOPTIONS` (h0+0x186/0x1a2).
* It opens the MNI driver exactly as in 4.5 (h0+0x16a8):
  * GetInfo with version 2, then Open with callbacks 0x1a78 (getbuf), 0x1aca (rxdone) and 0x1b2e (txdone), then Online.
  * Packet types requested by SANA-II readers are mapped to MNI Add/Remove packet type (h0+0x1450/0x1492). The type is added only when no other opener already holds it [inf].
  * S2_GETSTATIONADDRESS uses -66 (h0+0xfc6). CMD_WRITE and broadcast use -102 (h0+0x1a00).
  * DEVICEQUERY returns a fixed template.
* Packet types 2048, 2054 and 32821 are rejected so they cannot collide with MiamiDx on the same board (the message at h0+0xc80).
* Per the guide it does not implement multicast, statistics, online/offline events or most notifications.

### 4.7 Recommendation for the new stack

The MNI ABI is small, versioned and fully recoverable (19 functions plus three callbacks), and it supports several handles per board with per-handle ethertype filters. **Loading `.mni` drivers directly is feasible.** Implement GetInfo → Open → Online → AddType, the three callbacks and Send. You get zero-copy receive into your own buffers and promiscuous mode for free.

Keep the generic SANA-II path as a fallback for boards that have no MNI driver. Do not route through `sanamni.device`:

* it is locked to a registered MiamiDx installation;
* it blocks exactly the IP, ARP and RARP types you need;
* it adds a copy;
* its SANA-II emulation is incomplete [guide].

---

## 5. Network core function map (h0 offsets)

Primary evidence is the **inetsw protosw table at h4+0x4728**: 11 entries of 0x32 bytes, `{short type; domain *; short proto; short flags; 10 function pointers}`, with the domain at h4+0x4a48 [read+conf]. A duplicate table sits at h4+0x491c. The routing-socket protosw is at h4+0x4e48.. and the unix-domain protosw at h4+0x4384...

| Proto | input | output | ctlinput | ctloutput | usrreq | init | fasttimo | slowtimo | drain | sysctl |
|---|---|---|---|---|---|---|---|---|---|---|
| IP (0) | – | 0x5741c | – | – | – | 0x54d90 | – | 0x5574e | 0x557a8 | 0x55f08 |
| UDP (17) | 0x4a146 | – | 0x4a5ce | 0x56b1a | 0x4a832 | 0x4a114 | – | – | – | 0x4aa70 |
| TCP (6), flags 0x802c | 0x470ac | – | 0x6438c | 0x49ba8 | 0x494fc | 0x63810 | 0x49154 | 0x491a6 | 0x642ec | 0x49efc |
| RAW (255) | 0x4ac3e | 0x4ad7c | – | 0x4aec0 | 0x4b0f0 | | | | | |
| ICMP (1) | 0x5889e | rip | | rip | rip | | | | | 0x58f6a |
| IGMP (2) | 0x65562 | rip | | rip | rip | 0x65544 | 0x65bbe | 0x65c98 | – | 0x65e20 |
| RSVP (46) | 0x636ea | rip | | rip | rip | | | | | |
| IPIP (4) | 0x6309c | rip | | rip | rip | | | | | |
| DIVERT (254) | 0x574c0 | – | – | 0x56b1a | 0x57642 | 0x57460 | | | | |
| RAW default (0) | 0x4ac3e | 0x4ad7c | | 0x4aec0 | 0x4b0f0 | 0x4abf4 | | | | |

The table gives tcp_input, udp_input, icmp_input, igmp_input, rip_*, div_*, tcp_ctlinput, tcp_ctloutput, tcp_usrreq, ip_ctloutput, tcp_fasttimo, tcp_slowtimo, tcp_drain and tcp_sysctl directly.

Other functions:

| Function | Address | Evidence |
|---|---|---|
| ip_output (wrapper used as pr_output) | 0x5741c | Pushes an extra argument 1 and calls 0x5610c [read]. |
| ip_output (body) | 0x5610c | Uses ip_id (h4+0x3d78, `addq.w #1`) at 0x5628c; ipstat counters [read/inf]. |
| ipintr / ip_input | about 0x54ef2 | Follows ip_init; increments many ipstat fields (h4+0x3d44..0x3d74) [inf]. |
| ip_forward | about 0x55cea | ipstat forward/cantforward/redirect fields; calls ip_output [inf]. |
| ip_dooptions / ip_reass | about 0x557da / 0x5542c | [inf] |
| ip_fw (firewall) | 0x581b6..0x58640 | String "ip_fw_init" [read]. |
| icmp_error, icmp_reflect | 0x586a4, about 0x58cd4 | Strings "icmp_error" and "icmp len" [read]. |
| in_cksum | 0x54828 (mbuf walker) + unrolled `addx.l` core about 0x548c8..0x54966 | 16 `addx.l` in one routine [read]. |
| in_control (SIOC*) | about 0x52f1c | String "in_control" [read]. |
| in_pcb* | 0x53b28..0x54828 | Used from tcp/udp usrreq [inf]. |
| tcp_reass | 0x46e14 | Called twice from tcp_input; reassembly counters [inf]. |
| tcp_dooptions | 0x489b0 | Switch on option kinds 2..13 [read] (section 5.1). |
| tcp_pulloutofband | 0x48c54 | References the panic string at h0+0x47000 [read]. |
| tcp_xmit_timer | 0x48ccc | tcpstat rttupdated; 4 callers in tcp_input [inf]. |
| tcp_mss | 0x48d9c | [inf] |
| tcp_timers | 0x49292 | Switch REXMT/PERSIST/KEEP/2MSL; uses tcp_backoff [read] |
| tcp_init | 0x63810 | protosw; panic "tcp_init" [read]. |
| tcp_newtcpcb | 0x63b34 | `move.l #$3fffc000` (TCP_MAXWIN << 14) at 0x63bdc [read]. |
| tcp_respond / tcp_template | about 0x6393a / about 0x63888 | [inf] |
| tcp_drop | 0x640fe | tcpstat drops/conndrops, then tcp_close [inf]. |
| tcp_close | 0x6415c (reached through stub 0x4908e) | Frees the tcpcb and calls in_pcbdetach [read/inf]. |
| SACK scoreboard update | 0x63df0 | Called for each SACK block from tcp_dooptions [read]. |
| tcp_output | 0x648b0 | Panic "tcp_output"; calls m_copym/m_copydata, ip_output 0x5741c and tcp_setpersist [read]. |
| tcp_setpersist | 0x654cc | Panic "tcp_output REXMT"; uses tcp_backoff [read]. |
| udp_* | 0x4a114..0x4aa70 | protosw [conf]. |
| mbinit | 0x4b96c | String "mbinit" [read]. |
| m_get / m_gethdr | 0x4ba06 / 0x4ba70 | mbstat type counters; 13 and 14 callers [inf]. |
| m_free / m_freem | 0x4bc6e / 0x4bcf0 | 57 and 109 callers [inf]. |
| m_copym / m_copydata | 0x4be46 / 0x4c004 | Panic strings [read]. |
| m_pullup | 0x4c1c0 | Called with 0x14 from div_input [read/inf]. |
| socreate / soclose / soaccept / soconnect / soconnect2 / sofree | 0x4c5c2 / 0x4c846 / 0x4c97e / 0x4c9d6 / 0x4ca48 / 0x4c732 | Panics "soclose: NOFDREF", "soaccept: !NOFDREF" and "sofree dq"; socketpair uses them [read]. |
| sosend / soreceive | about 0x4caec / about 0x4d380 (variants 0x4d04a / 0x4db82) | Size, mbuf use and callers [inf]. |
| sb* (uipc_socket2) | 0x4e5a0..0x4f20e | Strings "sbappendaddr", "sbflush", "sbdrop panic: …" [read]. |
| Unix-domain sockets | 0x4f340.. | Strings "unp_attach", "unp_connect2", "uipc 1-4" [read]. |
| select core / WaitSelect | 0x5d9cc / 0x5b296 | [read] |
| Interfaces: loopback attach / looutput | 0x50c04 / 0x50cbc | Strings "lo", "looutput no HDR" [read]. |
| Kernel interface records | array h4+0x4468, count h4+0x446c; name→index 0x5288c | [read] |
| ARP (arpresolve, in_arpinput, arptimer, arptfree) | 0x5e2ae..0x5ec9e | String "arptfree" at h0+0x5e44a [read/inf]. |
| Routing: rtalloc / rtfree / ifafree / rtrequest / route_output / rt_msg1 / rn_init | 0x65f12 / 0x66004 / 0x660ce / 0x66334 / 0x66d60 / 0x67482 / 0x68ada | Panic strings and routesw [read/conf]. |
| Multicast routing (mrouted kernel side) | 0x6168a..0x631ca | mrtstat h4+0xbf70, mfctable h4+0xbfa4, viftable h4+0xc4a4 [read/inf]. |
| PPP | Protocol demux 0x38e28 (LCP 0xc021, IPCP 0x8021, CCP 0x80fd, PAP 0xc023, CHAP 0xc223) and 0x39a76; "ppp: received ip packet" in 0x38b82 [read]. Logs mention VJC, MPPC/MPPE, MS-CHAP(v2) and CBCP. | |
| MNI receive path / SANA-II receive path | 0x2f26e (type switch 0x2f326) / 0x36066 | [read] |
| Resolver | res_init 0x6c794, res_mkquery 0x6c008, res_send 0x6b6c0, res_query 0x6c210, res_querydomain 0x6c300, res_search 0x6c3f4, PTR name builder 0x6a17a ("in-addr.arpa"/"ip6.int") | [read] |
| sysctl dispatch | MiamiSysCtl 0x5f200 → 0x5f0f0 (name[0]==4, CTL_NET) → pr_sysctl | [read] |
| kvm symbol table | names h4+0x4d0c, values h4+0x4ca4 | [read] |

### 5.1 TCP features

* **RFC 1323** [read+guide]. tcp_dooptions handles WSCALE (3, length 3; sets t_flags and requested_s_scale at tcpcb+0x7f) and TIMESTAMP (8, length 10; ts_recent at tcpcb+0x80, ts_recent_age at +0x84 = tcp_now h4+0x3fb0). The sysctl default rfc1323 is 0; the guide says the extensions are switched on per interface automatically.
* **RFC 1644 T/TCP** [read]. CC, CCNEW and CCECHO (11, 12, 13; length 6) are parsed, gated by h4+0x2954. TCP protosw flags 0x802c. sysctl rfc1644 maps to h4+0x4e04, and the GUI default is 1.
* **SACK** [read]. SACK-permitted (4) and SACK blocks (5; 8-byte edges, scoreboard 0x63df0) are parsed when tcpcb+0xaa is set. sysctl `net.inet.tcp.sack` is a bitmask with default 208 = 0x10|0x40|0x80. The guide maps this to "SACK, NewReno, modified fast retransmit"; 0x20 is presumably TSACK [inf].
* **Congestion control**: 4.4BSD Reno (slow start, congestion avoidance, fast retransmit and recovery with tcprexmtthresh = 3 at h4+0x4040 [inf]), plus **NewReno** and SACK recovery per the bitmask. **initwin** (h4+0x4048, default 1 segment) sets the initial window. There is no CUBIC, ECN, PRR or RACK.
* Timers: 4.4BSD slow (500 ms) and fast (200 ms) ticks. keepidle = 14400 half-seconds (2 h) and keepintvl = 150 (75 s) at h4+0x4050/0x4054. tcp_backoff[] = {1,2,4,8,16,32,64,64,…} at h4+0x4060 [read].
* Other: **fastlocal** (h4+0x404c) is a localhost shortcut. Path-MTU discovery is on by default. Default mssdflt is 512 (h4+0x4df4). send/recvspace are 16384 (h4+0x4098/0x409c). VJ header compression (VJC) is negotiable on PPP and CSLIP [guide/strings]. There is no IPv6 (MiamiSupportsIPV6 returns 0).

TCP sysctl jump table at h0+0x49f3c (MIB → variable) [read]: 1 rfc1323 h4+0x4dfc, 2 rfc1644 h4+0x4e04, 3 mssdflt h4+0x4df4, 4 stats (unsupported), 5 rttdflt h4+0x4df8, 6 keepidle h4+0x4050, 7 keepintvl h4+0x4054, 8 sendspace h4+0x4098, 9 recvspace h4+0x409c, 10 bulkftp h4+0x4e0c, 11 initwin h4+0x4048, 12 fastlocal h4+0x404c, 13 sack h4+0x4e00. MIBs 11..13 are matched to names by the order of the default table [inf].

### 5.2 Globals (h4 offsets; a4 displacement = offset − 0x8000)

| Global | h4 offset | Source |
|---|---|---|
| ifnet (list head) | 0x3d08 | kvm table [conf] |
| ipstat | 0x3d14 | kvm table |
| ip_id (u_short) | 0x3d78 | `addq.w` in ip_output and rip_output [read] |
| rtstat | 0x3d7a | kvm |
| icmpstat | 0x3d84 | kvm |
| tcpstat | 0x3e40 (0x11c bytes; ordering differs from 4.4-Lite) | kvm |
| udpstat | 0x3f5c | kvm |
| tcp_now | 0x3fb0 | tcp_dooptions [read] |
| tcprexmtthresh? / initwin / fastlocal / keepidle / keepintvl / keepcnt(8) / maxpersistidle | 0x4040 / 0x4048 / 0x404c / 0x4050 / 0x4054 / 0x4058 / 0x405c | [read/inf] |
| tcp_backoff[13] | 0x4060 | [read] |
| tcp_sendspace / tcp_recvspace | 0x4098 / 0x409c | [read] |
| mbstat | 0x4124 | kvm |
| inetsw[] / inetdomain | 0x4728 / 0x4a48 | [read] |
| Kernel interface array / count | 0x4468 / 0x446c | [read] |
| tcp_mssdflt, tcp_rttdflt, tcp_do_rfc1323, sack mask, tcp_do_rfc1644, bulkftp | 0x4df4, 0x4df8, 0x4dfc, 0x4e00, 0x4e04, 0x4e0c | tcp_sysctl [read] |
| IP-NAT module pointer | 0x4d7c | miami -366/-372 |
| rt_tables | 0x9718 | kvm |
| tcb / udb (inpcb heads) | 0x97a0 / 0x97c0 | kvm |
| mrtstat / mfctable / viftable | 0xbf70 / 0xbfa4 / 0xc4a4 | kvm |
| igmpstat / locallist | 0xe4f0 / 0xe780 | kvm |
| hostname | 0x2988 | uname [read] |
| feature flag | 0x2954 | Many gates; not analysed |
| per-task ctx list | 0x8ac4 | [read] |
| big kernel lock (semaphore / owner / depth) | 0x8ad8 / 0x8ad4 / 0x3ce8 | [read] |
| spl semaphore / owner / level | 0x7cd0 / 0x2960 / 0x296c | [read] |
| SysBase / DOSBase / UtilityBase | 0x556c / 0x5568 / 0x1316 | [read] |

---

## 6. Sysctl tunables (defaults from h0+0xf184..0xf5b2; meanings per NODE_CONFIG_DATABASE_SYSCTL [guide])

| Name | Default | Meaning |
|---|---|---|
| net.inet.ip.forwarding | 1 | Act as a router (tied to the "TCP/IP Gateway" switch). |
| net.inet.ip.redirect | 1 | Send ICMP redirects. |
| net.inet.ip.ttl | 64 | Default TTL. |
| net.inet.ip.rtexpire / rtminexpire / rtmaxcache | 3600 / 10 / 128 | Cloned-route expiry and cache size. |
| net.inet.ip.sourceroute | 1 | Accept or forward source-routed packets (GUI-linked). |
| net.inet.ip.pathmtudisc | 1 | Path-MTU discovery. |
| net.inet.ip.portrange.lowfirst / lowlast | 1023 / 600 | Reserved (privileged) ephemeral range. |
| net.inet.ip.portrange.deffirst / deflast | 1024 / 5000 | Default ephemeral range. |
| net.inet.ip.portrange.hifirst / hilast | 49152 / 65535 | IANA ephemeral range. |
| net.inet.icmp.maskrepl | 0 | Answer ICMP mask requests. |
| net.inet.tcp.rfc1323 | 0 | Keep 0; window scaling and timestamps are enabled per interface automatically. |
| net.inet.tcp.rfc1644 | 1 | T/TCP (GUI-linked). |
| net.inet.tcp.mssdflt | 512 | Default MSS when PMTU information is unavailable. |
| net.inet.tcp.rttdflt | 1 | Initial RTT estimate; do not change. |
| net.inet.tcp.keepidle / keepintvl | 14400 / 150 | Keepalive idle time and interval in 0.5 s ticks (2 h / 75 s). |
| net.inet.tcp.sendspace / recvspace | 16384 / 16384 | Default socket buffers, which are also the window size. |
| net.inet.tcp.bulkftp | 0 | Reserved, non-functional. |
| net.inet.tcp.initwin | 1 | Initial congestion window in segments (2–3 suggested). |
| net.inet.tcp.fastlocal | 1 | Localhost fast path. |
| net.inet.tcp.sack | 208 | Bitmask: SACK + NewReno + modified fast retransmit. |
| net.inet.udp.checksum | 1 | Send UDP checksums. |
| net.inet.udp.maxdgram / recvspace | 9216 / 41600 | UDP limits. |
| dns.cache.size / flush / enabled / split | 32 / 0 / 2 / 0 | Built-in DNS cache: size; set flush to 1 to flush; enabled 2 = cache all, 1 = single-address entries only, 0 = off; split = separate forward and reverse caches. |
| inetd.retrytime / toomany / cntintvl / maxbuiltin / processpri / diagbufsize / diagtimeout | 30 / 10 / 60 / 20 / -5 / 4096 / 30 | Internal inetd tuning. |
| socket.maxqlen | 7 | Accept backlog used for listen(…, 5). |
| sys.maxmbufs | 2048 | Maximum number of 128-byte mbufs. |

The TCP group maps to MIBs 1..13 (5.1). The `dns.*`, `inetd.*`, `socket.*` and `sys.*` entries are MiamiDx-level settings stored in the `DBSY` chunk [inf].

---

## 7. Limitations of the original stack compared with a modern Linux TCP

1. **No IPv6.** MiamiSupportsIPV6 returns 0, and AF_INET6 is used only in API shims (inet_pton/getaddrinfo strings, "ip6.int").
2. **Small, static windows.** 16 KB send and receive buffers by default. RFC 1323 scaling is off by default and only enabled heuristically per interface. There is no receive-buffer auto-tuning, unlike Linux's tcp_rmem/tcp_wmem auto-tuning.
3. **Old congestion control.** Reno and NewReno with an optional early SACK. The initial window is 1 segment, against IW10 on Linux. There is no CUBIC or BBR, ECN, PRR, RACK-TLP, F-RTO, pacing or HyStart.
4. **Coarse timers.** 500 ms and 200 ms BSD ticks give a minimum RTO of about 1 s and coarse RTT sampling. Linux uses jiffy or high-resolution timers, RTO_min 200 ms and per-route RTT caching.
5. **T/TCP (RFC 1644)** is enabled by default. It is obsolete and has known spoofing weaknesses.
6. **Concurrency.** One recursive semaphore serialises the whole kernel, and protocol code runs on whichever client task makes the call. spl is emulated by another semaphore. There is no SMP and no per-socket locking, and priority inversion with client tasks is possible.
7. **Buffers.** 128-byte mbufs with 2 KB clusters, a hard cap of 2048 mbufs (`sys.maxmbufs`), and copies at the SANA-II boundary (MNI direct mode avoids copies on receive only). There is no GSO/GRO, no checksum offload and no scatter-gather transmit beyond the MNI fragment chain.
8. **Listen queue.** `socket.maxqlen` is 7, with no SYN cookies and no accept-queue protection. The default ephemeral port range of 1024–5000 is tiny and predictable.
9. **Security posture.** BIND 4.9-era resolver with no DNSSEC, query-ID randomisation not verified, and an internal inetd with diagnostic services. Passwords use iterated MD5. The DES-free design is explained in the guide.
10. **Platform coupling.** 68020 code, a per-opener library base model, several undocumented private LVOs that bundled tools depend on (section 3.3), an undocumented IFF settings format, and a single-stack guard against other TCP stacks.
11. **Feature gaps vs Linux.** No IPv6, no TCP Fast Open (T/TCP is the old equivalent), no MPTCP, no TCP_INFO-class introspection (only kvm-style struct reads), no netfilter-grade connection tracking (the NAT is a separate library with divert sockets), and no modern PMTU black-hole detection.

What is worth preserving in a reimplementation:

* the AmiTCP bsdsocket ABI (section 3.1) and usergroup ABI (3.2);
* the documented miami.library subset plus the undocumented resolver, kvm, socketpair, uname and DB entries that the bundled tools call (3.3);
* the MNI driver ABI (4), so existing `.mni` Ethernet drivers keep working.
