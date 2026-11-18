#!/usr/bin/env python3
"""Riptide on the classic Mac OS: turn the linked driver (ELF, linked at 0 with --emit-relocs) into
a self-relocating 'DRVR' image, and pack the Riptide extension file.

    elf2mac.py drvr.elf init.bin [riptide.conf] -o Riptide.bin

The image is .text, .data and the zero-filled .bss up to _image_end; after it come a long count
and the offsets of every 32-bit absolute field (R_68K_32 against a section-relative symbol), and
_reloc_off in the driver header gets the table's offset. drvr.S adds the load address to each
field at the first Open. Any other absolute relocation type is an error: the code must be built
-fno-pic with 32-bit absolute or PC-relative references only.

The output is a MacBinary II file (copy it to an HFS volume with `hcopy -m`): type 'INIT',
creator 'RIPT', resources INIT 128 (the installer, init.S), DRVR 128 (the stack), sysz 0 (the
system heap it needs at startup) and, if given,
TEXT 128 (riptide.conf, with Mac line ends).
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mac_rsrc import ResourceFork, macbinary
from mac_elf import build_image

ap = argparse.ArgumentParser()
ap.add_argument('drvr_elf'); ap.add_argument('init_bin'); ap.add_argument('conf', nargs='?')
ap.add_argument('-o', required=True); ap.add_argument('--raw', help='also write the bare DRVR image here')
a = ap.parse_args()
img, nrel, end = build_image(a.drvr_elf)
if a.raw:
    open(a.raw, 'wb').write(img)
rf = ResourceFork()
rf.add(b'INIT', 128, open(a.init_bin, 'rb').read(), name='Riptide', attrs=0x50)    # sysHeap, locked
rf.add(b'DRVR', 128, img, name='Riptide', attrs=0x40)                              # sysHeap
# 'sysz' 0: the system heap the INIT needs, grown by System 7's startup code before INITs run
# (the driver, the stack's 1 MB pool, the Ethernet rings, its own stack, and some slack)
rf.add(b'sysz', 0, struct.pack('>I', len(img) + 1024 * 1024 + 96 * 1024 + 256 * 1024))
if a.conf:
    rf.add(b'TEXT', 128, open(a.conf, 'rb').read().replace(b'\r\n', b'\r').replace(b'\n', b'\r') + b'\0', name='riptide.conf')
open(a.o, 'wb').write(macbinary('Riptide', b'INIT', b'RIPT', rsrc_fork=rf.build()))
print(f"{a.o}: DRVR {len(img)} bytes ({end} image, {nrel} relocations)")
