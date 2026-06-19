import json, os
D = r"C:\Users\ellio\AppData\Local\Temp\uevr_renderdoc_validate\UnrealGame-Win64-Shipping_2026.06.18_22.46_frame6711_20260618_224833"
# eventId -> action info
acts = {}
for L in open(os.path.join(D,"actions.jsonl")):
    a=json.loads(L); acts[a["eventId"]]=a
# eventId -> renderTargets [{resource,format}]
fmt_of = {}  # resourceId -> format
rt_draws = {}  # resourceId -> dict(draws, indices, depthbound, mrt_count_max, single, names)
for L in open(os.path.join(D,"state.jsonl")):
    s=json.loads(L); ev=s["eventId"]
    rts=s.get("renderTargets") or []
    a=acts.get(ev,{})
    nidx=a.get("numIndices",0); ninst=a.get("numInstances",0); depth=a.get("depthOut"); name=a.get("name","")
    is_draw = ("Draw" in name) or ("Dispatch" in name)
    nrt=len([r for r in rts if r.get("resource")])
    for r in rts:
        rid=r.get("resource"); f=r.get("format")
        if not rid: continue
        fmt_of[rid]=f
        d=rt_draws.setdefault(rid,{"fmt":f,"draws":0,"indices":0,"depth":False,"maxrt":0,"single":0,"mrt":0})
        if is_draw:
            d["draws"]+=1; d["indices"]+=nidx
            if depth: d["depth"]=True
            d["maxrt"]=max(d["maxrt"],nrt)
            if nrt==1: d["single"]+=1
            elif nrt>1: d["mrt"]+=1
# report velocity-shaped formats
print(f"{'ResourceId':>16} {'format':22} {'draws':>5} {'indices':>9} depth single mrt")
for rid,d in sorted(rt_draws.items(), key=lambda x:-x[1]["draws"]):
    if d["fmt"] in ("R16G16_FLOAT","R16G16_UNORM","R16G16B16A16_UNORM","R16G16B16A16_SNORM"):
        print(f"{rid:>16} {d['fmt']:22} {d['draws']:>5} {d['indices']:>9} {str(d['depth']):>5} {d['single']:>6} {d['mrt']:>3}")
