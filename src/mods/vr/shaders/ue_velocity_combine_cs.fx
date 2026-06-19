Texture2D<float4> VelocityTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
RWTexture2D<float2> OutVelocityCombinedTexture : register(u0);

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
    uint3 _pad;
};

float2 DecodeVelocityFromTexture(float4 encodedVelocity)
{
    const float invDiv = 1.0f / (0.499f * 0.5f);
    return encodedVelocity.xy * invDiv - (32767.0f / 65535.0f) * invDiv;
}

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

    const float4 encodedVelocity = VelocityTexture.Load(int3(velocityPixel, 0));
    const float depth = DepthTexture.Load(int3(depthPixel, 0)).x;

    float2 velocity = 0.0f;
    if (ForceReconstruct == 0u && encodedVelocity.x > 0.0f)
    {
        velocity = DecodeVelocityFromTexture(encodedVelocity);
    }
    else
    {
        const float2 viewportUv = (float2(outputPixel) + 0.5f) * InvOutputSize;
        const float2 clipXY = (viewportUv - 0.5f) * float2(2.0f, -2.0f);
        const float4 clipPos = float4(clipXY, depth, 1.0f);
        const float4 prevClipPos = mul(ClipToPrevClip, clipPos);

        if (prevClipPos.w > 0.0f)
        {
            velocity = clipPos.xy - (prevClipPos.xy / prevClipPos.w);
        }
    }

    const float2 outVelocity = velocity * float2(0.5f, -0.5f) * float2(OutputSize);
    OutVelocityCombinedTexture[outputPixel] = -outVelocity;
}
