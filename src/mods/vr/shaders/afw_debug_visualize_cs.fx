// AFW debug buffer visualizer - false-colors a selected frame resource (motion vectors,
// depth, raw/source scene velocity) into an RGBA output so it can be shown directly in the eye,
// cycled like the DLSS/Streamline buffer visualizer.
//
// Both the combined (reconstructed) velocity and the engine source velocity are RAW R16G16_FLOAT
// fields (0 = no motion), NOT UE's 0.5-encoded SceneVelocity, so every velocity mode reads raw.xy
// directly. (Decoding a raw buffer offsets every pixel by ~-2 and paints solid garbage.)
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutColor : register(u0);

cbuffer DebugVizConstants : register(b0)
{
    uint   Mode;        // 0=pixel MV hue,1=depth,2=src MV hue,3=src X,4=src Y,5=src mag,6=src valid,7=src Z,8=combined Z
    float  Scale;       // magnitude normalization for velocity modes
    uint2  InputSize;
    uint2  OutputSize;
    uint   SourceEncoded; // 1 = source is UE-encoded RGBA16_UNORM (decode V.z from B+A); 0 = not encoded
    uint   _pad;
};

float3 hsv2rgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float3 SignedChannelColor(float v)
{
    const float a = tanh(abs(v));
    const float3 neutral = float3(0.08, 0.08, 0.08);
    const float3 positive = float3(1.0, 0.08, 0.03);
    const float3 negative = float3(0.05, 0.55, 1.0);
    return lerp(neutral, v >= 0.0f ? positive : negative, a);
}

[numthreads(8, 8, 1)]
void DebugVisualizeCS(uint3 tid : SV_DispatchThreadID)
{
    const uint2 op = tid.xy;
    if (any(op >= OutputSize))
    {
        return;
    }
    const uint2 ip = min((op * InputSize) / OutputSize, InputSize - 1);
    float4 raw = InputTex.Load(int3(ip, 0));
    // Source-velocity modes (2-6) read the raw engine buffer. Reject two kinds of non-motion: the float16
    // overflow/sentinel (~65504) AND the engine's persistent near-plane baseline/jitter velocity (~0.8,
    // TAA sub-pixel jitter amplified by steep near-floor parallax) which otherwise paints a solid block at
    // the bottom of the frame even at rest. Keep real motion (dead-zone .. 1000). Mode 0 (combined, pixel
    // space) and Mode 1 (depth, < 1) are left untouched.
    if (Mode >= 2)
    {
        const float m = length(raw.xy);
        // FSR-style: only a tiny noise floor (FSR zeros sub-~0.1px motion) + reject the float16 overflow.
        // A small floor avoids the hard "topographic" contour banding that a large dead-zone produces; the
        // remaining near-plane baseline shows only faintly once the scale below is normalized.
        if (m < 0.1f || m > 1000.0f)
        {
            raw.x = 0.0f;
            raw.y = 0.0f;
        }
    }

    float3 col;
    if (Mode == 1)
    {
        // Depth (reverse-Z: near = 1 -> white, far = 0 -> black). Gamma a touch for contrast.
        const float d = saturate(raw.x);
        col = pow(float3(d, d, d), 0.5);
    }
    else if (Mode == 3)
    {
        col = SignedChannelColor(raw.x * Scale); // source velocity X (raw)
    }
    else if (Mode == 4)
    {
        col = SignedChannelColor(raw.y * Scale); // source velocity Y (raw)
    }
    else if (Mode == 5)
    {
        const float val = tanh(length(raw.xy) * Scale); // source velocity magnitude (raw)
        col = hsv2rgb(float3(0.12f, 1.0f, val));
    }
    else if (Mode == 6)
    {
        const float valid = (raw.x != 0.0f || raw.y != 0.0f) ? 1.0f : 0.0f;
        col = valid.xxx;
    }
    else if (Mode == 7)
    {
        // Source per-object depth velocity V.z (DeviceZ - PrevDeviceZ), decoded from the encoded buffer's
        // B (.z = high 16 bits) and A (.w = low 16 bits) channels. Red = nearer, blue = farther.
        float vz = 0.0f;
        if (SourceEncoded != 0u)
        {
            const uint zHi = (uint)(raw.z * 65535.0f + 0.5f);
            const uint zLo = ((uint)(raw.w * 65535.0f + 0.5f)) & 0xFFFEu;
            const float d = asfloat((zHi << 16) | zLo);
            vz = isfinite(d) ? d : 0.0f;
        }
        col = SignedChannelColor(vz * Scale);
    }
    else if (Mode == 8)
    {
        // Combined depth velocity V.z (already-decoded float in the combine output .z channel).
        col = SignedChannelColor(raw.z * Scale);
    }
    else
    {
        // Motion vectors: hue = direction, brightness = magnitude (tanh rolloff so the wide AFW
        // pixel-space range stays readable). Near-zero = dark grey. Both Mode 0 (combined pixels) and
        // Mode 2 (source) read raw.xy directly.
        const float2 v = raw.xy;
        const float val = tanh(length(v) * Scale);
        if (val < 0.02f)
        {
            col = float3(0.06, 0.06, 0.06);
        }
        else
        {
            const float ang = (atan2(v.y, v.x) / 6.2831853f) + 0.5f;
            col = hsv2rgb(float3(ang, 1.0f, val));
        }
    }
    OutColor[op] = float4(col, 1.0f);
}
