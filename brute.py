import sys
data=sys.stdin.read().split('\n')
n=int(data[0])
d={}
out=[]
for i in range(1,n+1):
    p=data[i].split()
    if not p: continue
    if p[0]=='insert': d.setdefault(p[1],set()).add(int(p[2]))
    elif p[0]=='delete':
        if p[1] in d: d[p[1]].discard(int(p[2]))
    else:
        s=d.get(p[1])
        out.append(' '.join(map(str,sorted(s))) if s else 'null')
print('\n'.join(out))
