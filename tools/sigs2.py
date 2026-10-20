import sys,struct,re
from capstone import *
code=open(sys.argv[1],'rb').read(); h3=open(sys.argv[2],'rb').read(); tab=int(sys.argv[3],0)
md=Cs(CS_ARCH_M68K,CS_MODE_M68K_020)
ARG=re.compile(r'\b(d[0-7]|a[0-3])\b')
def sig(addr):
    used=[];written=set()
    for ins in md.disasm(code[addr:addr+200],addr):
        m=ins.mnemonic; o=ins.op_str
        if m.startswith(('bsr','jsr','rts','jmp','bra','dbf')) or (m.startswith('b') and m[1:3] in('eq','ne','lt','gt','le','ge','cc','cs','pl','mi','hi','ls','vc','vs')): break
        if m.startswith('movem'):
            continue
        ops=[x.strip() for x in re.split(r',(?![^(]*\))',o)]
        src=ops[:-1] if len(ops)>1 else []
        for r in ARG.findall(','.join(src)):
            if r not in written and r not in used: used.append(r)
        dst=ops[-1] if ops else ''
        if re.fullmatch(r'(d[0-7]|a[0-7])',dst) and m.split('.')[0] in('move','movea','lea','moveq','clr'):
            written.add(dst)
        else:
            for r in ARG.findall(dst):
                if r not in written and r not in used: used.append(r)
    return used
n=0;o=tab
while True:
    v=struct.unpack('>i',h3[o:o+4])[0]
    if v==-1: break
    print(f'LVO -{30+6*n:<4d} fn@{v:#07x} args={"/".join(sig(v))}')
    n+=1;o+=4
