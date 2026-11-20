#include "common.hlsli"

Texture2D<float4> gInputTex0 : register(t0);
Texture2D<float4> gInputTex1 : register(t1);
Texture2D<float4> gInputTex2 : register(t2);
Texture2D<float>  gDepthTex  : register(t3);
Texture2D<float4> gDofTex    : register(t4);

SamplerState gLinearClamp : register(s0);

cbuffer PostCB : register(b2) {
    float4 gTexelSize;   // xy = 1/width, 1/height of current pass source, z = filterRadius, w = bloomIntensity
};

struct VSOut {
    float4 svPos : SV_POSITION;
    float2 uv    : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID) {
    VSOut o;
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? -3.0 : 1.0);
    o.svPos = float4(pos, 0.0, 1.0);
    o.uv = pos * float2(0.5, -0.5) + 0.5;
    return o;
}

// Compute signed Circle of Confusion in pixels (negative = foreground, positive = background)
float computeCoC(float rawDepth) {
    float linZ = linearizeDepth(rawDepth, gSunDirNear.w, gSunColorFar.w);
    float focusDist = gDofParams.x;
    float aperture  = gDofParams.y;
    float maxBlur   = gDofParams.z;
    float coc = ((linZ - focusDist) / max(linZ, 0.1)) * aperture * maxBlur;
    return clamp(coc, -maxBlur, maxBlur);
}

// 1) Cinematic Golden-Angle Spiral Bokeh Depth of Field
float4 PSBokehDoF(VSOut i) : SV_TARGET {
    float3 centerCol = gInputTex0.SampleLevel(gLinearClamp, i.uv, 0).rgb;
    if (gDofParams.w < 0.5) {
        return float4(centerCol, 1.0);
    }

    float centerDepth = gDepthTex.SampleLevel(gLinearClamp, i.uv, 0);
    float centerCoC   = computeCoC(centerDepth);
    float absCenterCoC = abs(centerCoC);

    if (absCenterCoC < 0.45) {
        return float4(centerCol, 1.0);
    }

    const int SAMPLES = 28;
    const float GOLDEN_ANGLE = 2.39996323;
    float2 invRes = gResolutionInv.zw;

    float3 accumCol = centerCol;
    float totalWeight = 1.0;

    // Slight per-pixel rotation via IGN prevents ring patterns
    float angleOffset = interleavedGradientNoise(i.svPos.xy) * TWO_PI;

    [loop]
    for (int s = 1; s <= SAMPLES; ++s) {
        float r = sqrt((float)s / (float)SAMPLES) * absCenterCoC;
        float theta = (float)s * GOLDEN_ANGLE + angleOffset;
        float2 offset = float2(cos(theta), sin(theta)) * r * invRes;
        float2 suv = saturate(i.uv + offset);

        float sDepth = gDepthTex.SampleLevel(gLinearClamp, suv, 0);
        float sCoC   = computeCoC(sDepth);
        float3 sCol  = gInputTex0.SampleLevel(gLinearClamp, suv, 0).rgb;

        // Prevent sharp foreground branch/droplet from bleeding into blurry background
        float weight = (sCoC >= centerCoC) ? 1.0 : saturate(abs(sCoC) / max(absCenterCoC, 1e-3));
        // Slight bokeh highlight emphasis for creamy circular specular orbs
        float lum = dot(sCol, float3(0.2126, 0.7152, 0.0722));
        float bokehBoost = 1.0 + saturate(lum - 1.2) * 0.35;

        accumCol += sCol * (weight * bokehBoost);
        totalWeight += weight * bokehBoost;
    }

    return float4(accumCol / max(totalWeight, 1e-4), 1.0);
}

// 2) HDR Bloom Prefilter (soft knee)
float4 PSBloomPrefilter(VSOut i) : SV_TARGET {
    float3 col = gInputTex0.SampleLevel(gLinearClamp, i.uv, 0).rgb;
    float br = max(col.r, max(col.g, col.b));
    float threshold = 1.10;
    float knee = 0.55;
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee + 1e-5);
    float contrib = max(soft, br - threshold) / max(br, 1e-5);
    return float4(col * contrib, 1.0);
}

// 3) 13-tap Jimenez HDR Bloom Downsample
float4 PSBloomDownsample(VSOut i) : SV_TARGET {
    float2 t = gTexelSize.xy;
    float2 uv = i.uv;

    float3 a = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-2*t.x,  2*t.y), 0).rgb;
    float3 b = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 0,     2*t.y), 0).rgb;
    float3 c = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 2*t.x,  2*t.y), 0).rgb;
    float3 d = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-2*t.x,  0),     0).rgb;
    float3 e = gInputTex0.SampleLevel(gLinearClamp, uv,                         0).rgb;
    float3 f = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 2*t.x,  0),     0).rgb;
    float3 g = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-2*t.x, -2*t.y), 0).rgb;
    float3 h = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 0,    -2*t.y), 0).rgb;
    float3 k = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 2*t.x, -2*t.y), 0).rgb;

    float3 j = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-t.x,  t.y), 0).rgb;
    float3 l = gInputTex0.SampleLevel(gLinearClamp, uv + float2( t.x,  t.y), 0).rgb;
    float3 m = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-t.x, -t.y), 0).rgb;
    float3 n = gInputTex0.SampleLevel(gLinearClamp, uv + float2( t.x, -t.y), 0).rgb;

    float3 outCol = e * 0.125;
    outCol += (a + c + g + k) * 0.03125;
    outCol += (b + d + f + h) * 0.0625;
    outCol += (j + l + m + n) * 0.125;
    return float4(max(outCol, 0.0), 1.0);
}

// 4) 9-tap Tent Filter Bloom Upsample (additive blend onto higher mip)
float4 PSBloomUpsample(VSOut i) : SV_TARGET {
    float2 t = gTexelSize.xy * gTexelSize.z;
    float2 uv = i.uv;

    float3 col = 0.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2(-t.x,  t.y), 0).rgb * 1.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2( 0.0,  t.y), 0).rgb * 2.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2( t.x,  t.y), 0).rgb * 1.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2(-t.x,  0.0), 0).rgb * 2.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv,                      0).rgb * 4.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2( t.x,  0.0), 0).rgb * 2.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2(-t.x, -t.y), 0).rgb * 1.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2( 0.0, -t.y), 0).rgb * 2.0;
    col += gInputTex0.SampleLevel(gLinearClamp, uv + float2( t.x, -t.y), 0).rgb * 1.0;
    return float4(col * (1.0 / 16.0), 1.0);
}

// 5) Final HDR Composite + Subtle Lens Chromatic Aberration + ACES Filmic Tone Mapping + Vignette
float4 PSFinalComposite(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 toCenter = uv - 0.5;
    float r2 = dot(toCenter, toCenter);

    // Very subtle optical radial chromatic aberration near frame corners (sharp full-res HDR)
    float2 caShift = toCenter * (r2 * 0.0042);
    float3 sharpCol;
    sharpCol.r = gInputTex0.SampleLevel(gLinearClamp, saturate(uv + caShift), 0).r;
    sharpCol.g = gInputTex0.SampleLevel(gLinearClamp, uv, 0).g;
    sharpCol.b = gInputTex0.SampleLevel(gLinearClamp, saturate(uv - caShift), 0).b;

    // Seamlessly composite half-res creamy bokeh blur onto out-of-focus regions while keeping subject razor-sharp!
    float3 hdrCol = sharpCol;
    if (gDofParams.w >= 0.5) {
        float depth = gDepthTex.SampleLevel(gLinearClamp, uv, 0);
        float absCoC = abs(computeCoC(depth));
        float dofBlend = smoothstep(0.45, 2.4, absCoC);
        float3 bokehCol = gDofTex.SampleLevel(gLinearClamp, uv, 0).rgb;
        hdrCol = lerp(sharpCol, bokehCol, dofBlend);
    }

    // Add volumetric god rays & HDR bloom
    float3 volumetric = gInputTex1.SampleLevel(gLinearClamp, uv, 0).rgb;
    float3 bloom      = gInputTex2.SampleLevel(gLinearClamp, uv, 0).rgb;
    hdrCol += volumetric + bloom * gFeatureFlags.z;

    // Gentle exposure & warm cinematic color grade
    float exposure = 1.08;
    hdrCol *= exposure;

    // ACES Filmic Tone Mapping
    float3 ldr = acesToneMap(hdrCol);

    // Subtle photographic vignette
    float vignette = 1.0 - smoothstep(0.32, 0.95, length(toCenter * float2(1.22, 1.0))) * 0.34;
    ldr *= vignette;

    // sRGB gamma encode + store perceptual luma in alpha for FXAA
    ldr = pow(saturate(ldr), 1.0 / 2.2);
    float luma = dot(ldr, float3(0.299, 0.587, 0.114));
    return float4(ldr, luma);
}

// --- Crisp 5x7 Bitmap Font for Top-Right FPS Counter HUD ---
static const uint gFont5x7[18][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // 2
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // 10: F
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // 11: P
    {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}, // 12: S
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}, // 13: m
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, // 14: s
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, // 15: .
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // 16: |
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  // 17: space
};

float3 compositeFpsHud(float3 sceneCol, float2 pixelPos) {
    if (gTexelSize.z < 0.5) return sceneCol;

    float resX = gResolutionInv.x;
    float resY = gResolutionInv.y;
    int scale = max(2, (int)round(resY / 480.0));

    int numChars = 17; // " 453 FPS |  2.2ms"
    int charAdv = 6 * scale;
    int textW = numChars * charAdv - scale;
    int textH = 7 * scale;

    int padX = 5 * scale;
    int padY = 4 * scale;
    int margin = 7 * scale;

    float boxW = (float)(textW + padX * 2);
    float boxH = (float)(textH + padY * 2);
    float2 boxMin = float2(resX - margin - boxW, (float)margin);
    float2 boxCenter = boxMin + float2(boxW, boxH) * 0.5;
    float2 halfSize = float2(boxW, boxH) * 0.5;
    float radius = 3.0 * scale;

    // Rounded box SDF
    float2 q = abs(pixelPos - boxCenter) - (halfSize - radius);
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    if (sdf > 1.5) return sceneCol;

    float boxMask = 1.0 - smoothstep(-0.75, 0.75, sdf);
    float borderMask = (1.0 - smoothstep(-0.75, 0.75, abs(sdf + 0.5) - 0.75)) * boxMask;

    // Dark translucent woodland glass pill
    float3 bgCol = float3(0.03, 0.055, 0.042);
    float3 borderCol = float3(0.32, 0.50, 0.36);
    float3 col = lerp(sceneCol, bgCol, boxMask * 0.74);
    col = lerp(col, borderCol, borderMask * 0.55);

    // Format "XXXX FPS | XX.Xms"
    int fpsInt = clamp((int)round(gTexelSize.x), 0, 9999);
    int msTenths = clamp((int)round(gTexelSize.y * 10.0), 0, 999);

    int d3 = (fpsInt / 1000) % 10;
    int d2 = (fpsInt / 100) % 10;
    int d1 = (fpsInt / 10) % 10;
    int d0 = fpsInt % 10;

    int m1 = (msTenths / 100) % 10;
    int m0 = (msTenths / 10) % 10;
    int mf = msTenths % 10;

    float2 rel = pixelPos - (boxMin + float2(padX, padY));
    if (rel.x >= 0.0 && rel.y >= 0.0 && rel.x < (float)textW && rel.y < (float)textH) {
        int gx = (int)floor(rel.x) / scale;
        int gy = (int)floor(rel.y) / scale;
        int charIdx = gx / 6;
        int colInChar = gx - charIdx * 6;

        if (colInChar < 5 && gy >= 0 && gy < 7) {
            int glyph = 17;
            if      (charIdx == 0)  glyph = (fpsInt >= 1000) ? d3 : 17;
            else if (charIdx == 1)  glyph = (fpsInt >= 100)  ? d2 : 17;
            else if (charIdx == 2)  glyph = (fpsInt >= 10)   ? d1 : 17;
            else if (charIdx == 3)  glyph = d0;
            else if (charIdx == 4)  glyph = 17;
            else if (charIdx == 5)  glyph = 10; // F
            else if (charIdx == 6)  glyph = 11; // P
            else if (charIdx == 7)  glyph = 12; // S
            else if (charIdx == 8)  glyph = 17;
            else if (charIdx == 9)  glyph = 16; // |
            else if (charIdx == 10) glyph = 17;
            else if (charIdx == 11) glyph = (msTenths >= 100) ? m1 : 17;
            else if (charIdx == 12) glyph = m0;
            else if (charIdx == 13) glyph = 15; // .
            else if (charIdx == 14) glyph = mf;
            else if (charIdx == 15) glyph = 13; // m
            else if (charIdx == 16) glyph = 14; // s

            uint rowBits = gFont5x7[glyph][gy];
            if (((rowBits >> (4 - colInChar)) & 1u) != 0u) {
                // Color-code FPS readout (emerald >= 60, amber 30-59, coral < 30); muted warm white for ms & labels
                float3 fpsColor = (gTexelSize.x >= 59.5) ? float3(0.56, 0.96, 0.68)
                                : (gTexelSize.x >= 29.5) ? float3(0.98, 0.82, 0.36)
                                                         : float3(0.98, 0.42, 0.38);
                float3 textColor = (charIdx <= 7) ? fpsColor
                                 : (charIdx == 9) ? float3(0.45, 0.56, 0.48)
                                                  : float3(0.88, 0.92, 0.86);
                col = textColor;
            }
        }
    }

    return col;
}

// 6) Fast Approximate Anti-Aliasing (FXAA) + Optional FPS HUD Overlay
float4 PSFXAA(VSOut i) : SV_TARGET {
    float2 invRes = gResolutionInv.zw;
    float2 uv = i.uv;

    float4 rgbM  = gInputTex0.SampleLevel(gLinearClamp, uv, 0);
    float lumaNW = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-1, -1) * invRes, 0).a;
    float lumaNE = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 1, -1) * invRes, 0).a;
    float lumaSW = gInputTex0.SampleLevel(gLinearClamp, uv + float2(-1,  1) * invRes, 0).a;
    float lumaSE = gInputTex0.SampleLevel(gLinearClamp, uv + float2( 1,  1) * invRes, 0).a;
    float lumaM  = rgbM.a;

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float range = lumaMax - lumaMin;

    float3 outRGB;
    if (range < max(0.0312, lumaMax * 0.125)) {
        outRGB = rgbM.rgb;
    } else {
        float2 dir;
        dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
        dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

        float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 1.0 / 128.0);
        float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
        dir = clamp(dir * rcpDirMin, float2(-8.0, -8.0), float2(8.0, 8.0)) * invRes;

        float3 rgbA = 0.5 * (
            gInputTex0.SampleLevel(gLinearClamp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
            gInputTex0.SampleLevel(gLinearClamp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb
        );
        float3 rgbB = rgbA * 0.5 + 0.25 * (
            gInputTex0.SampleLevel(gLinearClamp, uv + dir * -0.5, 0).rgb +
            gInputTex0.SampleLevel(gLinearClamp, uv + dir *  0.5, 0).rgb
        );
        float lumaB = dot(rgbB, float3(0.299, 0.587, 0.114));

        outRGB = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    }

    outRGB = compositeFpsHud(outRGB, i.svPos.xy);
    return float4(outRGB, 1.0);
}
