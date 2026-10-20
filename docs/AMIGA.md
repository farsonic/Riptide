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
`usergroup.library` is already in the system library list. In that case it
prints "A TCP/IP stack is already running." Quit MiamiDx, AmiTCP or
Roadshow first.

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
| `STATUS` | Ask the running Riptide for a summary: interfaces, addresses, packet counters, driver state and library users. |
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

These are handled by the protocol core.

```
hostname NAME
domain NAME                        # also the DNS search domain
nameserver A.B.C.D                 # up to the core's limit, in order
host NAME A.B.C.D                  # static host table entry
route add|del DEST[/BITS]|default GATEWAY
sysctl NAME=VALUE                  # e.g. sysctl net.inet.tcp.sack=1
ifconfig IFNAME A.B.C.D[/BITS] [peer A.B.C.D] [mtu N] [up|down]
dhcp IFNAME                        # run the DHCP client on IFNAME
```

### Amiga keywords

```
interface NAME device=DEVICE [unit=N] [mtu=N] [raw]
interface NAME mni=DRIVER [unit=N] [options="TEXT"] [mtu=N]
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

### Hosts file

`ENV:Riptide/hosts` uses the usual format, `A.B.C.D name [alias...]`, with
`#` comments. Its entries go into the static host table, which is checked
before DNS.

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
| `tests/amiga/run_fsuae.sh build/amiga` | Full system test. It boots FS-UAE (A1200 with the built-in AROS Kickstart, headless through Xvfb) from a directory hard drive whose Startup-Sequence runs `rtcheck`. `rtcheck` starts Riptide, runs the checks against the live stack with real multitasking, and then checks that `QUIT` is refused while the libraries are open and accepted after they are closed. The live checks include blocking connect, WaitSelect timeouts through timer.device, Ctrl-C → EINTR, and extra signals. |

`rtcheck CONNECT=a.b.c.d:port` also runs a TCP echo test through a real
interface.
