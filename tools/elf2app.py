#!/usr/bin/env python3
"""Riptide on the classic Mac OS: pack an application (linked at 0 with --emit-relocs:
src/mac/app/app.ld) as a MacBinary II file.

    elf2app.py prog.elf --name RiptidePrefs --creator RTPF [--size KB] [--rsrc TYPE:ID:file ...] [--bundle] -o out.bin

CODE 1 holds the self-relocating image (mac_elf.py); src/mac/app/crt0.S fixes it up at the
start. CODE 0 is the jump table with the one entry, the start of CODE 1; below A5 there is room
for QuickDraw's globals. SIZE -1: MultiFinder aware, takes suspend and resume events, runs in
the background, 32-bit clean, the partition --size KB.
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mac_rsrc import ResourceFork, macbinary
from mac_elf import build_image

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('--name', required=True); ap.add_argument('--creator', required=True)
ap.add_argument('--size', type=int, default=1024, help='the partition, KB')
ap.add_argument('--rsrc', action='append', default=[], help='TYPE:ID:file, another resource')
ap.add_argument('--bundle', action='store_true', help="the Finder's hasBundle flag: the program brings a BNDL (its icons, the files it opens)")
ap.add_argument('-o', required=True)
a = ap.parse_args()
img, nrel, end = build_image(a.elf)
code1 = struct.pack('>HH', 0, 1) + img
jt = struct.pack('>H', 0) + b'\x3f\x3c' + struct.pack('>H', 1) + b'\xa9\xf0'      # move.w #1,-(sp); _LoadSeg
code0 = struct.pack('>IIII', 0x20 + len(jt), 0x2000, len(jt), 0x20) + jt
rf = ResourceFork()
rf.add(b'CODE', 0, code0)
rf.add(b'CODE', 1, code1, attrs=0x20)          # preload
rf.add(b'SIZE', -1, struct.pack('>HII', 0x5880, a.size * 1024, a.size * 1024))
for r in a.rsrc:
    t, i, f = r.split(':', 2)
    rf.add(t.encode('mac_roman'), int(i), open(f, 'rb').read())
creator = a.creator.encode('mac_roman')[:4].ljust(4, b' ')
open(a.o, 'wb').write(macbinary(a.name, b'APPL', creator, b'', rf.build(), finder_flags=0x2000 if a.bundle else 0))
print(f"{a.o}: CODE 1 {len(code1)} bytes ({end} image, {nrel} relocations)")
