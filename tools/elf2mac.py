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

R_68K_32 = 1
SHN_ABS = 0xfff1

def elf_sections(d):
    assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 2, "not a 32-bit big-endian ELF"
    shoff, = struct.unpack('>I', d[32:36])
    shentsize, shnum, shstrndx = struct.unpack('>HHH', d[46:52])
    secs = []
    for i in range(shnum):
        f = struct.unpack('>IIIIIIIIII', d[shoff + i * shentsize: shoff + i * shentsize + 40])
        secs.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3], off=f[4], size=f[5], link=f[6], info=f[7], entsize=f[9]))
    strtab = secs[shstrndx]
    for s in secs:
        n = d[strtab['off'] + s['name']:]
        s['name'] = n[:n.index(b'\0')].decode()
    return secs

def symbols(d, secs):
    st = next(s for s in secs if s['type'] == 2)
    strs = secs[st['link']]
    out = []
    for i in range(st['size'] // 16):
        nm, val, sz, info, oth, shndx = struct.unpack('>IIIBBH', d[st['off'] + 16 * i: st['off'] + 16 * i + 16])
        n = d[strs['off'] + nm:]
        out.append((n[:n.index(b'\0')].decode(), val, shndx))
    return out

def build_image(elf):
    d = open(elf, 'rb').read()
    secs = elf_sections(d)
    syms = symbols(d, secs)
    sym = {n: v for n, v, _ in syms}
    end = sym['_image_end']
    img = bytearray(end)
    for s in secs:
        if s['flags'] & 2 and s['size'] and s['type'] == 1:          # SHF_ALLOC, PROGBITS
            img[s['addr']:s['addr'] + s['size']] = d[s['off']:s['off'] + s['size']]
    relocs = set()
    for s in secs:
        if s['type'] != 4:                                            # SHT_RELA
            continue
        target = secs[s['info']]
        if not target['flags'] & 2:
            continue                                                  # debug sections
        for i in range(s['size'] // 12):
            off, info, add = struct.unpack('>IIi', d[s['off'] + 12 * i: s['off'] + 12 * i + 12])
            typ, si = info & 0xff, info >> 8
            shndx = syms[si][2] if si < len(syms) else 0
            if typ == R_68K_32:
                if shndx == SHN_ABS:
                    continue                                          # a constant address (low memory)
                if off + 4 > end:
                    sys.exit(f"relocation at {off:#x} outside the image")
                relocs.add(off)
            elif typ in (2, 3, 6, 7, 8):                              # absolute 16/8 bit: cannot be moved
                if shndx != SHN_ABS:
                    sys.exit(f"absolute relocation type {typ} at {off:#x}: build with -fno-pic, 32-bit addresses")
    img += b'\0' * ((4 - len(img) % 4) % 4)
    table_off = len(img)
    img += struct.pack('>I', len(relocs)) + b''.join(struct.pack('>I', o) for o in sorted(relocs))
    ro = sym['_reloc_off']
    img[ro:ro + 4] = struct.pack('>I', table_off)
    return bytes(img), len(relocs), end

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
