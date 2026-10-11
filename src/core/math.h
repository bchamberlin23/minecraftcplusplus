#pragma once
#include <cmath>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(Vec3 v) {
    float l = std::sqrt(dot(v, v));
    return l > 0 ? v * (1.0f / l) : v;
}

// Column-major 4x4 (matches HLSL's default packing; use mul(M, v) in shaders).
struct Mat4 {
    float m[16] = {};
    float& operator()(int r, int c) { return m[c * 4 + r]; }
    float operator()(int r, int c) const { return m[c * 4 + r]; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a(i, k) * b(k, j);
            r(i, j) = s;
        }
    return r;
}

// Infinite far plane, reversed Z (near -> 1, infinity -> 0). View space: +z forward.
inline Mat4 perspectiveReverseZ(float fovY, float aspect, float zNear) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 p;
    p(0, 0) = f / aspect;
    p(1, 1) = f;
    p(2, 3) = zNear;
    p(3, 2) = 1.0f;
    return p;
}

// Rotation-only view matrix from an orthonormal camera basis.
inline Mat4 viewFromBasis(Vec3 right, Vec3 up, Vec3 forward) {
    Mat4 v;
    v(0, 0) = right.x, v(0, 1) = right.y, v(0, 2) = right.z;
    v(1, 0) = up.x, v(1, 1) = up.y, v(1, 2) = up.z;
    v(2, 0) = forward.x, v(2, 1) = forward.y, v(2, 2) = forward.z;
    v(3, 3) = 1.0f;
    return v;
}
