#include "common.hlsli"

Texture2D<float> gSceneDepth : register(t0);
Texture2D<float> gShadowMap  : register(t1);
SamplerState gLinearClamp    : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSOut {
    float4 svPos : SV_POSITION;
    float2 uv    : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID) {
    VSOut o;
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? -3.0 : 1.0);
    o.svPos = float4(pos, 1.0, 1.0);
    o.uv = pos * float2(0.5, -0.5) + 0.5;
    return o;
}

float3 reconstructRayDir(float2 uv) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    float4 worldFar = mul(float4(ndc, 1.0, 1.0), gInvViewProj);
    worldFar.xyz /= worldFar.w;
    return normalize(worldFar.xyz - gCameraPosTime.xyz);
}

float4 PSSky(VSOut i) : SV_TARGET {
    float3 dir = reconstructRayDir(i.uv);
    float3 sky = evalForestSky(dir, gSunDirNear.xyz, gSunColorFar.xyz);
    return float4(sky, 1.0);
}

// Henyey-Greenstein phase function for forward-scattering forest mist
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Animated dappled woodland canopy gobo projected along sun direction
float evalCanopyGobo(float3 worldPos, float3 sunDir) {
    // Project onto an imaginary canopy plane at y = 6.5 along sunDir
    float t = (6.5 - worldPos.y) / max(sunDir.y, 0.15);
    float2 canopyXZ = worldPos.xz + sunDir.xz * t;
    float wind = gCameraPosTime.w * 0.12;
    float n1 = fbm3D(float3(canopyXZ * 0.42 + float2(wind, -wind * 0.5), 1.3));
    float n2 = noise3D(float3(canopyXZ * 1.15 - float2(wind * 0.7, wind), 4.7));
    return smoothstep(0.46, 0.66, n1 * 0.75 + n2 * 0.25);
}

float4 PSVolumetric(VSOut i) : SV_TARGET {
    float strength = gFeatureFlags.y;
    if (strength <= 0.001) return 0.0;

    float3 rayOrigin = gCameraPosTime.xyz;
    float3 rayDir = reconstructRayDir(i.uv);
    float3 sunDir = normalize(gSunDirNear.xyz);

    float rawDepth = gSceneDepth.SampleLevel(gLinearClamp, i.uv, 0);
    float maxDist = 14.0;
    if (rawDepth < 0.9999) {
        float2 ndc = float2(i.uv.x * 2.0 - 1.0, (1.0 - i.uv.y) * 2.0 - 1.0);
        float4 wp = mul(float4(ndc, rawDepth, 1.0), gInvViewProj);
        wp.xyz /= wp.w;
        maxDist = min(maxDist, length(wp.xyz - rayOrigin));
    }

    const int STEPS = 28;
    float startDist = 0.35;
    float marchLen = max(0.0, maxDist - startDist);
    if (marchLen < 0.05) return 0.0;

    float stepSize = marchLen / (float)STEPS;
    float jitter = interleavedGradientNoise(i.svPos.xy + frac(gCameraPosTime.w * 11.7) * 64.0);

    float cosTheta = dot(rayDir, sunDir);
    float phase = lerp(phaseHG(cosTheta, 0.76), 1.0 / (4.0 * PI), 0.10);

    float3 accum = 0.0;
    float transmittance = 1.0;

    [loop]
    for (int s = 0; s < STEPS; ++s) {
        float t = startDist + ((float)s + jitter) * stepSize;
        float3 p = rayOrigin + rayDir * t;

        // Height-exponential woodland mist density (thicker near forest floor/pool)
        float heightDensity = exp(-max(p.y + 0.5, 0.0) * 0.55);
        float mist = (0.022 + 0.036 * heightDensity);

        // Shadow map check (branch & leaves carve volumetric shadow shafts!)
        float4 lightClip = mul(float4(p, 1.0), gLightViewProj);
        float3 proj = lightClip.xyz / lightClip.w;
        float2 suv = proj.xy * float2(0.5, -0.5) + 0.5;
        float shadow = 1.0;
        if (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0 && proj.z < 1.0) {
            shadow = gShadowMap.SampleCmpLevelZero(gShadowSamp, suv, proj.z - 0.002);
        }

        // Canopy dappled sunbeam gobo
        float gobo = evalCanopyGobo(p, sunDir);

        float3 inScatter = gSunColorFar.xyz * (shadow * gobo * phase * mist * stepSize * 0.65);
        accum += transmittance * inScatter;
        transmittance *= exp(-mist * stepSize * 0.28);
    }

    return float4(accum * strength, 1.0);
}
