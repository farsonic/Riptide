import sys,struct
sys.path.insert(0,sys.argv[2]); from hunk import parse
hs,_=parse(sys.argv[1])
for si,h in enumerate(hs):
    for th,offs in h['relocs'].items():
        if th!=0: continue
        offs=sorted(set(offs)); run=[offs[0]] if offs else []
        def flush(run):
            if len(run)>=int(sys.argv[3]):
                d=h['data']; nxt=run[-1]+4
                term=struct.unpack('>i',d[nxt:nxt+4])[0] if nxt+4<=len(d) else None
                vals=[struct.unpack('>I',d[o:o+4])[0] for o in run]
                print(f'hunk{si}@{run[0]:#x} n={len(run)} term={term} first={[hex(v) for v in vals[:6]]}')
        for o in offs[1:]:
            if o==run[-1]+4: run.append(o)
            else: flush(run); run=[o]
        if run: flush(run)
