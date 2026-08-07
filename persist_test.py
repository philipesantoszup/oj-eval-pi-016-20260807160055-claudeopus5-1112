import random,subprocess,os,sys
random.seed(int(sys.argv[1]))
d={}
expected=[]
got=[]
if os.path.exists("bpt_storage.dat"): os.remove("bpt_storage.dat")
for run in range(10):
    lines=[]
    n=random.randint(1,400)
    for _ in range(n):
        r=random.random(); k="key%d"%random.randint(1,40)
        if r<0.5:
            v=random.randint(-50,50); lines.append("insert %s %d"%(k,v)); d.setdefault(k,set()).add(v)
        elif r<0.75:
            v=random.randint(-50,50); lines.append("delete %s %d"%(k,v))
            if k in d: d[k].discard(v)
        else:
            lines.append("find %s"%k)
            s=d.get(k); expected.append(' '.join(map(str,sorted(s))) if s else 'null')
    inp=str(len(lines))+"\n"+"\n".join(lines)+"\n"
    out=subprocess.run(["./code"],input=inp,capture_output=True,text=True).stdout
    got.extend([l for l in out.split('\n') if l!=''])
if got==expected: print("OK")
else:
    print("MISMATCH")
    for i,(a,b) in enumerate(zip(got,expected)):
        if a!=b: print(i,repr(a),repr(b)); break
    print(len(got),len(expected))
