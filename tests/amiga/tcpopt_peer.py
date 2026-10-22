#!/usr/bin/env python3
# Riptide - the Linux end of tests/amiga/run_fsuae_tcpopt.sh (runs inside a
# network namespace with net.ipv4.tcp_fastopen=3):
#   ADDR:8300  TCP Fast Open server: reads a line, answers "OK <line>" and
#              logs whether the line came in the SYN (TCP_INFO
#              tcpi_options & TCPI_OPT_SYN_DATA)
#   ADDR:8301  accepts and stays idle until the peer closes
#
#   tcpopt_peer.py ADDR LOGFILE SECONDS
# SPDX-License-Identifier: GPL-2.0-only
import select
import socket
import sys
import time

addr, logname, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
log = open(logname, "w", buffering=1)


def listener(port, tfo):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if tfo:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_FASTOPEN, 16)
    s.bind((addr, port))
    s.listen(16)
    return s


tfo = listener(8300, True)
idle = listener(8301, False)
conns = {}
n = 0
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select([tfo, idle] + list(conns), [], [], 0.5)
    for s in r:
        if s is tfo or s is idle:
            c, peer = s.accept()
            if s is tfo:
                n += 1
                data = b""
                c.settimeout(10)
                try:
                    while b"\n" not in data:
                        k = c.recv(256)
                        if not k:
                            break
                        data += k
                except OSError:
                    pass
                info = c.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104)
                syn_data = (info[5] & 32) != 0
                log.write("tfo %d syn_data=%d line=%s\n" % (n, syn_data, data.decode(errors="replace").strip()))
                c.sendall(b"OK " + data)
                c.shutdown(socket.SHUT_WR)
                conns[c] = "tfo"
            else:
                log.write("idle connection from %s\n" % peer[0])
                conns[c] = "idle"
        else:
            try:
                k = s.recv(4096)
            except OSError:
                k = b""
            if not k:
                if conns[s] == "idle":
                    log.write("idle connection closed\n")
                del conns[s]
                s.close()
log.write("done\n")
