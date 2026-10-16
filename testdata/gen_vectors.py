"""Generates brush_vectors.json: brush validation cases shared by the C++ (core/tests) and
TypeScript (shell/tests) suites, so the two implementations of the rules can't drift.
Each case: {name, requireConvex, expect: null | "substring of the error", brush}."""
import json, copy

def box(bid, mn, mx):
    (x0,y0,z0),(x1,y1,z1)=mn,mx
    v=[(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),(x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    loops=[(0,3,2,1),(4,5,6,7),(0,4,7,3),(1,2,6,5),(0,1,5,4),(3,7,6,2)]
    return {"id":bid,"op":"add","vertices":[list(p) for p in v],
            "faces":[{"id":i,"loop":list(l)} for i,l in enumerate(loops)]}

def prism(bid, prof, y0=0, y1=1):
    n=len(prof)
    v=[[x,y0,z] for x,z in prof]+[[x,y1,z] for x,z in prof]
    faces=[{"id":0,"loop":list(reversed(range(n)))},{"id":1,"loop":[n+i for i in range(n)]}]
    for i in range(n):
        j=(i+1)%n; faces.append({"id":2+i,"loop":[i,j,n+j,n+i]})
    vol=0
    for f in faces:
        a=v[f["loop"][0]]
        for k in range(1,len(f["loop"])-1):
            b,c=v[f["loop"][k]],v[f["loop"][k+1]]
            cx=(b[1]*c[2]-b[2]*c[1],b[2]*c[0]-b[0]*c[2],b[0]*c[1]-b[1]*c[0])
            vol+=(a[0]*cx[0]+a[1]*cx[1]+a[2]*cx[2])/6
    if vol<0:
        for f in faces: f["loop"].reverse()
    return {"id":bid,"op":"add","vertices":v,"faces":faces}

cases=[]
def add(name, brush, expect, convex=True): cases.append({"name":name,"requireConvex":convex,"expect":expect,"brush":brush})

b=box("b",(0,0,0),(1,2,3)); add("box", b, None)
add("triangular prism", prism("p",[(0,0),(2,0),(0,2)]), None)
L=prism("l",[(0,0),(2,0),(2,1),(1,1),(1,2),(0,2)])
add("L prism where convex is required", L, "convex"); add("L prism where free-form is allowed", L, None, convex=False)
x=copy.deepcopy(b)
for f in x["faces"]: f["loop"].reverse()
add("inside-out box", x, "inward")
x=copy.deepcopy(b); x["faces"].pop(); add("open mesh (face missing)", x, "not closed")
x=copy.deepcopy(b); x["vertices"][6][1]+=0.5; add("non-planar faces", x, "planar", convex=False)
add("zero-height box", box("z",(0,0,0),(1,0,1)), "degenerate", convex=False)
x=copy.deepcopy(b); x["faces"][0]["loop"][0]=99; add("index out of range", x, "references vertex", convex=False)
x=copy.deepcopy(b); x["faces"][0]["loop"]=x["faces"][0]["loop"][:2]; add("loop too short", x, "fewer than 3", convex=False)
x=copy.deepcopy(b); x["faces"][0]["loop"][1]=x["faces"][0]["loop"][0]; add("repeated vertex in loop", x, "twice", convex=False)
x=copy.deepcopy(b); x["faces"][1]["id"]=0; add("duplicate face id", x, "duplicate face id", convex=False)
x=copy.deepcopy(b); x["faces"][0]["uvs"]=[[0,0]]; add("uv count mismatch", x, "uvs", convex=False)
json.dump({"cases":cases},open("brush_vectors.json","w"),indent=1)
print(len(cases),"cases")
