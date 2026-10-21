#!/usr/bin/env python3
# Riptide - drive the Amiga mouse pointer of a headless FS-UAE with xdotool.
#
#   fsuae_pointer.py click X Y [COUNT]   move the pointer to screen pixel X,Y
#                                         and click (COUNT=2: double-click)
#   fsuae_pointer.py stable SECS [MAX]   wait until the screen has not changed
#                                         for SECS seconds (at most MAX seconds)
#   fsuae_pointer.py wait SECS MAX X Y [PREV]
#                                         like stable, and pixel X,Y must be a
#                                         button's white edge and the screen must
#                                         differ from screenshot PREV
#   fsuae_pointer.py shot FILE           save a screenshot
#
# FS-UAE without input grab turns X pointer motion into relative Amiga mouse
# motion, so absolute X coordinates do not map. The pointer is found in a
# screenshot instead (the red Workbench pointer) and moved in small relative
# steps until it is on target (closed loop). Needs DISPLAY, xdotool, ImageMagick
# "import" and Python PIL.
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import os
import subprocess
import sys
import tempfile
import time

from PIL import Image

TMP = os.path.join(tempfile.gettempdir(), "fsuae_pointer_%d.png" % os.getuid())


def shot(path):
    subprocess.run(["import", "-window", "root", path], check=True)


def find_pointer(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    best = None
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r > 200 and g < 60 and b < 60:
                if best is None or (y, x) < (best[1], best[0]):
                    best = (x, y)
    return best


def rel(dx, dy):
    subprocess.run(["xdotool", "mousemove_relative", "--", str(dx), str(dy)], check=True)
    time.sleep(0.15)


def click(tx, ty, count=1):
    p = None
    for _ in range(10):
        shot(TMP)
        p = find_pointer(TMP)
        if p is None:
            rel(5, 5)
            continue
        ex, ey = tx - p[0], ty - p[1]
        if abs(ex) <= 3 and abs(ey) <= 3:
            break
        # about 1.6 x 3.2 screen pixels per X pixel of motion (hires, scaled)
        dx, dy = int(ex / 1.59), int(ey / 3.18)
        if dx == 0 and ex:
            dx = 1 if ex > 0 else -1
        if dy == 0 and ey:
            dy = 1 if ey > 0 else -1
        while dx or dy:
            sx, sy = max(-10, min(10, dx)), max(-10, min(10, dy))
            rel(sx, sy)
            dx -= sx
            dy -= sy
        time.sleep(0.3)
    for _ in range(count):
        subprocess.run(["xdotool", "mousedown", "1"], check=True)
        time.sleep(0.12 if count > 1 else 0.25)
        subprocess.run(["xdotool", "mouseup", "1"], check=True)
        time.sleep(0.12 if count > 1 else 0.3)
    print("click %d,%d (pointer at %s)" % (tx, ty, p))


def screen_hash(path):
    """Hash of the screen without the (red) pointer."""
    with open(path, "rb") as f:
        im = Image.open(f).convert("RGB")
    r, g, b = im.split()
    red = Image.eval(r, lambda v: 255 if v > 200 else 0)
    notgreen = Image.eval(g, lambda v: 255 if v < 60 else 0)
    mask = Image.composite(red, Image.new("L", im.size, 0), notgreen)
    im.paste((0, 0, 0), mask=mask)
    return hashlib.md5(im.tobytes()).hexdigest()


def stable(secs, maxwait, edge=None, prev=None):
    """Wait for a settled screen; 0 when found, 1 on timeout."""
    t0 = time.time()
    last, since = None, time.time()
    prevh = screen_hash(prev) if prev and os.path.exists(prev) else None
    while time.time() - t0 < maxwait:
        shot(TMP)
        h = screen_hash(TMP)
        if h != last:
            last, since = h, time.time()
        elif time.time() - since >= secs and h != prevh:
            if edge is None:
                return 0
            r, g, b = Image.open(TMP).convert("RGB").getpixel(edge)
            if r > 220 and g > 220 and b > 220:
                return 0
        time.sleep(0.5)
    return 1


def main(argv):
    if len(argv) >= 4 and argv[1] == "click":
        click(int(argv[2]), int(argv[3]), int(argv[4]) if len(argv) > 4 else 1)
        return 0
    if len(argv) >= 6 and argv[1] == "wait":
        return stable(float(argv[2]), float(argv[3]), (int(argv[4]), int(argv[5])),
                      argv[6] if len(argv) > 6 else None)
    if len(argv) >= 3 and argv[1] == "stable":
        return stable(float(argv[2]), float(argv[3]) if len(argv) > 3 else 120)
    if len(argv) == 3 and argv[1] == "shot":
        shot(argv[2])
        return 0
    sys.stderr.write("usage: fsuae_pointer.py click X Y [COUNT] | stable SECS [MAX] | "
                     "wait SECS MAX X Y [PREV] | shot FILE\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
