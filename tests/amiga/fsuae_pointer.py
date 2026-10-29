#!/usr/bin/env python3
# Riptide - drive the Amiga mouse pointer of a headless FS-UAE with xdotool.
#
#   fsuae_pointer.py click X Y [COUNT]   move the pointer to screen pixel X,Y
#                                         and click (COUNT=2: double-click)
#   fsuae_pointer.py move X Y            only move it there (help bubbles)
#   fsuae_pointer.py stable SECS [MAX]   wait until the screen has not changed
#                                         for SECS seconds (at most MAX seconds)
#   fsuae_pointer.py wait SECS MAX X Y [PREV]
#                                         like stable, and pixel X,Y must be a
#                                         button's white edge and the screen must
#                                         differ from screenshot PREV
#   fsuae_pointer.py shot FILE           save a screenshot
#   fsuae_pointer.py drag X Y X2 Y2      press the left button at X,Y, move
#                                         to X2,Y2 with it held, let go
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

# one scratch screenshot per X display, so tests on other displays can run
# at the same time
TMP = os.path.join(tempfile.gettempdir(), "fsuae_pointer_%d_%s.png" %
                   (os.getuid(), os.environ.get("DISPLAY", "").replace(":", "").replace("/", "_")))


def shot(path):
    subprocess.run(["import", "-window", "root", path], check=True)


def pointer_red(r, g, b):
    """The pointer's red: AROS draws it (255, 0, 0)-ish, Kickstart 3.1.4
    (238, 68, 68), close to but not RiptidePrefs' dark-theme red (229, 83, 75)."""
    return (r > 200 and g < 60 and b < 60) or (abs(r - 238) <= 6 and abs(g - 68) <= 6 and abs(b - 68) <= 6)


def find_pointer(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    best = None
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if pointer_red(r, g, b):
                if best is None or (y, x) < (best[1], best[0]):
                    best = (x, y)
    return best


def rel(dx, dy):
    # FS-UAE turns X pointer motion into Amiga mouse motion; an X pointer at
    # the edge of the X screen cannot move further that way, so bring it
    # back to the middle first (the Amiga pointer moves too, and the closed
    # loop in goto() corrects that)
    out = subprocess.run(["xdotool", "getmouselocation", "--shell"], capture_output=True, text=True).stdout
    loc = dict(l.split("=", 1) for l in out.split() if "=" in l)
    x, y = int(loc.get("X", 500)), int(loc.get("Y", 400))
    if x + dx < 20 or y + dy < 20 or x + dx > 980 or y + dy > 740:
        subprocess.run(["xdotool", "mousemove", "512", "384"], check=True)
        time.sleep(0.15)
    subprocess.run(["xdotool", "mousemove_relative", "--", str(dx), str(dy)], check=True)
    time.sleep(0.15)


def goto(tx, ty):
    """Move the pointer onto screen pixel tx,ty; its last position."""
    # screen pixels per X pixel of motion: about 1.6 x 3.2 on the AROS
    # screens (hires, scaled); other modes differ, so each step measures
    # how far the pointer really went and adjusts
    gx, gy = 1.59, 3.18
    p = last = None
    sent = (0, 0)
    for _ in range(16):
        shot(TMP)
        p = find_pointer(TMP)
        if p is None:
            rel(5, 5)
            last = None
            continue
        if last is not None:
            if abs(sent[0]) >= 3 and p[0] != last[0]:
                gx = max(0.5, min(6.0, (p[0] - last[0]) / float(sent[0])))
            if abs(sent[1]) >= 3 and p[1] != last[1]:
                gy = max(0.5, min(6.0, (p[1] - last[1]) / float(sent[1])))
        ex, ey = tx - p[0], ty - p[1]
        if abs(ex) <= 3 and abs(ey) <= 3:
            break
        dx, dy = int(ex / gx), int(ey / gy)
        if dx == 0 and ex:
            dx = 1 if ex > 0 else -1
        if dy == 0 and ey:
            dy = 1 if ey > 0 else -1
        sent, last = (dx, dy), p
        while dx or dy:
            sx, sy = max(-10, min(10, dx)), max(-10, min(10, dy))
            rel(sx, sy)
            dx -= sx
            dy -= sy
        time.sleep(0.3)
    return p


def click(tx, ty, count=1):
    p = goto(tx, ty)
    for _ in range(count):
        subprocess.run(["xdotool", "mousedown", "1"], check=True)
        time.sleep(0.12 if count > 1 else 0.25)
        subprocess.run(["xdotool", "mouseup", "1"], check=True)
        time.sleep(0.12 if count > 1 else 0.3)
    print("%s %d,%d (pointer at %s)" % ("click" if count else "move", tx, ty, p))


def drag(x, y, x2, y2):
    goto(x, y)
    subprocess.run(["xdotool", "mousedown", "1"], check=True)
    time.sleep(0.3)
    p = goto(x2, y2)
    time.sleep(0.3)
    subprocess.run(["xdotool", "mouseup", "1"], check=True)
    time.sleep(0.3)
    print("drag %d,%d to %d,%d (pointer at %s)" % (x, y, x2, y2, p))


def screen_hash(path):
    """Hash of the screen without the (red) pointer."""
    with open(path, "rb") as f:
        im = Image.open(f).convert("RGB")
    d = im.tobytes()
    mask = Image.frombytes("L", im.size, bytes(255 if pointer_red(d[i], d[i + 1], d[i + 2]) else 0
                                                for i in range(0, len(d), 3)))
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
    if len(argv) == 4 and argv[1] == "move":
        click(int(argv[2]), int(argv[3]), 0)
        return 0
    if len(argv) >= 6 and argv[1] == "wait":
        return stable(float(argv[2]), float(argv[3]), (int(argv[4]), int(argv[5])),
                      argv[6] if len(argv) > 6 else None)
    if len(argv) >= 3 and argv[1] == "stable":
        return stable(float(argv[2]), float(argv[3]) if len(argv) > 3 else 120)
    if len(argv) == 6 and argv[1] == "drag":
        drag(int(argv[2]), int(argv[3]), int(argv[4]), int(argv[5]))
        return 0
    if len(argv) == 3 and argv[1] == "shot":
        shot(argv[2])
        return 0
    sys.stderr.write("usage: fsuae_pointer.py click X Y [COUNT] | move X Y | stable SECS [MAX] | "
                     "wait SECS MAX X Y [PREV] | shot FILE | drag X Y X2 Y2\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
