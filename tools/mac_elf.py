#!/usr/bin/env python3
"""Riptide on the classic Mac OS: a program linked at 0 with --emit-relocs as a self-relocating
image (elf2mac.py for the .IPP driver, elf2app.py for applications).

build_image(elf) returns (image, relocations, end): .text, .data and the zero-filled .bss up to
_image_end, then a long count and the offsets of every 32-bit absolute field (R_68K_32 against
a section-relative symbol), with _reloc_off in the image set to the table's offset. The program
adds its load address to each field before it runs. Any other absolute relocation type is an
error: the code must be built -fno-pic with 32-bit absolute or PC-relative references only.

One exception: libgcc's soft-float routines (lb1sf68.S, built position-independent for Linux)
find their rounding-mode variable through a global offset table: R_68K_GOT32 (the table's
entry, PC-relative) and R_68K_GOT32O (an offset into the table) need nothing at load time, but
each entry of .got holds an absolute address, which the linker filled in without a relocation
record (Quill's Mac build, where doubles come from libgcc). Those entries are added to the table.
SPDX-License-Identifier: GPL-2.0-only
"""
import struct, sys

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
            elif typ in (7, 10):                                      # through the GOT (see above)
                continue
            elif typ in (2, 3, 6, 8):                                 # absolute 16/8 bit: cannot be moved
                if shndx != SHN_ABS:
                    sys.exit(f"absolute relocation type {typ} at {off:#x}: build with -fno-pic, 32-bit addresses")
    for s in secs:
        if s['name'] == '.got' and s['flags'] & 2:
            for o in range(s['addr'], s['addr'] + s['size'], 4):
                v, = struct.unpack('>I', img[o:o + 4])
                if 0 < v < end:                                       # an address in the image
                    relocs.add(o)
    img += b'\0' * ((4 - len(img) % 4) % 4)
    table_off = len(img)
    img += struct.pack('>I', len(relocs)) + b''.join(struct.pack('>I', o) for o in sorted(relocs))
    ro = sym['_reloc_off']
    img[ro:ro + 4] = struct.pack('>I', table_off)
    return bytes(img), len(relocs), end

