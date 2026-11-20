#include "common.hlsli"

Texture2D<float> gShadowMap : register(t0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 extra   : TEXCOORD1; // x=radius, y=wetnessChannel, z=mossMask, w=branchId
};

struct HSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 extra   : TEXCOORD1;
};

struct PatchTess {
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

struct DSOut {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float3 tangent  : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float4 extra    : TEXCOORD4;
    float  dispH    : TEXCOORD5;
};

HSIn VSMain(VSIn i) {
    HSIn o;
    o.pos = i.pos;
    o.normal = normalize(i.normal);
    o.tangent = normalize(i.tangent);
    o.uv = i.uv;
    o.extra = i.extra;
    return o;
}

float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(float4(i.pos, 1.0), gLightViewProj);
}

float computeEdgeTess(float3 p0, float3 p1) {
    float3 mid = 0.5 * (p0 + p1);
    float dist = length(mid - gCameraPosTime.xyz);
    float baseTess = clamp(18.0 / max(dist * 0.65, 0.45), 2.0, 16.0);
    // Boost tessellation near the hero drip tip
    float nearTip = exp(-length(mid - gDripOriginPool.xyz) * 1.8);
    return clamp((baseTess + nearTip * 10.0) * gFeatureFlags.x, 1.0, 24.0);
}

PatchTess HSConst(InputPatch<HSIn, 3> patch, uint patchId : SV_PrimitiveID) {
    PatchTess pt;
    pt.edges[0] = computeEdgeTess(patch[1].pos, patch[2].pos);
    pt.edges[1] = computeEdgeTess(patch[2].pos, patch[0].pos);
    pt.edges[2] = computeEdgeTess(patch[0].pos, patch[1].pos);
    pt.inside   = (pt.edges[0] + pt.edges[1] + pt.edges[2]) / 3.0;
    return pt;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConst")]
HSIn HSMain(InputPatch<HSIn, 3> patch, uint cpId : SV_OutputControlPointID) {
    return patch[cpId];
}

float evalBarkDisplacement(float3 p, float mossMask, float radius) {
    // Purely 3D world-space ridged bark grooves (stretched along branch X axis) — zero UV seams!
    float3 barkCoord = float3(p.x * 4.5, p.y * 19.0, p.z * 19.0);
    float ridges = ridgedFbm3D(barkCoord);
    float fineCrack = noise3D(p * 34.0);
    float barkH = (ridges * 0.78 + fineCrack * 0.22 - 0.45);

    // Cushioned mossy tufts on upper surfaces
    float mossTuft = fbm3D(p * 12.5 + float3(3.1, 0.0, 7.4));
    float mossGrow = smoothstep(0.30, 0.68, mossTuft) * mossMask;

    float dispScale = clamp(radius * 0.16, 0.0025, 0.026);
    return (barkH * dispScale) + mossGrow * (dispScale * 0.95);
}

[domain("tri")]
DSOut DSMain(PatchTess pt, float3 bary : SV_DomainLocation, const OutputPatch<HSIn, 3> patch) {
    float3 pos     = patch[0].pos     * bary.x + patch[1].pos     * bary.y + patch[2].pos     * bary.z;
    float3 normal  = normalize(patch[0].normal  * bary.x + patch[1].normal  * bary.y + patch[2].normal  * bary.z);
    float3 tangent = normalize(patch[0].tangent * bary.x + patch[1].tangent * bary.y + patch[2].tangent * bary.z);
    float2 uv      = patch[0].uv      * bary.x + patch[1].uv      * bary.y + patch[2].uv      * bary.z;
    float4 extra   = patch[0].extra   * bary.x + patch[1].extra   * bary.y + patch[2].extra   * bary.z;

    // Only displace hero foreground branches (branchId < 8); keep distant trunks cheap
    float disp = 0.0;
    float3 finalN = normal;
    if (extra.w < 8.0) {
        float3 bitan = normalize(cross(normal, tangent));
        float h0 = evalBarkDisplacement(pos, extra.z, extra.x);
        float eps = 0.006;
        float hT = evalBarkDisplacement(pos + tangent * eps, extra.z, extra.x);
        float hB = evalBarkDisplacement(pos + bitan   * eps, extra.z, extra.x);

        disp = h0;
        pos += normal * h0;

        float dT = (hT - h0) / eps;
        float dB = (hB - h0) / eps;
        finalN = normalize(normal - tangent * dT * 0.72 - bitan * dB * 0.72);
    }

    DSOut o;
    o.worldPos = pos;
    o.svPos = mul(float4(pos, 1.0), gViewProj);
    o.normal = finalN;
    o.tangent = tangent;
    o.uv = uv;
    o.extra = extra;
    o.dispH = disp;
    return o;
}

float4 PSMain(DSOut i) : SV_TARGET {
    // Distant background forest trunks: deep atmospheric silhouette with subtle golden edge rim
    if (i.extra.w >= 8.0) {
        float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
        float3 N = normalize(i.normal);
        if (dot(N, V) < 0.0) N = -N;
        float barkVar = fbm3D(float3(i.worldPos.x * 2.5, i.worldPos.y * 0.7, i.worldPos.z * 2.5));
        float rim = pow(1.0 - saturate(dot(N, V)), 3.8) * saturate(dot(N, gSunDirNear.xyz) * 0.5 + 0.5);
        float3 trunkCol = lerp(float3(0.010, 0.016, 0.012), float3(0.024, 0.032, 0.022), barkVar) + gSunColorFar.xyz * rim * 0.06;
        float dist = length(i.worldPos - gCameraPosTime.xyz);
        float fog = 1.0 - exp(-dist * 0.048);
        float3 fogCol = evalForestSky(-V, gSunDirNear.xyz, gSunColorFar.xyz) * 0.42;
        return float4(lerp(trunkCol, fogCol, saturate(fog)), 1.0);
    }

    float3 N = normalize(i.normal);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    // High-frequency bark & moss detail
    float grain = fbm3D(i.worldPos * 28.0);
    float micro = noise3D(i.worldPos * 85.0);
    float mossNoise = fbm3D(i.worldPos * 11.5 + float3(3.1, 0.0, 7.4));
    float mossFactor = smoothstep(0.35, 0.68, mossNoise) * saturate(i.extra.z * 1.15);

    // Rich woodland bark colors
    float3 darkBark  = float3(0.055, 0.038, 0.026);
    float3 warmBark  = float3(0.165, 0.115, 0.078);
    float3 barkCol   = lerp(darkBark, warmBark, saturate(grain * 1.1 + i.dispH * 18.0));

    // Velvety emerald & olive moss
    float3 deepMoss   = float3(0.045, 0.115, 0.032);
    float3 brightMoss = float3(0.160, 0.285, 0.068);
    float3 mossCol    = lerp(deepMoss, brightMoss, saturate(micro * 0.7 + saturate(N.y) * 0.4));

    float3 albedo = lerp(barkCol, mossCol, mossFactor);
    float roughness = lerp(0.76 - micro * 0.12, 0.88, mossFactor);

    // Animated moisture runoff film along the branch underside leading to the pooling droplet
    float runoffFlow = fbm3D(float3(i.worldPos.x * 10.0 - gCameraPosTime.w * 0.45, i.worldPos.y * 12.0, i.worldPos.z * 10.0));
    float wetness = saturate(i.extra.y * (0.65 + 0.55 * runoffFlow) + 0.22 * (1.0 - mossFactor));

    // Wet surfaces darken in diffuse albedo (porosity) and become mirror-glossy
    albedo *= lerp(1.0, 0.42, wetness);
    roughness = lerp(roughness, 0.08, wetness * 0.88);

    // Crevice AO from displacement
    float ao = saturate(0.65 + i.dispH * 22.0 + 0.2 * N.y);

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, ao);

    // Subtle warm backlit fuzzy rim on moss fibers
    float mossRim = pow(1.0 - saturate(dot(N, V)), 2.5) * mossFactor * saturate(dot(-V, L) * 0.5 + 0.5);
    float3 mossSheen = float3(0.28, 0.42, 0.10) * gSunColorFar.xyz * mossRim * 0.22;

    // Projected droplet lens caustic on branch underside
    float3 caustic = evalDropletLensCaustic(i.worldPos, N, gHeroDropCaustic.xyz, gHeroDropCaustic.w, L, gSunColorFar.xyz);

    float3 color = direct + ambient + mossSheen + caustic * 0.35;

    // Subtle woodland atmospheric perspective
    float dist = length(i.worldPos - gCameraPosTime.xyz);
    float fog = 1.0 - exp(-dist * 0.025);
    float3 fogCol = evalForestSky(-V, L, gSunColorFar.xyz) * 0.6;
    color = lerp(color, fogCol, saturate(fog * 0.55));

    return float4(color, 1.0);
}
