#include "common.hlsli"

Texture2D<float4> gSceneColor    : register(t0);
Texture2D<float4> gBackfaceData  : register(t1); // rgb = back world normal, a = linear view depth
SamplerState gLinearClamp        : register(s0);

struct VSIn {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float3 tangent : TANGENT;
    float2 uv      : TEXCOORD0;
    float4 extra   : TEXCOORD1;
};

struct HSIn {
    float3 dir : POSITION;
};

struct PatchTess {
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

struct DSOut {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float  viewZ    : TEXCOORD2;
    float3 localDir : TEXCOORD3;
};

HSIn VSMain(VSIn i) {
    HSIn o;
    o.dir = normalize(i.pos);
    return o;
}

PatchTess HSConst(InputPatch<HSIn, 3> patch, uint patchId : SV_PrimitiveID) {
    PatchTess pt;
    float dist = length(gDropPosRadius.xyz - gCameraPosTime.xyz);
    float factor = clamp((22.0 / max(dist * 0.65, 0.35)) * gFeatureFlags.x, 4.0, 22.0);
    pt.edges[0] = factor;
    pt.edges[1] = factor;
    pt.edges[2] = factor;
    pt.inside   = factor;
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

// Physically inspired pendant drop / Rayleigh-Plateau capillary deformation
float3 evalDropletPoint(float3 u) {
    float R          = gDropPosRadius.w;
    float3 center    = gDropPosRadius.xyz;
    float elongation = gDropDeform.x;
    float neckPinch  = gDropDeform.y;
    float osc        = gDropDeform.z;
    float mode       = gDropDeform.w;

    float y = u.y; // -1 (bottom) to +1 (top)
    float2 xz = u.xz;

    if (mode < 0.5) {
        // --- MODE 0: Pendant hanging drop clinging seamlessly to branch underside ---
        // Parameter s goes 0 (bottom apex of drop) -> 1 (top contact point inside branch bark)
        float s = y * 0.5 + 0.5;

        // Vertical span from branch attachment down to heavy bulbous bottom
        float dropHeight = R * (1.75 + elongation * 1.45);
        float wpY = gDropAttach.y + 0.014 - (1.0 - s) * dropHeight;

        // Smooth pendant teardrop radius profile:
        // Round heavy bottom (sin-like bulb) tapering smoothly into a neck & gentle meniscus at the bark
        float baseR = sqrt(max(0.0, 1.0 - y * y));
        float belly = exp(-pow((y + 0.42) / 0.52, 2.0)) * (0.16 * elongation + 0.12 * neckPinch);

        // Capillary neck constriction around s in [0.58, 0.88]
        float neckBand = exp(-pow((s - 0.72) / 0.17, 2.0));
        float pinchFactor = 1.0 - neckPinch * 0.82 * neckBand;

        // Smooth meniscus contact collar right at the bark (s -> 1)
        float topTaper = lerp(1.0, 0.55 - 0.18 * neckPinch, smoothstep(0.55, 0.96, s));
        float ripple = osc * sin(s * 9.0 + gCameraPosTime.w * 16.0) * 0.035 * (1.0 - s);

        float rScale = max(0.04, (baseR * pinchFactor * topTaper + belly * baseR + ripple) / sqrt(max(1.0 + elongation * 0.22, 0.6)));
        float radialLen = length(xz);
        float2 xzDir = (radialLen > 1e-5) ? (xz / radialLen) : float2(0.0, 0.0);

        return float3(
            gDropAttach.x + xzDir.x * rScale * R,
            wpY,
            gDropAttach.z + xzDir.y * rScale * R
        );
    }
    else if (mode < 1.5) {
        // --- MODE 1: Detached falling droplet with Rayleigh-Plateau capillary oscillation ---
        // Volume-preserving quadrupole oscillation (prolate <-> oblate)
        float sy = max(0.65, 1.0 + osc * 0.28);
        float sxz = 1.0 / sqrt(sy);

        // Slight aerodynamic drag flattening on the leading (bottom) face + pointed tail right after pinch
        float tailPull = smoothstep(0.2, 1.0, y) * elongation * 0.45;
        float bottomFlat = smoothstep(-0.3, -1.0, y) * 0.06;

        float3 localPos = float3(
            xz.x * (sxz * (1.0 - tailPull * 0.45) + bottomFlat * 0.5),
            y * sy + tailPull * 0.35 + bottomFlat * 0.4,
            xz.y * (sxz * (1.0 - tailPull * 0.45) + bottomFlat * 0.5)
        ) * R;
        return center + localPos;
    }
    else {
        // --- MODE 2: Rising Worthington jet / splash bead ---
        float sy = 1.0 + elongation * 1.4;
        float sxz = 1.0 / sqrt(sy);
        float3 localPos = float3(xz.x * sxz, y * sy, xz.y * sxz) * R;
        return center + localPos;
    }
}

[domain("tri")]
DSOut DSMain(PatchTess pt, float3 bary : SV_DomainLocation, const OutputPatch<HSIn, 3> patch) {
    float3 u = normalize(patch[0].dir * bary.x + patch[1].dir * bary.y + patch[2].dir * bary.z);

    // Build orthonormal tangent basis on unit sphere for exact finite-difference normal
    float3 upRef = (abs(u.y) < 0.96) ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t1 = normalize(cross(upRef, u));
    float3 t2 = cross(u, t1);

    float eps = 0.004;
    float3 p0 = evalDropletPoint(u);
    float3 p1 = evalDropletPoint(normalize(u + t1 * eps));
    float3 p2 = evalDropletPoint(normalize(u + t2 * eps));

    float3 n = normalize(cross(p1 - p0, p2 - p0));
    // Ensure outward-facing normal
    if (dot(n, u) < 0.0) n = -n;

    float4 clipPos = mul(float4(p0, 1.0), gViewProj);
    float4 viewPos = mul(float4(p0, 1.0), gView);

    DSOut o;
    o.svPos    = clipPos;
    o.worldPos = p0;
    o.normal   = n;
    o.viewZ    = viewPos.z;
    o.localDir = u;
    return o;
}

// Pass 1: Back-face thickness & exit normal capture (rendered with Front-Face Culling)
float4 PSBackface(DSOut i) : SV_TARGET {
    // Store outward normal of the back face + linear view depth
    return float4(normalize(i.normal), i.viewZ);
}

// Helper: sample refracted scene with graceful fallback to procedural HDR forest sky
float3 sampleRefractedEnvironment(float2 baseUV, float3 refractDir, float2 refrOffset, float refractStrength) {
    float2 sampleUV = baseUV + refrOffset;
    float edgeMask = smoothstep(0.0, 0.06, sampleUV.x) * smoothstep(1.0, 0.94, sampleUV.x)
                   * smoothstep(0.0, 0.06, sampleUV.y) * smoothstep(1.0, 0.94, sampleUV.y);
    float3 sceneCol = gSceneColor.SampleLevel(gLinearClamp, saturate(sampleUV), 0).rgb;
    float3 skyCol   = evalForestSky(refractDir, gSunDirNear.xyz, gSunColorFar.xyz);
    return lerp(skyCol, sceneCol, edgeMask * refractStrength);
}

// Pass 2: Front-face physical water shading (2-interface refraction, chromatic dispersion, internal caustic, Fresnel)
float4 PSFrontface(DSOut i) : SV_TARGET {
    float3 N = normalize(i.normal);
    float3 V = normalize(gCameraPosTime.xyz - i.worldPos);
    float3 I = -V; // Incident view ray
    float3 L = normalize(gSunDirNear.xyz);

    float2 screenUV = i.svPos.xy * gResolutionInv.zw;
    float4 backData = gBackfaceData.SampleLevel(gLinearClamp, screenUV, 0);
    float3 backN = (dot(backData.rgb, backData.rgb) > 0.01) ? normalize(backData.rgb) : -N;
    float thickness = max(0.005, backData.a - i.viewZ);
    // Clamp thickness in case back-face wasn't written cleanly on grazing silhouette pixel
    if (backData.a <= i.viewZ || backData.a > 50.0) {
        thickness = gDropPosRadius.w * 1.4;
    }

    // --- Two-interface Snell refraction with RGB Chromatic Dispersion ---
    // Water IOR: n_R = 1.328, n_G = 1.333, n_B = 1.342
    float3 refrInR = refract(I, N, 1.0 / 1.326);
    float3 refrInG = refract(I, N, 1.0 / 1.333);
    float3 refrInB = refract(I, N, 1.0 / 1.344);

    // Exit refraction at back surface (water -> air)
    float3 refrOutR = refract(refrInR, -backN, 1.326);
    float3 refrOutG = refract(refrInG, -backN, 1.333);
    float3 refrOutB = refract(refrInB, -backN, 1.344);

    // Handle Total Internal Reflection (TIR) at grazing back-face angles
    float tirMask = (dot(refrOutG, refrOutG) < 0.001) ? 1.0 : 0.0;
    if (dot(refrOutR, refrOutR) < 0.001) refrOutR = reflect(refrInR, -backN);
    if (dot(refrOutG, refrOutG) < 0.001) refrOutG = reflect(refrInG, -backN);
    if (dot(refrOutB, refrOutB) < 0.001) refrOutB = reflect(refrInB, -backN);

    // Project view-space refraction bend to screen-space UV offset (inverted lens effect!)
    float3 viewN = mul(float4(N, 0.0), gView).xyz;
    float3 viewBackN = mul(float4(backN, 0.0), gView).xyz;
    float2 bend = -(viewN.xy * 0.72 + viewBackN.xy * 0.35);
    float uvScale = clamp(thickness * 0.55, 0.025, 0.14);

    float3 colR = sampleRefractedEnvironment(screenUV, refrOutR, bend * (uvScale * 0.94), 0.88);
    float3 colG = sampleRefractedEnvironment(screenUV, refrOutG, bend * (uvScale * 1.00), 0.88);
    float3 colB = sampleRefractedEnvironment(screenUV, refrOutB, bend * (uvScale * 1.07), 0.88);
    float3 refractedCol = float3(colR.r, colG.g, colB.b);

    // Subtle Beer-Lambert optical absorption (crystal-clear woodland rainwater)
    float3 absorption = exp(-thickness * float3(0.45, 0.18, 0.10));
    refractedCol *= absorption;

    // --- Internal Backlit Lens Caustic (the iconic golden glow inside a sunlit dewdrop!) ---
    float3 refrSun = refract(-L, -backN, 1.0 / 1.333);
    float internalFocus = pow(saturate(dot(normalize(refrOutG), L)), 10.0) * 1.8
                        + pow(saturate(dot(-V, L)), 6.0) * pow(1.0 - saturate(dot(N, V)), 1.5) * 0.95;
    float3 internalCaustic = gSunColorFar.xyz * internalFocus * float3(1.05, 0.92, 0.74);

    // Subtle rainbow prismatic sparkle near TIR rim
    float rimAngle = 1.0 - saturate(dot(N, V));
    float3 prism = float3(
        pow(saturate(dot(refrOutR, L)), 18.0),
        pow(saturate(dot(refrOutG, L)), 18.0),
        pow(saturate(dot(refrOutB, L)), 18.0)
    ) * gSunColorFar.xyz * 2.2;

    // --- External Schlick Fresnel Reflection ---
    float NdotV = saturate(dot(N, V));
    float F = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
    // Boost reflection slightly on TIR zones for crystalline brilliance
    F = saturate(F + tirMask * 0.22);

    float3 R = reflect(I, N);
    float3 envRefl = evalForestSky(R, L, gSunColorFar.xyz);

    // Crisp dual specular highlights (primary sun glint + secondary internal bounce glint)
    float3 H = normalize(V + L);
    float specPrimary = pow(saturate(dot(N, H)), 1400.0) * 18.0 + pow(saturate(dot(N, H)), 180.0) * 1.6;
    float3 H_back = normalize(V - L);
    float specInternal = pow(saturate(dot(-backN, H_back)), 90.0) * 0.85;
    float3 specular = gSunColorFar.xyz * (specPrimary + specInternal);

    float3 finalCol = lerp(refractedCol + internalCaustic + prism, envRefl, F) + specular;
    return float4(finalCol, 1.0);
}
