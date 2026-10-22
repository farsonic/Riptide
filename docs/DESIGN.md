# Riptide — design notes

Riptide is a clean-room TCP/IP stack for AmigaOS (68020+). It replaces a
historical commercial Amiga stack with a 4.4BSD-Lite-derived kernel, whose
binary interfaces we reverse engineered (see `RE-NOTES.md`). Riptide keeps
those *binary interfaces*, so existing Amiga programs keep working. It
publishes `bsdsocket.library` (AmiTCP 4 API, 46 LVOs), its own
`riptide.library` (version 1; the extended API of the historical stack, with
the same vector layout, also published under that stack's library name as a
switchable compat alias) and `usergroup.library`. Underneath is a new
protocol core whose TCP follows the modern Linux TCP stack.
`COMPARISON.md` compares Riptide with the historical stack and with
Roadshow.

Licence: GPL-2.0-only (the TCP follows Linux's algorithms and structure).

## Source layout

| Path | Contents | Owner |
|---|---|---|
| `include/rt/*.h` | Contract headers shared by every module | integration |
| `src/core/` | Portable protocol core (freestanding C99) | see below |
| `src/host/` | Linux host OS layer: pthreads, TUN, simulated wire | infra |
| `src/arch/m68k/` | 680x0 assembly checksum and copy routines (vasm, Motorola syntax) | infra |
| `src/amiga/` | AmigaOS glue: libraries, SANA-II, MNI, startup, OS headers | amiga |
| `sdk/` | Developer SDK for riptide.library (generated FD and headers) | amiga |
| `src/amiga/cmd/` | Network tools (ip, ifconfig, route, ...) and their shared `cmdlib` | amiga |
| `src/apps/` | Portable applications with a thin OS shim per platform: `speedtest` (shims: `src/amiga/cmd/speedtest.c`, `st_posix.c` for Linux, the TUN test for the host stack); core rules apply, except that `st_posix.c` uses libc | amiga |
| `tests/` | Host tests (`make check`), interop with Linux via TUN | everybody |
| `tools/` | RE scripts (hunk parser etc.), `elf2hunk.py` | — |

The core files are owned per area:

* **infra**: `buf.c timer.c printf.c netif.c arp.c ip.c ip_frag.c route.c
  icmp.c udp.c raw.c loopback.c checksum.c arch.c init.c stats.c capture.c`
  (packet capture: every frame passes `rt_netif_input()` or `rt_netif_xmit()`;
  protocol code never calls `nif->link_output()` directly)
* **tcp**: `tcp*.c` plus internal `src/core/tcp.h`
* **sockets**: `socket.c socket_api.c ioctl_net.c sysctl.c config.c ctl.c resolv.c netdb.c dhcp.c errno.c`
* **ipv6**: `inet.c inet6_text.c ip6.c ip6_if.c ip6_frag.c nd6.c route6.c icmp6.c
  dhcp6.c` plus internal `src/core/ip6_internal.h` (see "IPv6" below)

## Rules for core code

* C99 (gnu99). From libc, use only `memcpy memmove memset memcmp strlen
  strcmp strncmp strchr` (`<string.h>`) plus the `rt_*` helpers in
  `rt_types.h`. No stdio, no malloc (use `rt_malloc`), no floating point
  (68020 machines often have no FPU; CUBIC uses an integer cube root as
  Linux does), no recursion.
* Library calls run on the *caller's* stack, and Amiga programs often have
  only 4 KB. Keep stack frames small: no big local arrays (<= 256 bytes per
  frame) and a shallow call depth.
* Big-endian (m68k) and little-endian (host tests) both have to work. Use
  `rt_htons/rt_ntohl`, `rt_get_be16/32` and `rt_put_be16/32`, and access
  option bytes with byte or memcpy accessors.
* Every protocol function runs with the single stack lock held (see
  `rt_os.h`). Socket-layer API functions take the lock themselves. Timer
  callbacks and frame input run in the stack thread, also under the lock.
* Errors are negative errno values (`-RT_EWOULDBLOCK`), with BSD numbering.
* Time: `rt_now_ms()` for timers, and `rt_now_us()` for RTT samples.
* Diagnostics (`rt_diag.h`): `RT_DBG(RT_DBG_TCP, ...)` logs to the debug
  ring when that category is on, and costs one test otherwise.
  `RT_PROF_ENTER/EXIT` bracket a profiler section. Sections run under the
  lock and must not span a sleep (`rt_unlock()` closes what is still open);
  code outside the lock (driver callbacks) uses the `RT_PROF_T0/ADD`
  counters. `-DRT_NO_DIAG` compiles all of it out.
* Every file starts with `/* ... SPDX-License-Identifier: GPL-2.0-only */`.
* Keep comments purposeful. Name Linux functions or RFC sections where an
  algorithm follows them (`/* RFC 8985 6.2: RACK detect loss */`).

## Arch ops: checksum and copy routines

The hot loops go through function pointers in `rt_arch`
(`include/rt/rt_arch.h`), so one binary can pick CPU specific code at run
time:

| Op | Used for | C version | 680x0 version |
|---|---|---|---|
| `csum` | `rt_csum_partial()`: every checksum | `rt_csum_partial_c` | `rt_csum_020` |
| `csum_copy` | `rt_csum_copy()`: copy and checksum in one pass (Linux `csum_partial_copy`) | `rt_csum_copy_c` | `rt_csum_copy_020` |
| `copy` | `rt_copy()`: payload between user or driver memory and packet buffers | `rt_copy_c` | `rt_copy_020` |
| `copy_buf` | `rt_copy_buf()`: packet buffer to packet buffer | `rt_copy_c` | `rt_copy_020`, or `rt_copy_040` (MOVE16) |

* `rt_copy()` and `rt_copy_buf()` call plain `memcpy()` below
  `RT_COPY_MIN` (128) bytes; the dispatch only pays off above it.
* Where they are used: TCP transmit copies the write-queue segment and sums
  its payload in one pass (`rt_buf_copy_csum()`), then adds the header sum;
  UDP send copies from the user's iov and sums in one pass
  (`rt_iov_to_buf_csum()`); the SANA-II `CopyToBuff` callback copies and
  sums each received frame in one pass (`RT_BUFF_CSUM_PART`, see "Receive
  path"); `send()`/`recv()` copies (`rt_iov_to_buf()`, `rt_buf_to_iov()`)
  and `CopyFromBuff` use `rt_copy()`; receive-queue coalescing and
  collapsing, `tcp_fragment`, IP reassembly/fragmentation and
  raw/broadcast clones use `rt_copy_buf()`. Received data is copied to the
  user with `rt_copy()` only: its checksum was verified at input.
* Headers (IPv4 headers, TCP headers of the segments we build) are summed
  with `rt_csum_hdr()`: 16-bit adds inline, no call into `rt_arch`. For
  20 to 60 bytes the bulk routine's call and set-up cost more than the
  loop.
* Checksum contract: a 32-bit partial sum whose `rt_csum_fold()` equals the
  C version's (implementations may return different but congruent 32-bit
  values, so compare only folded sums). Pieces at odd offsets are chained
  with `rt_csum_block_add()`.
* Every routine must be reentrant (the SANA-II callbacks run in driver
  interrupts) and use no 64-bit `mulu.l`/`divu.l` (the 68060 traps them).
* Packet buffer data starts on a 16-byte boundary (`buf.c`), so copies
  between buffers with equal headroom can use whole cache lines.
* The OS layer sets `rt_arch_cpu` before `rt_init()`, which selects
  `auto`; `cpu routines generic|020|040|060` (configuration) overrides it.
  Builds with `RT_ARCH_M68K_ASM` (the Amiga builds, and
  `make CROSS=m68k-linux-gnu-`) link `src/arch/m68k/rt_m68k.s`; the
  native host build uses C only. `tests/t_arch_m68k.c` tests every
  routine against the C versions (see `docs/AMIGA.md`, CPU support).
* Per-packet code avoids 64-bit multiply/divide where a 32-bit form is
  enough (clocks, RTO arithmetic): the compilers' 64-bit helpers use the
  64-bit `mulu.l`/`divu.l` forms, which the 68060 emulates in software.

## Builds

* `make`: native build plus tests in `build/host/`.
* `make SAN=1`: the same, with ASan and UBSan.
* `make CROSS=m68k-linux-gnu-`: big-endian 68020 static Linux binaries. Run
  them under `qemu-m68k` to test the core on the real target CPU and
  endianness. This build links the 680x0 assembly routines (vasm from
  `$VBCC`; `ASM=0` leaves them out). `RT_CPU=68040` (with
  `QEMU_CPU=m68040`) makes it select the 68040 routines.
* `make -f Makefile.amiga`: AmigaOS hunk executable `Riptide`, built with
  vbcc (`vc +aos68k -c99`, plus vasm and vlink built from source) against
  the free NDK 3.2 headers from Aminet. Expected locations are
  `VBCC=/opt/vbcc` and `/opt/ndk3.2`. Core code must therefore be plain C99
  with no GNU extensions.
* Amiga testing is done in two layers. `vamos Riptide SELFTEST` (amitools)
  checks logic quickly. A full-system run uses FS-UAE with its built-in free
  AROS Kickstart ROM, on an emulated A1200/68020 under Xvfb, with a Linux
  folder mounted as DH0:. Do a final check on a real ROM or real hardware.
* `tests/run_tests.sh <builddir>` runs every test. Tests that need TUN (root
  and `/dev/net/tun`) skip themselves cleanly when it is unavailable.

## Runtime architecture (AmigaOS)

```
 client tasks ──OpenLibrary("bsdsocket.library")──► per-opener SocketBase
      │  socket()/send()/WaitSelect() run in the client task:
      │  ObtainSemaphore(stack lock) → core → (sleep: Wait() on own signal)
      ▼
 ┌──────────────── Riptide process ────────────────┐
 │ timer.device (next deadline) + SANA-II replies   │
 │ → rt_netif_input() / rt_stack_poll() under lock  │
 └──────────────────────────────────────────────────┘
```

The host build mirrors this. A stack thread polls TUN or wire file
descriptors and runs timers, and test threads call `rt_api_*` directly.

### Receive path

On a 68020 a bulk download is CPU bound: the reader (nc, an FTP client)
gets what Riptide leaves of the CPU. What a received segment costs, and
what was done about it (`tests/t_rx_bench.c` counts instructions per
segment under `qemu-m68k`; the profiler and `Riptide BENCH` measure on
FS-UAE):

* **No copy, or one pass.** A driver that offers SANA-II R3 DMA
  (`S2_DMACopyToBuff32`/`S2_DMACopyFromBuff32`) gets the address of the
  packet buffer and moves the frame itself; receive buffers are 32-bit
  aligned and hold the largest frame, so the callback only refuses a
  buffer that is not (it returns NULL and the driver copies through
  `CopyToBuff`). Otherwise `CopyToBuff` copies and sums the frame in one
  pass and sets `RT_BUFF_CSUM_PART` with the sum in `b->csum`. IPv4 keeps
  that sum when it pulls its header (a valid header sums to zero); IPv6
  subtracts the sum of its headers; `rt_buf_trim()` (link padding, UDP
  length) and fragment reassembly drop the flag. TCP and UDP then verify
  with `rt_buf_csum()` and the pseudo header, without reading the payload
  again.
* **Receive batches.** The Riptide process inputs everything the drivers
  have between `rt_rx_batch_begin()` and `rt_rx_batch_end()`. Inside a
  batch TCP holds back the in-order ACK that `delack_segs` calls for and
  the reader's wakeup; the end sends one ACK per connection (unless a
  segment we sent carried it) and wakes each reader once. A held-back ACK
  never covers more than `2 * delack_segs` segments: the next one past
  that goes out at once. Quick ACKs are held like the others (one per
  batch); duplicate ACKs (out-of-order data) and ACKs asked for
  explicitly (`ICSK_ACK_NOW`) are not.
* **Window updates.** Linux sends a window update after a read that
  doubles a nearly closed window. A reader slower than the link (2 KB
  `recv()`s, the window at a segment or two) then costs one pure ACK per
  read. Riptide also asks for the window to open by four segments (or a
  quarter of the buffer, if smaller); the queue holds the rest of the
  buffer for the reader meanwhile, so the sender is not starved.
* **Clocks.** A segment reads the clock about nine times (TCP's clocks,
  timers, caches). On AmigaOS each is a `ReadEClock()` and divisions (about
  110 µs on the emulated A1200), so the holder of the stack lock reads it
  once per hold (`os_amiga.c`): the time is that of the first read until
  the outermost `rt_unlock()`. Code that waits releases the lock, so time
  still moves for it; a hold lasts milliseconds.
* **timer.device.** The idle tick (10 s, for `QUIT`) has its own request,
  so arming the stack's next deadline after an idle spell does not abort
  and resend the tick each time.

## IPv6

Riptide is a dual-stack host (RFC 8200, 4291, 4861, 4862, 8981, 7217,
3810, 6724, 4443, 8106, 8415 stateless). The contract is `rt_ip6.h`;
the code is in `src/core`:

| File | Contents |
|---|---|
| `inet6_text.c` | Text forms (RFC 4291 2.2 parsing, RFC 5952 output) and address helpers. Self contained, so the Amiga tools link it too. |
| `ip6.c` | Input (extension header walk, option action bits, hop-by-hop only first, atomic fragments), output (optional Router Alert), forwarding, the sysctls and the 1-second slow timer that drives lifetimes. |
| `ip6_if.c` | Per-interface state (`nif->ip6`): addresses, DAD (RFC 7527 nonces), SLAAC with the two-hour rule, RFC 7217 identifiers (SipHash-2-4), RFC 8981 temporary addresses, MLDv2 with v1 fallback, router solicitations, source selection (RFC 6724 rules 1-3, 5-8 and a fixed policy table). |
| `nd6.c` | Neighbour cache (RFC 4861 states incl. DELAY/PROBE; router entries) and NS/NA/RS/RA/redirect handling, RA options PIO, MTU, RIO (RFC 4191), RDNSS/DNSSL (RFC 8106). |
| `route6.c` | Longest-prefix table, default routers with preference, RA routes with lifetimes, PMTU entries (minimum 1280), redirects. |
| `ip6_frag.c` | Reassembly (RFC 5722: overlaps drop the datagram; RFC 7112: the whole header chain in the first fragment) and fragmentation with random identifiers (RFC 7739). |
| `icmp6.c` | Echo, errors with per-destination token buckets plus a global limit (packet too big exempt), error delivery to transports, ND/MLD dispatch. |
| `dhcp6.c` | Stateless DHCPv6 client (Information-request when an RA has O or M). |
| `inet.c` | The family-independent glue the transports use. |

**One address type in the transports.** TCP, UDP and raw keep every
address as `rt_in6_addr`, IPv4 as IPv4-mapped (`::ffff:a.b.c.d`), the
way dual-stack BSD and Linux sockets work. `inet.c` routes, sums and
sends by the destination's family (`rt_inet_route`, `rt_inet_pseudo`,
`rt_inet_output`), and its bind rules let an `AF_INET6` socket on `::`
also own the IPv4 port unless `IPV6_V6ONLY` is set. The socket layer
converts `sockaddr_in`/`sockaddr_in6` by the socket's domain, so the
protocols see one normalized `rt_sockaddr_in6`. IPv4 packets come out
byte for byte as before: the IPv4 paths still go through `ip.c`
unchanged and only the PCB keys widened.

**ABI choices** (documented in `AMIGA.md`, "IPv6"): `AF_INET6` 28 and the
`IPV6_*`/`AI_*` numbers are the KAME/BSD values, which is what the Miami
SDK shipped (its `MiamiSupportsIPV6()` never returned TRUE, but programs
built against it use those numbers); `sockaddr_in6` is the 28-byte RFC
3493 layout, and 24-byte RFC 2133 callers are accepted and served.
`IPV6_V6ONLY` defaults to 0 (RFC 3493, Linux), so dual-stack servers
work out of the box; FreeBSD's default of 1 is one sysctl away.

**Drivers.** `nif->mcast(nif, mac, join)` programs the link-layer
filter for the groups IPv6 joins; SANA-II queues the requests for the
Riptide process (the core may call from any task under the lock), MNI
calls the driver directly, the host TAP driver keeps its own filter.

**Testing.** The build machine's kernel has no IPv6, so interop runs
against gVisor's netstack (`tests/interop/gvpeer`, Go) attached to the
simulated wire: it plays the router (hand-built RAs, RDNSS, DHCPv6) and
serves TCP/UDP/DNS; `t_ip6_gvisor` checks SLAAC, ND, ping, TCP both ways
at MTU 1500 and 1280, UDP, DNS (AAAA, PTR) and DHCPv6, and prints TCP
throughput over IPv6 next to IPv4 on the same link. `t_tap` repeats the
interop against the Linux kernel over TAP where the kernel has IPv6.
`t_ip6_unit` and `t_ip6_loop` cover the protocol logic and a fuzzer
(run them under `SAN=1`).
