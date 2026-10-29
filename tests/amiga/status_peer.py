#!/usr/bin/env python3
# Riptide - the host end of tests/amiga/run_fsuae_prefs_status.sh: traffic
# for RiptidePrefs' Status page to show, and timed downloads.
#
#   status_peer.py ADDR SIZE SECONDS LOG
#
#   ADDR:7777  sends SIZE bytes as fast as the Amiga takes them, then
#              closes; logs "dl SECONDS BYTES" per connection
#   ADDR:7779  sends about 100 KB/s for SECONDS (a steady download)
#   ADDR:7780  reads about 50 KB/s and throws it away (a slow upload)
# SPDX-License-Identifier: GPL-2.0-only
import socket
import sys
import threading
import time

addr, size, secs, logpath = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
lock = threading.Lock()
end = time.time() + secs


def log(line):
    with lock:
        with open(logpath, 'a') as f:
            f.write(line + '\n')


def fast(c):
    buf = b'R' * 32768
    t0 = time.time()
    n = 0
    try:
        while n < size:
            n += c.send(buf[:min(len(buf), size - n)])
    except OSError as e:
        log('dl error %s' % e)
    c.close()
    log('dl %.3f %d' % (time.time() - t0, n))


def steady(c):
    buf = b'S' * 2048
    try:
        while time.time() < end:
            c.sendall(buf)
            time.sleep(0.02)
    except OSError:
        pass
    c.close()


def sink(c):
    n = 0
    try:
        while time.time() < end:
            d = c.recv(1024)
            if not d:
                break
            n += len(d)
            time.sleep(0.02)
    except OSError:
        pass
    c.close()
    log('ul %d' % n)


def serve(port, fn):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port))
    s.listen(4)
    s.settimeout(1)
    while time.time() < end:
        try:
            c, _ = s.accept()
        except socket.timeout:
            continue
        threading.Thread(target=fn, args=(c,), daemon=True).start()


for port, fn in ((7777, fast), (7779, steady), (7780, sink)):
    threading.Thread(target=serve, args=(port, fn), daemon=True).start()
while time.time() < end:
    time.sleep(1)
