#!/usr/bin/env python3
# Riptide - the far end of a VLAN for tests/amiga/run_fsuae_vlan.sh, on a
# Linux interface without 802.1Q support in the kernel: a packet socket
# that answers, on one VLAN id, ARP requests and pings for its address and
# UDP datagrams to port 7 (echo). The tag of a received frame is read from
# the frame or, where the kernel moved it into the packet's metadata, from
# PACKET_AUXDATA. Replies carry the same id with priority 0.
#
#   vlan_peer.py IFACE VID ADDR LOGFILE SECONDS
#
# The log has a line per answered request ("arp", "ping SIZE", "udp SIZE")
# with the priority (PCP) of the request's tag.
# SPDX-License-Identifier: GPL-2.0-only
import socket
import struct
import sys
import time

SOL_PACKET = 263
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
ETH_P_ALL = 3


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def main():
    ifname, vid, addr, logfile, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], float(sys.argv[5])
    me = socket.inet_aton(addr)
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
    s.bind((ifname, ETH_P_ALL))
    mymac = s.getsockname()[4][:6]
    s.settimeout(0.5)
    log = open(logfile, "w", buffering=1)
    end = time.time() + secs

    def send(dst, ethertype, payload):
        tci = vid & 0xfff
        s.send(dst + mymac + struct.pack("!HHH", 0x8100, tci, ethertype) + payload)

    while time.time() < end:
        try:
            data, anc, _flags, sa = s.recvmsg(4096, socket.CMSG_SPACE(64))
        except socket.timeout:
            continue
        if sa[2] == socket.PACKET_OUTGOING or len(data) < 14:
            continue
        tag = None
        for level, typ, cdata in anc:
            if level == SOL_PACKET and typ == PACKET_AUXDATA and len(cdata) >= 20:
                status = struct.unpack_from("I", cdata, 0)[0]
                tci = struct.unpack_from("H", cdata, 16)[0]
                if status & TP_STATUS_VLAN_VALID:
                    tag = tci
        src = data[6:12]
        et = struct.unpack("!H", data[12:14])[0]
        off = 14
        if et == 0x8100:
            tag = struct.unpack("!H", data[14:16])[0]
            et = struct.unpack("!H", data[16:18])[0]
            off = 18
        if tag is None or (tag & 0xfff) != vid:
            continue
        pcp = tag >> 13
        p = data[off:]
        if et == 0x0806 and len(p) >= 28 and struct.unpack("!H", p[6:8])[0] == 1 and p[24:28] == me:
            reply = p[:6] + struct.pack("!H", 2) + mymac + me + p[8:14] + p[14:18]
            send(src, 0x0806, reply)
            log.write("arp pcp %d\n" % pcp)
        elif et == 0x0800 and len(p) >= 20 and p[16:20] == me:
            hl = (p[0] & 15) * 4
            tot = struct.unpack("!H", p[2:4])[0]
            ip, body = p[:hl], p[hl:tot]
            proto = ip[9]
            if proto == 1 and body[:1] == b"\x08":
                icmp = b"\0\0\0\0" + body[4:]
                icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
                rip = bytearray(ip)
                rip[12:16], rip[16:20] = ip[16:20], ip[12:16]
                rip[8] = 64
                rip[10:12] = b"\0\0"
                rip[10:12] = struct.pack("!H", csum(bytes(rip)))
                send(src, 0x0800, bytes(rip) + icmp)
                log.write("ping %d pcp %d\n" % (tot, pcp))
            elif proto == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 7:
                sport, dport = struct.unpack("!HH", body[:4])
                payload = body[8:]
                udp = struct.pack("!HHHH", 7, sport, 8 + len(payload), 0) + payload
                rip = bytearray(ip[:20])
                rip[0] = 0x45
                rip[2:4] = struct.pack("!H", 20 + len(udp))
                rip[6:8] = b"\0\0"
                rip[12:16], rip[16:20] = ip[16:20], ip[12:16]
                rip[8] = 64
                rip[10:12] = b"\0\0"
                rip[10:12] = struct.pack("!H", csum(bytes(rip)))
                send(src, 0x0800, bytes(rip) + udp)
                log.write("udp %d pcp %d\n" % (len(payload), pcp))


if __name__ == "__main__":
    main()
