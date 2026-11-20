#include "common.hlsli"

Texture2D<float>  gShadowMap      : register(t0);
Texture2D<float4> gWaterNormalTex : register(t1); // rgb = normal, a = wave height
Texture2D<float4> gCausticTex     : register(t2); // rgb = underwater compute caustics
Texture2D<float4> gSceneColor     : register(t3); // pre-water HDR scene for refraction
Texture2D<float4> gGrassAlbedoTex : register(t4); // rgb = linear woodland grass albedo, a = AO
Texture2D<float4> gGrassNormalTex : register(t5); // rgb = tangent-space normal, a = roughness

SamplerState gLinearClamp         : register(s0);
SamplerComparisonState gShadowSamp: register(s1);
SamplerState gAnisoWrap           : register(s2);

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
    float3 geomN = normalize(i.normal);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    float waterDepth = i.extra.x;
    float radialDist = i.extra.y;
    float mossMask   = i.extra.z;

    // Procedural river pebbles & wet silt for the submerged pool bed
    float pebbleN = ridgedFbm3D(i.worldPos * 4.8);
    float fineN   = fbm3D(i.worldPos * 14.0);

    float3 siltCol   = float3(0.042, 0.035, 0.025);
    float3 stoneCol  = lerp(float3(0.10, 0.092, 0.078), float3(0.17, 0.15, 0.12), fineN);
    float3 bedAlbedo = lerp(siltCol, stoneCol, smoothstep(0.32, 0.72, pebbleN));

    // Dual-scale anti-tiled 1024x1024 realistic woodland grass PBR texture (Albedo + Normal + AO + Roughness)
    float2 uv1 = i.worldPos.xz * 0.65;
    float2x2 rot37 = float2x2(0.7986, -0.6018, 0.6018, 0.7986);
    float2 uv2 = mul(rot37, i.worldPos.xz) * 0.34 + float2(0.41, 0.73);
    float macroVar = fbm3D(i.worldPos * 0.55 + float3(2.3, 0.0, 5.1));
    float tileBlend = smoothstep(0.28, 0.72, macroVar);

    float4 gAlb1 = gGrassAlbedoTex.Sample(gAnisoWrap, uv1);
    float4 gAlb2 = gGrassAlbedoTex.Sample(gAnisoWrap, uv2);
    float4 gNrm1 = gGrassNormalTex.Sample(gAnisoWrap, uv1);
    float4 gNrm2 = gGrassNormalTex.Sample(gAnisoWrap, uv2);

    // Macro woodland color variation (lush emerald patches vs slightly warmer sunlit clover/moss)
    float3 grassAlbedo = lerp(gAlb1.rgb, gAlb2.rgb, tileBlend * 0.55);
    float3 tint = lerp(float3(0.84, 0.94, 0.80), float3(1.16, 1.10, 0.88), smoothstep(0.30, 0.75, macroVar));
    grassAlbedo *= tint;

    float grassAO    = lerp(gAlb1.a, gAlb2.a, tileBlend * 0.55);
    float grassRough = lerp(gNrm1.a, gNrm2.a, tileBlend * 0.55);

    // Reconstruct tangent-space grass blade normal and blend onto terrain geometry normal
    float3 tsN1 = gNrm1.rgb * 2.0 - 1.0;
    float3 tsN2 = gNrm2.rgb * 2.0 - 1.0;
    // Rotate secondary normal back into primary XZ frame
    float2 rotN2 = mul(transpose(rot37), tsN2.xy);
    float3 tsN = normalize(lerp(tsN1, float3(rotN2, tsN2.z), tileBlend * 0.55));

    float3 T = normalize(float3(1.0, 0.0, 0.0) - geomN * geomN.x);
    float3 B = normalize(cross(geomN, T));
    float3 grassWorldN = normalize(T * tsN.x + B * tsN.y + geomN * tsN.z);

    // Smoothly transition from lush textured grass bank to wet shoreline stones & submerged pool bed
    float grassBlend = saturate(mossMask * smoothstep(-0.02, 0.14, i.worldPos.y - gDripOriginPool.w));
    float3 N = normalize(lerp(geomN, grassWorldN, grassBlend * 0.90));
    float3 albedo = lerp(bedAlbedo, grassAlbedo, grassBlend);
    float ao = lerp(0.82, lerp(0.50, 1.0, grassAO), grassBlend);

    // Wet shoreline darkening & specular gloss near the water's edge
    float shorelineWet = smoothstep(0.32, -0.04, i.worldPos.y - gDripOriginPool.w);
    albedo *= lerp(1.0, 0.52, shorelineWet);
    float roughness = lerp(0.84, grassRough, grassBlend);
    roughness = lerp(roughness, 0.24, shorelineWet * (1.0 - grassBlend * 0.55));

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, ao);

    // Golden-hour backlit velvety sheen along grass blade tips
    float grassRim = pow(1.0 - saturate(dot(geomN, V)), 2.8) * grassBlend * saturate(dot(-V, L) * 0.55 + 0.45);
    float3 grassSheen = float3(0.24, 0.38, 0.09) * gSunColorFar.xyz * (grassRim * grassAO * 0.22 * lerp(0.35, 1.0, shadow));

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

    float3 color = direct + ambient + grassSheen + caustics + dropCaustic * 0.45;

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
