import random,sys
seed=int(sys.argv[1]); n=int(sys.argv[2]); keyspace=int(sys.argv[3])
random.seed(seed)
lines=[]
present=set()
for _ in range(n):
    r=random.random()
    k="k%d"%random.randint(1,keyspace)
    if r<0.45:
        v=random.randint(-1000,1000)
        lines.append("insert %s %d"%(k,v)); present.add((k,v))
    elif r<0.75:
        if present and random.random()<0.7:
            k,v=random.choice(list(present)); present.discard((k,v))
        else:
            v=random.randint(-1000,1000)
        lines.append("delete %s %d"%(k,v))
    else:
        lines.append("find %s"%k)
print(len(lines))
print("\n".join(lines))
