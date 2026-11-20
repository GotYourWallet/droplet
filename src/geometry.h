#pragma once
#include "math_utils.h"
#include <vector>
#include <cstdint>
#include <cmath>
#include <unordered_map>

namespace geo {

using namespace math;

struct Vertex {
    Vec3 pos;
    Vec3 normal;
    Vec3 tangent;
    Vec2 uv;
    Vec4 extra;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct DustParticle {
    Vec3 basePos;
    float size;
    Vec3 velocity;
    float phase;
};

// Exact drip origin slightly embedded into the underside of the curved branch tip
constexpr float DRIP_TIP_X = -0.02f;
constexpr float DRIP_TIP_Y =  1.355f;
constexpr float DRIP_TIP_Z =  0.00f;
constexpr float POOL_WATER_Y = -0.35f;

inline Vec3 evalCubicBezier(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    float u = 1.0f - t;
    float u2 = u * u;
    float t2 = t * t;
    return p0 * (u2 * u) + p1 * (3.0f * u2 * t) + p2 * (3.0f * u * t2) + p3 * (t2 * t);
}

inline Vec3 evalCubicBezierDeriv(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    float u = 1.0f - t;
    return (p1 - p0) * (3.0f * u * u) + (p2 - p1) * (6.0f * u * t) + (p3 - p2) * (3.0f * t * t);
}

inline float hash11(float p) {
    p = std::fmod(p * 0.1031f, 1.0f);
    if (p < 0.0f) p += 1.0f;
    p *= p + 33.33f;
    p *= p + p;
    return std::fmod(p, 1.0f);
}

inline float hash21(float x, float y) {
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453123f;
    return h - std::floor(h);
}

inline float valueNoise2D(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash21(ix, iy);
    float b = hash21(ix + 1.0f, iy);
    float c = hash21(ix, iy + 1.0f);
    float d = hash21(ix + 1.0f, iy + 1.0f);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

inline float fbm2D(float x, float y) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * valueNoise2D(x, y);
        float nx = x * 1.97f - y * 0.41f + 13.7f;
        float ny = x * 0.41f + y * 1.97f + 7.3f;
        x = nx; y = ny;
        amp *= 0.5f;
    }
    return sum;
}

inline void appendTubeBranch(
    MeshData& mesh,
    const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
    float rStart, float rEnd,
    int rings, int segs,
    float branchId,
    bool hasDripChannel,
    bool closeTip)
{
    uint32_t baseVertex = static_cast<uint32_t>(mesh.vertices.size());

    Vec3 prevT = normalize(evalCubicBezierDeriv(p0, p1, p2, p3, 0.0f));
    Vec3 refUp = (std::fabs(prevT.y) < 0.92f) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
    Vec3 frameN = normalize(cross(refUp, prevT));
    Vec3 frameB = normalize(cross(prevT, frameN));

    const int tipRings = closeTip ? 4 : 0;
    const int totalRings = rings + tipRings;

    for (int i = 0; i <= totalRings; ++i) {
        float v = std::min(1.0f, static_cast<float>(i) / static_cast<float>(rings));
        Vec3 center = evalCubicBezier(p0, p1, p2, p3, v);
        Vec3 tang = normalize(evalCubicBezierDeriv(p0, p1, p2, p3, v));

        Vec3 axis = cross(prevT, tang);
        float sinA = length(axis);
        if (sinA > 1e-5f) {
            Vec3 k = axis / sinA;
            float cosA = clamp(dot(prevT, tang), -1.0f, 1.0f);
            frameN = frameN * cosA + cross(k, frameN) * sinA + k * (dot(k, frameN) * (1.0f - cosA));
            frameN = normalize(frameN - tang * dot(frameN, tang));
            frameB = normalize(cross(tang, frameN));
        }
        prevT = tang;

        float taper = lerp(rStart, rEnd, std::pow(v, 0.80f));
        float knot = 1.0f + 0.06f * std::sin(v * 9.5f + branchId * 3.1f)
                          + 0.035f * std::cos(v * 21.0f - branchId * 1.7f);
        float radius = taper * knot;
        float normalAxialMix = 0.0f;

        // Smooth hemispherical rounded tip instead of a sharp cone
        if (i > rings) {
            float capT = static_cast<float>(i - rings) / static_cast<float>(tipRings);
            float capAngle = capT * (PI * 0.48f);
            center += tang * (rEnd * 1.15f * std::sin(capAngle));
            radius = rEnd * std::cos(capAngle);
            normalAxialMix = std::sin(capAngle);
        }

        for (int j = 0; j <= segs; ++j) {
            float u = static_cast<float>(j) / static_cast<float>(segs);
            float theta = u * TWO_PI;
            float ct = std::cos(theta);
            float st = std::sin(theta);

            float rMod = radius * (1.0f + 0.05f * std::sin(theta * 3.0f + v * 5.0f));
            Vec3 radialDir = normalize(frameN * ct + frameB * st);
            Vec3 pos = center + radialDir * rMod;
            Vec3 vertNormal = normalize(radialDir * std::sqrt(std::max(0.01f, 1.0f - normalAxialMix * normalAxialMix)) + tang * normalAxialMix);

            float underside = clamp(-radialDir.y, 0.0f, 1.0f);
            float wetness = hasDripChannel
                ? smoothstep(0.30f, 0.90f, v) * std::pow(underside, 1.3f)
                : 0.25f * smoothstep(0.5f, 1.0f, v) * underside;
            float moss = smoothstep(0.05f, 0.80f, radialDir.y) * smoothstep(1.0f, 0.12f, v);

            Vertex vert;
            vert.pos = pos;
            vert.normal = vertNormal;
            vert.tangent = tang;
            vert.uv = Vec2(u, v * 5.0f);
            vert.extra = Vec4(std::max(radius, 0.015f), wetness, moss, branchId);
            mesh.vertices.push_back(vert);
        }
    }

    int stride = segs + 1;
    for (int i = 0; i < totalRings; ++i) {
        for (int j = 0; j < segs; ++j) {
            uint32_t i0 = baseVertex + i * stride + j;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + stride;
            uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);

            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
}

inline MeshData buildBranchMesh() {
    MeshData mesh;

    // 1) Main gnarled mossy hero branch curving gracefully down to rounded tip right above (DRIP_TIP_X, DRIP_TIP_Y, DRIP_TIP_Z)
    Vec3 m0(-3.15f, 2.38f, -0.40f);
    Vec3 m1(-1.72f, 1.98f,  0.14f);
    Vec3 m2(-0.58f, 1.66f,  0.06f);
    Vec3 m3(-0.02f, 1.39f,  0.00f);
    appendTubeBranch(mesh, m0, m1, m2, m3, 0.23f, 0.044f, 40, 18, 0.0f, true, true);

    // 2) Upper fork twig sweeping up-right (frames golden sunbeams & backlit leaves)
    Vec3 t1_0(-1.45f, 1.90f,  0.02f);
    Vec3 t1_1(-0.82f, 2.15f,  0.18f);
    Vec3 t1_2(-0.12f, 2.30f,  0.34f);
    Vec3 t1_3( 0.65f, 2.28f,  0.45f);
    appendTubeBranch(mesh, t1_0, t1_1, t1_2, t1_3, 0.088f, 0.018f, 20, 12, 1.0f, false, true);

    // 3) Side twig reaching gently toward back-right
    Vec3 t2_0(-0.88f, 1.74f,  0.06f);
    Vec3 t2_1(-0.52f, 1.80f,  0.48f);
    Vec3 t2_2(-0.08f, 1.84f,  0.88f);
    Vec3 t2_3( 0.42f, 1.76f,  1.15f);
    appendTubeBranch(mesh, t2_0, t2_1, t2_2, t2_3, 0.056f, 0.015f, 16, 10, 2.0f, false, true);

    // 4) Delicate small spur twig near the tip (carries hero backlit leaves right above droplet)
    Vec3 t3_0(-0.48f, 1.60f,  0.04f);
    Vec3 t3_1(-0.30f, 1.72f, -0.14f);
    Vec3 t3_2(-0.08f, 1.78f, -0.32f);
    Vec3 t3_3( 0.14f, 1.74f, -0.44f);
    appendTubeBranch(mesh, t3_0, t3_1, t3_2, t3_3, 0.034f, 0.012f, 14, 10, 3.0f, false, true);

    // 5) Lower dead-wood stub on left
    Vec3 t4_0(-2.10f, 2.04f, -0.14f);
    Vec3 t4_1(-1.88f, 1.62f, -0.38f);
    Vec3 t4_2(-1.65f, 1.34f, -0.52f);
    Vec3 t4_3(-1.42f, 1.20f, -0.58f);
    appendTubeBranch(mesh, t4_0, t4_1, t4_2, t4_3, 0.070f, 0.020f, 12, 10, 4.0f, false, true);

    return mesh;
}

inline void appendLeaf(
    MeshData& mesh,
    const Vec3& rootPos,
    const Vec3& forwardDir,
    const Vec3& upHint,
    float length_,
    float width_,
    float droop_,
    float leafId)
{
    Vec3 fwd = normalize(forwardDir);
    Vec3 side = normalize(cross(upHint, fwd));
    Vec3 up = normalize(cross(fwd, side));

    const int segU = 14;
    const int segV = 8;
    uint32_t baseVertex = static_cast<uint32_t>(mesh.vertices.size());

    for (int i = 0; i <= segU; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(segU);
        float profile = std::sin(std::pow(u, 0.72f) * PI);
        float halfW = width_ * 0.5f * profile;
        float arch = std::sin(u * PI) * 0.06f * length_ - droop_ * u * u * length_;

        for (int j = 0; j <= segV; ++j) {
            float vNorm = static_cast<float>(j) / static_cast<float>(segV);
            float vSigned = vNorm * 2.0f - 1.0f;

            float cup = std::fabs(vSigned) * 0.055f * width_ - (vSigned * vSigned) * 0.03f * width_;
            float wave = std::sin(u * 10.0f + leafId * 2.3f) * 0.010f * std::fabs(vSigned);

            Vec3 p = rootPos
                + fwd * (u * length_)
                + side * (vSigned * halfW)
                + up * (arch + cup + wave);

            float dArch = (std::cos(u * PI) * PI * 0.06f - 2.0f * droop_ * u) * length_;
            Vec3 dPdu = normalize(fwd * length_ + up * dArch);
            float dCup = (vSigned >= 0.0f ? 1.0f : -1.0f) * 0.055f - 0.06f * vSigned;
            Vec3 dPdv = normalize(side * std::max(halfW, 0.01f) + up * (dCup * width_));
            Vec3 n = normalize(cross(dPdu, dPdv));
            if (dot(n, up) < 0.0f) n = -n;

            float thickness = clamp(0.18f + 0.65f * (1.0f - std::fabs(vSigned)) * (1.0f - 0.5f * u), 0.1f, 1.0f);

            Vertex vert;
            vert.pos = p;
            vert.normal = n;
            vert.tangent = dPdu;
            vert.uv = Vec2(u, vNorm);
            vert.extra = Vec4(thickness, leafId, u * u, 0.0f);
            mesh.vertices.push_back(vert);
        }
    }

    int stride = segV + 1;
    for (int i = 0; i < segU; ++i) {
        for (int j = 0; j < segV; ++j) {
            uint32_t i0 = baseVertex + i * stride + j;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + stride;
            uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
}

inline MeshData buildLeavesMesh() {
    MeshData mesh;
    // Lush backlit emerald forest leaves framing the hero branch & sunbeams
    appendLeaf(mesh, Vec3(-0.34f, 1.68f, -0.10f), Vec3( 0.55f, 0.20f, -0.78f), Vec3(0.0f, 1.0f, 0.0f), 0.52f, 0.24f, 0.30f, 1.0f);
    appendLeaf(mesh, Vec3( 0.08f, 1.75f, -0.40f), Vec3( 0.85f, 0.06f, -0.48f), Vec3(0.0f, 1.0f, 0.1f), 0.46f, 0.21f, 0.35f, 2.0f);
    appendLeaf(mesh, Vec3(-0.52f, 1.79f,  0.42f), Vec3( 0.45f, 0.22f,  0.86f), Vec3(0.0f, 1.0f, 0.0f), 0.58f, 0.27f, 0.26f, 3.0f);
    appendLeaf(mesh, Vec3( 0.22f, 1.80f,  0.95f), Vec3( 0.82f, 0.02f,  0.52f), Vec3(0.0f, 1.0f, 0.0f), 0.54f, 0.24f, 0.32f, 4.0f);
    appendLeaf(mesh, Vec3(-0.18f, 2.28f,  0.32f), Vec3( 0.78f, 0.14f,  0.58f), Vec3(0.0f, 1.0f, 0.0f), 0.62f, 0.28f, 0.28f, 5.0f);
    appendLeaf(mesh, Vec3( 0.58f, 2.28f,  0.44f), Vec3( 0.90f,-0.04f,  0.32f), Vec3(0.0f, 1.0f, 0.0f), 0.50f, 0.22f, 0.36f, 6.0f);
    appendLeaf(mesh, Vec3(-0.98f, 2.06f,  0.14f), Vec3(-0.32f, 0.32f,  0.88f), Vec3(0.0f, 1.0f, 0.0f), 0.58f, 0.26f, 0.24f, 7.0f);
    appendLeaf(mesh, Vec3(-1.48f, 1.24f, -0.55f), Vec3( 0.68f,-0.10f, -0.70f), Vec3(0.0f, 1.0f, 0.0f), 0.46f, 0.20f, 0.36f, 8.0f);
    appendLeaf(mesh, Vec3(-0.68f, 1.72f,  0.04f), Vec3( 0.42f, 0.28f,  0.84f), Vec3(0.0f, 1.0f, 0.0f), 0.44f, 0.20f, 0.28f, 9.0f);
    appendLeaf(mesh, Vec3(-1.18f, 1.85f, -0.02f), Vec3( 0.35f,-0.08f, -0.92f), Vec3(0.0f, 1.0f, 0.0f), 0.50f, 0.22f, 0.34f, 10.0f);
    return mesh;
}

inline MeshData buildIcosphereMesh(int subdivisions = 2) {
    MeshData mesh;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;

    std::vector<Vec3> positions = {
        normalize({-1,  t,  0}), normalize({ 1,  t,  0}), normalize({-1, -t,  0}), normalize({ 1, -t,  0}),
        normalize({ 0, -1,  t}), normalize({ 0,  1,  t}), normalize({ 0, -1, -t}), normalize({ 0,  1, -t}),
        normalize({ t,  0, -1}), normalize({ t,  0,  1}), normalize({-t,  0, -1}), normalize({-t,  0,  1})
    };

    std::vector<uint32_t> indices = {
        0,11,5,  0,5,1,  0,1,7,  0,7,10, 0,10,11,
        1,5,9,   5,11,4, 11,10,2, 10,7,6, 7,1,8,
        3,9,4,   3,4,2,  3,2,6,  3,6,8,  3,8,9,
        4,9,5,   2,4,11, 6,2,10, 8,6,7,  9,8,1
    };

    for (int s = 0; s < subdivisions; ++s) {
        std::unordered_map<uint64_t, uint32_t> midpointCache;
        auto getMidpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
            uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            auto it = midpointCache.find(key);
            if (it != midpointCache.end()) return it->second;
            Vec3 mid = normalize((positions[a] + positions[b]) * 0.5f);
            uint32_t idx = static_cast<uint32_t>(positions.size());
            positions.push_back(mid);
            midpointCache[key] = idx;
            return idx;
        };

        std::vector<uint32_t> nextIndices;
        nextIndices.reserve(indices.size() * 4);
        for (size_t i = 0; i < indices.size(); i += 3) {
            uint32_t v0 = indices[i];
            uint32_t v1 = indices[i + 1];
            uint32_t v2 = indices[i + 2];
            uint32_t a = getMidpoint(v0, v1);
            uint32_t b = getMidpoint(v1, v2);
            uint32_t c = getMidpoint(v2, v0);
            nextIndices.insert(nextIndices.end(), {v0, a, c, v1, b, a, v2, c, b, a, b, c});
        }
        indices.swap(nextIndices);
    }

    mesh.vertices.reserve(positions.size());
    for (const Vec3& p : positions) {
        Vertex v;
        v.pos = p;
        v.normal = p;
        Vec3 up = (std::fabs(p.y) < 0.99f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        v.tangent = normalize(cross(up, p));
        v.uv = Vec2(0.5f + std::atan2(p.z, p.x) / TWO_PI, 0.5f - std::asin(clamp(p.y, -1.0f, 1.0f)) / PI);
        v.extra = Vec4(0, 0, 0, 0);
        mesh.vertices.push_back(v);
    }
    mesh.indices = std::move(indices);
    return mesh;
}

// Organic woodland pool basin & mossy riverbank (pure multi-octave noise, zero periodic stripes)
inline MeshData buildPoolBedMesh() {
    MeshData mesh;
    const int N = 112;
    const float halfSize = 7.5f;

    auto bedHeight = [](float x, float z) -> float {
        float r = std::sqrt(x * x + z * z);
        float basin = -0.68f + 0.52f * smoothstep(0.8f, 3.6f, r) + 0.35f * smoothstep(3.5f, 7.2f, r);
        float organic = (fbm2D(x * 0.55f + 4.1f, z * 0.55f - 2.3f) - 0.5f) * 0.28f;
        float pebbles = (fbm2D(x * 2.8f - 1.7f, z * 2.8f + 5.9f) - 0.5f) * 0.065f;
        float bankMounds = smoothstep(1.9f, 4.2f, r) * (fbm2D(x * 0.9f, z * 0.9f) * 0.24f);
        return basin + organic + pebbles + bankMounds;
    };

    mesh.vertices.reserve((N + 1) * (N + 1));
    for (int iz = 0; iz <= N; ++iz) {
        for (int ix = 0; ix <= N; ++ix) {
            float fx = static_cast<float>(ix) / static_cast<float>(N);
            float fz = static_cast<float>(iz) / static_cast<float>(N);
            float x = (fx * 2.0f - 1.0f) * halfSize;
            float z = (fz * 2.0f - 1.0f) * halfSize;
            float y = bedHeight(x, z);

            float eps = 0.06f;
            float hx = bedHeight(x + eps, z) - bedHeight(x - eps, z);
            float hz = bedHeight(x, z + eps) - bedHeight(x, z - eps);
            Vec3 n = normalize(Vec3(-hx, 2.0f * eps, -hz));

            float waterDepth = std::max(0.0f, POOL_WATER_Y - y);
            float r = std::sqrt(x * x + z * z);
            float mossMask = smoothstep(1.65f, 3.1f, r);

            Vertex v;
            v.pos = Vec3(x, y, z);
            v.normal = n;
            v.tangent = Vec3(1, 0, 0);
            v.uv = Vec2((x / 5.2f) + 0.5f, (z / 5.2f) + 0.5f);
            v.extra = Vec4(waterDepth, r, mossMask, 0.0f);
            mesh.vertices.push_back(v);
        }
    }

    int stride = N + 1;
    for (int iz = 0; iz < N; ++iz) {
        for (int ix = 0; ix < N; ++ix) {
            uint32_t i0 = iz * stride + ix;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + stride;
            uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
    return mesh;
}

inline MeshData buildPoolWaterMesh() {
    MeshData mesh;
    const int N = 128;
    const float halfSize = 2.6f;

    mesh.vertices.reserve((N + 1) * (N + 1));
    for (int iz = 0; iz <= N; ++iz) {
        for (int ix = 0; ix <= N; ++ix) {
            float u = static_cast<float>(ix) / static_cast<float>(N);
            float v = static_cast<float>(iz) / static_cast<float>(N);
            float x = (u * 2.0f - 1.0f) * halfSize;
            float z = (v * 2.0f - 1.0f) * halfSize;

            Vertex vert;
            vert.pos = Vec3(x, POOL_WATER_Y, z);
            vert.normal = Vec3(0, 1, 0);
            vert.tangent = Vec3(1, 0, 0);
            vert.uv = Vec2(u, v);
            vert.extra = Vec4(std::sqrt(x * x + z * z), 0, 0, 0);
            mesh.vertices.push_back(vert);
        }
    }

    int stride = N + 1;
    for (int iz = 0; iz < N; ++iz) {
        for (int ix = 0; ix < N; ++ix) {
            uint32_t i0 = iz * stride + ix;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + stride;
            uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
    return mesh;
}

inline MeshData buildBackgroundForestMesh() {
    MeshData mesh;
    struct TrunkSpec { float x, z, radius, leanX, leanZ; };
    const TrunkSpec trunks[] = {
        {-5.6f,  6.2f, 0.58f,  0.3f,  0.1f},
        {-2.6f,  8.8f, 0.62f, -0.2f,  0.2f},
        { 1.7f,  8.0f, 0.46f,  0.35f,-0.1f},
        { 5.1f,  6.6f, 0.72f, -0.3f,  0.1f},
        {-7.2f, 11.8f, 0.88f,  0.2f,  0.0f},
        {-0.3f, 12.6f, 0.58f, -0.1f,  0.2f},
        { 3.6f, 12.0f, 0.75f,  0.3f, -0.2f},
        { 7.8f, 10.5f, 0.90f, -0.4f,  0.1f},
    };

    for (int k = 0; k < 8; ++k) {
        const auto& tr = trunks[k];
        Vec3 p0(tr.x, -1.5f, tr.z);
        Vec3 p1(tr.x + tr.leanX * 0.3f, 2.0f, tr.z + tr.leanZ * 0.3f);
        Vec3 p2(tr.x + tr.leanX * 0.7f, 6.0f, tr.z + tr.leanZ * 0.7f);
        Vec3 p3(tr.x + tr.leanX,       10.5f, tr.z + tr.leanZ);
        appendTubeBranch(mesh, p0, p1, p2, p3, tr.radius, tr.radius * 0.65f, 8, 10, 10.0f + k, false, false);
    }
    return mesh;
}

inline std::vector<DustParticle> buildDustParticles(int count = 140) {
    std::vector<DustParticle> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        float fi = static_cast<float>(i);
        float rx = hash11(fi * 1.17f + 0.31f) * 2.0f - 1.0f;
        float ry = hash11(fi * 2.43f + 0.77f);
        float rz = hash11(fi * 3.89f + 0.19f) * 2.0f - 1.0f;
        DustParticle p;
        p.basePos = Vec3(rx * 4.2f, -0.1f + ry * 3.2f, rz * 4.2f + 1.5f);
        p.size = 0.005f + 0.011f * hash11(fi * 5.13f + 0.55f);
        p.velocity = Vec3(
            (hash11(fi * 7.1f) - 0.5f) * 0.06f,
            0.015f + hash11(fi * 9.3f) * 0.03f,
            (hash11(fi * 11.7f) - 0.5f) * 0.06f
        );
        p.phase = hash11(fi * 13.3f) * TWO_PI;
        out.push_back(p);
    }
    return out;
}

} // namespace geo
