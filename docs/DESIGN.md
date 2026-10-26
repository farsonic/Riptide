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
* Where they are used: TCP `send()` and UDP send copy from the user's iov
  and sum in one pass (`rt_iov_to_buf_csum()`; TCP keeps the sum with the
  queued segment, see "Send path"), and a TCP retransmission whose earlier
  copy still waits in a driver is copied and summed in one pass
  (`rt_buf_copy_csum()`); the SANA-II `CopyToBuff` callback copies and
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

### Sleepers and signals

A blocking call sleeps on its task's `rt_sleeper` (a signal bit plus a
timer request with its own port), found by `rt_sleeper_self()`: the one
in the task's library context, or for a task without one (the Riptide
process, a program calling through a base another task opened) an
"orphan" sleeper on a list keyed by the `struct Task` pointer. Orphan
sleepers are never freed. Known weakness, not seen to matter so far: when
such a task ends and a new task is later created at the same address,
the new task finds the old sleeper, whose signal bits it never allocated
(they may collide with its own ports). Fixing it needs a way to see that
a task has ended (exec has none short of patching `RemTask()`), so the
library-context path, which every program that opens bsdsocket.library
itself takes, is the supported one.

### A stall that is not Riptide's (FS-UAE with the AROS ROM)

Rare bsdsocktest and tools runs used to stop with no result: the stack
process stopped answering `Riptide STATUS`, loopback traffic stopped,
the test program waited forever, everything else ran. A probe
(`tests/amiga/taskprobe.c`, run by `run_fsuae_bsdsocktest.sh` when vbcc
is there) caught the Riptide process in state `TS_READY`, priority 2, in
neither `SysBase->TaskReady` nor `TaskWait`, its `ln_Succ` still
`&TaskReady.lh_Tail` and its `ln_Pred` the DH0 handler task, while
priority 0 tasks kept running: exec had taken it off the ready list
without running it, and it never ran again. Its saved stack showed the
`Wait()` of `amiga_stack_loop()`, nothing in Riptide in between.

`tests/amiga/execrace.c` reproduces it without any TCP/IP stack: a
priority 2 task and a DOS process wake each other with `Signal()`/`Wait()`
while a third task keeps timer.device busy with 3 ms requests. On FS-UAE
3.2.35 with the AROS Kickstart ROM and a loaded host, the DOS process
ended up in no task list in about half of the 240 s runs, with every
`Signal()` inside `Disable()`/`Enable()` as well, and with its log on
`RAM:` instead of the host filesystem; without the timer task four runs
of four finished. So it is exec in that ROM (or FS-UAE's interrupt
emulation) under timer interrupt load, not a Riptide bug, and wrapping
Riptide's `Signal()` calls would not help. The Riptide process, woken by
`Signal()` for every packet and with timer requests of its own, is the
likeliest task to be hit. `run_fsuae_execrace.sh` counts such stalls on
fresh boots; `run_fsuae_bsdsocktest.sh` says "emulator lost the stack
process" when the probe saw it.

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
  explicitly (`ICSK_ACK_NOW`) are not. A connection destroyed inside the
  batch (an orphan's last FIN) never joins it: `tcp_reap()` frees it
  before the batch ends. The host build batches each driver read the same
  way, so the TUN and wire tests (and their ASan runs) cover it.
* **Window updates.** Linux sends a window update after a read that
  doubles a nearly closed window. A reader slower than the link (2 KB
  `recv()`s, the window at a segment or two) then costs one pure ACK per
  read. While data is still queued, Riptide also asks for the window to
  open by four segments (or a quarter of the buffer, if smaller); the
  queue holds the rest of the buffer for the reader meanwhile, so the
  sender is not starved. A read that drains the queue always sends it.
* **Clocks.** A segment reads the clock about nine times (TCP's clocks,
  timers, caches). On AmigaOS each is a `ReadEClock()` and divisions (about
  110 µs on the emulated A1200), so the holder of the stack lock reads it
  once per hold (`os_amiga.c`): the time is that of the first read until
  the outermost `rt_unlock()`. Code that waits releases the lock, so time
  still moves for it; a hold lasts milliseconds.
* **timer.device.** The idle tick (10 s, for `QUIT`) has its own request,
  so arming the stack's next deadline after an idle spell does not abort
  and resend the tick each time.

### Send path

An upload on a 68020 is CPU bound as well. The application's `send()`
does most of the work in its own task under the stack lock: it copies the
data into write-queue segments and, as far as the congestion and receive
windows allow, builds and hands them to the driver
(`__tcp_push_pending_frames()`); the Riptide process sends the rest when
ACKs open the window. What a segment costs, and what was done about it
(`Riptide BENCH TX` runs the send path against a scripted receiver in one
task, `src/core/txbench.c`; `tests/t_tx_bench.c` does the same on the
host and under `qemu-m68k`):

* **The lock wait is the application's send().** In the profile of an
  upload the Riptide process spends a quarter of the time in `lock wait
  rt`: about one wait per `send()` call, for the 20 ms or so the
  application holds the lock to queue 16 KB and send what the window
  allows. `netbench -I` shows the CPU 100 % busy throughout, so the wait
  is not idle time; it overlaps the application's work, which the
  `socket send`, `copy from app`, `tcp output` and `driver send` sections
  show.
* **One pass over the payload.** `tcp_v4_send()` sums the data while it
  copies it from the application (`rt_iov_to_buf_csum()`) and keeps the
  sum with the queued segment (`RT_BUFF_CSUM_PART`, chained with
  `rt_csum_block_add()` when a later `send()` appends to it; trimming or
  splitting the segment drops the flag and the transmit path sums it
  again). The segment goes out as a header-only clone that shares the
  payload (`rt_buf_clone()`): the TCP, IP and link headers are pushed into
  the shared headroom, and only the header sum is computed. The clone has
  no tailroom and nobody writes the shared bytes; a queued segment freed
  (ACKed) while a driver still holds its clone is freed with the clone.
  A segment whose earlier transmission still waits in a driver is copied
  as before, since its headers sit in the headroom; loopback unshares
  clones, so the receiving socket owns and accounts its buffers.
* **Route cache.** A connection keeps its IPv4 route (`struct rt_ip_dst`
  in the `tcp_sock`, filled by `rt_ip_output()` for a unicast destination
  and next hop): later segments skip the route lookup, the local address
  scan and both broadcast tests. `rt_route_changed()` bumps
  `rt_route_gen` on every route, interface and address change, which
  empties all caches; an entry is also used for at most `RT_IP_DST_MS`
  (1 s), which bounds how late a change made elsewhere applies (expiry of
  a dynamic route, a field set directly).
* **Clocks.** Cached clock reads (about ten per segment) return from a
  small `rt_now_ms()`/`rt_now_us()` without the conversion's prologue.
* **Timers.** `rt_timer_mod()` kicks the Riptide process only for a first
  timer due before the deadline it sleeps until (`rt_timers_run()`
  records it); TCP re-arms its retransmit timer with every segment and
  ACK, usually for a later time. No measurable gain: most kicks left come
  from the Riptide process itself, where they cost a signal.
* **Compiler.** The per-packet core files are built with vbcc `-O3`
  (`HOT_OBJ` in `Makefile.amiga`).

On the emulated A1200 (68020, 14 MHz) these took the send path from 3231
to about 2220 µs per segment in `BENCH TX`, and netbench uploads from
309 to 414 KB/s (68040: 1054 to 1353 KB/s; `docs/COMPARISON.md` 11). What is left per segment: about 350 µs for the copy and sum
from the application (`rt_csum_copy_020`), about 450 µs in the driver's
`SendIO()` (the uaenet device copies the frame through `CopyFromBuff`),
and the TCP/IP code; interrupts and the emulated device take a share the
profiler cannot see. A driver with `S2_DMACopyFromBuff32` would save its
copy only for frames that start on a 32-bit boundary, which TCP frames
(behind a 14-byte Ethernet header) do not.

## TCP connection behaviour (Fast Open, connecting, keepalive, cork)

User-visible behaviour and the knobs are in AMIGA.md ("Connecting, Fast
Open, keepalive, cork"); this is how they are built.

* **Fast Open client** (`tcp_fastopen.c`, Linux `tcp_fastopen.c` and the
  fastopen part of `tcp_metrics.c`). The cookie cache is a 32-entry array
  keyed by server address (LRU), with the server's MSS and a SYN-loss
  count; a global run of lost TFO SYNs switches TFO off for
  `fastopen_blackhole << n` seconds. `tcp_connect()` is split:
  `tcp_connect_init()` queues the SYN unsent, `tcp_connect_xmit()` sends
  it. A deferred connect (`TCP_FASTOPEN_CONNECT` with a known cookie) is
  hashed and in SYN-SENT but has sent nothing (`TCP_FO_DEFER`,
  `RT_SS_DEFERCONN`); the first `send()` runs init, queues its data behind
  the SYN like any send, and `tcp_send_syn_data()` sends the SYN with a
  copy of the first data segment (fragmented to the cached MSS less 40)
  while the queue keeps SYN and data as two segments in flight. So the
  retransmission machinery needs no special case: a retransmitted SYN is
  the plain SYN skb (and goes without the option once the first timed
  out), data the SYN-ACK did not acknowledge is retransmitted at once from
  `tcp_rcv_synsent_state_process()`. `recv()`, `shutdown()` and
  `WaitSelect()` for reading (`RT_SIOC_KICKCONN`, sent by `sel_scan()` for
  sockets with `RT_SS_DEFERCONN`) start the handshake without data.
  `shutdown(SHUT_WR)` of a TFO socket in SYN-SENT sets `fin_pending`; the
  FIN goes after the handshake (an HTTP/1.0-style client shuts down right
  after its request, before the SYN-ACK).
* **Connecting.** SYN retransmissions use `syn_rto_initial_ms`, then double
  or grow linearly up to `syn_rto_max_ms` (`tcp_retransmit_timer()`);
  `connect_timeout` (or `TCP_KEEPINIT`) is the user timeout of SYN-SENT and
  also clamps the last RTO, so the connect fails on time.
  `syn_icmp_abort` in `tcp_err()` decides whether net/host unreachable
  (soft in RFC 1122) end a connect: the default ends it on the second
  report, which also covers Riptide's own ARP/ND failures (they answer
  queued packets with ICMP host unreachable).
* **Keepalive for all.** `keepalive_all` sets `ka_auto` at attach and on
  accepted children; `keepalive_time_when()` then uses it as the idle
  time. An explicit `SO_KEEPALIVE` clears `ka_auto`.
* **Cork.** `TCP_CORK` and `MSG_MORE` push with `TCP_NAGLE_CORK` and arm
  `cork_timer` (200 ms), which pushes with `TCP_NAGLE_PUSH`; `TCP_NOPUSH`
  has no timer. With nothing in flight the zero window probe timer also
  sends held data after an RTO (as Linux).
* **Counters.** Appended to `struct rt_tcp_stats` (Riptide 0.15); `netstat`
  reads the old part from older stacks and the new counters only when
  the record is long enough.

## Background transfers (tcp_ledbat.c)

One controller, `struct ledbat`, serves both directions. It keeps a window
in segments * 256 (fixed point, no FPU), a base RTT (the minimum of each
of the last four minutes) and a current RTT (the minimum of the last four
samples); the queueing delay is their difference. Per acknowledged (or
received) segment it follows LEDBAT++: below the 60 ms target it grows,
in slow start by `acked / gain_inv`, else by `gain * (1 - delay/target)`
per window (remainders carried), with `gain = 1 / min(16, ceil(2 *
target / base))`; above the target it decreases once per RTT by `W *
(delay / target - 1)`, at most half; slow start ends at 3/4 of the
target. A loss halves once per RTT. Two RTTs after slow start, and then
nine times the last slowdown's duration later, it freezes at two segments
for two RTTs and ramps back to the old window: the queue drains and the
base delay is measured again (late-comer advantage, base drift).

* **Sender:** `tcp_congestion_ops` "ledbat" (`ca_priv` holds the 80
  bytes). RTT samples come from `pkts_acked()`; the stack's own changes to
  `snd_cwnd` (recovery, RTO) are taken over at the next ACK.
* **Receiver (rLEDBAT, RFC 9840):** a `struct tcp_rledbat` allocated per
  connection that wants it (and has timestamps). Samples are those of
  `tcp_rcv_rtt_measure_ts()` (the first full segment echoing a new TSval
  of ours); in-order data counts as acknowledged; a new hole in the
  out-of-order queue counts as a loss. `__tcp_select_window()` clamps the
  free space to the controller's window (at least two segments);
  `tcp_select_window()` already never shrinks the right edge, so a lower
  limit takes effect as data arrives.
* **Policy:** `tcp_bg_attach()` matches the creating program (its name
  through `rt_os_program_name()`, in the program's context) when a socket
  is made; `tcp_bg_classify()` adds the port rules when connect() picks
  the local port or a passive connection is created (children inherit the
  listener's program match and `TCP_BACKGROUND`). `TCP_BACKGROUND` and an
  explicit `TCP_CONGESTION` win over the rules.

The host test (`tests/t_ledbat_tun.c`) needed a real bottleneck queue in
front of a Linux sender: a qdisc on the sender's own host never fills
(TCP Small Queues count the bytes until the qdisc releases them), so
Riptide reads its TUN device at a fixed rate (`rt_host_tun_rx_rate()`)
and the tun queue, whose packets the kernel orphans, is the bottleneck.
The container's Linux defaults to BBR, which builds no standing queue;
the test uses CUBIC senders.

## Socket memory (tcp_mem.c)

The use is the sum of `rmem_alloc + wmem_queued` over the connections
(truesizes) and the datagram receive queues, recomputed at most once per
millisecond below `mem_pressure` and at every check above it (loopback
moves hundreds of KB per millisecond). Checks: receive auto-tuning,
`tcp_sndbuf_expand()` and `tcp_grow_window()` stop under pressure; the
window scale at SYN time is chosen for `tcp_rmem_max_now()` (64 KB under
pressure); above `mem_high`, `tcp_try_rmem_schedule()` prunes the
out-of-order queue and refuses data beyond an 8 KB minimum, the send path
and `tcp_stream_is_writeable()` refuse to queue beyond 8 KB, and
`rcv_ssthresh` is clamped to four segments. The minimums mean the use can
exceed `mem_high` by up to two minimums and two segments per connection
that was below them (Linux keeps 4 KB, `tcp_rmem[0]`).

### Transmit path: priority bands

`rt_netif_xmit()` hands every frame to `rt_qos_xmit()` when the interface
has bands (`src/core/qos.c`; for drivers that report
completions through `rt_drv_tx_start()`/`rt_drv_tx_done()`, i.e. SANA-II,
MNI and the host TAP driver; off by default because on a link faster
than the Amiga, e.g. FS-UAE's uaenet, about half of an upload's frames
wait for the stack task to see a completion, about 5% of upload speed).
While the driver holds fewer than
`net.inet.ip.qos_inflight` frames (4) and nothing waits, the frame goes
straight through. Otherwise it is classified
(`rt_qos_band()`: ARP, ICMP, DNS/NTP/mDNS/DHCP, TCP without data or with
up to 64 bytes, DSCP, then the socket priority carried in `rt_buf.prio`)
and queued in its band; `rt_drv_tx_done()` sends the next one from the
highest band. The SANA-II and MNI drivers report a completion only after
the request is back on their free list, so the next frame can use it. A
failsafe timer sends one frame every 250 ms if a driver stops reporting
completions, so a broken driver cannot hold the queue forever. Capture
records frames when they go to the driver, i.e. in wire order.

Socket priority (`SO_PRIORITY`, `rt_socket.priority`) and `qos rule`s
(`rt_qos_sock_rules()`, applied at socket creation for program rules and
at bind/connect/accept/sendto for port rules) end up in two places: the
TOS/traffic class byte through `rt_qos_sock_tos()` in TCP, UDP and raw
output (only when the program's own TOS has DSCP 0), and `rt_buf.prio`.

### Several IPv4 addresses

`struct rt_netif` keeps its primary address in `addr`/`netmask`/
`broadaddr` (so the ~60 users of the single address, the ioctls and the
`net.riptide.ifaces` record are unchanged) and up to `RT_IF_MAXADDR4` (8)
more in `alias[]`. The places that ask "is this ours" use
`rt_netif_has_addr()` (`rt_netif_by_addr()`, `rt_ip_is_local()`, ARP),
broadcast checks walk all prefixes, connected routes are installed for
each prefix (`install_connected()`), and the route lookup picks the source
with `rt_netif_src()`: the address whose subnet holds the next hop, then
the destination's, then the primary. Deleting the primary promotes the
first additional address.

### VLAN interfaces (vlan.c)

A VLAN is an ordinary `struct rt_netif` (type Ethernet, the parent's MAC)
with `vlan_parent`/`vlan_id`/`vlan_pcp` set and a `link_output` that
pushes the 4-byte tag control field and inner ethertype in front of the
packet and calls `rt_netif_xmit(parent, b, dsthw, 0x8100)`. The tag thus
travels as payload of a frame of type 0x8100, which every driver path
already builds correctly: SANA-II cooked (the device writes the header,
`PacketType` 0x8100), SANA-II raw and MNI (Riptide writes the header),
TAP and the simulated wire. No driver needs to know about VLANs on
transmit, and there is no raw-mode requirement. The tag is the payload's
first 4 bytes, so a full-size tagged frame is 1518 bytes; the VLAN's MTU
defaults to the parent's less 4 because many SANA-II drivers refuse
frames over 1514 bytes. A partial checksum carried with the buffer
(`RT_BUFF_CSUM_PART`) is dropped before tagging: it covered the payload
from the old start, and the tag is not part of it.

Receive: `rt_netif_input()` was split into the counting/capture part and
`rt_netif_deliver()` (the ethertype dispatch), so `rt_vlan_input()` can
strip the tag and deliver the inner frame on the VLAN interface (after
counting it there) through the same dispatch. VID 0 re-enters the parent;
unknown ids and stacked tags count as `noproto` on the parent. SANA-II
devices only return frames of the types that have reads queued, so the
driver exports `nif->vlan_rx(nif, on)`: the first VLAN on a parent queues
reads of type 0x8100 (as many as the IPv4 reads; their own queue index,
outside the per-type counters), the last one removed retires them.

Parent and VLANs share one MAC and so one hardware multicast filter: two
interfaces joining 33:33:00:00:00:01 must not cause two `S2_ADDMULTICASTADDRESS`
calls and, worse, one leave must not drop the group for both. Drivers
keep a small reference-counted table (`rt_mc_ref()`, 32 entries) and talk
to the device only on the first join and the last leave.

Link state propagates in `rt_vlan_parent_changed()`, called from
`rt_netif_up()`/`down()`, the MTU ioctl and DHCP's MTU option: a VLAN is
RUNNING only while its parent is, and its MTU is clamped to the parent's.
`rt_netif_remove()` of a parent removes its VLANs first.

### Happy Eyeballs

`rt_resolve_name46()` (`resolv.c`) asks the local sources (hosts table,
cache, our mDNS name) for both types first; for what is left it sends A
and AAAA queries for each search candidate on one socket per server and
waits for both, but no longer than `net.inet.he.resolution_delay` for the
AAAA answer once the A records are in. `rt_he_connect()` (`happy.c`) runs
in the caller's task without the lock, like the resolver: it sorts with
`rt_inet_sort_dest()`, interleaves the families and keeps non-blocking
connects going with `rt_api_select()`, starting the next one every
`attempt_delay` ms or when one fails. riptide.library 2 exports it as
`RiptideConnectName()` (LVO -528, beyond the historical table so the
compat alias keeps its layout).

## Sending UDP: pacing, batches, no checksum (0.17)

**Pacing.** A UDP socket remembers the interface its last datagram left
by (`so->tx_ifindex`, from `rt_netif_tx_last`, which `rt_netif_xmit()`
sets; for a VLAN the parent). `udp_send()` asks `rt_netif_tx_room()`
for that interface before it builds a datagram: with QoS bands, whether
the socket's band (`rt_qos_sock_prio()`) is below `qos_limit`; without,
whether the driver holds fewer than its `tx_slots` (SANA-II: 32). No
room: the socket sets `tx_wait`, the interface `tx_waiters`, and the
send says `EWOULDBLOCK`, so `sock_sendit()` sleeps on the socket's wait
queue for a blocking socket (honouring `SO_SNDTIMEO`) and a non-blocking
one returns. `udp_poll()` reports `POLLOUT` only with room (and arms the
same flags), so WaitSelect() is the pacing signal.
`rt_drv_tx_done()`, after `rt_qos_run()`, wakes the waiters with
`rt_udp_tx_wake()` once the interface has room again (`RT_SEV_WRITE`:
WaitSelect, select, `FD_WRITE` events). `rt_netif_down()` wakes them too, so a
blocked sender does not outlive its interface. An `ENOBUFS` from the output path
with no room left becomes `EWOULDBLOCK` too. The first datagram, and one
to another interface, is sent as before; drivers without completions
(`tx_slots` 0, loopback) always have room. The cost is one interface
lookup by index and a compare per datagram; `udpstat.TxNoRoom` counts
the sends that waited.

**Batches.** `rt_api_sendmmsg()` sends up to n messages under one lock
(`fd_get()` again for each, since a sleep may let the socket go),
stopping at the first error; `rt_api_recvmmsg()` takes the first message
as the flags say and the rest with `MSG_DONTWAIT`. riptide.library 3
exports them as `RiptideSendMMsg()`/`RiptideRecvMMsg()` (LVO -534/-540),
converting the caller's `struct mmsghdr` array in chunks of 16. Most of
a single `sendto()`'s cost on a 68030/040 is not the copy or the
checksum but the call itself: the library entry, the lock, the switch to
the (higher priority) stack task that takes the driver's completion, and
back; a batch pays those once per 16 datagrams.

**No checksum.** `SO_NO_CHECK` (0x1101, Linux's 11) sets `so->no_check`
on datagram sockets; `udp_send()` then copies without summing and leaves
the field 0 for IPv4. IPv6 ignores it (RFC 8200 requires the checksum).

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
