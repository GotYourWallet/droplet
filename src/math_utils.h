#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace math {

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float DEG2RAD = PI / 180.0f;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float v) : x(v), y(v), z(v) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { float inv = 1.0f / s; return {x * inv, y * inv, z * inv}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return {v.x * s, v.y * s, v.z * s}; }

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(const Vec3& v) {
    float len = length(v);
    if (len < 1e-7f) return {0.0f, 1.0f, 0.0f};
    return v / len;
}

inline float clamp(float x, float lo, float hi) {
    return std::max(lo, std::min(hi, x));
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return a + (b - a) * t;
}

inline float smoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Row-major 4x4 matrix; transposed before upload to HLSL (column_major default)
struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 r = identity();
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0][0] = s.x;
        r.m[1][1] = s.y;
        r.m[2][2] = s.z;
        return r;
    }

    static Mat4 lookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 zaxis = normalize(target - eye);
        Vec3 xaxis = normalize(cross(up, zaxis));
        Vec3 yaxis = cross(zaxis, xaxis);

        Mat4 r = identity();
        r.m[0][0] = xaxis.x; r.m[0][1] = yaxis.x; r.m[0][2] = zaxis.x; r.m[0][3] = 0.0f;
        r.m[1][0] = xaxis.y; r.m[1][1] = yaxis.y; r.m[1][2] = zaxis.y; r.m[1][3] = 0.0f;
        r.m[2][0] = xaxis.z; r.m[2][1] = yaxis.z; r.m[2][2] = zaxis.z; r.m[2][3] = 0.0f;
        r.m[3][0] = -dot(xaxis, eye);
        r.m[3][1] = -dot(yaxis, eye);
        r.m[3][2] = -dot(zaxis, eye);
        r.m[3][3] = 1.0f;
        return r;
    }

    static Mat4 perspectiveFovLH(float fovY, float aspect, float zn, float zf) {
        float yScale = 1.0f / std::tan(fovY * 0.5f);
        float xScale = yScale / aspect;
        Mat4 r;
        r.m[0][0] = xScale;
        r.m[1][1] = yScale;
        r.m[2][2] = zf / (zf - zn);
        r.m[2][3] = 1.0f;
        r.m[3][2] = (-zn * zf) / (zf - zn);
        return r;
    }

    static Mat4 orthoOffCenterLH(float l, float r_, float b, float t, float zn, float zf) {
        Mat4 r;
        r.m[0][0] = 2.0f / (r_ - l);
        r.m[1][1] = 2.0f / (t - b);
        r.m[2][2] = 1.0f / (zf - zn);
        r.m[3][0] = (l + r_) / (l - r_);
        r.m[3][1] = (t + b) / (b - t);
        r.m[3][2] = zn / (zn - zf);
        r.m[3][3] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] +
                            m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
            }
        }
        return r;
    }

    Mat4 transpose() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[j][i];
        return r;
    }

    Mat4 inverse() const {
        float inv[16];
        const float* a = &m[0][0];

        inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
        inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
        inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
        inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];

        inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
        inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
        inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
        inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];

        inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
        inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
        inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
        inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];

        inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
        inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
        inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
        inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];

        float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
        if (std::fabs(det) < 1e-12f) return identity();
        float invDet = 1.0f / det;
        Mat4 r;
        float* out = &r.m[0][0];
        for (int i = 0; i < 16; ++i) out[i] = inv[i] * invDet;
        return r;
    }
};

} // namespace math
