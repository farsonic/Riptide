#!/usr/bin/env python3
"""Minimal IPv6 router on a Linux interface without kernel IPv6, for testing
an emulated Amiga bridged through pcap: sends Router Advertisements for
fd00:77::/64, answers Neighbour Solicitations and ICMPv6 echo requests for
fd00:77::1 (and its link-local address). Logs what it sees.

usage: tests/amiga/v6responder.py IFACE [seconds]   (root)
SPDX-License-Identifier: GPL-2.0-only"""
import socket, struct, sys, time, select

IF = sys.argv[1]
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 600
ETH_P_ALL, ETH_P_IPV6 = 0x0003, 0x86DD
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
s.bind((IF, 0))
MAC = s.getsockname()[4]
LL = bytes.fromhex('fe80000000000000') + bytes([MAC[0] ^ 2, MAC[1], MAC[2], 0xff, 0xfe, MAC[3], MAC[4], MAC[5]])
GUA = bytes.fromhex('fd000077000000000000000000000001')
PREFIX = bytes.fromhex('fd000077000000000000000000000000')
MINE = {LL, GUA}

def csum(data):
    if len(data) & 1:
        data += b'\0'
    t = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while t >> 16:
        t = (t & 0xffff) + (t >> 16)
    return ~t & 0xffff

def send6(dmac, src, dst, icmp):
    ph = src + dst + struct.pack('!I3xB', len(icmp), 58)
    c = csum(ph + icmp[:2] + b'\0\0' + icmp[4:])
    icmp = icmp[:2] + struct.pack('!H', c) + icmp[4:]
    ip = struct.pack('!IHBB', 6 << 28, len(icmp), 58, 255) + src + dst
    s.send(dmac + MAC + struct.pack('!H', ETH_P_IPV6) + ip + icmp)

def ra():
    # type 134, hop limit 64, flags 0, lifetime 1800, reachable 0, retrans 0
    body = struct.pack('!BBHBBHII', 134, 0, 0, 64, 0, 1800, 0, 0)
    body += struct.pack('!BB6s', 1, 1, MAC)                         # source link-layer address
    body += struct.pack('!BBBBIII', 3, 4, 64, 0xc0, 86400, 14400, 0) + PREFIX   # prefix, L+A
    body += struct.pack('!BBHI', 5, 1, 0, 1500)                     # MTU
    send6(bytes.fromhex('333300000001'), LL, bytes.fromhex('ff020000000000000000000000000001'), body)

seen = set()
end = time.time() + DUR
last_ra = 0
while time.time() < end:
    if time.time() - last_ra > 3:
        ra(); last_ra = time.time()
    r, _, _ = select.select([s], [], [], 0.5)
    if not r:
        continue
    f = s.recv(2048)
    if len(f) < 54 or f[12:14] != b'\x86\xdd' or f[6:12] == MAC:
        continue
    smac, ip = f[6:12], f[14:]
    nh, src, dst, pl = ip[6], ip[8:24], ip[24:40], ip[40:]
    if nh != 58:
        print('v6: next header', nh, socket.inet_ntop(socket.AF_INET6, src), flush=True)
        continue
    t = pl[0]
    key = (t, src)
    if key not in seen:
        seen.add(key)
        print('v6: ICMPv6 type %d from %s to %s' % (t, socket.inet_ntop(socket.AF_INET6, src),
              socket.inet_ntop(socket.AF_INET6, dst)), flush=True)
    if t == 133:                                   # router solicitation
        ra(); last_ra = time.time()
    elif t == 135 and len(pl) >= 24 and pl[8:24] in MINE:   # NS for us
        tgt = pl[8:24]
        na = struct.pack('!BBHI', 136, 0, 0, 0x60000000) + tgt + struct.pack('!BB6s', 2, 1, MAC)
        send6(smac, tgt, src if src != bytes(16) else bytes.fromhex('ff020000000000000000000000000001'), na)
    elif t == 128 and dst in MINE:                 # echo request
        print('v6: echo request seq %d from %s' % (struct.unpack('!H', pl[6:8])[0],
              socket.inet_ntop(socket.AF_INET6, src)), flush=True)
        send6(smac, dst, src, bytes([129, 0, 0, 0]) + pl[4:])
