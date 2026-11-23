#!/usr/bin/env python3
"""The testbed's name server, with what Aurora68's tools/pinet.py leaves out: EDNS (an OPT record in
every answer to a query that had one, the size the query asked for logged), negative answers with an
SOA record (so that they may be cached, RFC 2308), answers too large for UDP (TC, then TCP), all
over UDP and TCP on one port on the host. pinet hands the Mac's DNS to it:

    python3 tests/mac/dns_peer.py 5353 &
    PINET_UDP=53=127.0.0.1:5353 PINET_FORWARD=53=127.0.0.1:5353 python3 tools/pinet.py /tmp/net peer &

The names are pinet's (its dns_answer), so the Mac's other tests see the same answers:
  peer.testbed      A 192.168.68.1, AAAA fd68::1        anything.testbed  A 192.168.68.1 (no AAAA)
  v6only.testbed    AAAA fd68::80 (no A)                silent.testbed    never answered
  many.testbed      CNAME many-cdn.testbed: nine AAAA fd68::901 ... that nothing answers, and A
  1.68.168.192.in-addr.arpa  PTR peer.testbed          66.68.168.192.in-addr.arpa  never answered
and two of its own:
  big.testbed       100 A records (192.168.68.1, then 198.51.100.1 ...): 1,612 bytes, more than the
                    Mac's EDNS size (1232) or 512 without EDNS, so over UDP only TC and the question
  slow6.testbed     A 192.168.68.1 at once, AAAA fd68::1 a second later
Everything else is NXDOMAIN, with an SOA (negative TTL 300). Every query is logged:
  "DNS UDP A big.testbed EDNS 1232: truncated (1612 bytes), TC"
SPDX-License-Identifier: GPL-2.0-only
"""
import select
import socket
import struct
import sys
import time

PEER4 = bytes([192, 168, 68, 1])
PEER6 = bytes.fromhex('fd680000000000000000000000000001')
V6ONLY6 = bytes.fromhex('fd680000000000000000000000000080')
TYPES = {1: 'A', 5: 'CNAME', 6: 'SOA', 12: 'PTR', 28: 'AAAA', 41: 'OPT'}


def labels(name):
    out = b''
    for lab in name.split('.'):
        if lab:
            out += bytes([len(lab)]) + lab.encode()
    return out + b'\0'


def rr(owner, rtype, ttl, rdata):
    return owner + struct.pack('>HHIH', rtype, 1, ttl, len(rdata)) + rdata


def soa():
    rd = labels('ns.testbed') + labels('hostmaster.testbed') + struct.pack('>IIIII', 1, 3600, 600, 86400, 300)
    return rr(b'\xc0\x0c', 6, 300, rd)


def answer(q, tcp):
    """the answer to the query q, None for none, and what to log"""
    if len(q) < 17:
        return None, 'short query'
    qid, flags, qd, an, ns, ar = struct.unpack('>HHHHHH', q[:12])
    p, labs = 12, []
    while p < len(q) and q[p]:
        labs.append(q[p + 1:p + 1 + q[p]].decode('ascii', 'replace'))
        p += 1 + q[p]
    p += 1
    if p + 4 > len(q):
        return None, 'bad question'
    qtype = struct.unpack('>H', q[p:p + 2])[0]
    name = '.'.join(labs).lower()
    question = q[12:p + 4]
    edns = None
    if ar and p + 4 + 11 <= len(q) and q[p + 4] == 0 and struct.unpack('>H', q[p + 5:p + 7])[0] == 41:
        edns = struct.unpack('>H', q[p + 7:p + 9])[0]
    what = 'DNS %s %s %s%s' % ('TCP' if tcp else 'UDP', TYPES.get(qtype, qtype), name,
                               ' EDNS %d' % edns if edns is not None else '')
    if name in ('silent.testbed', '66.68.168.192.in-addr.arpa'):
        return None, what + ': not answered'
    ans, auth, rcode = [], [], 0
    if name == 'many.testbed':
        target = labels('many-cdn.testbed')
        ans.append(rr(b'\xc0\x0c', 5, 60, target))
        tref = struct.pack('>H', 0xC000 | (12 + len(question) + 12))
        if qtype == 28:
            for k in range(9):
                ans.append(rr(tref, 28, 60, bytes.fromhex('fd68000000000000000000000000%04x' % (0x901 + k))))
        elif qtype == 1:
            ans.append(rr(tref, 1, 60, PEER4))
    elif name == 'v6only.testbed':
        if qtype == 28:
            ans.append(rr(b'\xc0\x0c', 28, 60, V6ONLY6))
    elif name == 'big.testbed':
        if qtype == 1:
            ans.append(rr(b'\xc0\x0c', 1, 60, PEER4))
            for k in range(1, 100):
                ans.append(rr(b'\xc0\x0c', 1, 60, bytes([198, 51, 100, k])))
    elif name in ('peer.testbed', 'slow6.testbed') and qtype == 28:
        ans.append(rr(b'\xc0\x0c', 28, 60, PEER6))
    elif name == 'testbed' or name.endswith('.testbed'):
        if qtype == 1:
            ans.append(rr(b'\xc0\x0c', 1, 60, PEER4))
    elif qtype == 12 and name == '1.68.168.192.in-addr.arpa':
        ans.append(rr(b'\xc0\x0c', 12, 60, labels('peer.testbed')))
    else:
        rcode = 3
    if not ans:
        auth.append(soa())                    # NXDOMAIN or NODATA: cacheable for 300 s
    opt = [b'\0' + struct.pack('>HHIH', 41, 1232, 0, 0)] if edns is not None else []
    body = b''.join(ans + auth + opt)
    msg = struct.pack('>HHHHHH', qid, 0x8180 | rcode, 1, len(ans), len(auth), len(opt)) + question + body
    limit = 65535 if tcp else (edns if edns is not None and edns >= 512 else 512)
    if len(msg) > limit:
        full = len(msg)
        msg = struct.pack('>HHHHHH', qid, 0x8380 | rcode, 1, 0, 0, len(opt)) + question + b''.join(opt)
        return msg, what + ': truncated (%d bytes), TC' % full
    res = 'NXDOMAIN' if rcode == 3 else '%d answer(s)' % len(ans) if ans else 'NODATA'
    return msg, what + ': %s, %d bytes' % (res, len(msg))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5353
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(('127.0.0.1', port))
    t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(('127.0.0.1', port))
    t.listen(8)
    later = []                              # (when, message, address): slow6.testbed's AAAA
    conns = {}                              # TCP: socket -> bytes so far
    print('dns_peer: UDP and TCP on 127.0.0.1:%d' % port, flush=True)
    while True:
        wait = 0.05 if later else 1.0
        r, _, _ = select.select([u, t] + list(conns), [], [], wait)
        now = time.monotonic()
        for item in list(later):
            if item[0] <= now:
                u.sendto(item[1], item[2])
                later.remove(item)
        for s in r:
            if s is u:
                q, addr = u.recvfrom(4096)
                msg, what = answer(q, False)
                if msg is not None and what.startswith('DNS UDP AAAA slow6.testbed'):
                    later.append((now + 1.0, msg, addr))
                    what += ' (a second late)'
                elif msg is not None:
                    u.sendto(msg, addr)
                print(what, flush=True)
            elif s is t:
                c, _ = t.accept()
                conns[c] = b''
            else:
                d = s.recv(65536)
                if not d:
                    s.close()
                    del conns[s]
                    continue
                conns[s] += d
                b = conns[s]
                while len(b) >= 2 and len(b) >= 2 + struct.unpack('>H', b[:2])[0]:
                    n = struct.unpack('>H', b[:2])[0]
                    msg, what = answer(b[2:2 + n], True)
                    b = b[2 + n:]
                    if msg is not None:
                        s.sendall(struct.pack('>H', len(msg)) + msg)
                    print(what, flush=True)
                conns[s] = b


if __name__ == '__main__':
    main()
