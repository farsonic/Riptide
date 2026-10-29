#!/usr/bin/env python3
# Riptide - the far end for stackprobe PEER=host: TCP and UDP echo on
# 7007, TCP discard on 7009 (closes once the client has finished sending),
# TCP chargen on 7019. Port 7099 stays closed (stackprobe expects a refusal).
# SPDX-License-Identifier: GPL-2.0-only
#
#   tests/amiga/stackprobe_peer.py [--bind ADDRESS] [--time SECONDS]
import argparse
import socketserver
import threading
import time

ECHO, DISCARD, CHARGEN = 7007, 7009, 7019
PATTERN = bytes(range(32, 127)) * 100


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            d = self.request.recv(65536)
            if not d:
                return
            self.request.sendall(d)


class Discard(socketserver.BaseRequestHandler):
    def handle(self):
        while self.request.recv(65536):
            pass


class Chargen(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            while True:
                self.request.sendall(PATTERN)
        except OSError:
            pass


class UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(data, self.client_address)


class TCP(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--time', type=float, default=0, help='stop after this many seconds (0: never)')
    a = ap.parse_args()
    servers = [TCP((a.bind, ECHO), Echo), TCP((a.bind, DISCARD), Discard), TCP((a.bind, CHARGEN), Chargen),
               socketserver.ThreadingUDPServer((a.bind, ECHO), UdpEcho)]
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    print('stackprobe_peer: echo %d (TCP, UDP), discard %d, chargen %d on %s' % (ECHO, DISCARD, CHARGEN, a.bind),
          flush=True)
    try:
        if a.time:
            time.sleep(a.time)
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    for s in servers:
        s.shutdown()


if __name__ == '__main__':
    main()
