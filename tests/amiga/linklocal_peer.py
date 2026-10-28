#!/usr/bin/env python3
# Riptide - the host side of tests/amiga/run_fsuae_linklocal.sh, on the
# veth end IFACE (with ADDR, a 169.254/16 address, so Linux has the
# link-local route). No DHCP server: the Amiga picks a link-local address.
#   - watches ARP: the Amiga's probes (sender 0.0.0.0) and announcements
#   - watches mDNS: the Amiga announcing NAME.local with that address
#   - asks for NAME.local (legacy unicast query) until answered, then pings
#     the address it got
# Results go to stdout as "ok WHAT ..." lines, then "done ...".
# Usage: linklocal_peer.py IFACE ADDR NAME TIMEOUT
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import struct
import subprocess
import sys
import time

iface, addr, name, timeout = sys.argv[1:5]
timeout = float(timeout)
GROUP = '224.0.0.251'
ETH_P_ARP = 0x0806
me = name.lower() + '.local'


def log(*a):
    print(*a, flush=True)


def qname(n):
    return b''.join(bytes([len(x)]) + x.encode() for x in n.rstrip('.').split('.')) + b'\0'


def read_name(m, off):
    labels, end = [], None
    for _ in range(64):
        ln = m[off]
        if ln & 0xc0 == 0xc0:
            if end is None:
                end = off + 2
            off = ((ln & 0x3f) << 8) | m[off + 1]
            continue
        off += 1
        if ln == 0:
            break
        labels.append(m[off:off + ln].decode('latin-1'))
        off += ln
    return '.'.join(labels).lower(), (end if end is not None else off)


def answers_a(m):
    """(id, flags, [(name, addr)]) of the A records in a DNS message"""
    mid, flags, qd, an, ns, ar = struct.unpack('!6H', m[:12])
    off, out = 12, []
    for _ in range(qd):
        off = read_name(m, off)[1] + 4
    for _ in range(an + ns + ar):
        n, off = read_name(m, off)
        t, c, ttl, rdl = struct.unpack('!HHIH', m[off:off + 10])
        off += 10
        if t == 1 and rdl == 4:
            out.append((n, socket.inet_ntoa(m[off:off + 4])))
        off += rdl
    return mid, flags, out


arp = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(ETH_P_ARP))
arp.bind((iface, ETH_P_ARP))

md = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
md.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
md.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
md.bind(('', 5353))
md.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GROUP) + socket.inet_aton(addr))

leg = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
leg.bind((addr, 0))
leg.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(addr))

ll = None                  # the Amiga's announced address
probes = set()
announced = resolved = pinged = False
t0 = time.time()
next_q = 0
while time.time() - t0 < timeout and not pinged:
    now = time.time()
    if ll and not resolved and now >= next_q:
        next_q = now + 1.0
        leg.sendto(struct.pack('!6H', 0x4c4c, 0, 1, 0, 0, 0) + qname(me) + struct.pack('!HH', 1, 1),
                   (GROUP, 5353))
    if resolved and not pinged:
        r = subprocess.run(['ping', '-c', '2', '-W', '2', '-I', iface, ll],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            pinged = True
            log('ok ping', ll)
            break
    rd, _, _ = select.select([arp, md, leg], [], [], 0.2)
    for s in rd:
        m, src = s.recvfrom(2048)
        if s is arp:
            if len(m) < 28 or struct.unpack('!H', m[6:8])[0] != 1:
                continue
            sip, tip = socket.inet_ntoa(m[14:18]), socket.inet_ntoa(m[24:28])
            if not tip.startswith('169.254.'):
                continue
            if sip == '0.0.0.0' and tip not in probes:
                probes.add(tip)
                log('ok probe', tip)
            elif sip == tip and tip in probes and ll != tip:
                ll = tip
                log('ok arp announce', tip)
            continue
        try:
            mid, flags, a = answers_a(m)
        except (IndexError, struct.error, UnicodeDecodeError):
            continue
        if not flags & 0x8000:
            continue
        for n, ip in a:
            if n != me or not ip.startswith('169.254.'):
                continue
            if s is md and not announced:
                announced = True
                log('ok mdns announce', me, ip)
            if s is leg and mid == 0x4c4c and not resolved and ip == ll:
                resolved = True
                log('ok resolved', me, ip)

log('done' if pinged else 'done incomplete', 'address', ll)
