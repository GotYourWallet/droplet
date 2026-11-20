#ifndef COMMON_HLSLI
#define COMMON_HLSLI

static const float PI = 3.14159265359;
static const float TWO_PI = 6.28318530718;

cbuffer FrameCB : register(b0) {
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gLightViewProj;

    float4 gCameraPosTime;      // xyz = cameraPos, w = time
    float4 gSunDirNear;         // xyz = normalized dir TOWARD sun, w = nearPlane
    float4 gSunColorFar;        // xyz = HDR sun radiance, w = farPlane
    float4 gResolutionInv;      // xy = res, zw = 1/res
    float4 gDofParams;          // x = focusDist, y = aperture, z = maxBlurPx, w = enableDof
    float4 gDripOriginPool;     // xyz = branch drip tip, w = poolWaterY
    float4 gHeroDropCaustic;    // xyz = active droplet center, w = active droplet radius
    float4 gFeatureFlags;       // x = tessScale, y = volumetricStrength, z = bloomStrength, w = wireframeMode
};

cbuffer DropletCB : register(b1) {
    float4 gDropPosRadius;      // xyz = droplet center, w = radius
    float4 gDropDeform;         // x = elongation, y = neckPinch, z = oscillation, w = mode (0=pendant, 1=falling, 2=jet)
    float4 gDropAttach;         // xyz = attachment point on branch, w = age/phase
};

// --- Fast Procedural Noise ---
float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float hash31(float3 p3) {
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return frac((p3.x + p3.y) * p3.z);
}

float noise3D(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash31(i + float3(0, 0, 0));
    float n100 = hash31(i + float3(1, 0, 0));
    float n010 = hash31(i + float3(0, 1, 0));
    float n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1));
    float n101 = hash31(i + float3(1, 0, 1));
    float n011 = hash31(i + float3(0, 1, 1));
    float n111 = hash31(i + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

float fbm3D(float3 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * noise3D(p);
        p = p * 2.03 + float3(17.1, 31.7, 11.3);
        amp *= 0.5;
    }
    return sum;
}

float ridgedFbm3D(float3 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        float n = 1.0 - abs(noise3D(p) * 2.0 - 1.0);
        sum += amp * n * n;
        p = p * 2.11 + float3(5.3, 19.7, 29.1);
        amp *= 0.5;
    }
    return sum;
}

// Interleaved Gradient Noise (Jimenez 2014) for clean volumetric dithering
float interleavedGradientNoise(float2 pixelPos) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixelPos, magic.xy)));
}

// --- Golden-Hour Woodland Atmospheric Sky (smooth, moody emerald & warm amber) ---
float3 evalForestSky(float3 dir, float3 sunDir, float3 sunColor) {
    float3 d = normalize(dir);
    float upBlend = smoothstep(-0.32, 0.55, d.y);
    float horizon = exp(-pow(d.y * 3.4, 2.0));

    // Deep moody woodland canopy backdrop so backlit water & golden god rays pop!
    float3 groundCol  = float3(0.008, 0.018, 0.012);
    float3 midForest  = float3(0.020, 0.052, 0.038);
    float3 zenithCol  = float3(0.018, 0.062, 0.095);
    float3 horizonCol = float3(0.16, 0.12, 0.065);

    float3 sky = lerp(groundCol, midForest, smoothstep(-0.35, 0.08, d.y));
    sky = lerp(sky, zenithCol, smoothstep(0.08, 0.72, d.y));
    sky = lerp(sky, horizonCol, horizon * 0.48);

    // Mie forward-scattering golden-hour sunbreak
    float sunCos = max(dot(d, sunDir), 0.0);
    float mieGlow = pow(sunCos, 8.0) * 0.12 + pow(sunCos, 48.0) * 0.55 + pow(sunCos, 320.0) * 3.5;
    sky += sunColor * mieGlow;

    // Soft dappled woodland canopy bokeh & sun-dappled foliage silhouettes
    float2 canopyUV = float2(atan2(d.x, d.z) * 1.8, d.y * 3.2);
    float foliage1 = fbm3D(float3(canopyUV * 1.4, gCameraPosTime.w * 0.025));
    float foliage2 = noise3D(float3(canopyUV * 4.2, 2.1));
    float canopyMask = smoothstep(0.24, 0.62, foliage1 * 0.75 + foliage2 * 0.25) * smoothstep(-0.22, 0.30, d.y);
    float3 canopyDark = float3(0.008, 0.026, 0.014) + sunColor * 0.035 * pow(sunCos, 4.0);
    sky = lerp(sky, canopyDark, canopyMask * 0.78);

    // Subtle glowing sunlit foliage bokeh dots in the distance
    float bokeh = smoothstep(0.76, 0.88, foliage2) * smoothstep(0.2, 0.8, foliage1) * pow(sunCos, 2.5);
    sky += sunColor * bokeh * 0.36 * upBlend;

    return max(sky, 0.0);
}

// --- Cook-Torrance GGX PBR ---
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 evalPBRDirect(float3 N, float3 V, float3 L, float3 albedo, float roughness, float metallic, float3 lightRadiance) {
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return 0.0;

    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float D = distributionGGX(NdotH, max(roughness, 0.04));
    float G = geometrySmith(NdotV, NdotL, max(roughness, 0.04));
    float3 F = fresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-5);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * lightRadiance * NdotL;
}

float3 evalIBLAmbient(float3 N, float3 V, float3 albedo, float roughness, float metallic, float ao) {
    float3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 1e-4);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = F0 + (max(1.0 - roughness, F0) - F0) * pow(saturate(1.0 - NdotV), 5.0);

    // Hemisphere diffuse irradiance from canopy & mossy floor
    float3 skyIrr = float3(0.20, 0.31, 0.34);
    float3 groundIrr = float3(0.09, 0.13, 0.07);
    float3 irradiance = lerp(groundIrr, skyIrr, N.y * 0.5 + 0.5);

    // Approximate glossy reflection from procedural forest sky
    float3 envRefl = evalForestSky(R, gSunDirNear.xyz, gSunColorFar.xyz * 0.45);
    float reflAtten = lerp(1.0, 0.22, roughness * roughness);

    float3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo * irradiance + F * envRefl * reflAtten) * ao;
}

// --- 16-tap Rotated Poisson PCF Soft Shadows ---
static const float2 gPoissonDisk[16] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790)
};

float sampleShadowPCF(Texture2D<float> shadowMap, SamplerComparisonState shadowSamp, float3 worldPos, float3 N) {
    float4 lightClip = mul(float4(worldPos + N * 0.012, 1.0), gLightViewProj);
    float3 projCoords = lightClip.xyz / lightClip.w;
    float2 uv = projCoords.xy * float2(0.5, -0.5) + 0.5;
    float depth = projCoords.z;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depth > 1.0)
        return 1.0;

    float bias = max(0.0018 * (1.0 - saturate(dot(N, gSunDirNear.xyz))), 0.0005);
    float angle = hash12(worldPos.xz * 47.3) * TWO_PI;
    float s = sin(angle), c = cos(angle);
    float2x2 rot = float2x2(c, -s, s, c);

    float texel = 1.0 / 2048.0;
    float shadow = 0.0;
    [unroll]
    for (int i = 0; i < 16; ++i) {
        float2 offset = mul(gPoissonDisk[i], rot) * (texel * 2.2);
        shadow += shadowMap.SampleCmpLevelZero(shadowSamp, uv + offset, depth - bias);
    }
    return shadow * (1.0 / 16.0);
}

// --- Analytical Projected Droplet Lens Caustics (with subtle spectral dispersion) ---
float3 evalDropletLensCaustic(float3 worldPos, float3 N, float3 dropCenter, float dropRadius, float3 sunDir, float3 sunColor) {
    if (dropRadius < 0.005) return 0.0;

    // Ray from droplet center along -sunDir (direction light travels)
    float3 lightRay = -normalize(sunDir);
    float3 toPos = worldPos - dropCenter;
    float along = dot(toPos, lightRay);
    if (along <= 0.0 || along > 4.5) return 0.0;

    float3 closest = dropCenter + lightRay * along;
    float distPerp = length(worldPos - closest);

    // Spherical water lens focuses light tightly near focal length ~2.0*R, then softly spreads
    float focalSpread = max(dropRadius * 0.35, abs(along - dropRadius * 2.2) * 0.14 + dropRadius * 0.25);

    // Subtle RGB chromatic dispersion ring
    float cR = exp(-pow(distPerp / (focalSpread * 1.06), 2.0));
    float cG = exp(-pow(distPerp / (focalSpread * 1.00), 2.0));
    float cB = exp(-pow(distPerp / (focalSpread * 0.94), 2.0));

    // Bright ring + focused core
    float ring = exp(-pow((distPerp - focalSpread * 0.65) / (focalSpread * 0.35), 2.0)) * 0.55;
    float atten = 1.0 / (1.0 + along * along * 0.45);
    float facing = saturate(dot(N, sunDir) * 0.7 + 0.3);

    return (float3(cR, cG, cB) + ring * float3(1.1, 0.95, 0.8)) * sunColor * (1.6 * atten * facing);
}

// --- ACES Filmic Tone Mapping ---
float3 acesToneMap(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float linearizeDepth(float d, float nearZ, float farZ) {
    return (nearZ * farZ) / max(farZ - d * (farZ - nearZ), 1e-5);
}

#endif
