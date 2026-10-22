#!/usr/bin/env python3
# Riptide - a mock speedtest.net (configuration + server list) and speedtest
# server in one, for testing the speedtest tool without the real service.
# Python 3 standard library only.
#
#   tools/speedtest_mock.py [--bind ADDR] [--port N] [options]
#
# Endpoints (HTTP/1.1, keep-alive):
#   /speedtest-config.php            <settings> with <client ip= lat= lon= isp=/> etc.
#   /speedtest-servers-static.php    a server list: this server (id 1001), a
#   /speedtest-servers.php           slower twin (1002), an unreachable one (1003),
#                                    an ignored one (1004) and --servers far ones
#   /speedtest/latency.txt           "test=test"
#   /speedtest/random<S>x<S>.jpg     S*S*2 random bytes (/ --size-div)
#   /speedtest/upload.php            POST: counts the body, replies "size=N"
#   /slow/...                        the same, 200 ms slower (server 1002)
#   /__stats                         JSON counters; /__reset clears them
#
# The server list points at the host:port the client used (its Host
# header), so it works behind NAT: under FS-UAE/slirp run this on the host
# and on the Amiga
#   speedtest --mirror http://10.0.2.2:8080           (full discovery)
#   speedtest --server-url http://10.0.2.2:8080/speedtest/upload.php
#
# Options for testing the client: --chunked (chunked bodies with odd chunk
# sizes), --redirect (configuration and list via 302), --https-redirect
# (they move to https://, which the client cannot follow), --delay MS (every
# response), --rate KBYTES (per connection), --upload-rate KBYTES (reads
# upload bodies that slowly), --close (no keep-alive),
# --size-div N (smaller images), --servers N (more entries; 5000 makes a
# list of about a megabyte).
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import json
import os
import random
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BLOCK = os.urandom(1 << 20)
STATS_LOCK = threading.Lock()
STATS = {}


def stat(key, n=1):
    with STATS_LOCK:
        STATS[key] = STATS.get(key, 0) + n


def reset_stats():
    with STATS_LOCK:
        STATS.clear()
        STATS.update({"config": 0, "servers": 0, "latency": 0, "download": 0, "download_bytes": 0,
                      "download_complete": 0, "upload": 0, "upload_bytes": 0, "upload_complete": 0,
                      "redirects": 0, "requests": 0, "connections": 0})


reset_stats()


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True
    server_version = "speedtest_mock/1.0"

    def setup(self):
        super().setup()
        stat("connections")

    def log_message(self, fmt, *args):
        if self.server.opts.verbose:
            sys.stderr.write("mock: %s %s\n" % (self.address_string(), fmt % args))

    # ---- helpers
    def host(self):
        h = self.headers.get("Host")
        if not h:
            h = "%s:%d" % self.server.server_address[:2]
        return h

    def head(self, code, ctype, length=None, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-cache")
        if length is not None:
            self.send_header("Content-Length", str(length))
        else:
            self.send_header("Transfer-Encoding", "chunked")
        for k, v in extra:
            self.send_header(k, v)
        if self.server.opts.close:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()

    def write_body(self, data, chunked):
        """data: bytes or an iterable of bytes pieces"""
        rate = self.server.opts.rate * 1024
        sent = 0
        t0 = time.monotonic()
        pieces = [data] if isinstance(data, (bytes, bytearray)) else data
        for piece in pieces:
            pos = 0
            while pos < len(piece):
                n = min(len(piece) - pos, random.choice((1, 7, 100, 1000, 4093, 16384, 65536)) if chunked else 65536)
                part = piece[pos:pos + n]
                if chunked:
                    self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                else:
                    self.wfile.write(part)
                pos += n
                sent += n
                if rate:
                    ahead = sent / rate - (time.monotonic() - t0)
                    if ahead > 0:
                        time.sleep(ahead)
                yield n
        if chunked:
            self.wfile.write(b"0\r\n\r\n")

    def send_doc(self, body, ctype="text/xml"):
        chunked = self.server.opts.chunked
        self.head(200, ctype, None if chunked else len(body))
        for _ in self.write_body(body, chunked):
            pass

    def delay(self, extra_ms=0):
        ms = self.server.opts.delay + extra_ms
        if ms:
            time.sleep(ms / 1000.0)

    # ---- documents
    def config_xml(self):
        o = self.server.opts
        ip = self.client_address[0]
        return ('<?xml version="1.0" encoding="UTF-8"?>\n<settings>\n'
                '<!-- mock configuration; <client> below is the one to read -->\n'
                '<client ip="%s" lat="%s" lon="%s" isp="%s" isprating="3.7" rating="0" '
                'ispdlavg="0" ispulavg="0" loggedin="0" country="AU"/>\n'
                '<server-config threadcount="%d" ignoreids="1004,77777" notonmap="" forcepingid="" preferredserverid=""/>\n'
                '<licensekey>mock</licensekey>\n'
                '<times dl1="5000000" dl2="35000000" dl3="800000000" ul1="1000000" ul2="8400000" ul3="35000000"/>\n'
                '<download testlength="%d" initialtest="250K" mintestsize="250K" threadsperurl="%d"/>\n'
                '<upload testlength="%d" ratio="5" initialtest="0" mintestsize="32K" threads="%d" '
                'maxchunksize="512K" maxchunkcount="%d" threadsperurl="4"/>\n'
                '<latency testlength="10" waittime="50" timeout="20"/>\n'
                '</settings>\n' % (ip, o.lat, o.lon, esc(o.isp), o.threadcount, o.testlength, o.threadsperurl,
                                   o.testlength, o.ul_threads, o.maxchunkcount)).encode()

    def servers_xml(self):
        o = self.server.opts
        h = self.host()
        hostname = h.rsplit(":", 1)[0] if ":" in h else h
        lat, lon = float(o.lat), float(o.lon)

        def srv(id_, dlat, dlon, url, name, sponsor, host, order=0):
            attrs = [("url", url), ("lat", "%.4f" % (lat + dlat)), ("lon", "%.4f" % (lon + dlon)),
                     ("name", name), ("country", "Mockland"), ("cc", "MK"), ("sponsor", sponsor),
                     ("id", str(id_)), ("host", host)]
            if order:
                random.Random(id_).shuffle(attrs)
            q = "'" if id_ % 2 else '"'
            return "<server %s />\n" % " ".join("%s=%s%s%s" % (k, q, esc(v), q) for k, v in attrs)

        parts = ['<?xml version="1.0" encoding="UTF-8"?>\n<settings>\n<servers>\n']
        parts.append(srv(1001, 0.5, 0.0, "http://%s/speedtest/upload.php" % h, "Mock City",
                         "Riptide & Friends", h, order=1))
        parts.append(srv(1002, 0.1, 0.0, "http://%s/slow/upload.php" % h, "Slow Town", "Slow Twin", h))
        parts.append(srv(1003, 0.05, 0.0, "http://%s:1/speedtest/upload.php" % hostname, "Nowhere",
                         "Refused Inc", "%s:1" % hostname))
        parts.append(srv(1004, 0.01, 0.0, "http://%s/speedtest/upload.php" % h, "Ignored", "Ignored Ltd", h))
        rnd = random.Random(42)
        for i in range(o.servers):
            parts.append(srv(2000 + i, rnd.uniform(-60, 60) + 5, rnd.uniform(-180, 180),
                             "http://far%d.invalid:8080/speedtest/upload.php" % i,
                             "Far %d & Away" % i, "Far Sponsor %d" % i, "far%d.invalid:8080" % i, order=i % 3))
        parts.append("</servers>\n</settings>\n")
        return "".join(parts).encode()

    # ---- request handling
    def do_GET(self):
        stat("requests")
        o = self.server.opts
        path = self.path.split("?", 1)[0]
        if path in ("/__stats",):
            with STATS_LOCK:
                body = json.dumps(STATS).encode()
            self.head(200, "application/json", len(body))
            self.wfile.write(body)
            return
        if path == "/__reset":
            reset_stats()
            self.head(200, "text/plain", 3)
            self.wfile.write(b"ok\n")
            return
        if path.startswith("/r/"):
            path = path[2:]
        elif path in ("/speedtest-config.php", "/speedtest-servers-static.php", "/speedtest-servers.php"):
            if o.https_redirect:
                stat("redirects")
                self.head(301, "text/html", 0, [("Location", "https://www.speedtest.net" + self.path)])
                return
            if o.redirect:
                stat("redirects")
                self.head(302, "text/html", 0, [("Location", "/r" + self.path)])
                return
        self.delay()
        if path == "/speedtest-config.php":
            stat("config")
            self.send_doc(self.config_xml())
            return
        if path in ("/speedtest-servers-static.php", "/speedtest-servers.php"):
            if o.no_static and path.endswith("static.php"):
                self.head(404, "text/plain", 0)
                return
            stat("servers")
            self.send_doc(self.servers_xml())
            return
        m = re.match(r"^/(speedtest|slow)/(latency\.txt|random(\d+)x(\d+)\.jpg)$", path)
        if not m:
            self.head(404, "text/plain", 10)
            self.wfile.write(b"not found\n")
            return
        if m.group(1) == "slow":
            self.delay(200)
        if m.group(2) == "latency.txt":
            stat("latency")
            self.head(200, "text/plain", 10)
            self.wfile.write(b"test=test\n")
            return
        s = int(m.group(3))
        size = max(1, s * s * 2 // o.size_div)
        stat("download")
        chunked = o.chunked

        def pieces():
            left = size
            off = 0
            while left:
                n = min(left, len(BLOCK) - off)
                yield BLOCK[off:off + n]
                left -= n
                off = (off + n) % len(BLOCK)

        self.head(200, "image/jpeg", None if chunked else size)
        try:
            for n in self.write_body(pieces(), chunked):
                stat("download_bytes", n)
            stat("download_complete")
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def do_POST(self):
        stat("requests")
        path = self.path.split("?", 1)[0]
        if not re.match(r"^/(speedtest|slow)/upload\.php$", path):
            self.head(404, "text/plain", 0)
            return
        self.delay(200 if path.startswith("/slow") else 0)
        stat("upload")
        n = 0
        try:
            if "chunked" in self.headers.get("Transfer-Encoding", ""):
                while True:
                    line = self.rfile.readline()
                    k = int(line.split(b";")[0], 16)
                    if not k:
                        self.rfile.readline()
                        break
                    while k:
                        d = self.rfile.read(min(k, 65536))
                        if not d:
                            raise ConnectionResetError
                        n += len(d)
                        k -= len(d)
                        stat("upload_bytes", len(d))
                    self.rfile.readline()
            else:
                left = int(self.headers.get("Content-Length", "0"))
                first = True
                urate = self.server.opts.upload_rate * 1024
                t0 = time.monotonic()
                while left:
                    if urate:
                        # read no faster than --upload-rate: the client's
                        # send queue fills, as behind a slow uplink
                        ahead = n / urate - (time.monotonic() - t0)
                        if ahead > 0:
                            time.sleep(ahead)
                    d = self.rfile.read(min(left, 4096 if urate else 65536))
                    if not d:
                        raise ConnectionResetError
                    if first and not d.startswith(b"content1="[:len(d)]):
                        stat("upload_badprefix")
                    first = False
                    n += len(d)
                    left -= len(d)
                    stat("upload_bytes", len(d))
        except (BrokenPipeError, ConnectionResetError, ValueError):
            self.close_connection = True
            return
        stat("upload_complete")
        body = ("size=%d" % n).encode()
        self.head(200, "text/html", len(body))
        self.wfile.write(body)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64

    def handle_error(self, request, client_address):
        if self.opts.verbose:
            super().handle_error(request, client_address)


def main():
    ap = argparse.ArgumentParser(description="mock speedtest.net + speedtest server")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080, help="0: any free port")
    ap.add_argument("--lat", default="-33.8600")
    ap.add_argument("--lon", default="151.2100")
    ap.add_argument("--isp", default="Mock ISP & Co")
    ap.add_argument("--threadcount", type=int, default=2)
    ap.add_argument("--threadsperurl", type=int, default=2)
    ap.add_argument("--ul-threads", type=int, default=2)
    ap.add_argument("--maxchunkcount", type=int, default=20)
    ap.add_argument("--testlength", type=int, default=10)
    ap.add_argument("--servers", type=int, default=5, help="extra far away servers in the list")
    ap.add_argument("--size-div", type=int, default=1, help="divide the image sizes by N")
    ap.add_argument("--chunked", action="store_true", help="chunked transfer coding for documents and images")
    ap.add_argument("--redirect", action="store_true", help="302 for the configuration and server lists")
    ap.add_argument("--https-redirect", action="store_true", help="301 to https:// for them")
    ap.add_argument("--no-static", action="store_true", help="404 for speedtest-servers-static.php")
    ap.add_argument("--delay", type=int, default=0, help="milliseconds before every response")
    ap.add_argument("--rate", type=int, default=0, help="KB/s per connection for images (0: unlimited)")
    ap.add_argument("--upload-rate", type=int, default=0,
                    help="KB/s per connection read from upload bodies (0: unlimited)")
    ap.add_argument("--close", action="store_true", help="Connection: close on every response")
    ap.add_argument("-v", "--verbose", action="store_true")
    o = ap.parse_args()
    o.size_div = max(1, o.size_div)
    srv = Server((o.bind, o.port), Handler)
    srv.opts = o
    host, port = srv.server_address[:2]
    print("speedtest_mock: listening on http://%s:%d/" % (host, port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
