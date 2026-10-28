#!/usr/bin/env python3
# Riptide - a tiny authoritative DNS server for tests/amiga/run_fsuae_dns.sh:
# UDP answers are cut at 512 bytes, or at the EDNS0 size of the query
# (then only the question goes back, with TC set, as real servers do);
# TCP (RFC 7766, two byte length) answers in full. Names under dns.test:
#   big.dns.test, big2.dns.test   40 A records (about 690 bytes: EDNS0)
#   huge.dns.test                 100 A and 60 AAAA records (TCP)
#   www.dns.test                  one A record
# Every query is logged as "UDP|TCP NAME TYPE edns=SIZE|- tc=0|1".
#
#   python3 -I dns_server.py ADDRESS SECONDS LOGFILE
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import struct
import sys
import time

addr, secs, logname = sys.argv[1], float(sys.argv[2]), sys.argv[3]
log = open(logname, "a", buffering=1)


def records(name, qtype):
    """[(type, rdata)] for name, or None for NXDOMAIN"""
    if name in ("big.dns.test", "big2.dns.test"):
        return [(1, bytes([10, 2, 0, i + 1])) for i in range(40)] if qtype == 1 else []
    if name == "huge.dns.test":
        if qtype == 1:
            return [(1, bytes([10, 3, 0, i + 1])) for i in range(100)]
        if qtype == 28:
            return [(28, bytes([0xfd, 0x0d] + [0] * 12 + [0, i + 1])) for i in range(60)]
        return []
    if name == "www.dns.test":
        return [(1, bytes([10, 1, 0, 1]))] if qtype == 1 else []
    return None


def parse(q):
    """(id, flags, name, qtype, question end, edns size or None)"""
    qid, flags, qd, an, ns, ar = struct.unpack("!6H", q[:12])
    p, labels = 12, []
    while q[p]:
        labels.append(q[p + 1:p + 1 + q[p]].decode("ascii", "replace"))
        p += 1 + q[p]
    p += 1
    qtype = struct.unpack("!H", q[p:p + 2])[0]
    p += 4
    edns = None
    if ar and len(q) >= p + 11 and q[p] == 0 and struct.unpack("!H", q[p + 1:p + 3])[0] == 41:
        edns = struct.unpack("!H", q[p + 3:p + 5])[0]
    return qid, flags, ".".join(labels).lower(), qtype, p, edns


def answer(q, tcp):
    qid, flags, name, qtype, qend, edns = parse(q)
    rrs = records(name, qtype)
    rflags = 0x8400 | (flags & 0x0100)          # QR AA, RD copied
    body = b""
    if rrs is None:
        rflags |= 3
        rrs = []
    for t, rd in rrs:
        body += struct.pack("!HHHIH", 0xc00c, t, 1, 60, len(rd)) + rd
    opt = struct.pack("!BHHIH", 0, 41, 1232, 0, 0) if edns is not None else b""
    msg = struct.pack("!6H", qid, rflags, 1, len(rrs), 0, 1 if opt else 0) + q[12:qend] + body + opt
    limit = max(512, edns or 512)
    tc = 0
    if not tcp and len(msg) > limit:
        tc = 1
        msg = struct.pack("!6H", qid, rflags | 0x0200, 1, 0, 0, 1 if opt else 0) + q[12:qend] + opt
    log.write("%s %s %d edns=%s tc=%d len=%d\n" % ("TCP" if tcp else "UDP", name, qtype,
                                                   edns if edns is not None else "-", tc, len(msg)))
    return msg


def main():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.bind((addr, 53))
    t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind((addr, 53))
    t.listen(8)
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([u, t], [], [], 0.5)
        if u in r:
            q, peer = u.recvfrom(4096)
            try:
                u.sendto(answer(q, False), peer)
            except (IndexError, struct.error):
                pass
        if t in r:
            c, _ = t.accept()
            c.settimeout(5)
            try:
                hdr = c.recv(2, socket.MSG_WAITALL)
                n = struct.unpack("!H", hdr)[0]
                q = c.recv(n, socket.MSG_WAITALL)
                a = answer(q, True)
                c.sendall(struct.pack("!H", len(a)) + a)
            except (OSError, IndexError, struct.error):
                pass
            c.close()


main()
