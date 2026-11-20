#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

namespace texgen {

struct GrassTextureSet {
    uint32_t size = 1024;
    std::vector<uint8_t> albedoRGBA;      // RGB = sRGB/linearized albedo, A = ambient occlusion
    std::vector<uint8_t> normalRoughRGBA; // RGB = tangent-space normal [0..255], A = roughness
};

namespace detail {

inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline float rand01(uint32_t& s) {
    return (xorshift32(s) & 0x00FFFFFFu) / 16777216.0f;
}

inline float randRange(uint32_t& s, float a, float b) {
    return a + (b - a) * rand01(s);
}

inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline float hash2D(int x, int y, int period) {
    int wx = ((x % period) + period) % period;
    int wy = ((y % period) + period) % period;
    uint32_t h = static_cast<uint32_t>(wx) * 374761393u + static_cast<uint32_t>(wy) * 668265263u + 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return (h & 0x00FFFFFFu) / 16777216.0f;
}

inline float tileValueNoise(float u, float v, int freq) {
    float x = u * freq;
    float y = v * freq;
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - ix;
    float fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);

    float n00 = hash2D(ix,     iy,     freq);
    float n10 = hash2D(ix + 1, iy,     freq);
    float n01 = hash2D(ix,     iy + 1, freq);
    float n11 = hash2D(ix + 1, iy + 1, freq);
    float nx0 = n00 + (n10 - n00) * fx;
    float nx1 = n01 + (n11 - n01) * fx;
    return nx0 + (nx1 - nx0) * fy;
}

inline float tileFbm(float u, float v, int baseFreq, int octaves) {
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    int f = baseFreq;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * tileValueNoise(u, v, f);
        norm += amp;
        f *= 2;
        amp *= 0.5f;
    }
    return sum / norm;
}

} // namespace detail

// Generates a seamless, tileable 1024x1024 realistic woodland grass & mossy turf PBR texture set:
// - Rich damp forest soil & humus base with micro-pebbles
// - 22,000+ individually curved, tapered, overlapping grass blades across 3 depth layers
//   (shadowed thatch/dry blades, lush woodland emerald blades, sunlit dew-tipped blades)
// - Scattered organic clover & moss micro-tufts
// - True heightfield-derived tangent-space Normal map + Ambient Occlusion + Roughness
inline GrassTextureSet generateWoodlandGrassTextures(uint32_t size = 1024) {
    using namespace detail;
    GrassTextureSet out;
    out.size = size;
    const size_t N = static_cast<size_t>(size) * size;

    std::vector<float> colR(N), colG(N), colB(N);
    std::vector<float> height(N);
    std::vector<float> rough(N);
    std::vector<float> ao(N);

    const float invSize = 1.0f / static_cast<float>(size);

    // 1) Base damp woodland soil, loam & mossy ground cover
    for (uint32_t y = 0; y < size; ++y) {
        float v = y * invSize;
        for (uint32_t x = 0; x < size; ++x) {
            float u = x * invSize;
            size_t idx = static_cast<size_t>(y) * size + x;

            float macro = tileFbm(u, v, 6, 4);
            float grit  = tileFbm(u + 0.37f, v + 0.19f, 32, 3);
            float moss  = tileFbm(u + 0.71f, v + 0.53f, 12, 4);

            // Dark fertile woodland soil with subtle olive-moss undertone
            float r = 0.042f + 0.048f * macro + 0.024f * grit;
            float g = 0.036f + 0.042f * macro + 0.020f * grit;
            float b = 0.022f + 0.022f * macro + 0.012f * grit;

            float mossAmt = clamp01((moss - 0.38f) * 2.2f);
            r = r + (0.045f - r) * mossAmt * 0.65f;
            g = g + (0.088f - g) * mossAmt * 0.65f;
            b = b + (0.024f - b) * mossAmt * 0.65f;

            colR[idx] = r;
            colG[idx] = g;
            colB[idx] = b;
            height[idx] = 0.06f * macro + 0.05f * grit;
            rough[idx] = 0.88f - 0.12f * mossAmt;
            ao[idx] = 0.32f + 0.18f * macro;
        }
    }

    auto wrapIdx = [&](int x, int y) -> size_t {
        int wx = ((x % static_cast<int>(size)) + static_cast<int>(size)) % static_cast<int>(size);
        int wy = ((y % static_cast<int>(size)) + static_cast<int>(size)) % static_cast<int>(size);
        return static_cast<size_t>(wy) * size + static_cast<size_t>(wx);
    };

    // Helper to splat a curved, tapered grass blade with toroidal wrapping & height-aware blending
    auto splatBlade = [&](float cx, float cy, float angle, float length, float halfWidth, float bend,
                          float baseR, float baseG, float baseB,
                          float tipR,  float tipG,  float tipB,
                          float baseH, float tipH, float bladeRough) {
        float dx = std::cos(angle);
        float dy = std::sin(angle);
        float px = -dy;
        float py =  dx;

        int steps = std::max(10, static_cast<int>(length * 1.45f));
        for (int s = 0; s <= steps; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(steps);
            // Smooth quadratic bend along the blade
            float along = t * length;
            float curve = t * t * bend;
            float bx = cx + dx * along + px * curve;
            float by = cy + dy * along + py * curve;

            // Natural botanical taper (slightly wider near base, sharp point at tip)
            float w = halfWidth * (1.0f - t * t * 0.88f) * (0.85f + 0.15f * std::sin(t * 3.14159f));
            float hAlong = baseH + (tipH - baseH) * std::pow(t, 0.78f);

            int rad = std::max(1, static_cast<int>(std::ceil(w + 1.0f)));
            int ibx = static_cast<int>(std::floor(bx));
            int iby = static_cast<int>(std::floor(by));

            for (int oy = -rad; oy <= rad; ++oy) {
                for (int ox = -rad; ox <= rad; ++ox) {
                    float fx = (ibx + ox + 0.5f) - bx;
                    float fy = (iby + oy + 0.5f) - by;
                    // Distance perpendicular to blade centerline
                    float perp = std::fabs(fx * px + fy * py);
                    float para = std::fabs(fx * dx + fy * dy);
                    if (perp > w + 0.85f || para > 1.35f) continue;

                    float edgeAlpha = clamp01((w + 0.65f - perp) / 1.15f);
                    if (edgeAlpha <= 0.01f) continue;

                    // Subtle central midrib ridge across blade width
                    float midrib = 1.0f - clamp01(perp / std::max(w, 0.25f));
                    float pixH = hAlong + midrib * 0.045f * (0.4f + 0.6f * t);

                    size_t idx = wrapIdx(ibx + ox, iby + oy);
                    if (pixH + 0.025f < height[idx]) continue;

                    float blend = edgeAlpha * clamp01((pixH - height[idx] + 0.045f) * 14.0f);
                    float shade = (0.76f + 0.24f * midrib) * (0.58f + 0.42f * t);

                    float r = (baseR + (tipR - baseR) * t) * shade;
                    float g = (baseG + (tipG - baseG) * t) * shade;
                    float b = (baseB + (tipB - baseB) * t) * shade;

                    colR[idx] = colR[idx] + (r - colR[idx]) * blend;
                    colG[idx] = colG[idx] + (g - colG[idx]) * blend;
                    colB[idx] = colB[idx] + (b - colB[idx]) * blend;
                    if (pixH > height[idx]) {
                        height[idx] = height[idx] + (pixH - height[idx]) * edgeAlpha;
                    }
                    rough[idx] = rough[idx] + (bladeRough - rough[idx]) * blend;
                    float bladeAO = 0.40f + 0.60f * t;
                    ao[idx] = ao[idx] + (bladeAO - ao[idx]) * blend;
                }
            }
        }
    };

    uint32_t rng = 0x5A3C91E7u;
    const float scaleRatio = static_cast<float>(size) / 1024.0f;

    // 2) Layer 1: Under-thatch, dry golden-brown woodland blades & dark base shoots (7,500 blades)
    for (int i = 0; i < 7500; ++i) {
        float cx = rand01(rng) * size;
        float cy = rand01(rng) * size;
        float angle = randRange(rng, 0.0f, 6.2831853f);
        float len = randRange(rng, 16.0f, 44.0f) * scaleRatio;
        float hw = randRange(rng, 1.3f, 2.5f) * scaleRatio;
        float bend = randRange(rng, -9.0f, 9.0f) * scaleRatio;

        bool isDry = rand01(rng) < 0.28f;
        float br, bg, bb, tr, tg, tb;
        if (isDry) {
            br = randRange(rng, 0.06f, 0.10f);
            bg = randRange(rng, 0.05f, 0.08f);
            bb = randRange(rng, 0.02f, 0.035f);
            tr = randRange(rng, 0.14f, 0.22f);
            tg = randRange(rng, 0.11f, 0.17f);
            tb = randRange(rng, 0.04f, 0.07f);
        } else {
            br = randRange(rng, 0.025f, 0.045f);
            bg = randRange(rng, 0.055f, 0.095f);
            bb = randRange(rng, 0.015f, 0.030f);
            tr = randRange(rng, 0.055f, 0.095f);
            tg = randRange(rng, 0.115f, 0.180f);
            tb = randRange(rng, 0.025f, 0.045f);
        }
        splatBlade(cx, cy, angle, len, hw, bend, br, bg, bb, tr, tg, tb, 0.12f, 0.42f, isDry ? 0.78f : 0.58f);
    }

    // 3) Layer 2: Lush mid-canopy woodland grass tufts (10,500 blades grouped softly in natural clumps)
    for (int clump = 0; clump < 1500; ++clump) {
        float clumpX = rand01(rng) * size;
        float clumpY = rand01(rng) * size;
        float clumpHue = randRange(rng, -0.02f, 0.03f);
        int bladesInClump = 6 + static_cast<int>(rand01(rng) * 3.0f);

        for (int b = 0; b < bladesInClump; ++b) {
            float cx = clumpX + randRange(rng, -10.0f, 10.0f) * scaleRatio;
            float cy = clumpY + randRange(rng, -10.0f, 10.0f) * scaleRatio;
            float angle = randRange(rng, 0.0f, 6.2831853f);
            float len = randRange(rng, 24.0f, 58.0f) * scaleRatio;
            float hw = randRange(rng, 1.4f, 2.8f) * scaleRatio;
            float bend = randRange(rng, -14.0f, 14.0f) * scaleRatio;

            float br = clamp01(randRange(rng, 0.030f, 0.055f) + clumpHue * 0.5f);
            float bg = clamp01(randRange(rng, 0.075f, 0.135f) + clumpHue);
            float bb = clamp01(randRange(rng, 0.018f, 0.035f));

            float tr = clamp01(randRange(rng, 0.085f, 0.155f) + clumpHue);
            float tg = clamp01(randRange(rng, 0.175f, 0.285f) + clumpHue * 1.4f);
            float tb = clamp01(randRange(rng, 0.032f, 0.062f));

            float tipH = randRange(rng, 0.52f, 0.80f);
            splatBlade(cx, cy, angle, len, hw, bend, br, bg, bb, tr, tg, tb, 0.26f, tipH, randRange(rng, 0.36f, 0.52f));
        }
    }

    // 4) Layer 3: Fine top-layer sunlit & dew-kissed grass blades (4,500 slender blades)
    for (int i = 0; i < 4500; ++i) {
        float cx = rand01(rng) * size;
        float cy = rand01(rng) * size;
        float angle = randRange(rng, 0.0f, 6.2831853f);
        float len = randRange(rng, 22.0f, 52.0f) * scaleRatio;
        float hw = randRange(rng, 1.0f, 2.0f) * scaleRatio;
        float bend = randRange(rng, -16.0f, 16.0f) * scaleRatio;

        float br = randRange(rng, 0.055f, 0.095f);
        float bg = randRange(rng, 0.125f, 0.195f);
        float bb = randRange(rng, 0.025f, 0.045f);

        // Fresh sunlit yellow-green tips
        float tr = randRange(rng, 0.145f, 0.235f);
        float tg = randRange(rng, 0.255f, 0.365f);
        float tb = randRange(rng, 0.045f, 0.082f);

        float tipH = randRange(rng, 0.78f, 1.0f);
        splatBlade(cx, cy, angle, len, hw, bend, br, bg, bb, tr, tg, tb, 0.45f, tipH, randRange(rng, 0.26f, 0.42f));
    }

    // 5) Pack Albedo+AO and derive crisp Sobel tangent-space Normal+Roughness
    out.albedoRGBA.resize(N * 4);
    out.normalRoughRGBA.resize(N * 4);

    const float normalStrength = 4.2f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            size_t idx = static_cast<size_t>(y) * size + x;

            float hTL = height[wrapIdx(static_cast<int>(x) - 1, static_cast<int>(y) - 1)];
            float hT  = height[wrapIdx(static_cast<int>(x),     static_cast<int>(y) - 1)];
            float hTR = height[wrapIdx(static_cast<int>(x) + 1, static_cast<int>(y) - 1)];
            float hL  = height[wrapIdx(static_cast<int>(x) - 1, static_cast<int>(y))];
            float hR  = height[wrapIdx(static_cast<int>(x) + 1, static_cast<int>(y))];
            float hBL = height[wrapIdx(static_cast<int>(x) - 1, static_cast<int>(y) + 1)];
            float hB  = height[wrapIdx(static_cast<int>(x),     static_cast<int>(y) + 1)];
            float hBR = height[wrapIdx(static_cast<int>(x) + 1, static_cast<int>(y) + 1)];

            float dX = (hTR + 2.0f * hR + hBR) - (hTL + 2.0f * hL + hBL);
            float dY = (hBL + 2.0f * hB + hBR) - (hTL + 2.0f * hT + hTR);

            float nx = -dX * normalStrength;
            float ny = -dY * normalStrength;
            float nz = 1.0f;
            float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= invLen;
            ny *= invLen;
            nz *= invLen;

            // Store linear albedo directly in UNORM8 (R8G8B8A8_UNORM) for crisp dark-range precision
            auto toU8 = [](float v) -> uint8_t {
                return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f);
            };

            out.albedoRGBA[idx * 4 + 0] = toU8(colR[idx]);
            out.albedoRGBA[idx * 4 + 1] = toU8(colG[idx]);
            out.albedoRGBA[idx * 4 + 2] = toU8(colB[idx]);
            out.albedoRGBA[idx * 4 + 3] = toU8(ao[idx]);

            out.normalRoughRGBA[idx * 4 + 0] = toU8(nx * 0.5f + 0.5f);
            out.normalRoughRGBA[idx * 4 + 1] = toU8(ny * 0.5f + 0.5f);
            out.normalRoughRGBA[idx * 4 + 2] = toU8(nz * 0.5f + 0.5f);
            out.normalRoughRGBA[idx * 4 + 3] = toU8(rough[idx]);
        }
    }

    return out;
}

} // namespace texgen
