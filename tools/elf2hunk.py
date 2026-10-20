#!/usr/bin/env python3
# Riptide - convert a static m68k ELF image linked at address 0 (with
# --emit-relocs) into an AmigaOS hunk executable.
# SPDX-License-Identifier: GPL-2.0-only
#
# Output layout: HUNK_HEADER, one HUNK_CODE holding every allocated
# PROGBITS section (text, rodata, data) at its link address, HUNK_RELOC32
# for every absolute 32-bit relocation, HUNK_END. The header's hunk size
# also covers the NOBITS sections (bss) that follow, so the loader
# allocates them; the startup code clears them.
#
# Relocation handling (target section must be allocated):
#   R_68K_32           -> HUNK_RELOC32 entry, unless the symbol is absolute
#   R_68K_PC8/16/32    -> already resolved by the linker, position independent
#   R_68K_NONE         -> ignored
#   anything else      -> error (would not survive relocation)
import struct
import sys

SHT_PROGBITS, SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 1, 2, 4, 8, 9
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS = 0, 0xfff1
R_68K_NONE, R_68K_32, R_68K_16, R_68K_8, R_68K_PC32, R_68K_PC16, R_68K_PC8 = 0, 1, 2, 3, 4, 5, 6
HUNK_HEADER, HUNK_CODE, HUNK_RELOC32, HUNK_END = 0x3f3, 0x3e9, 0x3ec, 0x3f2
HUNKF_ANY_MEM = 0


class ElfError(Exception):
    pass


def read_elf(data):
    if data[:4] != b'\x7fELF':
        raise ElfError('not an ELF file')
    if data[4] != 1 or data[5] != 2:
        raise ElfError('need ELF32 big-endian')
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('>HHIIIIIHHHHHH', data, 16)
    if e_machine != 4:
        raise ElfError('not an m68k ELF file')
    if e_type != 2:
        raise ElfError('need a linked executable (ET_EXEC)')
    secs = []
    for i in range(e_shnum):
        f = struct.unpack_from('>IIIIIIIIII', data, e_shoff + i * e_shentsize)
        secs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3], offset=f[4],
                         size=f[5], link=f[6], info=f[7], align=f[8], entsize=f[9], idx=i))
    strtab = secs[e_shstrndx]
    for s in secs:
        o = strtab['offset'] + s['name_off']
        s['name'] = data[o:data.index(b'\0', o)].decode()
    return e_entry, secs


def convert(data, verbose=False):
    entry, secs = read_elf(data)
    if entry != 0:
        raise ElfError('entry point must be at address 0 (got 0x%x)' % entry)
    alloc = [s for s in secs if s['flags'] & SHF_ALLOC and s['size'] > 0]
    prog = [s for s in alloc if s['type'] != SHT_NOBITS]
    nobits = [s for s in alloc if s['type'] == SHT_NOBITS]
    for s in alloc:
        if s['type'] not in (SHT_PROGBITS, SHT_NOBITS) and not s['name'].startswith('.init_array'):
            raise ElfError('unexpected allocated section %s (type %d)' % (s['name'], s['type']))
    code_end = max((s['addr'] + s['size'] for s in prog), default=0)
    total_end = max([code_end] + [s['addr'] + s['size'] for s in nobits])
    for s in nobits:
        if s['addr'] < code_end:
            raise ElfError('bss section %s overlaps initialised data' % s['name'])
    image = bytearray(code_end)
    for s in prog:
        image[s['addr']:s['addr'] + s['size']] = data[s['offset']:s['offset'] + s['size']]

    relocs = set()
    for rs in secs:
        if rs['type'] == SHT_REL:
            raise ElfError('REL relocations are not supported')
        if rs['type'] != SHT_RELA:
            continue
        target = secs[rs['info']]
        if not target['flags'] & SHF_ALLOC:
            continue
        symtab = secs[rs['link']]
        if symtab['type'] != SHT_SYMTAB:
            raise ElfError('relocation section %s without symbol table' % rs['name'])
        for i in range(rs['size'] // 12):
            r_offset, r_info, r_addend = struct.unpack_from('>IIi', data, rs['offset'] + 12 * i)
            rtype, rsym = r_info & 0xff, r_info >> 8
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                '>IIIBBH', data, symtab['offset'] + 16 * rsym)
            if rtype == R_68K_NONE or rtype in (R_68K_PC32, R_68K_PC16, R_68K_PC8):
                continue
            if rtype != R_68K_32:
                raise ElfError('unsupported relocation type %d at 0x%x in %s'
                               % (rtype, r_offset, rs['name']))
            if st_shndx == SHN_ABS:
                continue
            if st_shndx == SHN_UNDEF and rsym != 0:
                raise ElfError('undefined symbol in relocation at 0x%x' % r_offset)
            if target['type'] == SHT_NOBITS:
                raise ElfError('relocation inside bss at 0x%x' % r_offset)
            if r_offset + 4 > code_end:
                raise ElfError('relocation outside the image at 0x%x' % r_offset)
            if r_offset & 1:
                raise ElfError('odd relocation offset 0x%x (68000 would trap)' % r_offset)
            value = struct.unpack_from('>I', image, r_offset)[0]
            if value > total_end:
                # legal (GCC may fold "array - k" into an address), but rare
                sys.stderr.write('elf2hunk: note: value 0x%x at 0x%x points outside the image\n'
                                 % (value, r_offset))
            relocs.add(r_offset)

    pad = (-len(image)) % 4
    image += b'\0' * pad
    code_longs = len(image) // 4
    alloc_longs = max(code_longs, (total_end + 3) // 4)
    out = bytearray()
    out += struct.pack('>IIIII', HUNK_HEADER, 0, 1, 0, 0)
    out += struct.pack('>I', alloc_longs | HUNKF_ANY_MEM)
    out += struct.pack('>II', HUNK_CODE, code_longs)
    out += image
    if relocs:
        offs = sorted(relocs)
        out += struct.pack('>I', HUNK_RELOC32)
        # entries are limited only by the 32-bit count; emit one group
        out += struct.pack('>II', len(offs), 0)
        for o in offs:
            out += struct.pack('>I', o)
        out += struct.pack('>I', 0)
    out += struct.pack('>I', HUNK_END)
    if verbose:
        sys.stderr.write('elf2hunk: code %d bytes, bss %d bytes, %d relocs\n'
                         % (code_longs * 4, (alloc_longs - code_longs) * 4, len(relocs)))
    return bytes(out), sorted(relocs), code_longs * 4, alloc_longs * 4


def main():
    args = [a for a in sys.argv[1:] if a != '-v']
    if len(args) != 2:
        sys.exit('usage: elf2hunk.py [-v] input.elf output')
    try:
        out = convert(open(args[0], 'rb').read(), '-v' in sys.argv)[0]
    except ElfError as e:
        sys.exit('elf2hunk: %s: %s' % (args[0], e))
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
