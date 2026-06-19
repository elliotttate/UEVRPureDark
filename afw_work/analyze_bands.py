import numpy as np, os, struct
P = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(P,"combined_velocity.bin"),"rb") as f:
    W,H,rp,fmt = struct.unpack("<IIII", f.read(16)); raw=f.read()
out=np.zeros((H,W,2),np.float32)
for y in range(H):
    out[y]=np.frombuffer(raw[y*rp:y*rp+W*4],dtype=np.float16).astype(np.float32).reshape(W,2)
mag=np.sqrt((out**2).sum(-1))
# per-row mean magnitude profile (bands show as periodic structure along Y)
prof=mag.mean(1)
print("row-magnitude profile (32 buckets, near=top):")
buck=prof.reshape(32,-1).mean(1)
mx=buck.max()+1e-6
for i,b in enumerate(buck):
    print(f"{i:2d} {'#'*int(60*b/mx)} {b:.1f}")
# vertical autocorrelation: is row y ~ row y+shift ?
col=prof-prof.mean()
ac=np.correlate(col,col,'full')[len(col)-1:]; ac/=ac[0]
peaks=[(s,ac[s]) for s in range(20,H//2) if ac[s]>0.5 and ac[s]>ac[s-1] and ac[s]>=ac[s+1]]
print("strong vertical autocorr peaks (shift,score):", peaks[:6])
print(f"size {W}x{H}  out.x[{out[...,0].min():.1f},{out[...,0].max():.1f}] out.y[{out[...,1].min():.1f},{out[...,1].max():.1f}]")
