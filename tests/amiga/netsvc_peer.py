#!/usr/bin/env python3
# Riptide - the host side of tests/amiga/run_fsuae_netsvc.sh, on the veth
# end IFACE (ADDR, e.g. 192.168.81.1):
#   - an SNTP server (UDP 123) with the host's clock
#   - an mDNS responder for "linuxpeer.local" -> ADDR (legacy unicast
#     answers too, which is what Riptide's resolver asks for)
#   - from START seconds on, queries for NAME.local (multicast QM, QU, legacy
#     unicast, the reverse PTR) until each was answered; results go to stdout
#     as "ok WHAT ..." lines
# Usage: netsvc_peer.py IFACE ADDR NAME AMIGA_ADDR START TIMEOUT
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import struct
import sys
import time

iface, addr, name, amiga, start_s, timeout = sys.argv[1:7]
start_s, timeout = float(start_s), float(timeout)
GROUP = '224.0.0.251'
NTP_EPOCH = 2208988800


def log(*a):
    print(*a, flush=True)


def qname(n):
    return b''.join(bytes([len(l)]) + l.encode() for l in n.rstrip('.').split('.')) + b'\0'


def read_name(m, off):
    labels, jumped, end = [], False, None
    while True:
        ln = m[off]
        if ln & 0xc0 == 0xc0:
            if not jumped:
                end = off + 2
            off = ((ln & 0x3f) << 8) | m[off + 1]
            jumped = True
            continue
        off += 1
        if ln == 0:
            break
        labels.append(m[off:off + ln].decode('latin-1'))
        off += ln
    return '.'.join(labels), (end if jumped else off)


def parse(m):
    """(id, flags, questions, records) of a DNS message"""
    mid, flags, qd, an, ns, ar = struct.unpack('!6H', m[:12])
    off, qs, rrs = 12, [], []
    for _ in range(qd):
        n, off = read_name(m, off)
        t, c = struct.unpack('!HH', m[off:off + 4])
        off += 4
        qs.append((n.lower(), t, c))
    for i in range(an + ns + ar):
        n, off = read_name(m, off)
        t, c, ttl, rdl = struct.unpack('!HHIH', m[off:off + 10])
        off += 10
        rd = m[off:off + rdl]
        if t == 12:
            rd = read_name(m, off)[0]
        rrs.append((n.lower(), t, c, ttl, rd, 'an' if i < an else 'ns' if i < an + ns else 'ar'))
        off += rdl
    return mid, flags, qs, rrs


def query(mid, n, t, qu=False):
    return struct.pack('!6H', mid, 0, 1, 0, 0, 0) + qname(n) + struct.pack('!HH', t, 0x8001 if qu else 1)


ntp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ntp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ntp.bind((addr, 123))

md = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
md.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
md.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
md.bind(('', 5353))
md.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GROUP) + socket.inet_aton(addr))
md.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(addr))
md.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
md.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
md.setsockopt(socket.IPPROTO_IP, socket.IP_PKTINFO, 1)

leg = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
leg.bind((addr, 0))
leg.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(addr))

me = name.lower() + '.local'
rev = '.'.join(reversed(amiga.split('.'))) + '.in-addr.arpa'
peer = 'linuxpeer.local'
todo = {'qm': None, 'qu': None, 'legacy': None, 'ptr': None}
seen_probe = seen_announce = False
t0 = time.time()
next_q = t0 + start_s

done = False
while time.time() - t0 < timeout:
    if not done and all(v is not None for v in todo.values()):
        done = True
        log('done all answered')
    now = time.time()
    if now >= next_q:
        next_q = now + 1.5
        # QU first (answered by unicast), then QM (multicast)
        if todo['qu'] is None:
            md.sendto(query(0, me, 1, qu=True), (GROUP, 5353))
        elif todo['qm'] is None:
            md.sendto(query(0, me, 1), (GROUP, 5353))
        if todo['legacy'] is None:
            leg.sendto(query(0x4242, me, 1), (GROUP, 5353))
        if todo['ptr'] is None:
            leg.sendto(query(0x4343, rev, 12), (GROUP, 5353))
    r, _, _ = select.select([ntp, md, leg], [], [], 0.2)
    for s in r:
        dst = None
        if s is md:
            m, anc, _, src = s.recvmsg(2048, 256)
            for lvl, typ, data in anc:
                if lvl == socket.IPPROTO_IP and typ == socket.IP_PKTINFO:
                    dst = socket.inet_ntoa(data[8:12])
        else:
            m, src = s.recvfrom(2048)
        if s is ntp:
            if len(m) < 48 or m[0] & 7 != 3:
                continue
            t = time.time()
            st = struct.pack('!II', int(t) + NTP_EPOCH, int((t % 1) * 2 ** 32))
            reply = bytes([0x24, 2, 6, 0xec]) + bytes(8) + b'LOCL' + st + m[40:48] + st + st
            ntp.sendto(reply, src)
            log('ntp request from', src[0])
            continue
        try:
            mid, flags, qs, rrs = parse(m)
        except (IndexError, struct.error, UnicodeDecodeError):
            log('malformed from', src)
            continue
        if src[0] != amiga:
            continue
        if s is md and not flags & 0x8000:
            # the Amiga asking (its resolver, or probes)
            if any(q[0] == me for q in qs) and any(rr[5] == 'ns' for rr in rrs):
                if not seen_probe:
                    log('ok probe', me, 'from', src)
                seen_probe = True
            for qn, qt, qc in qs:
                if qn == peer and qt in (1, 255):
                    ans = qname(peer) + struct.pack('!HHIH', 1, 1 if src[1] != 5353 else 0x8001, 10, 4) + \
                        socket.inet_aton(addr)
                    if src[1] != 5353:   # legacy unicast: echo id and question
                        q = qname(qn) + struct.pack('!HH', qt, qc)
                        pkt = struct.pack('!6H', mid, 0x8400, 1, 1, 0, 0) + q + ans
                        md.sendto(pkt, src)
                    else:
                        md.sendto(struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0) + ans, (GROUP, 5353))
                    log('ok answered', qn, 'for', src)
            continue
        a = [rr for rr in rrs if rr[0] == me and rr[1] == 1]
        if s is md and src[1] == 5353 and flags & 0x8000:
            if a and a[0][4] == socket.inet_aton(amiga):
                if mid == 0 and not qs and not seen_announce:
                    seen_announce = True
                    log('ok announce', me, 'ttl', a[0][3], 'cache-flush', bool(a[0][2] & 0x8000))
                if dst == addr and todo['qu'] is None:
                    todo['qu'] = a[0]
                    log('ok qu (unicast)', me, socket.inet_ntoa(a[0][4]), 'ttl', a[0][3])
                elif dst == GROUP and todo['qu'] is not None and todo['qm'] is None:
                    todo['qm'] = a[0]
                    log('ok qm (multicast)', me, socket.inet_ntoa(a[0][4]), 'ttl', a[0][3])
        if s is leg and flags & 0x8000:
            if mid == 0x4242 and a and qs and qs[0][0] == me:
                todo['legacy'] = a[0]
                log('ok legacy', me, socket.inet_ntoa(a[0][4]), 'ttl', a[0][3],
                    'cache-flush', bool(a[0][2] & 0x8000))
            p = [rr for rr in rrs if rr[0] == rev and rr[1] == 12]
            if mid == 0x4343 and p:
                todo['ptr'] = p[0]
                log('ok ptr', rev, p[0][4])

if not done:
    log('done missing: ' + ','.join(k for k, v in todo.items() if v is None))
