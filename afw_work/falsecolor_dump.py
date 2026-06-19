"""False-color the in-game combined-velocity dump (RG16F) produced by our plugin's
combine pass, to compare against the engine's DLSSCombinedVelocity reference."""
import numpy as np
from PIL import Image
import os, struct

P = os.path.dirname(os.path.abspath(__file__))
bin_path = os.path.join(P, "combined_velocity.bin")
with open(bin_path, "rb") as f:
    W, H, row_pitch, fmt = struct.unpack("<IIII", f.read(16))
    raw = f.read()
print(f"dump: {W}x{H} rowPitch={row_pitch} fmt={fmt} bytes={len(raw)}")

# RG16F = 2 half per texel, 4 bytes/texel; rows strided by row_pitch
row_bytes = W * 4
out = np.zeros((H, W, 2), dtype=np.float32)
for y in range(H):
    off = y * row_pitch
    row = np.frombuffer(raw[off:off + row_bytes], dtype=np.float16).astype(np.float32)
    out[y] = row.reshape(W, 2)

vx, vy = out[..., 0], out[..., 1]
mag = np.sqrt(vx * vx + vy * vy)
nz = np.mean(mag > 1e-3) * 100
print(f"velocity px  x[{vx.min():.2f},{vx.max():.2f}] y[{vy.min():.2f},{vy.max():.2f}]  |v|max={mag.max():.2f}")
print(f"non-zero coverage = {nz:.1f}%  (dense field ~ high)")

# vis-like: R=saturate(vx*k), G=saturate(vy*k), B=0 (clamps negatives, like UE 'vis' RGB)
k = 1.0 / max(1.0, np.percentile(mag[mag > 1e-3], 70) if np.any(mag > 1e-3) else 1.0)
visR = np.clip(vx * k, 0, 1); visG = np.clip(vy * k, 0, 1)
vis = (np.stack([visR, visG, np.zeros_like(visR)], -1) * 255).astype(np.uint8)
Image.fromarray(vis).save(os.path.join(P, "combined_velocity_vislike.png"))

# direction-as-hue (full signed info): hue=angle, value=normalized magnitude
import colorsys
ang = (np.arctan2(vy, vx) / (2 * np.pi)) + 0.5
val = np.clip(mag / (np.percentile(mag, 99) + 1e-6), 0, 1)
hsv = np.stack([ang, np.ones_like(ang), val], -1)
# vectorized hsv->rgb
h6 = hsv[..., 0] * 6.0; i = np.floor(h6).astype(int) % 6; ff = h6 - np.floor(h6)
s = hsv[..., 1]; v = hsv[..., 2]
p = v * (1 - s); q = v * (1 - s * ff); t = v * (1 - s * (1 - ff))
rgb = np.zeros_like(hsv)
for idx, (rr, gg, bb) in enumerate([(v,t,p),(q,v,p),(p,v,t),(p,q,v),(t,p,v),(v,p,q)]):
    m = i == idx; rgb[m] = np.stack([rr,gg,bb], -1)[m]
Image.fromarray((rgb * 255).astype(np.uint8)).save(os.path.join(P, "combined_velocity_hue.png"))
print("wrote combined_velocity_vislike.png + combined_velocity_hue.png")
