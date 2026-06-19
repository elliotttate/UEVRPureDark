"""
CPU reimplementation of src/mods/vr/shaders/ue_velocity_combine_cs.fx
to validate that the combine math produces a DENSE full-frame motion-vector
field (matching the in-engine DLSSCombinedVelocity reference) for a camera pan,
plus a decoded "written" moving object. Pure math check — no GPU.

Conventions: column-major matrices, clip = P @ view (vectors on the right),
reverse-Z perspective (near->1, far->0) like UE SceneDepthZ, RH view space
(camera at origin, looking down -Z, +Y up, +X right).
"""
import numpy as np
from PIL import Image
import os

W, H = 320, 200                     # output size (aspect ~1.6, like 1280x800)
NEAR, FAR = 0.10, 50.0
FOVY = np.radians(60.0)
YAW_PAN_DEG = 2.0                   # camera yaw change between frames (pan right)

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ---- reverse-Z RH perspective, column-major (clip = P @ view) ----
yscale = 1.0 / np.tan(FOVY * 0.5)
xscale = yscale / (W / H)
a = NEAR / (FAR - NEAR)
b = NEAR * FAR / (FAR - NEAR)
P = np.array([
    [xscale, 0,      0,  0],
    [0,      yscale, 0,  0],
    [0,      0,      a,  b],
    [0,      0,     -1,  0],
], dtype=np.float64)
Pinv = np.linalg.inv(P)

# ---- synthetic view-space scene: floor (y=-2) + back wall (z=-20) ----
FLOOR_Y = -2.0
WALL_Z = -20.0
ys, xs = np.mgrid[0:H, 0:W]
uv_x = (xs + 0.5) / W
uv_y = (ys + 0.5) / H
ndc_x = (uv_x - 0.5) * 2.0
ndc_y = (uv_y - 0.5) * (-2.0)
# view-space ray dir through this NDC (see P): dir = (ndcx/xscale, ndcy/yscale, -1)
dir_x = ndc_x / xscale
dir_y = ndc_y / yscale
dir_z = -np.ones_like(dir_x)

# intersect floor: origin + t*dir, y = FLOOR_Y -> t = FLOOR_Y / dir_y (dir_y<0 hits floor)
with np.errstate(divide="ignore", invalid="ignore"):
    t_floor = np.where(dir_y < -1e-6, FLOOR_Y / dir_y, np.inf)
t_wall = np.where(dir_z < 0, WALL_Z / dir_z, np.inf)
t = np.minimum(t_floor, t_wall)
t = np.clip(t, NEAR, 1e6)

vx = dir_x * t
vy = dir_y * t
vz = dir_z * t
view_pts = np.stack([vx, vy, vz, np.ones_like(vx)], axis=-1)  # (H,W,4)

# project -> depth (NDC z = clip.z/clip.w)
clip = view_pts @ P.T           # (H,W,4), equivalent to (P @ v) per pixel
depth = clip[..., 2] / clip[..., 3]
depth = np.clip(depth, 0.0, 1.0)

# ---- ClipToPrevClip from a yaw pan: PrevView = Ry(theta) (curr view = I) ----
th = np.radians(YAW_PAN_DEG)
Ry = np.array([
    [np.cos(th), 0, np.sin(th), 0],
    [0,          1, 0,          0],
    [-np.sin(th),0, np.cos(th), 0],
    [0,          0, 0,          1],
], dtype=np.float64)
ClipToPrevClip = P @ Ry @ Pinv   # current clip -> previous clip

# ---- sparse encoded velocity (cleared 0 except a "character" rect) ----
enc = np.zeros((H, W, 4), dtype=np.float64)
INV_DIV = 1.0 / (0.499 * 0.5)
BIAS = 32767.0 / 65535.0
def encode(v):  # inverse of DecodeVelocityFromTexture
    return v / INV_DIV + BIAS
cx0, cx1 = int(W*0.44), int(W*0.56)
cy0, cy1 = int(H*0.45), int(H*0.85)
obj_vel = np.array([0.012, -0.004])   # object clip-space motion (distinct from camera)
enc[cy0:cy1, cx0:cx1, 0] = encode(obj_vel[0])
enc[cy0:cy1, cx0:cx1, 1] = encode(obj_vel[1])

# ---- the shader body (vectorized) ----
def decode(e):
    return e[..., :2] * INV_DIV - BIAS * INV_DIV

written = enc[..., 0] > 0.0
# reconstruction branch
clip_xy = np.stack([ndc_x, ndc_y], axis=-1)
clip_pos = np.stack([ndc_x, ndc_y, depth, np.ones_like(depth)], axis=-1)  # (H,W,4)
prev = clip_pos @ ClipToPrevClip.T
prev_w = prev[..., 3]
recon = clip_xy - np.where(prev_w[..., None] != 0, prev[..., :2] / prev_w[..., None], 0.0)
recon = np.where((prev_w > 0.0)[..., None], recon, 0.0)

dec = decode(enc)
velocity = np.where(written[..., None], dec, recon)
out_vel = velocity * np.array([0.5, -0.5]) * np.array([W, H])
out = -out_vel   # final RG16F contents (pixel units)

print(f"output size {W}x{H}  yaw_pan={YAW_PAN_DEG}deg")
print(f"reconstructed (static-world) px velocity x range = [{recon[...,0].min()*0.5*W:.2f},{recon[...,0].max()*0.5*W:.2f}]")
print(f"out.x  min/mean/max = {out[...,0].min():.2f} / {out[...,0].mean():.2f} / {out[...,0].max():.2f}")
print(f"out.y  min/mean/max = {out[...,1].min():.2f} / {out[...,1].mean():.2f} / {out[...,1].max():.2f}")
nonzero = np.mean(np.abs(out).sum(-1) > 1e-3) * 100
print(f"non-zero coverage = {nonzero:.1f}%  (dense field should be ~100%)")

# ---- vis-like false color: R = saturate(out.x*k), G = saturate(out.y*k), B=0 ----
k = 1.0 / max(1.0, np.percentile(np.abs(out), 80))
visR = np.clip(out[..., 0] * k, 0, 1)
visG = np.clip(out[..., 1] * k, 0, 1)
visB = np.zeros_like(visR)
vis = (np.stack([visR, visG, visB], -1) * 255).astype(np.uint8)
Image.fromarray(vis).resize((W*2, H*2), Image.NEAREST).save(os.path.join(OUT_DIR, "combine_sim_vislike.png"))

# ---- signed debug: R=+x B=-x  G=+y ----
m = max(np.abs(out).max(), 1e-3)
dbgR = np.clip(out[..., 0] / m, 0, 1)
dbgB = np.clip(-out[..., 0] / m, 0, 1)
dbgG = np.clip(np.abs(out[..., 1]) / m, 0, 1)
dbg = (np.stack([dbgR, dbgG, dbgB], -1) * 255).astype(np.uint8)
Image.fromarray(dbg).resize((W*2, H*2), Image.NEAREST).save(os.path.join(OUT_DIR, "combine_sim_signed.png"))

Image.fromarray((np.clip(depth,0,1)*255).astype(np.uint8)).resize((W*2,H*2),Image.NEAREST).save(os.path.join(OUT_DIR,"combine_sim_depth.png"))
print("wrote combine_sim_vislike.png, combine_sim_signed.png, combine_sim_depth.png")
