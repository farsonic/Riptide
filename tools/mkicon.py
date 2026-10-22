#!/usr/bin/env python3
# Riptide - write classic Workbench icons (.info files, OS 2.x/3.x format).
#
#   tools/mkicon.py project OUT.info [--tool T] [--tooltype S]... [--stack N] [--image install|doc]
#   tools/mkicon.py tool    OUT.info [--tooltype S]... [--stack N] [--image prefs|install]
#   tools/mkicon.py drawer  OUT.info [--image drawer|docs] [--window X,Y,W,H]
#                   (both also take --pos X,Y: the icon's place in its window)
#   tools/mkicon.py dump    FILE.info        parse a .info file and print it
#
# The format is the one icon.library's PutDiskObject() writes (see the NDK's
# workbench/workbench.h and intuition/intuition.h):
#
#   struct DiskObject       78 bytes (magic 0xE310, version 1, a Gadget,
#                           do_Type, pointers written as 0 / non-0 flags)
#   struct OldDrawerData    56 bytes, drawers only (NewWindow + CurrentX/Y)
#   struct Image            20 bytes + planar image data (16-bit aligned rows)
#   [second Image]          when do_Gadget.SelectRender != 0
#   DefaultTool             ULONG length (with NUL) + text
#   ToolTypes               ULONG (count+1)*4, then each as length + text
#   [ToolWindow]            not written by this tool
#   dd_Flags, dd_ViewModes  6 bytes, drawers with Gadget.UserData revision 1
#
# The images use 2 bitplanes and the standard Workbench 2/3 palette:
# 0 grey, 1 black, 2 white, 3 blue. Selected state is complemented
# (GFLG_GADGHCOMP), so only one image is stored.
# SPDX-License-Identifier: GPL-2.0-only
import math
import struct
import sys

WB_DISKMAGIC = 0xE310
WB_DISKVERSION = 1
WB_DISKREVISION = 1
WBDISK, WBDRAWER, WBTOOL, WBPROJECT, WBGARBAGE = 1, 2, 3, 4, 5
TYPE_NAMES = {1: "DISK", 2: "DRAWER", 3: "TOOL", 4: "PROJECT", 5: "GARBAGE", 6: "DEVICE", 7: "KICK", 8: "APPICON"}
NO_ICON_POSITION = 0x80000000
GFLG_GADGIMAGE = 0x0004
GFLG_GADGHCOMP = 0x0000
GFLG_GADGHIMAGE = 0x0002
GACT_RELVERIFY = 0x0001
GACT_IMMEDIATE = 0x0002
GTYP_BOOLGADGET = 0x0001
WBENCHSCREEN = 0x0001

DISKOBJECT_SIZE = 78
OLDDRAWERDATA_SIZE = 56
IMAGE_SIZE = 20

GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3


# ---------------------------------------------------------------- images --

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[GREY] * w for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1):
            self.put(x, y0, c)
            self.put(x, y1, c)
        for y in range(y0, y1 + 1):
            self.put(x0, y, c)
            self.put(x1, y, c)

    def hline(self, x0, x1, y, c):
        for x in range(x0, x1 + 1):
            self.put(x, y, c)

    def wave(self, x0, x1, ymid, amp, periods, c, thick=2, crest=None):
        """A sine wave from x0 to x1, filled below down to ymid+amp+thick."""
        prev = None
        for x in range(x0, x1 + 1):
            t = (x - x0) / max(1, (x1 - x0))
            y = int(round(ymid - amp * math.sin(t * periods * 2 * math.pi)))
            for k in range(thick):
                self.put(x, y + k, c)
            if prev is not None and abs(y - prev) > 1:
                for yy in range(min(y, prev), max(y, prev)):
                    self.put(x, yy, c)
            if crest is not None:
                self.put(x, y - 1, crest)
            prev = y

    def planes(self, depth=2):
        """Planar data: plane 0 first, rows padded to 16 bits."""
        words = (self.w + 15) // 16
        out = bytearray()
        for p in range(depth):
            for y in range(self.h):
                row = bytearray(words * 2)
                for x in range(self.w):
                    if (self.px[y][x] >> p) & 1:
                        row[x >> 3] |= 0x80 >> (x & 7)
                out += row
        return bytes(out)


def image_install():
    """An open box (the package) with a blue wave rolling out of it."""
    c = Canvas(48, 24)
    # box front
    c.rect(8, 11, 39, 22, WHITE)
    c.frame(8, 11, 39, 22, BLACK)
    # side shading and flaps
    c.rect(36, 12, 38, 21, GREY)
    c.hline(4, 12, 7, BLACK)
    for i in range(4):
        c.put(4 + i, 7 + i, BLACK)
        c.put(43 - i, 7 + i, BLACK)
    c.hline(35, 43, 7, BLACK)
    c.rect(5, 8, 10, 9, WHITE)
    c.rect(37, 8, 42, 9, WHITE)
    # the wave
    c.wave(2, 45, 6, 3, 1.5, BLUE, thick=3, crest=WHITE)
    c.rect(14, 15, 33, 18, BLUE)
    c.hline(14, 33, 15, WHITE)
    return c


def image_doc():
    """A page of text with a small wave at the top."""
    c = Canvas(32, 22)
    c.rect(6, 1, 25, 20, WHITE)
    c.frame(6, 1, 25, 20, BLACK)
    c.rect(21, 1, 25, 4, GREY)
    for i in range(5):
        c.put(21 + i, 1 + i, BLACK)
    c.wave(8, 19, 4, 1, 1.0, BLUE, thick=2)
    for y in (8, 11, 14, 17):
        c.hline(9, 22 if y != 17 else 17, y, BLACK)
    return c


def image_drawer(label_wave=True):
    """A Workbench style drawer with a wave on its front."""
    c = Canvas(56, 24)
    c.rect(2, 2, 53, 21, GREY)
    c.frame(2, 2, 53, 21, BLACK)
    c.hline(3, 52, 3, WHITE)
    for y in range(3, 21):
        c.put(3, y, WHITE)
    c.hline(2, 53, 22, BLACK)
    # drawer front
    c.frame(6, 7, 49, 19, BLACK)
    c.hline(7, 48, 8, WHITE)
    # handle
    c.rect(22, 14, 33, 16, BLACK)
    c.hline(23, 32, 15, WHITE)
    if label_wave:
        c.wave(9, 46, 11, 2, 2.0, BLUE, thick=2)
    return c


def image_prefs():
    """A preferences window: title bar, two input fields, a wave below."""
    c = Canvas(40, 24)
    c.rect(2, 1, 37, 21, WHITE)
    c.frame(2, 1, 37, 21, BLACK)
    c.rect(3, 2, 36, 4, BLUE)
    c.hline(3, 36, 5, BLACK)
    for y in (8, 13):
        c.hline(6, 12, y + 1, BLACK)
        c.frame(15, y, 33, y + 3, BLACK)
        c.hline(16, 32, y + 1, GREY)
    c.wave(4, 35, 18, 1, 1.5, BLUE, thick=2)
    c.hline(3, 38, 22, BLACK)
    return c


IMAGES = {"install": image_install, "doc": image_doc, "drawer": image_drawer,
          "docs": lambda: image_drawer(True), "prefs": image_prefs}


# ---------------------------------------------------------------- writer --

def ulong_str(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">L", len(b)) + b


def build(kind, img, tool=None, tooltypes=(), stack=0, window=(50, 50, 400, 150), pos=None):
    depth = 2
    data = img.planes(depth)
    is_drawer = kind in (WBDRAWER, WBDISK, WBGARBAGE)
    gadget = struct.pack(
        ">LhhhhHHHLLLLLHL",
        0,                       # NextGadget
        0, 0,                    # LeftEdge, TopEdge
        img.w, img.h,            # Width, Height
        GFLG_GADGIMAGE | GFLG_GADGHCOMP,
        GACT_RELVERIFY | GACT_IMMEDIATE,
        GTYP_BOOLGADGET,
        1,                       # GadgetRender (an image follows)
        0,                       # SelectRender (none: complement)
        0, 0, 0,                 # GadgetText, MutualExclude, SpecialInfo
        0,                       # GadgetID
        WB_DISKREVISION)         # UserData: icon revision
    assert len(gadget) == 44
    dobj = struct.pack(">HH", WB_DISKMAGIC, WB_DISKVERSION) + gadget + struct.pack(
        ">BxLLLLLLl",
        kind,
        1 if tool else 0,
        1 if tooltypes else 0,
        pos[0] if pos else NO_ICON_POSITION, pos[1] if pos else NO_ICON_POSITION,
        1 if is_drawer else 0,
        0,                       # ToolWindow
        stack)
    assert len(dobj) == DISKOBJECT_SIZE, len(dobj)
    out = bytearray(dobj)
    if is_drawer:
        x, y, w, h = window
        newwin = struct.pack(">hhhhBBLLLLLLLhhHHH",
                             x, y, w, h, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0,
                             90, 40, 0xFFFF, 0xFFFF, WBENCHSCREEN)
        assert len(newwin) == 48
        out += newwin + struct.pack(">ll", 0, 0)
    # struct Image: LeftEdge, TopEdge, Width, Height, Depth, ImageData,
    # PlanePick, PlaneOnOff, NextImage
    out += struct.pack(">hhhhhLBBL", 0, 0, img.w, img.h, depth, 1, (1 << depth) - 1, 0, 0)
    out += data
    if tool:
        out += ulong_str(tool)
    if tooltypes:
        out += struct.pack(">L", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            out += ulong_str(t)
    if is_drawer:
        out += struct.pack(">LH", 0, 0)    # dd_Flags, dd_ViewModes (default)
    return bytes(out)


# ---------------------------------------------------------------- reader --

class IconError(Exception):
    pass


def parse(buf):
    """Parse a .info file; returns a dict. Raises IconError on bad data."""
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(buf):
            raise IconError("truncated at offset %d (need %d bytes)" % (pos, n))
        b = buf[pos:pos + n]
        pos += n
        return b

    def string():
        (n,) = struct.unpack(">L", take(4))
        if n == 0 or n > 4096:
            raise IconError("bad string length %d" % n)
        s = take(n)
        if s[-1] != 0:
            raise IconError("string not NUL terminated")
        return s[:-1].decode("latin-1")

    magic, version = struct.unpack(">HH", take(4))
    if magic != WB_DISKMAGIC:
        raise IconError("bad magic 0x%04x" % magic)
    g = struct.unpack(">LhhhhHHHLLLLLHL", take(44))
    kind, tool, tts, cx, cy, ddata, twin, stack = struct.unpack(">BxLLLLLLl", take(30))
    d = {"version": version, "type": TYPE_NAMES.get(kind, kind), "width": g[3], "height": g[4],
         "flags": g[5], "revision": g[14] & 0xFF, "x": cx, "y": cy, "stack": stack}
    if ddata:
        nw = struct.unpack(">hhhhBBLLLLLLLhhHHH", take(48))
        d["window"] = nw[:4]
        take(8)
    imgs = []
    for render in (g[8], g[9]):
        if not render:
            continue
        le, te, w, h, depth, idata, pick, onoff, nxt = struct.unpack(">hhhhhLBBL", take(20))
        if not (0 < w <= 1024 and 0 < h <= 1024 and 0 < depth <= 8):
            raise IconError("bad image %dx%dx%d" % (w, h, depth))
        size = ((w + 15) // 16) * 2 * h * depth
        imgs.append({"w": w, "h": h, "depth": depth, "data": take(size)})
    d["images"] = imgs
    if tool:
        d["default_tool"] = string()
    if tts:
        (n,) = struct.unpack(">L", take(4))
        if n % 4 or n < 4:
            raise IconError("bad tooltype count %d" % n)
        d["tooltypes"] = [string() for _ in range(n // 4 - 1)]
    if twin:
        d["tool_window"] = string()
    if ddata and d["revision"] >= 1:
        if len(buf) - pos >= 6:
            d["dd_flags"], d["dd_viewmodes"] = struct.unpack(">LH", take(6))
    if pos != len(buf):
        raise IconError("%d trailing bytes" % (len(buf) - pos))
    return d


def ascii_art(img):
    w, h, depth, data = img["w"], img["h"], img["depth"], img["data"]
    words = (w + 15) // 16
    rows = []
    for y in range(h):
        line = ""
        for x in range(w):
            c = 0
            for p in range(depth):
                b = data[(p * h + y) * words * 2 + (x >> 3)]
                if b & (0x80 >> (x & 7)):
                    c |= 1 << p
            line += ".#ob"[c] if depth == 2 else "%x" % c
        rows.append(line)
    return "\n".join(rows)


# ------------------------------------------------------------------ main --

def main(argv):
    if len(argv) < 3 or argv[1] not in ("project", "drawer", "tool", "dump"):
        sys.stderr.write("usage: mkicon.py project|drawer|tool OUT.info [options] | dump FILE.info\n")
        return 2
    cmd, path = argv[1], argv[2]
    if cmd == "dump":
        with open(path, "rb") as f:
            d = parse(f.read())
        for k in ("type", "version", "revision", "width", "height", "x", "y", "stack", "window",
                  "default_tool", "tooltypes", "dd_flags", "dd_viewmodes"):
            if k in d:
                print("%-13s %s" % (k, d[k]))
        for i, im in enumerate(d["images"]):
            print("image %d: %dx%d, %d planes" % (i, im["w"], im["h"], im["depth"]))
            print(ascii_art(im))
        return 0
    tool, tooltypes, stack, image, window, pos = None, [], 0, None, (50, 50, 400, 150), None
    i = 3
    while i < len(argv):
        a = argv[i]
        v = argv[i + 1] if i + 1 < len(argv) else None
        if a == "--tool":
            tool = v
        elif a == "--tooltype":
            tooltypes.append(v)
        elif a == "--stack":
            stack = int(v)
        elif a == "--image":
            image = v
        elif a == "--pos":
            pos = tuple(int(n) for n in v.split(","))
        elif a == "--window":
            window = tuple(int(n) for n in v.split(","))
        else:
            sys.stderr.write("mkicon.py: unknown option %s\n" % a)
            return 2
        i += 2
    kind = {"project": WBPROJECT, "drawer": WBDRAWER, "tool": WBTOOL}[cmd]
    if image is None:
        image = {"project": "doc", "drawer": "drawer", "tool": "install"}[cmd]
    buf = build(kind, IMAGES[image](), tool, tooltypes, stack, window, pos)
    parse(buf)  # self check
    with open(path, "wb") as f:
        f.write(buf)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
