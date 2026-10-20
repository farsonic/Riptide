import sys,math,collections
sys.path.insert(0,sys.argv[3]) 
from hunk import parse
hs,_=parse(sys.argv[1]); out=sys.argv[2]
for i,h in enumerate(hs):
    open(f'{out}.h{i}.bin','wb').write(h['data'])
    d=h['data']
    if d:
        for off in range(0,len(d),65536):
            c=collections.Counter(d[off:off+65536]); n=sum(c.values())
            print(i,h['type'],hex(off),round(-sum(v/n*math.log2(v/n) for v in c.values()),2))
