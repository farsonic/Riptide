#!/usr/bin/env python3
"""Host side of tests/amiga/netbench.c: a TCP source and sink.

usage: netbench_host.py ADDRESS PORT

A client sends one line, then:
  "D <n>\\n"  the host sends n bytes and closes (download);
  "U <n>\\n"  the host reads n bytes, answers "OK <n>\\n" and closes (upload).
Each connection is logged with the host-side time and rate.
SPDX-License-Identifier: GPL-2.0-only"""
import socket, sys, threading, time

CHUNK = bytes((ord('a') + i % 26) for i in range(65536))


def serve(conn, peer):
    with conn:
        line = b''
        while not line.endswith(b'\n') and len(line) < 64:
            c = conn.recv(1)
            if not c:
                return
            line += c
        try:
            op, n = line.split()
            n = int(n)
        except ValueError:
            return
        t0 = time.time()
        done = 0
        if op == b'D':
            while done < n:
                k = min(len(CHUNK), n - done)
                conn.sendall(CHUNK[:k])
                done += k
            conn.shutdown(socket.SHUT_WR)
            while conn.recv(4096):      # wait for the client's close
                pass
        elif op == b'U':
            while done < n:
                d = conn.recv(65536)
                if not d:
                    break
                done += len(d)
            conn.sendall(b'OK %d\n' % done)
        dt = time.time() - t0
        print('host: %s %s %d bytes %.3f s %.1f KB/s' % (peer[0], op.decode(), done, dt,
              done / 1024 / dt if dt else 0), flush=True)


def main():
    addr, port = sys.argv[1], int(sys.argv[2])
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port))
    s.listen(8)
    while True:
        conn, peer = s.accept()
        threading.Thread(target=serve, args=(conn, peer), daemon=True).start()


main()
