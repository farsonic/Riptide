#!/usr/bin/env python3
"""Riptide on the classic Mac OS: the Riptide DNR file, the resolver MacTCP programs load.

    mkdnr.py dnrp.elf -o "Riptide DNR.bin"

A MacBinary file of type 'cdev' and creator 'ztcp' (what Apple's DNR glue looks for in the System
Folder and the Control Panels folder) holding 'dnrp' 0: the self-relocating image of
src/mac/dnr (mac_elf.py).
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mac_rsrc import ResourceFork, macbinary
from mac_elf import build_image

ap = argparse.ArgumentParser()
ap.add_argument('elf'); ap.add_argument('-o', required=True)
a = ap.parse_args()
img, nrel, end = build_image(a.elf)
rf = ResourceFork()
rf.add(b'dnrp', 0, img, name='Riptide')
open(a.o, 'wb').write(macbinary('Riptide DNR', b'cdev', b'ztcp', rsrc_fork=rf.build()))
print(f"{a.o}: dnrp {len(img)} bytes ({nrel} relocations)")
