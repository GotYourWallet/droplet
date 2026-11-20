#include "common.hlsli"

Texture2D<float>  gShadowMap      : register(t0);
Texture2D<float4> gWaterNormalTex : register(t1); // rgb = normal, a = wave height
Texture2D<float4> gCausticTex     : register(t2); // rgb = underwater compute caustics
Texture2D<float4> gSceneColor     : register(t3); // pre-water HDR scene for refraction

SamplerState gLinearClamp         : register(s0);
SamplerComparisonState gShadowSamp: register(s1);

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 extra   : TEXCOORD1;
};

struct PSIn {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float4 extra    : TEXCOORD3;
};

// --- Pool Bed & Mossy Forest Floor ---
PSIn VSPoolBed(VSIn i) {
    PSIn o;
    o.worldPos = i.pos;
    o.svPos = mul(float4(i.pos, 1.0), gViewProj);
    o.normal = normalize(i.normal);
    o.uv = i.uv;
    o.extra = i.extra;
    return o;
}

float4 PSPoolBed(PSIn i) : SV_TARGET {
    float3 N = normalize(i.normal);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    float waterDepth = i.extra.x;
    float radialDist = i.extra.y;
    float mossMask   = i.extra.z;

    // Procedural river pebbles + wet loam + mossy forest bank
    float pebbleN = ridgedFbm3D(i.worldPos * 4.8);
    float fineN   = fbm3D(i.worldPos * 14.0);
    float mossN   = fbm3D(i.worldPos * 3.5 + float3(4.2, 0.0, 1.9));

    float3 siltCol   = float3(0.045, 0.038, 0.028);
    float3 stoneCol  = lerp(float3(0.11, 0.10, 0.085), float3(0.18, 0.16, 0.13), fineN);
    float3 bedAlbedo = lerp(siltCol, stoneCol, smoothstep(0.32, 0.72, pebbleN));

    float3 mossCol = lerp(float3(0.035, 0.095, 0.024), float3(0.125, 0.225, 0.048), fineN);
    float mossBlend = saturate(mossMask * smoothstep(0.28, 0.68, mossN));
    float3 albedo = lerp(bedAlbedo, mossCol, mossBlend);

    // Wet shoreline darkening
    float shorelineWet = smoothstep(0.35, -0.05, i.worldPos.y - gDripOriginPool.w);
    albedo *= lerp(1.0, 0.55, shorelineWet);
    float roughness = lerp(0.84, 0.28, shorelineWet * (1.0 - mossBlend * 0.6));

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, 0.8);

    // Compute-generated underwater caustics projected onto the submerged pool bed (soft 5-tap filter)
    float3 caustics = 0.0;
    if (waterDepth > 0.002 && i.uv.x > 0.0 && i.uv.x < 1.0 && i.uv.y > 0.0 && i.uv.y < 1.0) {
        float2 refrShift = -L.xz * (waterDepth * 0.22);
        float2 cuv = saturate(i.uv + refrShift);
        float dt = 1.5 / 256.0;
        float3 cSample = gCausticTex.SampleLevel(gLinearClamp, cuv, 0).rgb * 0.36
                       + gCausticTex.SampleLevel(gLinearClamp, saturate(cuv + float2( dt,  0.0)), 0).rgb * 0.16
                       + gCausticTex.SampleLevel(gLinearClamp, saturate(cuv + float2(-dt,  0.0)), 0).rgb * 0.16
                       + gCausticTex.SampleLevel(gLinearClamp, saturate(cuv + float2( 0.0,  dt)), 0).rgb * 0.16
                       + gCausticTex.SampleLevel(gLinearClamp, saturate(cuv + float2( 0.0, -dt)), 0).rgb * 0.16;
        float depthWeight = smoothstep(0.0, 0.06, waterDepth) * exp(-waterDepth * 1.15);
        caustics = cSample * gSunColorFar.xyz * (0.36 * depthWeight * lerp(0.20, 1.0, shadow));
    }

    // Analytical lens caustic focused by the hanging/falling droplet above
    float3 dropCaustic = evalDropletLensCaustic(i.worldPos, N, gHeroDropCaustic.xyz, gHeroDropCaustic.w, L, gSunColorFar.xyz);

    float3 color = direct + ambient + caustics + dropCaustic * 0.45;

    // Gentle woodland mist
    float dist = length(i.worldPos - gCameraPosTime.xyz);
    float fog = 1.0 - exp(-dist * 0.055);
    float3 fogCol = evalForestSky(-V, L, gSunColorFar.xyz) * 0.65;
    color = lerp(color, fogCol, saturate(fog * 0.72));

    return float4(color, 1.0);
}

// --- Rippling Pool Water Surface ---
PSIn VSPoolWater(VSIn i) {
    float4 waterSample = gWaterNormalTex.SampleLevel(gLinearClamp, i.uv, 0);
    float3 wp = i.pos;
    wp.y += waterSample.a * 0.14;

    PSIn o;
    o.worldPos = wp;
    o.svPos = mul(float4(wp, 1.0), gViewProj);
    o.normal = normalize(waterSample.rgb);
    o.uv = i.uv;
    o.extra = float4(i.extra.x, waterSample.a, 0.0, 0.0);
    return o;
}

float4 PSPoolWater(PSIn i) : SV_TARGET {
    float4 waterSample = gWaterNormalTex.SampleLevel(gLinearClamp, i.uv, 0);
    float3 N = normalize(waterSample.rgb);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    // Smooth circular shoreline fade into the mossy bank
    float r = i.extra.x;
    float edgeAlpha = 1.0 - smoothstep(1.95, 2.48, r);
    if (edgeAlpha < 0.005) discard;

    // Screen-space refraction of the submerged caustic-lit pool bed
    float2 screenUV = i.svPos.xy * gResolutionInv.zw;
    float2 refrOffset = N.xz * 0.042;
    float3 refrR = gSceneColor.SampleLevel(gLinearClamp, saturate(screenUV + refrOffset * 0.94), 0).rgb;
    float3 refrG = gSceneColor.SampleLevel(gLinearClamp, saturate(screenUV + refrOffset * 1.00), 0).rgb;
    float3 refrB = gSceneColor.SampleLevel(gLinearClamp, saturate(screenUV + refrOffset * 1.06), 0).rgb;
    float3 refracted = float3(refrR.r, refrG.g, refrB.b);

    // Subtle deep-emerald woodland water clarity
    float3 waterTint = float3(0.46, 0.66, 0.58);
    refracted *= waterTint;

    // Schlick Fresnel reflection of the moody woodland canopy & golden sun
    float NdotV = saturate(dot(N, V));
    float F = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);

    float3 R = reflect(-V, N);
    float3 skyRefl = evalForestSky(R, L, gSunColorFar.xyz) * 0.36;

    // Crisp sun specular sparkles along expanding ripple crests
    float3 H = normalize(V + L);
    float spec = pow(saturate(dot(N, H)), 720.0) * 2.4 + pow(saturate(dot(N, H)), 120.0) * 0.14;
    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 specular = gSunColorFar.xyz * spec * lerp(0.20, 1.0, shadow);

    float3 color = lerp(refracted, skyRefl, saturate(F * 0.72)) + specular;
    return float4(color, edgeAlpha);
}
