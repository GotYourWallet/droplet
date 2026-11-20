#include "common.hlsli"

struct FoliageInstance {
    float3 pos;
    float  height;
    float  yaw;
    float  lean;
    float  width;
    float  tintSeed;
};

StructuredBuffer<FoliageInstance> gInstances : register(t0);
Texture2D<float>                  gShadowMap : register(t1);

SamplerState gLinearClamp          : register(s0);
SamplerComparisonState gShadowSamp : register(s1);

struct VSIn {
    float3 pos     : POSITION; // Local template: x = across (-1..1), y = along (0..1), z = local arch/thickness
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0; // x = across (0..1), y = along (0..1)
    float4 extra   : TEXCOORD1; // x = leaflet/blade mask
};

struct PSIn {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float4 extra    : TEXCOORD3; // x = tintSeed, y = heightNorm (0..1), z = camDist, w = type (0=grass, 1=fern)
};

// Coherent traveling woodland wind field (Crysis/Ghost-of-Tsushima style gust waves + flutter)
float2 evalWindField(float3 worldPos, float time, float phaseSeed) {
    float2 windDir = normalize(float2(0.82, 0.57));
    float gustWave = sin(dot(worldPos.xz, windDir) * 0.65 - time * 1.65) * 0.5 + 0.5;
    float gustWave2 = sin(dot(worldPos.xz, float2(-0.45, 0.89)) * 1.15 - time * 1.10) * 0.5 + 0.5;
    float gust = 0.25 + 0.75 * (gustWave * 0.7 + gustWave2 * 0.3);

    float flutter = sin(time * 3.8 + phaseSeed * 19.3 + worldPos.x * 2.4 + worldPos.z * 1.9);
    float crossFlutter = cos(time * 2.9 + phaseSeed * 13.7 + worldPos.z * 2.1);

    float2 baseSway = windDir * (gust * 0.14 + flutter * 0.032);
    float2 perpDir = float2(-windDir.y, windDir.x);
    return baseSway + perpDir * (crossFlutter * 0.025);
}

// =============================================================================
// 1) GPU-INSTANCED 3D CURVED GRASS BLADES (Zero Alpha-Test Overdraw)
// =============================================================================
PSIn VSGrass(VSIn i, uint instId : SV_InstanceID) {
    FoliageInstance inst = gInstances[instId];

    float t = saturate(i.uv.y); // 0 at root, 1 at tip
    float camDist = length(inst.pos - gCameraPosTime.xyz);

    // Subtle distance width compensation (prevents sub-pixel aliasing in distant meadow)
    float distScale = lerp(1.0, 1.75, smoothstep(4.0, 12.0, camDist));
    float bladeW = inst.width * distScale;
    float bladeH = inst.height;

    // Natural quadratic arch along blade facing direction
    float archForward = inst.lean * bladeH * (t * t);
    float yLocal = bladeH * t * (1.0 - 0.18 * inst.lean * t);

    // Local orientation around yaw
    float c = cos(inst.yaw);
    float s = sin(inst.yaw);
    float3 fwd   = float3(s, 0.0, c);
    float3 right = float3(c, 0.0, -s);

    // Modern grazing-angle billboarding nudge so edge-on blades stay lush and never vanish to 0px
    float3 toCamXZ = normalize(float3(gCameraPosTime.x - inst.pos.x, 0.0, gCameraPosTime.z - inst.pos.z) + 1e-4);
    float3 camRight = normalize(cross(float3(0.0, 1.0, 0.0), toCamXZ));
    float edgeOn = 1.0 - abs(dot(fwd, toCamXZ));
    float3 finalRight = normalize(lerp(right, camRight * (dot(right, camRight) >= 0.0 ? 1.0 : -1.0), edgeOn * 0.48));

    // Coherent wind displacement (anchored at root, quadratic bend toward tip)
    float2 wind = evalWindField(inst.pos, gCameraPosTime.w, inst.tintSeed);
    float bendFactor = t * t * (0.65 + bladeH * 1.4);

    float3 wp = inst.pos
              + finalRight * (i.pos.x * bladeW)
              + fwd * archForward
              + float3(wind.x, -dot(wind, wind) * 0.6, wind.y) * bendFactor;
    wp.y += yLocal;

    // Analytical bent blade normal with subtle V-fold ridge across blade width
    float3 tangentUp = normalize(float3(fwd.x * (2.0 * inst.lean * t) + wind.x * 1.4 * t,
                                        1.0 - 0.32 * inst.lean * t,
                                        fwd.z * (2.0 * inst.lean * t) + wind.y * 1.4 * t));
    float3 baseN = normalize(cross(finalRight, tangentUp));
    // Tilt normal slightly left/right across width for realistic folded grass midrib sheen
    float foldTilt = (i.uv.x * 2.0 - 1.0) * 0.38;
    float3 N = normalize(baseN + finalRight * foldTilt + float3(0.0, 0.25, 0.0));

    PSIn o;
    o.worldPos = wp;
    o.svPos = mul(float4(wp, 1.0), gViewProj);
    o.normal = N;
    o.uv = i.uv;
    o.extra = float4(inst.tintSeed, t, camDist, 0.0);
    return o;
}

float4 PSGrass(PSIn i, bool isFrontFace : SV_IsFrontFace) : SV_TARGET {
    float3 N = normalize(i.normal);
    if (!isFrontFace) N = -N;
    // Soft upward normal bias keeps meadow lighting cohesive and prevents dark back-face harshness
    N = normalize(lerp(N, float3(0.0, 1.0, 0.0), 0.24));

    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);
    float t = i.extra.y;
    float seed = i.extra.x;

    // Organic woodland grass palette: deep damp emerald base -> sunlit chartreuse tips (+ occasional golden dry blade)
    float isDry = smoothstep(0.86, 0.96, seed);
    float3 freshBase = lerp(float3(0.016, 0.048, 0.012), float3(0.028, 0.072, 0.016), frac(seed * 7.3));
    float3 freshTip  = lerp(float3(0.095, 0.215, 0.032), float3(0.185, 0.295, 0.048), frac(seed * 13.1));

    float3 dryBase = float3(0.055, 0.045, 0.018);
    float3 dryTip  = float3(0.210, 0.165, 0.062);

    float3 baseCol = lerp(freshBase, dryBase, isDry);
    float3 tipCol  = lerp(freshTip,  dryTip,  isDry);
    float3 albedo  = lerp(baseCol, tipCol, pow(t, 0.85));

    // Subtle central midrib brightening
    float midrib = 1.0 - abs(i.uv.x * 2.0 - 1.0);
    albedo *= (0.86 + 0.22 * midrib);

    // Root-to-tip ambient occlusion (deep grounded shadows at base)
    float ao = lerp(0.26, 1.0, smoothstep(0.0, 0.78, t));
    float roughness = lerp(0.46, 0.28, t * (1.0 - isDry * 0.7));

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, ao);

    // Physically inspired backlit Subsurface Scattering (SSS) when sun shines through thin grass blades
    float backScatter = pow(saturate(dot(-V, L)), 3.2) * 0.85 + pow(saturate(dot(-V, L)), 1.2) * 0.25;
    float sssMask = (0.35 + 0.65 * t) * lerp(0.22, 1.0, shadow);
    float3 sssColor = lerp(float3(0.28, 0.52, 0.08), float3(0.42, 0.34, 0.10), isDry);
    float3 sss = sssColor * gSunColorFar.xyz * (backScatter * sssMask * 0.42);

    // Subtle grazing dew-kissed rim highlight
    float rim = pow(1.0 - saturate(dot(N, V)), 3.0) * t * lerp(0.2, 1.0, shadow);
    float3 sheen = gSunColorFar.xyz * float3(0.18, 0.26, 0.08) * rim * 0.16;

    float3 color = direct + ambient + sss + sheen;

    // Match woodland atmospheric perspective from pool.hlsl
    float dist = i.extra.z;
    float fog = 1.0 - exp(-dist * 0.055);
    float3 fogCol = evalForestSky(-V, L, gSunColorFar.xyz) * 0.55;
    color = lerp(color, fogCol, saturate(fog * 0.70));

    return float4(color, 1.0);
}

// =============================================================================
// 2) GPU-INSTANCED CRYSIS / FAR CRY 3D WOODLAND FERNS (True Pinnate Geometry)
// =============================================================================
float3 transformFernVertex(VSIn i, FoliageInstance inst, out float3 outNormal) {
    // Local fern frond: +Z = outward along frond (0..1), +Y = arch height, X = leaflet spread
    float along = saturate(i.uv.y);
    float scale = inst.height;

    // Graceful Crysis-style outward frond arch (rises up from crown, then droops organically)
    float archY = sin(along * 2.25) * 0.42 - along * along * 0.22;
    float3 localP = float3(i.pos.x * inst.width, (i.pos.y + archY) * scale, i.pos.z * scale);

    // Pitch frond upward from crown based on inst.lean (0.15 = low outer frond, 0.75 = upright inner frond)
    float pitch = inst.lean;
    float cp = cos(pitch), sp = sin(pitch);
    float3 pitchedP = float3(
        localP.x,
        localP.y * cp + localP.z * sp,
        -localP.y * sp + localP.z * cp
    );
    float3 pitchedN = float3(
        i.normal.x,
        i.normal.y * cp + i.normal.z * sp,
        -i.normal.y * sp + i.normal.z * cp
    );

    // Rotate around yaw
    float cy = cos(inst.yaw), sy = sin(inst.yaw);
    float3 rotP = float3(
        pitchedP.x * cy + pitchedP.z * sy,
        pitchedP.y,
        -pitchedP.x * sy + pitchedP.z * cy
    );
    outNormal = normalize(float3(
        pitchedN.x * cy + pitchedN.z * sy,
        pitchedN.y,
        -pitchedN.x * sy + pitchedN.z * cy
    ));

    // Coherent wind sway (heavier, slower frond bobbing + tip flutter)
    float2 wind = evalWindField(inst.pos, gCameraPosTime.w * 0.85, inst.tintSeed);
    float sway = along * along * (0.45 + scale * 0.55);
    float bob = sin(gCameraPosTime.w * 2.1 + inst.tintSeed * 15.0 + along * 3.0) * 0.025 * along * along;

    return inst.pos + rotP + float3(wind.x * sway, bob - length(wind) * sway * 0.25, wind.y * sway);
}

PSIn VSFern(VSIn i, uint instId : SV_InstanceID) {
    FoliageInstance inst = gInstances[instId];
    float3 worldN;
    float3 wp = transformFernVertex(i, inst, worldN);

    PSIn o;
    o.worldPos = wp;
    o.svPos = mul(float4(wp, 1.0), gViewProj);
    o.normal = worldN;
    o.uv = i.uv;
    o.extra = float4(inst.tintSeed, saturate(i.uv.y), length(wp - gCameraPosTime.xyz), i.extra.x);
    return o;
}

float4 VSFernShadow(VSIn i, uint instId : SV_InstanceID) : SV_POSITION {
    FoliageInstance inst = gInstances[instId];
    float3 dummyN;
    float3 wp = transformFernVertex(i, inst, dummyN);
    return mul(float4(wp, 1.0), gLightViewProj);
}

float4 PSFern(PSIn i, bool isFrontFace : SV_IsFrontFace) : SV_TARGET {
    float3 N = normalize(i.normal);
    if (!isFrontFace) N = -N;

    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 L = normalize(gSunDirNear.xyz);

    float along = i.extra.y;
    float seed  = i.extra.x;
    float isStem = i.extra.w; // 1.0 on central rachis stem, 0.0 on pinnate leaflets

    // Rich woodland sword-fern shading with procedural leaflet vein detail
    float vein = abs(sin(i.uv.x * 22.0 + i.uv.y * 8.0));
    float micro = noise3D(i.worldPos * 24.0);

    float3 stemCol   = float3(0.055, 0.095, 0.025);
    float3 baseLeaf  = lerp(float3(0.022, 0.088, 0.020), float3(0.036, 0.115, 0.024), frac(seed * 5.7));
    float3 tipLeaf   = lerp(float3(0.085, 0.225, 0.036), float3(0.145, 0.285, 0.048), frac(seed * 11.3));
    float3 leafCol   = lerp(baseLeaf, tipLeaf, smoothstep(0.15, 0.95, along));
    leafCol *= (0.86 + 0.18 * vein + 0.10 * micro);

    float3 albedo = lerp(leafCol, stemCol, isStem);
    float roughness = lerp(0.32, 0.52, isStem) + (1.0 - vein) * 0.08;
    float ao = lerp(0.38, 1.0, smoothstep(0.0, 0.55, along));

    float shadow = sampleShadowPCF(gShadowMap, gShadowSamp, i.worldPos, N);
    float3 direct = evalPBRDirect(N, V, L, albedo, roughness, 0.0, gSunColorFar.xyz) * shadow;
    float3 ambient = evalIBLAmbient(N, V, albedo, roughness, 0.0, ao);

    // Signature Crysis/Far Cry backlit translucent emerald-gold fern glow!
    float backDot = saturate(dot(-V, L));
    float sssFactor = (pow(backDot, 3.5) * 0.95 + pow(backDot, 1.4) * 0.28) * (1.0 - isStem * 0.75);
    float3 sssCol = float3(0.24, 0.58, 0.07) * gSunColorFar.xyz * (sssFactor * lerp(0.20, 1.0, shadow) * 0.52);

    float3 color = direct + ambient + sssCol;

    float dist = i.extra.z;
    float fog = 1.0 - exp(-dist * 0.055);
    float3 fogCol = evalForestSky(-V, L, gSunColorFar.xyz) * 0.55;
    color = lerp(color, fogCol, saturate(fog * 0.70));

    return float4(color, 1.0);
}
