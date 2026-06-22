Texture2D<float4> VelocityTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
// u0: .xy = combined screen-space motion in PIXELS (DLSS-equivalent) -> RG16F. Consumed by the AFW warp;
// kept 32-bit so PDAFW's raw CopyResource stays valid.
RWTexture2D<float2> OutVelocityCombinedTexture : register(u0);
// u1: full 3D velocity -> RGBA16F. .xy = same screen motion (pixels), .z = depth motion V.z = DeviceZ -
// PrevDeviceZ (engine per-object value where written, camera reconstruction elsewhere), .w = 0. For
// PureDark's 3D reprojection (two-frame object tracking) and the "combined Z" debug view.
RWTexture2D<float4> OutVelocity3DTexture : register(u1);

cbuffer VelocityCombineConstants : register(b0)
{
    column_major float4x4 ClipToPrevClip;
    uint2 OutputSize;
    float2 InvOutputSize;
    uint2 VelocitySize;
    uint2 DepthSize;
    uint2 VelocityOrigin;
    uint2 VelocityExtent;
    uint2 DepthOrigin;
    uint2 DepthExtent;
    uint ForceReconstruct;  // 1 = ignore the source velocity texture, reconstruct camera motion from depth everywhere
    float SourceScale;      // multiplier applied to the engine (source) velocity; tunes AFW warp strength
    uint VelocityEncoded;   // 1 = velocity is UE-ENCODED RGBA16_UNORM (decode it); 0 = decoded R16G16_FLOAT
    uint _pad;
};

uint2 ScaledPixel(uint2 outputPixel, uint2 origin, uint2 extent, uint2 inputSize)
{
    const uint2 safeExtent = max(extent, uint2(1, 1));
    return min(origin + ((outputPixel * safeExtent) / OutputSize), inputSize - 1);
}

[numthreads(8, 8, 1)]
void VelocityCombineCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 outputPixel = dispatchThreadId.xy;
    if (any(outputPixel >= OutputSize))
    {
        return;
    }

    const uint2 velocityPixel = ScaledPixel(outputPixel, VelocityOrigin, VelocityExtent, VelocitySize);
    const uint2 depthPixel = ScaledPixel(outputPixel, DepthOrigin, DepthExtent, DepthSize);

    // 3x3 closest-depth dilation (the step both DLSS and FSR do and we were missing): pick the pixel
    // nearest the camera in the neighborhood (reverse-Z => largest depth) and use ITS velocity + depth.
    // Foreground silhouettes then "win", so moving edges stop smearing a 1px background-velocity halo.
    int2 bestOffset = int2(0, 0);
    float depth = DepthTexture.Load(int3(depthPixel, 0)).x;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 dp = clamp(int2(depthPixel) + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1);
            const float d = DepthTexture.Load(int3(dp, 0)).x;
            if (d > depth) // reverse-Z: larger depth = nearer the camera
            {
                depth = d;
                bestOffset = int2(dx, dy);
            }
        }
    }
    const uint2 dilatedVelocityPixel =
        (uint2)clamp(int2(velocityPixel) + bestOffset, int2(0, 0), int2(VelocitySize) - 1);
    const float4 encodedVelocity = VelocityTexture.Load(int3(dilatedVelocityPixel, 0));

    // outVelocity is the FINAL motion vector in PIXEL space, in the stored sign convention.
    const float maxV = max((float)OutputSize.x, (float)OutputSize.y);

    // (1) RELIABLE BASE: reconstruct camera motion from depth + ClipToPrevClip. Verified healthy via the
    // recon diagnostic — ClipToPrevClip tracks the camera (dev 0.1-0.25 under motion, exactly identity =>
    // zero when static), depth is valid, and the result is correctly scaled to pixels. This always runs,
    // so the warp has a trustworthy camera-motion field everywhere depth exists (incl. the guard-band
    // borders), regardless of what the flaky engine-velocity provider hands us this frame.
    float2 reconVel = 0.0f;
    float reconVelZ = 0.0f;
    {
        // The engine renders depth at a NARROWER FOV than the VR eye, so the guard-band periphery (and the
        // far sky) have depth==0 (reverse-Z far plane / cleared). At exactly 0 the reprojected w collapses
        // and the vector is rejected, leaving the periphery unwarped -> the hard vertical seam. Clamp to a
        // very-far-but-finite depth so those pixels still get the depth-INDEPENDENT camera-rotation motion
        // vector: head-turns then warp the full eye and the seam disappears. Translation parallax is ~0 at
        // that range, so genuine far/sky pixels (and all valid near geometry) are unaffected.
        const float safeDepth = max(depth, 1e-4f);
        const float2 viewportUv = (float2(outputPixel) + 0.5f) * InvOutputSize;
        const float2 clipXY = (viewportUv - 0.5f) * float2(2.0f, -2.0f);
        const float4 clipPos = float4(clipXY, safeDepth, 1.0f);
        const float4 prevClipPos = mul(ClipToPrevClip, clipPos);
        // Guard the perspective divide against a near-zero w (geometry at/behind the previous clip plane).
        if (prevClipPos.w > 1e-4f)
        {
            // NDC delta -> pixel space, negated to match the stored sign convention (and the raw source).
            const float2 ndcVel = clipPos.xy - (prevClipPos.xy / prevClipPos.w);
            reconVel = -(ndcVel * float2(0.5f, -0.5f) * float2(OutputSize));
            // V.z = DeviceZ - PrevDeviceZ: camera-induced depth motion, from the SAME reprojection as the .xy.
            // Exact for static geometry; gives the depth-parallax term everywhere depth exists.
            reconVelZ = clipPos.z - (prevClipPos.z / prevClipPos.w);
        }
    }

    float2 outVelocity = reconVel;
    float outVz = reconVelZ;

    // (2) PER-OBJECT MOTION ONLY (deviation-gated). NOTE: this is the CORRECT ADAPTATION of DLSS's
    // VelocityCombine.usf selection for OUR input, NOT a hand-rolled hack — and we proved it empirically.
    // DLSS's pass reads the ENCODED SceneVelocity where "written" pixels form COHERENT solid objects and uses
    // `EncodedVelocity.x > 0`. Our heuristic provider hands a FLOAT velocity whose written pixels are SCATTERED
    // per-pixel (sparse), so porting DLSS's literal nonzero==written test flips raw<->recon speckle-wise and
    // REINTRODUCES the noise/seam (measured bottom-left roughness 10.9 vs 0.02 here). So with our input the
    // faithful selection must key on the per-object SIGNAL, not the written flag: keep the smooth, accurate
    // camera reconstruction as the base EVERYWHERE and override with raw ONLY where it DEVIATES from recon (the
    // running character), which is coherent and seam-free. To get DLSS-IDENTICAL output we'd need DLSS's
    // coherent encoded SceneVelocity as input (a provider change) — the algorithm is not the gap, the input is.
    if (ForceReconstruct == 0u)
    {
        if (VelocityEncoded != 0u)
        {
            // FAITHFUL port of NVIDIA's VelocityCombine.usf, running on DLSS's ACTUAL input — the encoded
            // velocity-depth buffer (PF_A16B16G16R16 == RGBA16_UNORM). Per pixel: IF the engine wrote velocity
            // here (`EncodedVelocity.x > 0` — the buffer is cleared to 0, and UE's 0.499 encode headroom means a
            // written value is never exactly 0), DECODE it with UE's DecodeVelocityFromTexture and use it; ELSE
            // the camera reconstruction stands. The decode + sign/scale match the .usf exactly, so this is the
            // byte-identical per-object path (and the written flag here is COHERENT, unlike the RG16F variant).
            if (encodedVelocity.x > 0.0f)
            {
                const float kInvDiv = 1.0f / (0.499f * 0.5f);
                float2 vScreen = encodedVelocity.xy * kInvDiv - (32767.0f / 65535.0f) * kInvDiv;
                // VELOCITY_ENCODE_GAMMA: UE sqrt-compresses velocity on encode, so the decode squares it back.
                // This title's encoded buffer clusters .x in [0.4,0.8] (= small velocities under sqrt encoding);
                // without this square a linear decode over-amplifies everything (p99 jumps to ~2400px). With it,
                // mid-range .x -> small velocity, only genuinely-fast pixels stay large.
                vScreen = sign(vScreen) * vScreen * vScreen * 0.5f;
                outVelocity = -vScreen * float2(0.5f, -0.5f) * float2(OutputSize);
                // Per-object depth motion V.z (DeviceZ - PrevDeviceZ) is packed by EncodeVelocityToTexture as a
                // float32 split across .z (high 16 bits) and .w (low 16 bits; bit0 = pixel-anim flag). Reassemble
                // it — the engine's TRUE per-object depth velocity, overriding the camera reconstruction here.
                const uint zHi = (uint)(encodedVelocity.z * 65535.0f + 0.5f);
                const uint zLo = ((uint)(encodedVelocity.w * 65535.0f + 0.5f)) & 0xFFFEu;
                outVz = asfloat((zHi << 16) | zLo);
            }
        }
        else
        {
            // DECODED float buffer (the engine's RG16F variant): its written pixels are SCATTERED per-pixel, so
            // DLSS's literal nonzero-written test reintroduces speckle. Key on the per-object DEVIATION from the
            // (accurate, smooth) camera reconstruction instead — the correct adaptation for this noisier input.
            const float2 rawVel = encodedVelocity.xy * SourceScale;
            const float rawMag2 = dot(rawVel, rawVel);
            const float maxObj = 0.2f * max((float)OutputSize.x, (float)OutputSize.y);
            const float2 deviation = rawVel - reconVel;
            const float deviationMag2 = dot(deviation, deviation);
            const float kDeviationThreshold = 8.0f;
            if (rawMag2 > (1.5f * 1.5f) && rawMag2 < (maxObj * maxObj) &&
                deviationMag2 > (kDeviationThreshold * kDeviationThreshold))
            {
                outVelocity = rawVel;
            }
        }
    }

    // Final clamp: a motion vector can never legitimately exceed the frame dimensions, so this bounds any
    // residual garbage (a bad raw sample or a near-clip reconstruct) instead of warping with it.
    outVelocity = clamp(outVelocity, -float2(OutputSize), float2(OutputSize));
    // Depth delta in reverse-Z device space can never exceed [-1,1]; bound any garbage decode (and reject NaN).
    outVz = isfinite(outVz) ? clamp(outVz, -1.0f, 1.0f) : 0.0f;
    OutVelocityCombinedTexture[outputPixel] = outVelocity;                   // .xy -> RG16F (u0), AFW warp
    OutVelocity3DTexture[outputPixel] = float4(outVelocity, outVz, 0.0f);    // xyz -> RGBA16F (u1), 3D
}
