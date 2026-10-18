cbuffer Params : register(b0)
{
    float2 gSrcSize;
    float2 gInvSrcSize;
    float2 gDstSize;
    float2 gInvDstSize;
    float2 gFullSize;
    float2 gInvFullSize;
    float  gLambda;
    float  gZeroBias;
    int    gRadius;
    uint   gFlags;
    float  gSceneCut;
    float  gFlowScale;
    float  gOcclK;
    float  gT;
};

SamplerState gLinear : register(s0);

Texture2D<float4> tColA : register(t0);
Texture2D<float4> tColB : register(t1);
Texture2D<float>  tLumA : register(t0);
Texture2D<float>  tLumB : register(t1);
Texture2D<float4> tFlow : register(t2);
Texture2D<float>  tStats : register(t3);

RWTexture2D<float>  uLum   : register(u0);
RWTexture2D<float4> uFlow  : register(u0);
RWTexture2D<float4> uOut  : register(u0);
RWTexture2D<float> uStats : register(u0);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;

    float2 uv = (float2(id.xy) * 2.0 + 1.0) * gInvFullSize;
    float3 c = tColA.SampleLevel(gLinear, uv, 0).rgb;
    uLum[id.xy] = Luma(c);
}

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;
    float2 uv = (float2(id.xy) * 2.0 + 1.0) * gInvSrcSize;
    uLum[id.xy] = tLumA.SampleLevel(gLinear, uv, 0);
}

float PatchCost(float2 centre, float2 v)
{
    float2 pa = (centre - 0.5 * v) * gInvSrcSize;
    float2 pb = (centre + 0.5 * v) * gInvSrcSize;
    float a[25];
    float b[25];
    float ma = 0.0, mb = 0.0;
    [unroll] for (int j = 0; j < 5; ++j)
    {
        [unroll] for (int i = 0; i < 5; ++i)
        {
            float2 o = float2(i - 2, j - 2) * gInvSrcSize;
            float la = tLumA.SampleLevel(gLinear, pa + o, 0);
            float lb = tLumB.SampleLevel(gLinear, pb + o, 0);
            a[j * 5 + i] = la;
            b[j * 5 + i] = lb;
            ma += la;
            mb += lb;
        }
    }
    ma *= (1.0 / 25.0);
    mb *= (1.0 / 25.0);
    float d = ma - mb;
    float s = 0.0;
    [unroll] for (int k = 0; k < 25; ++k)
        s += abs((a[k] - b[k]) - d);
    return (s + 6.0 * abs(d)) * (1.0 / 25.0);
}

float TotalCost(float2 centre, float2 v, float2 pred)
{
    return PatchCost(centre, v) + gLambda * length(v - pred) + gZeroBias * length(v);
}

[numthreads(8, 8, 1)]
void CSSearchCoarse(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gSrcSize.x || id.y >= (uint)gSrcSize.y) return;
    float2 c = float2(id.xy) + 0.5;
    float  best = 1e30;
    float2 bestV = float2(0.0, 0.0);
    int r = gRadius;
    [loop] for (int y = -r; y <= r; ++y)
    {
        [loop] for (int x = -r; x <= r; ++x)
        {
            float2 v = float2(x, y);
            float cst = TotalCost(c, v, float2(0.0, 0.0));
            if (cst < best) { best = cst; bestV = v; }
        }
    }

    float2 base = bestV;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        float2 v = base + 0.5 * float2((k % 3) - 1, (k / 3) - 1);
        float cst = TotalCost(c, v, float2(0.0, 0.0));
        if (cst < best) { best = cst; bestV = v; }
    }
    uFlow[id.xy] = float4(bestV, PatchCost(c, bestV), 0.0);
}

[numthreads(8, 8, 1)]
void CSSearchRefine(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;
    float2 c = float2(id.xy) + 0.5;

    uint cw, ch;
    tFlow.GetDimensions(cw, ch);
    int2 cp = int2(id.xy) >> 1;
    int2 cmax = int2(cw, ch) - 1;

    float2 pred = tFlow.SampleLevel(gLinear, c * 0.5 / float2(cw, ch), 0).xy * 2.0;

    float  best = TotalCost(c, pred, pred);
    float2 bestV = pred;

    [unroll] for (int k = 0; k < 9; ++k)
    {
        int2 q = clamp(cp + int2((k % 3) - 1, (k / 3) - 1), int2(0, 0), cmax);
        float2 v = tFlow.Load(int3(q, 0)).xy * 2.0;
        float cst = TotalCost(c, v, pred);
        if (cst < best) { best = cst; bestV = v; }
    }
    {
        float cst = TotalCost(c, float2(0.0, 0.0), pred);
        if (cst < best) { best = cst; bestV = float2(0.0, 0.0); }
    }

    float2 base = bestV;
    int r = gRadius;
    [loop] for (int y = -r; y <= r; ++y)
    {
        [loop] for (int x = -r; x <= r; ++x)
        {
            if (x == 0 && y == 0) continue;
            float2 v = base + float2(x, y);
            float cst = TotalCost(c, v, pred);
            if (cst < best) { best = cst; bestV = v; }
        }
    }

    base = bestV;
    [unroll] for (int s = 0; s < 9; ++s)
    {
        if (s == 4) continue;
        float2 v = base + 0.5 * float2((s % 3) - 1, (s / 3) - 1);
        float cst = TotalCost(c, v, pred);
        if (cst < best) { best = cst; bestV = v; }
    }

    uFlow[id.xy] = float4(bestV, PatchCost(c, bestV), 0.0);
}

[numthreads(8, 8, 1)]
void CSPropagate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;
    float2 c = float2(id.xy) + 0.5;
    int2 p = int2(id.xy);
    int2 pmax = int2(gDstSize) - 1;
    int st = max(gRadius, 1);

    float2 mean = float2(0.0, 0.0);
    float2 vs[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        int2 q = clamp(p + st * int2((k % 3) - 1, (k / 3) - 1), int2(0, 0), pmax);
        vs[k] = tFlow.Load(int3(q, 0)).xy;
        mean += vs[k];
    }
    mean *= (1.0 / 9.0);

    float  best = TotalCost(c, vs[4], mean);
    float2 bestV = vs[4];
    [unroll] for (int k2 = 0; k2 < 9; ++k2)
    {
        if (k2 == 4) continue;
        float cst = TotalCost(c, vs[k2], mean);
        if (cst < best) { best = cst; bestV = vs[k2]; }
    }
    uFlow[id.xy] = float4(bestV, PatchCost(c, bestV), 0.0);
}

[numthreads(1, 1, 1)]
void CSStats(uint3 id : SV_DispatchThreadID)
{
    uint w = (uint)gSrcSize.x, h = (uint)gSrcSize.y;
    float sc = 0.0, sz = 0.0;
    for (uint y = 0; y < h; ++y)
    {
        for (uint x = 0; x < w; ++x)
        {
            int3 q = int3(x, y, 0);
            sc += tFlow.Load(q).z;
            sz += abs(tLumA.Load(q) - tLumB.Load(q));
        }
    }
    float n = max(1.0, (float)(w * h));
    uStats[uint2(0, 0)] = sc / n;
    uStats[uint2(1, 0)] = sz / n;
}

float3 FlowColor(float2 v)
{

    float len = length(v);
    float m = saturate(len / 64.0);
    float2 d = len > 0.001 ? v / len : float2(0.0, 0.0);
    return float3(0.5 + 0.5 * d.x * m, 0.5 + 0.5 * d.y * m, 0.5 * (1.0 - m) + m);
}

[numthreads(8, 8, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gFullSize.x || id.y >= (uint)gFullSize.y) return;
    float2 uv = (float2(id.xy) + 0.5) * gInvFullSize;

    float4 z0 = tColA.SampleLevel(gLinear, uv, 0);
    float4 z1 = tColB.SampleLevel(gLinear, uv, 0);

    if (tStats.Load(int3(0, 0, 0)) > gSceneCut)
    {
        uOut[id.xy] = gT < 0.5 ? z0 : z1;
        return;
    }

    float4 f = tFlow.SampleLevel(gLinear, uv, 0);
    float2 v = f.xy * gFlowScale;

    if ((gFlags & 1u) != 0u)
    {
        uOut[id.xy] = float4(FlowColor(v), 1.0);
        return;
    }
    if ((gFlags & 2u) != 0u)
    {
        uOut[id.xy] = lerp(z0, z1, gT);
        return;
    }

    float2 d = v * gInvFullSize;
    float4 a = tColA.SampleLevel(gLinear, uv - gT * d, 0);
    float4 b = tColB.SampleLevel(gLinear, uv + (1.0 - gT) * d, 0);

    float dw = abs(Luma(a.rgb) - Luma(b.rgb));
    float dz = abs(Luma(z0.rgb) - Luma(z1.rgb));

    float4 warped = lerp(a, b, gT);
    float4 still  = lerp(z0, z1, gT);

    float wStill = max(saturate((dw - dz) * gOcclK), saturate(1.0 - dz * 1000.0));
    float4 outc = lerp(warped, still, wStill);

    float mis = min(dw, dz);
    if (mis > 0.12)
    {
        float da = length(a.rgb - still.rgb);
        float db = length(b.rgb - still.rgb);
        float4 pick = (da < db) ? a : b;
        outc = lerp(outc, pick, saturate((mis - 0.12) * 8.0));
    }
    uOut[id.xy] = float4(outc.rgb, 1.0);
}
