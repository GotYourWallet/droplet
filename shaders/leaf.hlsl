#include "common.hlsli"

Texture2D<float> gShadowMap : register(t0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 extra   : TEXCOORD1; // x=thickness, y=leafId, z=windWeight, w=0
};

struct PSIn {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float3 tangent  : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float4 extra    : TEXCOORD4;
};

float3 applyLeafWind(float3 pos, float leafId, float windWeight) {
    float t = gCameraPosTime.w;
    float sway = sin(t * 1.45 + leafId * 1.9 + pos.x * 2.5) * 0.014
               + cos(t * 2.75 + leafId * 3.1) * 0.005;
    return pos + float3(sway * 0.4, sway, sway * 0.3) * windWeight;
}

PSIn VSMain(VSIn i) {
    PSIn o;
    float3 wp = applyLeafWind(i.pos, i.extra.y, i.extra.z);
    o.worldPos = wp;
    o.svPos = mul(float4(wp, 1.0), gViewProj);
    o.normal = normalize(i.normal);
    o.tangent = normalize(i.tangent);
    o.uv = i.uv;
    o.extra = i.extra;
    return o;
}

float4 VSShadow(VSIn i) : SV_POSITION {
    float3 wp = applyLeafWind(i.pos, i.extra.y, i.extra.z);
    return mul(float4(wp, 1.0), gLightViewProj);
}

float4 PSMain(PSIn i, bool isFrontFace : SV_IsFrontFace) : SV_TARGET {
    float3 N = normalize(i.normal) * (isFrontFace ? 1.0 : -1.0);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    float u = i.uv.x; // 0=stem, 1=tip
    float v = abs(i.uv.y * 2.0 - 1.0); // 0=midrib, 1=edge

    // Procedural midrib + lateral leaf veins
    float midrib = exp(-v * 26.0) * (1.0 - 0.55 * u);
    float veinWave = abs(sin((u * 15.0 - v * 6.5) * PI));
    float lateralVeins = pow(1.0 - veinWave, 10.0) * smoothstep(0.02, 0.14, v) * (1.0 - v * 0.65);
    float veins = saturate(midrib + lateralVeins * 0.55);

    // Subtle organic color variation & warm autumn/gold edge hint
    float leafVar = fbm3D(i.worldPos * 7.5 + i.extra.y * 4.0);
    float3 deepGreen   = float3(0.032, 0.125, 0.028);
    float3 vibrantLeaf = float3(0.115, 0.275, 0.045);
    float3 veinTint    = float3(0.185, 0.310, 0.075);
    float3 albedo = lerp(deepGreen, vibrantLeaf, saturate(leafVar * 0.9 + u * 0.25));
    albedo = lerp(albedo, veinTint, veins * 0.6);

    // Wet glossy dew film on leaves
    float dewBead = smoothstep(0.76, 0.90, noise3D(i.worldPos * 42.0 + i.extra.y));
    float roughness = lerp(0.24, 0.06, dewBead);
    albedo *= lerp(0.85, 0.72, dewBead);

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, 0.85);

    // Backlit Subsurface Scattering (Barré-Brisebois / Dice foliage translucency model)
    float thickness = saturate(i.extra.x + veins * 0.35);
    float translucency = 1.0 - thickness * 0.72;
    float3 H_back = normalize(L + N * 0.35);
    float VdotH_back = pow(saturate(dot(V, -H_back)), 3.2);
    float wrapLight = saturate(dot(-N, L) * 0.6 + 0.4);
    float3 sssTint = float3(0.45, 0.82, 0.12);
    float3 sss = sssTint * gSunColorFar.xyz * (VdotH_back * 1.45 + wrapLight * 0.28) * translucency * lerp(0.35, 1.0, shadow);

    float3 color = direct + ambient + sss;

    float dist = length(i.worldPos - gCameraPosTime.xyz);
    float fog = 1.0 - exp(-dist * 0.025);
    float3 fogCol = evalForestSky(-V, L, gSunColorFar.xyz) * 0.6;
    color = lerp(color, fogCol, saturate(fog * 0.55));

    return float4(color, 1.0);
}
