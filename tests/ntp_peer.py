#!/usr/bin/env python3
# Riptide - a host on the simulated Ethernet wire (src/host/wire.c: Ethernet
# frames as AF_UNIX datagrams) for tests/t_ntp_wire.c: ARP, IPv6 neighbour
# discovery, a DHCP server whose lease names an NTP server (option 42), a DNS
# server and several SNTP servers, each address answering in its own way:
#
#   192.0.2.1    good, the clock 100 s ahead (also DHCP and DNS)
#   192.0.2.11   leap indicator 3 (clock not synchronised)
#   192.0.2.12   stratum 16
#   192.0.2.13   transmit time stamp 0
#   192.0.2.14   originate time stamp not the client's
#   192.0.2.15   mode 3 (not a server's answer)
#   192.0.2.20   good, but in NTP era 1: 2036-02-08 00:00:00 UTC when it starts
#   2001:db8::1  good, the clock 42.5 s behind
#
# DNS: time.example A 192.0.2.1; time6.example only AAAA 2001:db8::1.
# Prints one line per query ("ntp 192.0.2.11 li3", "dns A time.example",
# "dhcp ACK") on stdout. Usage: ntp_peer.py MY_SOCKET RIPTIDE_SOCKET
# SPDX-License-Identifier: GPL-2.0-only
import os
import socket
import struct
import sys
import time

MAC = bytes([0x02, 0x00, 0x5e, 0x00, 0x00, 0x01])
BCAST = b"\xff" * 6
SRV = "192.0.2.1"
CLIENT = "192.0.2.2"
NTP_EPOCH = 2208988800
ERA1_START = 2086041600         # Unix time of 2036-02-08 00:00:00 UTC
START = time.time()

V4 = {
    "192.0.2.1": "good",
    "192.0.2.11": "li3",
    "192.0.2.12": "s16",
    "192.0.2.13": "zerotx",
    "192.0.2.14": "badorig",
    "192.0.2.15": "mode3",
    "192.0.2.20": "era1",
}
V6 = {"2001:db8::1": "v6"}
OFFSET = {"good": 100.0, "v6": -42.5}
DNS_A = {"time.example": "192.0.2.1"}
DNS_AAAA = {"time6.example": "2001:db8::1"}


def say(*a):
    print(*a, flush=True)


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip4(s):
    return socket.inet_pton(socket.AF_INET, s)


def ip6(s):
    return socket.inet_pton(socket.AF_INET6, s)


class Wire:
    def __init__(self, me, peer):
        self.peer = peer
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.s.bind(me)

    def send(self, dst, etype, payload):
        try:
            self.s.sendto(dst + MAC + struct.pack("!H", etype) + payload, self.peer)
        except OSError:
            pass                # Riptide's end not there yet: the frame is lost


def udp4(src, dst, sport, dport, data):
    u = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
    ph = ip4(src) + ip4(dst) + struct.pack("!BBH", 0, 17, len(u))
    c = csum(ph + u) or 0xFFFF
    u = u[:6] + struct.pack("!H", c) + u[8:]
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(u), 0, 0, 64, 17, 0, ip4(src), ip4(dst))
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + u


def ip6pkt(src, dst, nh, payload, hlim=64):
    return struct.pack("!IHBB", 0x60000000, len(payload), nh, hlim) + src + dst + payload


def l4sum6(src, dst, nh, data):
    return csum(src + dst + struct.pack("!I3xB", len(data), nh) + data)


def ntp_ts(t):
    sec = int(t // 1)
    frac = int((t - sec) * 4294967296.0) & 0xFFFFFFFF
    return struct.pack("!II", (sec + NTP_EPOCH) & 0xFFFFFFFF, frac)


def ntp_answer(req, mode):
    """the answer of a server behaving as 'mode', or None"""
    if len(req) < 48 or (req[0] & 7) != 3:
        return None
    now = time.time()
    if mode == "era1":
        t = ERA1_START + (now - START)
    else:
        t = now + OFFSET.get(mode, 0.0)
    vn = (req[0] >> 3) & 7
    li, stratum, m = 0, 2, 4
    if mode == "li3":
        li = 3
    if mode == "s16":
        stratum = 16
    if mode == "mode3":
        m = 3
    orig = req[40:48]
    if mode == "badorig":
        orig = bytes(b ^ 0x5A for b in orig)
    xmt = b"\0" * 8 if mode == "zerotx" else ntp_ts(t)
    return (bytes([li << 6 | vn << 3 | m, stratum, req[2], 0xE9]) + struct.pack("!II", 0x10, 0x20) +
            ip4("192.0.2.254") + ntp_ts(t - 30) + orig + ntp_ts(t) + xmt)


def dns_answer(q):
    if len(q) < 17:
        return None, None
    qid, flags, qd = struct.unpack("!HHH", q[:6])
    i, labels = 12, []
    while i < len(q) and q[i]:
        labels.append(q[i + 1:i + 1 + q[i]].decode("ascii", "replace"))
        i += 1 + q[i]
    name = ".".join(labels).lower()
    qtype, qclass = struct.unpack("!HH", q[i + 1:i + 5])
    question = q[12:i + 5]
    answers = []
    if qtype == 1 and name in DNS_A:
        answers.append(struct.pack("!HHHIH", 0xC00C, 1, 1, 60, 4) + ip4(DNS_A[name]))
    if qtype == 28 and name in DNS_AAAA:
        answers.append(struct.pack("!HHHIH", 0xC00C, 28, 1, 60, 16) + ip6(DNS_AAAA[name]))
    known = name in DNS_A or name in DNS_AAAA
    rcode = 0 if known else 3
    hdr = struct.pack("!HHHHHH", qid, 0x8180 | rcode | (flags & 0x0100), 1, len(answers), 0, 0)
    return hdr + question + b"".join(answers), "dns %s %s" % ({1: "A", 28: "AAAA"}.get(qtype, qtype), name)


def dhcp_answer(m):
    if len(m) < 240 or m[0] != 1 or m[236:240] != b"\x63\x82\x53\x63":
        return None, None
    xid, chaddr = m[4:8], m[28:34]
    i, mtype = 240, 0
    while i < len(m) and m[i] != 255:
        if m[i] == 0:
            i += 1
            continue
        if m[i] == 53:
            mtype = m[i + 2]
        i += 2 + m[i + 1]
    reply = {1: 2, 3: 5}.get(mtype)      # DISCOVER -> OFFER, REQUEST -> ACK
    if not reply:
        return None, None
    o = bytes([53, 1, reply]) + bytes([54, 4]) + ip4(SRV)
    o += bytes([1, 4]) + ip4("255.255.255.0") + bytes([3, 4]) + ip4(SRV) + bytes([6, 4]) + ip4(SRV)
    o += bytes([42, 4]) + ip4(SRV) + bytes([51, 4]) + struct.pack("!I", 3600) + b"\xff"
    b = bytes([2, 1, 6, 0]) + xid + b"\0" * 8 + ip4(CLIENT) + ip4(SRV) + b"\0" * 4 + chaddr + b"\0" * 202
    b += b"\x63\x82\x53\x63" + o
    b += b"\0" * max(0, 300 - len(b))
    return b, "dhcp %s" % {2: "OFFER", 5: "ACK"}[reply]


def main():
    me, peer = sys.argv[1], sys.argv[2]
    w = Wire(me, peer)
    say("ready")
    while True:
        f = w.s.recv(2048)
        if len(f) < 14:
            continue
        src_mac, etype, p = f[6:12], struct.unpack("!H", f[12:14])[0], f[14:]
        if etype == 0x0806 and len(p) >= 28 and struct.unpack("!H", p[6:8])[0] == 1:
            tip = socket.inet_ntop(socket.AF_INET, p[24:28])
            if tip in V4:
                w.send(src_mac, 0x0806, p[:6] + struct.pack("!H", 2) + MAC + p[24:28] + p[8:18])
        elif etype == 0x0800 and len(p) >= 28 and p[9] == 17:
            hl = (p[0] & 15) * 4
            src = socket.inet_ntop(socket.AF_INET, p[12:16])
            dst = socket.inet_ntop(socket.AF_INET, p[16:20])
            sport, dport = struct.unpack("!HH", p[hl:hl + 4])
            data = p[hl + 8:]
            if dport == 67:
                ans, what = dhcp_answer(data)
                if ans:
                    say(what)
                    w.send(BCAST, 0x0800, udp4(SRV, "255.255.255.255", 67, 68, ans))
            elif dport == 53 and dst == SRV:
                ans, what = dns_answer(data)
                if ans:
                    say(what)
                    w.send(src_mac, 0x0800, udp4(dst, src, 53, sport, ans))
            elif dport == 123 and dst in V4:
                say("ntp %s %s" % (dst, V4[dst]))
                ans = ntp_answer(data, V4[dst])
                if ans:
                    w.send(src_mac, 0x0800, udp4(dst, src, 123, sport, ans))
        elif etype == 0x86DD and len(p) >= 48:
            nh, src, dst, data = p[6], p[8:24], p[24:40], p[40:]
            if nh == 58 and len(data) >= 24 and data[0] == 135:      # neighbour solicitation
                target = data[8:24]
                if socket.inet_ntop(socket.AF_INET6, target) in V6:
                    to = src if src != b"\0" * 16 else ip6("ff02::1")
                    na = bytes([136, 0, 0, 0]) + struct.pack("!I", 0x60000000) + target + bytes([2, 1]) + MAC
                    c = l4sum6(target, to, 58, na)
                    na = na[:2] + struct.pack("!H", c) + na[4:]
                    w.send(src_mac, 0x86DD, ip6pkt(target, to, 58, na, 255))
            elif nh == 17 and len(data) >= 8:
                sport, dport = struct.unpack("!HH", data[:4])
                d6 = socket.inet_ntop(socket.AF_INET6, dst)
                if dport == 123 and d6 in V6:
                    say("ntp %s %s" % (d6, V6[d6]))
                    ans = ntp_answer(data[8:], V6[d6])
                    if ans:
                        u = struct.pack("!HHHH", 123, sport, 8 + len(ans), 0) + ans
                        c = l4sum6(dst, src, 17, u) or 0xFFFF
                        u = u[:6] + struct.pack("!H", c) + u[8:]
                        w.send(src_mac, 0x86DD, ip6pkt(dst, src, 17, u))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            os.unlink(sys.argv[1])
        except OSError:
            pass
