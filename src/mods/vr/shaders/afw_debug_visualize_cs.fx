// AFW debug buffer visualizer - false-colors a selected frame resource (motion vectors,
// depth, raw/source scene velocity) into an RGBA output so it can be shown directly in the eye,
// cycled like the DLSS/Streamline buffer visualizer.
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutColor : register(u0);

cbuffer DebugVizConstants : register(b0)
{
    uint   Mode;        // 0=pixel MV hue, 1=depth, 2=source MV hue, 3=source X, 4=source Y, 5=source mag, 6=source valid
    float  Scale;       // magnitude normalization for velocity modes
    uint2  InputSize;
    uint2  OutputSize;
    uint2  _pad;
};

float3 hsv2rgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float2 DecodeVelocityFromTexture(float4 e)
{
    const float invDiv = 1.0f / (0.499f * 0.5f);
    return e.xy * invDiv - (32767.0f / 65535.0f) * invDiv;
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
    const float4 raw = InputTex.Load(int3(ip, 0));

    float3 col;
    if (Mode == 1)
    {
        // Depth (reverse-Z: near = 1 -> white, far = 0 -> black). Gamma a touch for contrast.
        const float d = saturate(raw.x);
        col = pow(float3(d, d, d), 0.5);
    }
    else if (Mode == 3)
    {
        const float2 v = DecodeVelocityFromTexture(raw);
        col = SignedChannelColor(v.x * Scale);
    }
    else if (Mode == 4)
    {
        const float2 v = DecodeVelocityFromTexture(raw);
        col = SignedChannelColor(v.y * Scale);
    }
    else if (Mode == 5)
    {
        const float2 v = DecodeVelocityFromTexture(raw);
        const float val = tanh(length(v) * Scale);
        col = hsv2rgb(float3(0.12f, 1.0f, val));
    }
    else if (Mode == 6)
    {
        // UE encodes invalid/no-velocity pixels as zero, while valid encoded pixels have a nonzero R.
        const float valid = raw.x > 0.0f ? 1.0f : 0.0f;
        col = valid.xxx;
    }
    else
    {
        // Motion vectors: hue = direction, brightness = magnitude (FSR4-style tanh rolloff so the
        // huge AFW pixel-space range stays readable). Near-zero = dark grey (shows sparse regions).
        const float2 v = (Mode == 2) ? DecodeVelocityFromTexture(raw) : raw.xy;
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
