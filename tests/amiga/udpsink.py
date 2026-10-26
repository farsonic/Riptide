#!/usr/bin/env python3
# Riptide - receiving end of tests/amiga/udpblast.c (run_fsuae_udpblast.sh).
# Watches IFACE with a packet socket for UDP datagrams to PORT (unicast or
# multicast), counts them per burst (a burst ends after GAP seconds without
# one) and how many carried no checksum (0), and prints one line per burst:
#
#   burst 1: 3000 datagrams, 4200000 payload bytes, 0 without checksum, 2.31 s
#
# A bound UDP socket takes the unicast datagrams too, so the kernel sends
# no ICMP port unreachable back. Python 3 standard library only.
#
#   udpsink.py IFACE ADDR PORT [GROUP] [GAP]
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import struct
import sys
import time

iface, addr, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
group = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != "-" else None
gap = float(sys.argv[5]) if len(sys.argv) > 5 else 1.5

raw = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0800))
raw.bind((iface, 0))
raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sink.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
sink.bind(("", port))
if group:
    mreq = socket.inet_aton(group) + socket.inet_aton(addr)
    sink.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

burst = 0
n = nbytes = nock = 0
t0 = t1 = 0.0


def flush():
    global n, nbytes, nock, burst
    if n:
        burst += 1
        print("burst %d: %d datagrams, %d payload bytes, %d without checksum, %.2f s"
              % (burst, n, nbytes, nock, t1 - t0), flush=True)
    n = nbytes = nock = 0


print("udpsink: listening on %s port %d%s" % (iface, port, " group " + group if group else ""), flush=True)
while True:
    r, _, _ = select.select([raw, sink], [], [], 0.2)
    now = time.monotonic()
    if sink in r:
        try:
            while True:
                sink.recv(65536, socket.MSG_DONTWAIT)
        except BlockingIOError:
            pass
    if raw in r:
        while True:
            try:
                f = raw.recv(65536, socket.MSG_DONTWAIT)
            except BlockingIOError:
                break
            if len(f) < 14 + 20 + 8 or f[14 + 9] != 17:
                continue
            ihl = (f[14] & 15) * 4
            u = 14 + ihl
            dport, ulen, ck = struct.unpack("!HHH", f[u + 2:u + 8])
            if dport != port:
                continue
            if not n:
                t0 = now
            t1 = now
            n += 1
            nbytes += ulen - 8
            if ck == 0:
                nock += 1
    if n and now - t1 > gap:
        flush()
