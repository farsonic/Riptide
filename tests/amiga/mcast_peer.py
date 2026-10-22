#!/usr/bin/env python3
# Riptide - the Linux end of tests/amiga/run_fsuae_mcast.sh.
#
#   mcast_peer.py IFNAME LOCALADDR AMIGAADDR TIMEOUT
#
# Watches the IGMP the Amiga sends (packet socket) and answers its join of
# 239.82.0.1 with a v3 general query (expects the IS_EX record back);
# receives on 239.82.0.2:5402 what the Amiga sends there. Once both
# happened it sends datagrams to 239.82.0.1:5401 (for "nc -u -l -m") every
# half second. Event driven: the emulated Amiga's clock does not keep
# real time. Prints one result line per finding, for the shell script.
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import struct
import sys
import time

ETH_P_IP = 0x0800
PACKET_OUTGOING = 4
IP_RECVTTL = 12
G_RECV, P_RECV = "239.82.0.1", 5401     # the Amiga joins and listens
G_SEND, P_SEND = "239.82.0.2", 5402     # the Amiga sends, Linux listens


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def v3_records(msg):
    n = struct.unpack("!H", msg[6:8])[0]
    p, out = 8, []
    for _ in range(n):
        t, aux, ns = msg[p], msg[p + 1], struct.unpack("!H", msg[p + 2:p + 4])[0]
        out.append((t, socket.inet_ntoa(msg[p + 4:p + 8])))
        p += 8 + 4 * ns + 4 * aux
    return out


def query(local):
    # v3 general query, Max Resp Code 10 (1 s), QRV 2, QQIC 125
    q = bytearray(struct.pack("!BBH4sBBH", 0x11, 10, 0, b"\0" * 4, 2, 125, 0))
    q[2:4] = struct.pack("!H", csum(bytes(q)))
    rs = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_IGMP)
    rs.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local))
    rs.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    rs.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
    rs.sendto(bytes(q), ("224.0.0.1", 0))
    rs.close()
    print("QUERY_SENT", flush=True)


def main():
    ifname, local, amiga, timeout = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    end = time.time() + timeout
    ps = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(ETH_P_IP))
    ps.bind((ifname, ETH_P_IP))
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.setsockopt(socket.IPPROTO_IP, IP_RECVTTL, 1)
    rx.bind((G_SEND, P_SEND))
    rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                  socket.inet_aton(G_SEND) + socket.inet_aton(local))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local))
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    joined = answered = ready = left = False
    next_send = 0
    sent = 0
    print("peer: listening on", ifname, flush=True)
    while time.time() < end:
        now = time.time()
        if answered and ready and now >= next_send and sent < 200:
            tx.sendto(b"multicast to amiga %d\n" % sent, (G_RECV, P_RECV))
            sent += 1
            next_send = now + 0.5
        r, _, _ = select.select([ps, rx], [], [], 0.2)
        if ps in r:
            pkt, addr = ps.recvfrom(2048)
            if addr[2] == PACKET_OUTGOING or len(pkt) < 24 or pkt[9] != 2:
                continue
            if socket.inet_ntoa(pkt[12:16]) != amiga:
                continue
            hl = (pkt[0] & 15) * 4
            ra = hl == 24 and pkt[20:24] == b"\x94\x04\0\0"
            msg = pkt[hl:]
            dst = socket.inet_ntoa(pkt[16:20])
            if msg[0] == 0x22:
                recs = v3_records(msg)
                print("IGMPV3 dst=%s ttl=%d tos=0x%02x ra=%d records=%s" %
                      (dst, pkt[8], pkt[1], ra, " ".join("%d:%s" % x for x in recs)), flush=True)
                if (4, G_RECV) in recs and not joined:
                    joined = True
                    query(local)
                if (2, G_RECV) in recs and joined and not answered:
                    answered = True
                    print("QUERY_ANSWERED", flush=True)
                if (3, G_RECV) in recs and not left:
                    left = True
                    print("LEFT", flush=True)
            else:
                print("IGMP type=0x%02x dst=%s" % (msg[0], dst), flush=True)
        if rx in r:
            data, anc, _, src = rx.recvmsg(2048, 64)
            ttl = -1
            for lvl, typ, val in anc:
                if lvl == socket.IPPROTO_IP and typ == socket.IP_TTL:
                    ttl = struct.unpack("i", val[:4])[0]
            print("GOT_FROM_AMIGA src=%s ttl=%d data=%r" % (src[0], ttl, data.strip()), flush=True)
            ready = True
    print("peer: done, sent %d" % sent, flush=True)


main()
