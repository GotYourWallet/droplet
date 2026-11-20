#include "common.hlsli"

struct DustParticle {
    float3 basePos;
    float  size;
    float3 velocity;
    float  phase;
};

StructuredBuffer<DustParticle> gParticles : register(t0);

struct PSIn {
    float4 svPos  : SV_POSITION;
    float2 quadUV : TEXCOORD0;
    float  alpha  : TEXCOORD1;
};

static const float2 kQuadCorners[6] = {
    float2(-1, -1), float2(-1,  1), float2( 1,  1),
    float2(-1, -1), float2( 1,  1), float2( 1, -1)
};

PSIn VSMain(uint vid : SV_VertexID) {
    uint pIdx = vid / 6;
    uint cIdx = vid % 6;
    DustParticle p = gParticles[pIdx];

    float t = gCameraPosTime.w;
    float3 drift = float3(
        sin(t * 0.45 + p.phase) * 0.25 + p.velocity.x * t,
        sin(t * 0.35 + p.phase * 1.7) * 0.18 + fmod(p.velocity.y * t, 3.2),
        cos(t * 0.40 + p.phase * 0.9) * 0.25 + p.velocity.z * t
    );

    float3 worldCenter = p.basePos + drift;
    // Wrap gracefully within woodland volume
    worldCenter.x = fmod(worldCenter.x + 9.0, 9.0) - 4.5;
    worldCenter.y = fmod(worldCenter.y + 3.4, 3.4) - 0.2;
    worldCenter.z = fmod(worldCenter.z + 9.0, 9.0) - 3.3;

    // Camera right & up from view matrix rows (column-major transposed in CB -> columns of gView)
    float3 camRight = float3(gView[0][0], gView[1][0], gView[2][0]);
    float3 camUp    = float3(gView[0][1], gView[1][1], gView[2][1]);

    float2 corner = kQuadCorners[cIdx];
    float dist = length(worldCenter - gCameraPosTime.xyz);
    // Out-of-focus dust motes expand subtly into soft bokeh circles!
    float defocus = saturate(abs(dist - gDofParams.x) * 0.32);
    float radius = p.size * (1.0 + defocus * 2.4);

    float3 wp = worldCenter + (camRight * corner.x + camUp * corner.y) * radius;

    PSIn o;
    o.svPos = mul(float4(wp, 1.0), gViewProj);
    o.quadUV = corner;
    // Gentle twinkling & distance fade
    float twinkle = 0.55 + 0.45 * sin(t * 2.3 + p.phase * 3.1);
    float distFade = smoothstep(0.45, 1.2, dist) * (1.0 - smoothstep(7.5, 12.0, dist));
    o.alpha = twinkle * distFade * lerp(0.85, 0.35, defocus);
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET {
    float r = length(i.quadUV);
    if (r > 1.0) discard;
    // Soft bokeh disc with subtle glowing core
    float disc = smoothstep(1.0, 0.35, r) * 0.45 + exp(-r * r * 6.0) * 0.45;
    float3 col = gSunColorFar.xyz * float3(1.05, 0.94, 0.72) * (disc * i.alpha * 0.24);
    return float4(col, 1.0);
}
