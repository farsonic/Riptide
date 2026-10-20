#!/usr/bin/env python3
# Riptide - checks of the AmigaOS executables and of tools/elf2hunk.py.
# SPDX-License-Identifier: GPL-2.0-only
#
#   test_hunk.py [HUNKFILE...]
#
# 1. Every HUNKFILE is loaded with amitools' hunk loader (BinFmt); every
#    32-bit relocation must lie inside its segment (and, for our single
#    hunk images, point inside the allocation). If HUNKFILE.elf exists (GCC build) the
#    relocations must be exactly the R_68K_32 relocations of the ELF image.
# 2. elf2hunk unit tests on small assembler programs (needs
#    m68k-linux-gnu-gcc): absolute relocations become HUNK_RELOC32, PC
#    relative ones none, absolute symbols none, 16-bit absolute
#    relocations are rejected, bss is covered by the hunk size, and the
#    result loads and runs under vamos when it is installed.
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import elf2hunk  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if cond:
        print('ok   ' + what)
    else:
        failures += 1
        print('FAIL ' + what)


def load(path):
    from amitools.binfmt.BinFmt import BinFmt
    img = BinFmt().load_image(path)
    if img is None:
        raise RuntimeError('cannot load ' + path)
    return img


def check_image(path):
    img = load(path)
    segs = img.get_segments()
    sizes = [s.get_size() for s in segs]
    bad = 0
    total = 0
    for s in segs:
        data = s.get_data()
        for to in s.get_reloc_to_segs():
            for r in s.get_reloc(to).get_relocs():
                total += 1
                off = r.get_offset()
                if off + 4 > len(data):
                    bad += 1
                    continue
                v = struct.unpack_from('>I', data, off)[0] + r.get_addend()
                if v > sizes[to.id]:
                    bad += 1
    if len(segs) == 1:
        check(bad == 0, '%s: %d relocations, all inside the image' % (path, total))
    else:
        # vlink output: small-data bases (_LinkerDB = data + 0x7ffe) legally
        # point past a segment, so only report them
        check(total > 0, '%s: %d segments, %d relocations (%d point past their target segment, e.g. _LinkerDB)'
              % (path, len(segs), total, bad))
    elf = path + '.elf'
    if os.path.exists(elf):
        _, relocs, code, alloc = elf2hunk.convert(open(elf, 'rb').read())
        got = sorted(r.get_offset() for r in segs[0].get_reloc(segs[0]).get_relocs()) if segs[0].get_reloc_to_segs() else []
        check(len(segs) == 1 and got == relocs, '%s: relocations identical to the ELF R_68K_32 list (%d)' % (path, len(relocs)))
        check(sizes[0] == alloc and len(segs[0].get_data()) >= code, '%s: hunk allocation covers bss' % path)
        check(segs[0].get_data()[:2] == b'\x48\xe7', '%s: entry point (movem) at offset 0' % path)


ASM = r'''
	.section .text.amiga_entry,"ax"
	.globl _start
_start:
	lea	msg(%pc),%a0          | PC relative: no relocation
	move.l	ptr,%a1               | absolute: relocation
	cmp.l	%a0,%a1
	bne.s	bad
	move.l	tbl+4,%d0             | absolute with addend
	cmp.l	#bsszero,%d0          | immediate absolute
	bne.s	bad
	move.l	bsszero,%d0           | bss must be zeroed by the loader
	bne.s	bad
	moveq	#0,%d0
	rts
bad:	moveq	#20,%d0
	rts
	.data
	.even
ptr:	.long msg
tbl:	.long _start, bsszero, 0x1234
	.long ABSVAL
msg:	.asciz "x"
	.bss
	.even
bsszero: .space 4096
	.set ABSVAL, 0xdeadbeef
'''
BAD16 = r'''
	.section .text.amiga_entry,"ax"
	.globl _start
_start:
	rts
	.data
foo:	.word foo                     | 16-bit absolute address: not relocatable
'''


def unit_tests():
    cc = shutil.which('m68k-linux-gnu-gcc')
    ld = shutil.which('m68k-linux-gnu-ld')
    if not cc or not ld:
        print('SKIP elf2hunk unit tests (no m68k-linux-gnu toolchain)')
        return
    with tempfile.TemporaryDirectory() as d:
        def build(src, name, absolute_short=False):
            s = os.path.join(d, name + '.S')
            open(s, 'w').write(src)
            o = os.path.join(d, name + '.o')
            subprocess.run([cc, '-m68020', '-c', s, '-o', o], check=True)
            e = os.path.join(d, name + '.elf')
            subprocess.run([ld, '-T', os.path.join(ROOT, 'src/amiga/link.ld'), '--emit-relocs',
                            '--no-warn-rwx-segments', '-o', e, o], check=True,
                           stderr=subprocess.DEVNULL)
            return e
        e = build(ASM, 'ok')
        out, relocs, code, alloc = elf2hunk.convert(open(e, 'rb').read())
        h = os.path.join(d, 'ok')
        open(h, 'wb').write(out)
        # move.l ptr / move.l tbl+4 / cmp.l #bsszero / move.l bsszero
        # + ptr, tbl[0], tbl[1]  (ABSVAL is absolute: no relocation)
        check(len(relocs) == 7, 'elf2hunk: 7 relocations for 4 code + 3 data references (got %d)' % len(relocs))
        check(alloc - code >= 4096, 'elf2hunk: header size includes 4 KB of bss')
        img = load(h)
        segs = img.get_segments()
        check(len(segs) == 1 and segs[0].get_size() == alloc, 'elf2hunk: amitools loads one CODE hunk with bss')
        data = segs[0].get_data()
        absval = struct.pack('>I', 0xdeadbeef)
        check(absval in data, 'elf2hunk: absolute symbol value kept as is')
        if shutil.which('vamos'):
            r = subprocess.run(['vamos', '-q', '-C', '68020', h], capture_output=True)
            check(r.returncode == 0, 'elf2hunk: relocated program runs correctly under vamos (rc %d)' % r.returncode)
        e = build(BAD16, 'bad')
        try:
            elf2hunk.convert(open(e, 'rb').read())
            check(False, 'elf2hunk: 16-bit absolute relocation rejected')
        except elf2hunk.ElfError as ex:
            check('unsupported relocation' in str(ex), 'elf2hunk: 16-bit absolute relocation rejected')


def main():
    for p in sys.argv[1:]:
        if os.path.exists(p):
            check_image(p)
        else:
            print('SKIP %s (not built)' % p)
    unit_tests()
    print('test_hunk: %s' % ('FAILED' if failures else 'all passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
