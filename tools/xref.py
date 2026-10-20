import sys,struct
sys.path.insert(0,'tools'); from hunk import parse
hs,_=parse(sys.argv[1]); src=int(sys.argv[2]); tgt=int(sys.argv[3]); want=[int(x,0) for x in sys.argv[4:]]
h=hs[src]
for o in h['relocs'].get(tgt,[]):
    v=struct.unpack('>I',h['data'][o:o+4])[0]
    for w in want:
        if v==w: print(f'hunk{src}+{o:#x} -> hunk{tgt}+{v:#x}')
