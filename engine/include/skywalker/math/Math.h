#pragma once
// Minimal, header-only math library. Conventions:
//   * Right-handed world, +Y up, cameras look down -Z.
//   * Mat4 is column-major (matches Metal's float4x4 / simd layout), m[col*4+row].
//   * Clip-space depth is [0, 1] (Metal / D3D / WebGPU convention).
//   * Rotations exposed to users and agents are Euler angles in DEGREES, applied
//     in Y (yaw) -> X (pitch) -> Z (roll) order. Degrees are far less error-prone
//     for language models than quaternions or radians.

#include <algorithm>
#include <cmath>

namespace sky {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * (kPi / 180.f); }
inline float degrees(float rad) { return rad * (180.f / kPi); }

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    bool operator==(const Vec2&) const = default;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(Vec3 o) const { return {x / o.x, y / o.y, z / o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    bool operator==(const Vec3&) const = default;
};

inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline float distance(Vec3 a, Vec3 b) { return length(a - b); }
inline Vec3 normalize(Vec3 v) {
    float len = length(v);
    return len > 1e-8f ? v / len : Vec3{0, 0, 0};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline Vec3 vmin(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec3 xyz() const { return {x, y, z}; }
    Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    bool operator==(const Vec4&) const = default;
};

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return {}; }
    float& at(int col, int row) { return m[col * 4 + row]; }
    float at(int col, int row) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(k, rr) * o.at(c, k);
                r.at(c, rr) = s;
            }
        }
        return r;
    }

    Vec4 operator*(Vec4 v) const {
        return {at(0, 0) * v.x + at(1, 0) * v.y + at(2, 0) * v.z + at(3, 0) * v.w,
                at(0, 1) * v.x + at(1, 1) * v.y + at(2, 1) * v.z + at(3, 1) * v.w,
                at(0, 2) * v.x + at(1, 2) * v.y + at(2, 2) * v.z + at(3, 2) * v.w,
                at(0, 3) * v.x + at(1, 3) * v.y + at(2, 3) * v.z + at(3, 3) * v.w};
    }

    Vec3 transformPoint(Vec3 p) const {
        Vec4 r = (*this) * Vec4(p, 1.f);
        return r.w != 0.f ? Vec3{r.x / r.w, r.y / r.w, r.z / r.w} : r.xyz();
    }
    Vec3 transformDir(Vec3 d) const { return ((*this) * Vec4(d, 0.f)).xyz(); }
    Vec3 translation() const { return {at(3, 0), at(3, 1), at(3, 2)}; }

    static Mat4 translate(Vec3 t) {
        Mat4 r;
        r.at(3, 0) = t.x;
        r.at(3, 1) = t.y;
        r.at(3, 2) = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r;
        r.at(0, 0) = s.x;
        r.at(1, 1) = s.y;
        r.at(2, 2) = s.z;
        return r;
    }

    static Mat4 rotateX(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(1, 1) = c; r.at(2, 1) = -s;
        r.at(1, 2) = s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateY(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(2, 0) = s;
        r.at(0, 2) = -s; r.at(2, 2) = c;
        return r;
    }
    static Mat4 rotateZ(float rad) {
        Mat4 r;
        float c = std::cos(rad), s = std::sin(rad);
        r.at(0, 0) = c; r.at(1, 0) = -s;
        r.at(0, 1) = s; r.at(1, 1) = c;
        return r;
    }

    /// Euler degrees, yaw(Y) * pitch(X) * roll(Z).
    static Mat4 rotateEulerDeg(Vec3 deg) {
        return rotateY(radians(deg.y)) * rotateX(radians(deg.x)) * rotateZ(radians(deg.z));
    }

    static Mat4 trs(Vec3 t, Vec3 eulerDeg, Vec3 s) { return translate(t) * rotateEulerDeg(eulerDeg) * scale(s); }

    /// Rotation from a unit quaternion (x, y, z, w).
    static Mat4 fromQuat(Vec4 q) {
        float x = q.x, y = q.y, z = q.z, w = q.w;
        Mat4 r;
        r.at(0, 0) = 1 - 2 * (y * y + z * z); r.at(1, 0) = 2 * (x * y - z * w);     r.at(2, 0) = 2 * (x * z + y * w);
        r.at(0, 1) = 2 * (x * y + z * w);     r.at(1, 1) = 1 - 2 * (x * x + z * z); r.at(2, 1) = 2 * (y * z - x * w);
        r.at(0, 2) = 2 * (x * z - y * w);     r.at(1, 2) = 2 * (y * z + x * w);     r.at(2, 2) = 1 - 2 * (x * x + y * y);
        return r;
    }

    /// Right-handed perspective, depth [0,1].
    static Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
        Mat4 r;
        float ys = 1.f / std::tan(fovYRad * 0.5f);
        float xs = ys / aspect;
        float zs = zFar / (zNear - zFar);
        r.m[0] = xs;
        r.m[5] = ys;
        r.m[10] = zs;
        r.m[11] = -1.f;
        r.m[14] = zs * zNear;
        r.m[15] = 0.f;
        return r;
    }

    /// Right-handed orthographic, depth [0,1].
    static Mat4 orthographic(float halfHeight, float aspect, float zNear, float zFar) {
        Mat4 r;
        float halfWidth = halfHeight * aspect;
        r.m[0] = 1.f / halfWidth;
        r.m[5] = 1.f / halfHeight;
        r.m[10] = 1.f / (zNear - zFar);
        r.m[14] = zNear / (zNear - zFar);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = normalize(target - eye);
        if (length(cross(f, up)) < 1e-5f) up = std::fabs(f.y) > 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 0) = s.y; r.at(2, 0) = s.z;
        r.at(0, 1) = u.x; r.at(1, 1) = u.y; r.at(2, 1) = u.z;
        r.at(0, 2) = -f.x; r.at(1, 2) = -f.y; r.at(2, 2) = -f.z;
        r.at(3, 0) = -dot(s, eye);
        r.at(3, 1) = -dot(u, eye);
        r.at(3, 2) = dot(f, eye);
        return r;
    }

    Mat4 transposed() const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) r.at(c, rr) = at(rr, c);
        return r;
    }

    /// General 4x4 inverse (cofactor expansion). Returns identity if singular.
    Mat4 inverse() const {
        const float* a = m;
        float inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
                 a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
                 a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
                 a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
                  a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
                 a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
                 a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
                 a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
                  a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
                 a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
                 a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
                  a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
                  a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
                 a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
                 a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
                  a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
                  a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (std::fabs(det) < 1e-12f) return {};
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
        return r;
    }
};

struct Ray {
    Vec3 origin;
    Vec3 dir;
};

/// Axis-aligned bounding box.
struct Aabb {
    Vec3 min{0.f};
    Vec3 max{0.f};
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extents() const { return (max - min) * 0.5f; }
    Aabb transformed(const Mat4& m) const {
        Aabb r{Vec3(1e30f), Vec3(-1e30f)};
        for (int i = 0; i < 8; ++i) {
            Vec3 c{(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z};
            Vec3 p = m.transformPoint(c);
            r.min = vmin(r.min, p);
            r.max = vmax(r.max, p);
        }
        return r;
    }
};

/// Ray/AABB slab test. Returns distance along the ray or a negative value on miss.
inline float intersect(const Ray& ray, const Aabb& box) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float o = ray.origin[i], d = ray.dir[i], lo = box.min[i], hi = box.max[i];
        if (std::fabs(d) < 1e-9f) {
            if (o < lo || o > hi) return -1.f;
            continue;
        }
        float t1 = (lo - o) / d, t2 = (hi - o) / d;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return -1.f;
    }
    if (tmax < 0) return -1.f;
    return tmin >= 0 ? tmin : tmax;
}

}  // namespace sky
