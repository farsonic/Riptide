import sys,struct,re,bisect
dis=sys.argv[1]; h3=open(sys.argv[2],'rb').read(); tab=int(sys.argv[3],0); bias=30
lines=open(dis).read().split('\n')
idx={}
for i,l in enumerate(lines):
    m=re.match(r'\s+([0-9a-f]+):\t',l)
    if m: idx[int(m.group(1),16)]=i
ARG=re.compile(r'%(d[0-7]|a[0-3])\b')
n=0;o=tab
while True:
    v=struct.unpack('>i',h3[o:o+4])[0]
    if v==-1: break
    i=idx.get(v); used=[]; written=set(['a4','a5'])
    for l in lines[i:i+25]:
        parts=l.split('\t')
        if len(parts)<3: continue
        ins=parts[2].strip()
        if ins.startswith(('bsr','jsr','rts','b','jmp','dbf')) and not ins.startswith(('btst','bset','bclr','bchg')): break
        op=ins.split(' ',1)
        if len(op)<2: continue
        mnem,args=op
        if mnem.startswith('moveml'):
            continue
        ops=args.split(',')
        srcregs=ARG.findall(','.join(ops[:-1])) if len(ops)>1 else []
        dst=ops[-1]
        for r in srcregs:
            if r not in written and r not in used: used.append(r)
        if mnem.startswith(('mov','lea','clr','moveq')):
            m=re.fullmatch(r'%(d[0-7]|a[0-7])',dst.strip())
            if m: written.add(m.group(1))
        else:
            for r in ARG.findall(dst):
                if r not in written and r not in used: used.append(r)
    print(f'LVO -{bias+6*n:<4d} fn@{v:#07x} args={"/".join(used)}')
    n+=1;o+=4
