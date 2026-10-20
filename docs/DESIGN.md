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
| `src/amiga/` | AmigaOS glue: libraries, SANA-II, MNI, startup, OS headers | amiga |
| `sdk/` | Developer SDK for riptide.library (generated FD and headers) | amiga |
| `src/amiga/cmd/` | Network tools (ip, ifconfig, route, ...) and their shared `cmdlib` | amiga |
| `tests/` | Host tests (`make check`), interop with Linux via TUN | everybody |
| `tools/` | RE scripts (hunk parser etc.), `elf2hunk.py` | — |

The core files are owned per area:

* **infra**: `buf.c timer.c printf.c netif.c arp.c ip.c ip_frag.c route.c
  icmp.c udp.c raw.c loopback.c checksum.c init.c stats.c`
* **tcp**: `tcp*.c` plus internal `src/core/tcp.h`
* **sockets**: `socket.c socket_api.c ioctl_net.c sysctl.c config.c ctl.c resolv.c netdb.c dhcp.c errno.c`

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
* Every file starts with `/* ... SPDX-License-Identifier: GPL-2.0-only */`.
* Keep comments purposeful. Name Linux functions or RFC sections where an
  algorithm follows them (`/* RFC 8985 6.2: RACK detect loss */`).

## Builds

* `make`: native build plus tests in `build/host/`.
* `make SAN=1`: the same, with ASan and UBSan.
* `make CROSS=m68k-linux-gnu-`: big-endian 68020 static Linux binaries. Run
  them under `qemu-m68k` to test the core on the real target CPU and
  endianness.
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
