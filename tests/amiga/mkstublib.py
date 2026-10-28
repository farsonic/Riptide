#!/usr/bin/env python3
# Riptide - write the assembler source (vasm, Motorola syntax) of a stub
# Amiga library whose every function fails: it returns 0, except the
# overrides given as BIAS=instructions (';' separates instructions).
# For tests only: the AROS Kickstart replacement of FS-UAE has no
# diskfont, iffparse, locale, commodities, asl or datatypes.library, which
# MUI 3.8 opens; with these stubs it runs without fonts from disk, saved
# MUI settings, catalogs or file requesters (see run_fsuae_prefs_mui.sh).
#
#   mkstublib.py NAME VERSION NFUNCS [BIAS=asm ...] > NAME.s
#   vasmm68k_mot -Fhunkexe -nosym -o LIBS/NAME NAME.s
# SPDX-License-Identifier: GPL-2.0-only
import sys


def main(argv):
    if len(argv) < 4:
        sys.exit('usage: mkstublib.py NAME VERSION NFUNCS [BIAS=asm ...]')
    name, ver, n = argv[1], int(argv[2]), int(argv[3])
    over = {}
    for a in argv[4:]:
        k, v = a.split('=', 1)
        over[int(k)] = v
    o = ['\tsection code,code',
         # run as a program: do nothing
         'start:\tmoveq\t#-1,d0', '\trts',
         # the resident tag LoadSeg()ed libraries are found by
         'romtag:\tdc.w\t$4afc', '\tdc.l\tromtag', '\tdc.l\ttagend',
         '\tdc.b\t$80,%d,9,0' % ver,             # RTF_AUTOINIT, version, NT_LIBRARY, priority
         '\tdc.l\tlname', '\tdc.l\tidstr', '\tdc.l\tinittab',
         "lname:\tdc.b\t'%s',0" % name,
         "idstr:\tdc.b\t'%s %d.0 (Riptide test stub)',13,10,0" % (name, ver),
         '\teven',
         # data size (struct Library + seglist), vectors, no InitStruct data, init
         'inittab:\tdc.l\t34+4', '\tdc.l\tfuncs', '\tdc.l\t0', '\tdc.l\tinit',
         # d0 library, a0 seglist: fill in the node and the version
         'init:\tmove.l\td0,a1', '\tmove.b\t#9,8(a1)', '\tmove.l\t#lname,10(a1)', '\tmove.b\t#6,14(a1)',
         '\tmove.w\t#%d,20(a1)' % ver, '\tmove.w\t#0,22(a1)', '\tmove.l\t#idstr,24(a1)', '\tmove.l\ta0,34(a1)',
         '\trts',
         'lopen:\taddq.w\t#1,32(a6)', '\tbclr\t#3,14(a6)', '\tmove.l\ta6,d0', '\trts',
         'lclose:\tsubq.w\t#1,32(a6)', '\tmoveq\t#0,d0', '\trts',
         'lnull:\tmoveq\t#0,d0', '\trts']
    for i in range(n):
        bias = 30 + 6 * i
        body = over.get(bias, 'moveq\t#0,d0')
        o.append('f%d:\t%s' % (bias, body.replace(';', '\n\t')))
        o.append('\trts')
    o.append('funcs:\tdc.l\tlopen,lclose,lnull,lnull')
    o += ['\tdc.l\tf%d' % (30 + 6 * i) for i in range(n)]
    o += ['\tdc.l\t-1', 'tagend:']
    print('\n'.join(o))


if __name__ == '__main__':
    main(sys.argv)
