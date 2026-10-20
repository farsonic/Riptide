import struct,sys,os
NAMES={0x3e7:'UNIT',0x3e8:'NAME',0x3e9:'CODE',0x3ea:'DATA',0x3eb:'BSS',0x3ec:'RELOC32',0x3ed:'RELOC16',0x3ee:'RELOC8',0x3ef:'EXT',0x3f0:'SYMBOL',0x3f1:'DEBUG',0x3f2:'END',0x3f3:'HEADER',0x3f7:'DREL32',0x3f8:'DREL16',0x3f9:'DREL8',0x3fc:'RELOC32SHORT'}
def parse(path):
    d=open(path,'rb').read(); p=0
    def L():
        nonlocal p; v=struct.unpack('>I',d[p:p+4])[0]; p+=4; return v
    assert L()==0x3f3
    while L(): pass
    n=L(); first=L(); last=L(); sizes=[L()&0x3fffffff for _ in range(last-first+1)]
    hunks=[]; cur=None
    while p<len(d):
        t=L()&0x3fffffff
        if t in (0x3e9,0x3ea):
            sz=L()*4; cur={'type':NAMES[t],'data':bytearray(d[p:p+sz]),'relocs':{},'syms':[]}; p+=sz; hunks.append(cur)
        elif t==0x3eb:
            sz=L()*4; cur={'type':'BSS','data':bytearray(sz),'relocs':{},'syms':[]}; hunks.append(cur)
        elif t==0x3ec:
            while True:
                c=L()
                if c==0: break
                h=L(); cur['relocs'].setdefault(h,[]).extend(L() for _ in range(c))
        elif t in (0x3fc,0x3f7):
            while True:
                c=struct.unpack('>H',d[p:p+2])[0]; p+=2
                if c==0: break
                h=struct.unpack('>H',d[p:p+2])[0]; p+=2
                cur['relocs'].setdefault(h,[]).extend(struct.unpack('>H',d[p+2*i:p+2*i+2])[0] for i in range(c)); p+=2*c
            if p%4: p+=2
        elif t==0x3f0:
            while True:
                nl=L()
                if nl==0: break
                nm=d[p:p+nl*4].rstrip(b'\0').decode('latin1'); p+=nl*4; cur['syms'].append((nm,L()))
        elif t==0x3f1:
            sz=L()*4; p+=sz
        elif t==0x3f2: pass
        else: raise Exception('unknown hunk %x at %x'%(t,p))
    return hunks,sizes
if __name__=='__main__':
    hs,sz=parse(sys.argv[1])
    for i,h in enumerate(hs): print(i,h['type'],len(h['data']),sz[i],'relocs->',{k:len(v) for k,v in h['relocs'].items()},'syms',len(h['syms']))
