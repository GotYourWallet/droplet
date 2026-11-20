cbuffer WaterSimCB : register(b0) {
    float4 gSplashParams; // xy = UV center (0.5, 0.5), z = radius, w = impulse strength
    float4 gSimParams;    // x = time, y = dt, z = gridSize, w = invGridSize
    float4 gSunDirSim;    // xyz = normalized sun dir, w = poolDepth
};

Texture2D<float2>   gStateIn       : register(t0); // x = h_curr, y = h_prev
RWTexture2D<float2> gStateOut      : register(u0);

Texture2D<float2>   gHeightForNorm : register(t0);
RWTexture2D<float4> gNormalOut     : register(u0); // xyz = world normal, w = height
RWTexture2D<float4> gCausticOut    : register(u1); // rgb = chromatic underwater caustic intensity

[numthreads(16, 16, 1)]
void CSStepWave(uint3 tid : SV_DispatchThreadID) {
    uint size = (uint)gSimParams.z;
    if (tid.x >= size || tid.y >= size) return;

    int2 p = int2(tid.xy);
    int maxIdx = (int)size - 1;

    float2 c = gStateIn.Load(int3(p, 0));
    float hL  = gStateIn.Load(int3(clamp(p + int2(-1,  0), 0, maxIdx), 0)).x;
    float hR  = gStateIn.Load(int3(clamp(p + int2( 1,  0), 0, maxIdx), 0)).x;
    float hD  = gStateIn.Load(int3(clamp(p + int2( 0, -1), 0, maxIdx), 0)).x;
    float hU  = gStateIn.Load(int3(clamp(p + int2( 0,  1), 0, maxIdx), 0)).x;
    float hDL = gStateIn.Load(int3(clamp(p + int2(-1, -1), 0, maxIdx), 0)).x;
    float hDR = gStateIn.Load(int3(clamp(p + int2( 1, -1), 0, maxIdx), 0)).x;
    float hUL = gStateIn.Load(int3(clamp(p + int2(-1,  1), 0, maxIdx), 0)).x;
    float hUR = gStateIn.Load(int3(clamp(p + int2( 1,  1), 0, maxIdx), 0)).x;

    // 9-point isotropic Laplacian (eliminates square-grid dispersion artifacts -> perfectly circular ripples!)
    float laplacian = (4.0 * (hL + hR + hD + hU) + (hDL + hDR + hUL + hUR) - 20.0 * c.x) * 0.2;

    // Circular mossy pool edge absorption
    float2 uv = (float2(tid.xy) + 0.5) * gSimParams.w;
    float distEdge = length(uv - 0.5) * 2.0;
    float damping = lerp(0.990, 0.90, smoothstep(0.68, 0.96, distEdge));

    float hNext = (2.0 * c.x - c.y + 0.16 * laplacian) * damping;

    // Inject smooth Gaussian droplet impact impulse (prevents Nyquist ringing)
    if (abs(gSplashParams.w) > 1e-4) {
        float d = length(uv - gSplashParams.xy);
        float r = max(gSplashParams.z, 0.015);
        float crater = exp(-(d * d) / (r * r));
        float rim = exp(-pow((d - r * 1.55) / (r * 0.75), 2.0)) * 0.30;
        hNext += gSplashParams.w * (-crater + rim);
    }

    hNext = clamp(hNext, -0.25, 0.25);
    gStateOut[tid.xy] = float2(hNext, c.x);
}

// Compute surface normals & refractive Jacobian underwater caustics
float2 evalRefractHit(float2 uv, float3 N, float3 L, float ior, float depth) {
    float3 refr = refract(-L, N, 1.0 / ior);
    float t = depth / max(-refr.y, 0.15);
    return uv + refr.xz * t;
}

float3 computeNormalAt(int2 p, int maxIdx, float time) {
    float hL = gHeightForNorm.Load(int3(clamp(p + int2(-1, 0), 0, maxIdx), 0)).x;
    float hR = gHeightForNorm.Load(int3(clamp(p + int2( 1, 0), 0, maxIdx), 0)).x;
    float hD = gHeightForNorm.Load(int3(clamp(p + int2( 0,-1), 0, maxIdx), 0)).x;
    float hU = gHeightForNorm.Load(int3(clamp(p + int2( 0, 1), 0, maxIdx), 0)).x;

    // Subtle organic ambient breeze micro-ripples so the pool always shimmers gently
    float2 uv = float2(p) * gSimParams.w;
    float bX = cos(uv.x * 38.0 + time * 1.6) * sin(uv.y * 29.0 - time * 1.1) * 0.0022;
    float bZ = sin(uv.x * 31.0 - time * 1.3) * cos(uv.y * 41.0 + time * 1.4) * 0.0022;

    float scale = 5.5;
    float dx = (hR - hL) * scale + bX;
    float dz = (hU - hD) * scale + bZ;
    return normalize(float3(-dx, 1.0, -dz));
}

[numthreads(16, 16, 1)]
void CSCausticsNormals(uint3 tid : SV_DispatchThreadID) {
    uint size = (uint)gSimParams.z;
    if (tid.x >= size || tid.y >= size) return;

    int2 p = int2(tid.xy);
    int maxIdx = (int)size - 1;
    float time = gSimParams.x;
    float3 L = normalize(gSunDirSim.xyz);
    float depth = gSunDirSim.w;

    float hC = gHeightForNorm.Load(int3(p, 0)).x;
    float3 nC = computeNormalAt(p, maxIdx, time);
    float3 nR = computeNormalAt(clamp(p + int2(1, 0), 0, maxIdx), maxIdx, time);
    float3 nU = computeNormalAt(clamp(p + int2(0, 1), 0, maxIdx), maxIdx, time);

    gNormalOut[tid.xy] = float4(nC, hC);

    // Jacobian area-ratio caustic focusing with RGB chromatic dispersion
    float2 uv  = (float2(p) + 0.5) * gSimParams.w;
    float2 uvR = uv + float2(gSimParams.w, 0.0);
    float2 uvU = uv + float2(0.0, gSimParams.w);
    float origArea = gSimParams.w * gSimParams.w;

    float3 causticRGB;
    const float iors[3] = { 1.326, 1.333, 1.344 };
    float causticDepth = depth * 0.18;
    [unroll]
    for (int ch = 0; ch < 3; ++ch) {
        float2 hitC = evalRefractHit(uv,  nC, L, iors[ch], causticDepth);
        float2 hitR = evalRefractHit(uvR, nR, L, iors[ch], causticDepth);
        float2 hitU = evalRefractHit(uvU, nU, L, iors[ch], causticDepth);
        float2 dX = hitR - hitC;
        float2 dY = hitU - hitC;
        float newArea = abs(dX.x * dY.y - dX.y * dY.x);
        float focus = clamp(origArea / max(newArea, origArea * 0.35), 0.0, 2.4);
        // Smooth luminous caustic ribbons without harsh foldover ringing
        float val = pow(max(focus - 0.96, 0.0), 1.35) * 0.55;
        if (ch == 0) causticRGB.r = val;
        if (ch == 1) causticRGB.g = val;
        if (ch == 2) causticRGB.b = val;
    }

    // Fade caustics cleanly at outer mossy rim
    float edgeFade = 1.0 - smoothstep(0.62, 0.94, length(uv - 0.5) * 2.0);
    gCausticOut[tid.xy] = float4(causticRGB * edgeFade, 1.0);
}
