#!/usr/bin/env python3
# Riptide - a minimal DHCP server for the TAP and FS-UAE tests: one fixed
# address for every client, with classless static routes (option 121,
# RFC 3442) and a domain search list (option 119, RFC 3397, compressed).
# Answers DISCOVER with OFFER and REQUEST with ACK, broadcast on --iface.
# Prints "dhcp: <TYPE> <mac>" for every message it handles.
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import ipaddress
import socket
import struct
import sys

SO_BINDTODEVICE = 25


def ip4(s):
    return socket.inet_aton(s)


def opt(code, data):
    out = b""
    # RFC 3396: long options are split into several instances
    while True:
        out += bytes([code, min(len(data), 255)]) + data[:255]
        data = data[255:]
        if not data:
            return out


def classless(spec):
    """'10.99.0.0/16,10.82.1.1;0.0.0.0/0,10.82.1.1' -> RFC 3442 encoding"""
    out = b""
    for ent in filter(None, spec.split(";")):
        net, gw = ent.split(",")
        n = ipaddress.ip_network(net, strict=False)
        w = n.prefixlen
        out += bytes([w]) + n.network_address.packed[:(w + 7) // 8] + ip4(gw)
    return out


def search(names):
    """RFC 1035 names with compression (offsets within the option)"""
    out = b""
    seen = {}
    for name in filter(None, names.split(",")):
        labels = name.strip(".").split(".")
        for i in range(len(labels)):
            suffix = ".".join(labels[i:]).lower()
            if suffix in seen:
                out += struct.pack("!H", 0xC000 | seen[suffix])
                break
            if len(out) < 0x3FFF:
                seen[suffix] = len(out)
            out += bytes([len(labels[i])]) + labels[i].encode()
        else:
            out += b"\0"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True)
    ap.add_argument("--server", required=True)
    ap.add_argument("--addr", required=True)
    ap.add_argument("--mask", default="255.255.255.0")
    ap.add_argument("--router", default="")
    ap.add_argument("--dns", default="")
    ap.add_argument("--domain", default="")
    ap.add_argument("--classless", default="")
    ap.add_argument("--search", default="")
    ap.add_argument("--lease", type=int, default=3600)
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, a.iface.encode())
    s.bind(("0.0.0.0", 67))
    print("dhcp: ready", flush=True)
    while True:
        m, _ = s.recvfrom(1500)
        if len(m) < 240 or m[0] != 1 or m[236:240] != b"\x63\x82\x53\x63":
            continue
        i, mtype = 240, 0
        while i + 1 < len(m) and m[i] != 255:
            if m[i] == 0:
                i += 1
                continue
            if m[i] == 53:
                mtype = m[i + 2]
            i += 2 + m[i + 1]
        if mtype not in (1, 3):
            continue
        mac = ":".join("%02x" % b for b in m[28:34])
        print("dhcp: %s %s" % ("DISCOVER" if mtype == 1 else "REQUEST", mac), flush=True)
        r = bytearray(240)
        r[0], r[1], r[2] = 2, 1, 6
        r[4:8] = m[4:8]                       # xid
        r[10:12] = m[10:12]                   # flags
        r[16:20] = ip4(a.addr)
        r[20:24] = ip4(a.server)
        r[28:44] = m[28:44]
        r[236:240] = b"\x63\x82\x53\x63"
        o = opt(53, bytes([2 if mtype == 1 else 5]))
        o += opt(54, ip4(a.server)) + opt(51, struct.pack("!I", a.lease))
        o += opt(1, ip4(a.mask))
        if a.classless:
            o += opt(121, classless(a.classless))
        if a.router:
            o += opt(3, ip4(a.router))
        if a.dns:
            o += opt(6, ip4(a.dns))
        if a.domain:
            o += opt(15, a.domain.encode())
        if a.search:
            o += opt(119, search(a.search))
        r += o + b"\xff"
        if len(r) < 300:
            r += bytes(300 - len(r))
        s.sendto(bytes(r), ("255.255.255.255", 68))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
