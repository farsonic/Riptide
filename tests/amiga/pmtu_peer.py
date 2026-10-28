#!/usr/bin/env python3
# Riptide - host side of tests/amiga/run_fsuae_pmtu.sh: a sink that takes
# one upload on ADDR:9000 (saved to DIR/up.bin), an HTTP-like server on
# ADDR:8080 that sends DIR/down.bin, and a client that pulls from the
# Amiga's "nc -l -p 9100" (saved to DIR/pull.bin). Every transfer is
# logged with its duration to DIR/peer.log.
#   python3 -I pmtu_peer.py ADDR AMIGA DIR TIMEOUT
# SPDX-License-Identifier: GPL-2.0-only
import socket
import sys
import threading
import time

addr, amiga, d, timeout = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
lock = threading.Lock()


def log(msg):
    with lock:
        with open(d + "/peer.log", "a") as f:
            f.write(msg + "\n")


def listener(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port))
    s.listen(4)
    return s


def sink():
    s = listener(9000)
    c, _ = s.accept()
    c.settimeout(120)
    t0 = time.time()
    data = bytearray()
    try:
        while True:
            b = c.recv(65536)
            if not b:
                break
            data += b
    except OSError as e:
        log("upload error %s" % e)
    log("upload %d bytes in %.2f s" % (len(data), time.time() - t0))
    with open(d + "/up.bin", "wb") as f:
        f.write(data)
    c.close()


def server():
    s = listener(8080)
    body = open(d + "/down.bin", "rb").read()
    c, _ = s.accept()
    c.settimeout(120)
    t0 = time.time()
    req = b""
    while b"\r\n\r\n" not in req:
        b = c.recv(1024)
        if not b:
            break
        req += b
    try:
        c.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        c.shutdown(socket.SHUT_WR)
        while c.recv(1024):
            pass
    except OSError as e:
        log("download error %s" % e)
    log("download %d bytes in %.2f s" % (len(body), time.time() - t0))
    c.close()


def pull():
    end = time.time() + timeout
    while time.time() < end:
        c = socket.socket()
        c.settimeout(2)
        try:
            c.connect((amiga, 9100))
        except OSError:
            c.close()
            time.sleep(0.5)
            continue
        c.settimeout(120)
        t0 = time.time()
        data = bytearray()
        try:
            while True:
                b = c.recv(65536)
                if not b:
                    break
                data += b
        except OSError as e:
            log("pull error %s" % e)
        log("pull %d bytes in %.2f s" % (len(data), time.time() - t0))
        with open(d + "/pull.bin", "wb") as f:
            f.write(data)
        c.close()
        return


ts = [threading.Thread(target=f, daemon=True) for f in (sink, server, pull)]
for t in ts:
    t.start()
for t in ts:
    t.join(timeout)
