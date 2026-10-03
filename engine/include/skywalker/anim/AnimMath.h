#pragma once
// Animation math: quaternions and TRS transforms (header-only).
//
// Conventions match Math.h: right-handed, +Y up, column-major Mat4. Quaternions are
// (x, y, z, w) with the Hamilton product; Euler angles use the engine's order
// (yaw Y, then pitch X, then roll Z, i.e. R = Ry * Rx * Rz) in DEGREES.

#include <algorithm>
#include <cmath>

#include "skywalker/math/Math.h"

namespace sky::anim {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;

    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return {}; }
    static Quat axisAngle(Vec3 axis, float radiansAngle) {
        Vec3 a = normalize(axis);
        float s = std::sin(radiansAngle * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(radiansAngle * 0.5f)};
    }
    /// Engine Euler degrees [pitch X, yaw Y, roll Z] applied as Ry * Rx * Rz (like Mat4::rotateEulerDeg).
    static Quat fromEulerDeg(Vec3 deg) {
        return axisAngle({0, 1, 0}, radians(deg.y)) * axisAngle({1, 0, 0}, radians(deg.x)) *
               axisAngle({0, 0, 1}, radians(deg.z));
    }
    /// Rotation part of a matrix (columns are normalized first, so scaled matrices are fine).
    static Quat fromMatrix(const Mat4& m) {
        Vec3 c0 = normalize(Vec3{m.at(0, 0), m.at(0, 1), m.at(0, 2)});
        Vec3 c1 = normalize(Vec3{m.at(1, 0), m.at(1, 1), m.at(1, 2)});
        Vec3 c2 = normalize(Vec3{m.at(2, 0), m.at(2, 1), m.at(2, 2)});
        // r[row][col]
        float r00 = c0.x, r10 = c0.y, r20 = c0.z;
        float r01 = c1.x, r11 = c1.y, r21 = c1.z;
        float r02 = c2.x, r12 = c2.y, r22 = c2.z;
        Quat q;
        float trace = r00 + r11 + r22;
        if (trace > 0.f) {
            float s = std::sqrt(trace + 1.f) * 2.f;
            q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
        } else if (r00 > r11 && r00 > r22) {
            float s = std::sqrt(1.f + r00 - r11 - r22) * 2.f;
            q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
        } else if (r11 > r22) {
            float s = std::sqrt(1.f + r11 - r00 - r22) * 2.f;
            q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
        } else {
            float s = std::sqrt(1.f + r22 - r00 - r11) * 2.f;
            q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
        }
        return q.normalized();
    }
    /// Shortest rotation taking unit vector `from` onto unit vector `to`.
    static Quat fromTo(Vec3 from, Vec3 to) {
        Vec3 f = normalize(from), t = normalize(to);
        float d = dot(f, t);
        if (d > 0.999999f) return {};
        if (d < -0.999999f) {
            Vec3 axis = cross({1, 0, 0}, f);
            if (length(axis) < 1e-4f) axis = cross({0, 1, 0}, f);
            return axisAngle(axis, kPi);
        }
        Vec3 c = cross(f, t);
        return Quat{c.x, c.y, c.z, 1.f + d}.normalized();
    }

    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
    float lengthSq() const { return x * x + y * y + z * z + w * w; }
    Quat normalized() const {
        float l = std::sqrt(lengthSq());
        return l > 1e-12f ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
    Vec3 rotate(Vec3 v) const {
        Vec3 u{x, y, z};
        Vec3 t = cross(u, v) * 2.f;
        return v + t * w + cross(u, t);
    }
    Mat4 matrix() const { return Mat4::fromQuat({x, y, z, w}); }
    /// Rotation angle in radians (0..pi).
    float angle() const { return 2.f * std::acos(std::clamp(std::fabs(w), 0.f, 1.f)); }
    Vec4 toVec4() const { return {x, y, z, w}; }
    bool operator==(const Quat&) const = default;
};

inline float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/// Spherical interpolation along the shortest arc.
inline Quat slerp(Quat a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0.f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {  // nearly parallel: normalized lerp is exact enough and stable
        return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
    }
    float theta = std::acos(std::clamp(d, -1.f, 1.f));
    float s = std::sin(theta);
    float wa = std::sin((1.f - t) * theta) / s, wb = std::sin(t * theta) / s;
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}

/// Engine Euler degrees [pitch, yaw, roll] of a rotation matrix (R = Ry * Rx * Rz).
inline Vec3 eulerDegFromMatrix(const Mat4& m) {
    Vec3 c0 = normalize(Vec3{m.at(0, 0), m.at(0, 1), m.at(0, 2)});
    Vec3 c1 = normalize(Vec3{m.at(1, 0), m.at(1, 1), m.at(1, 2)});
    Vec3 c2 = normalize(Vec3{m.at(2, 0), m.at(2, 1), m.at(2, 2)});
    // r[row][col]: column vectors c0..c2.
    float r12 = c2.y, r02 = c2.x, r22 = c2.z, r10 = c0.y, r11 = c1.y, r00 = c0.x, r20 = c0.z;
    float sx = std::clamp(-r12, -1.f, 1.f);
    float pitch = std::asin(sx), yaw, roll;
    if (std::fabs(sx) < 0.9999f) {
        yaw = std::atan2(r02, r22);
        roll = std::atan2(r10, r11);
    } else {  // gimbal lock: fold roll into yaw
        yaw = std::atan2(-r20, r00);
        roll = 0.f;
    }
    return {degrees(pitch), degrees(yaw), degrees(roll)};
}

inline Vec3 eulerDegFromQuat(const Quat& q) { return eulerDegFromMatrix(q.matrix()); }

/// Translation / rotation / scale: a bone's local transform.
struct Trs {
    Vec3 t{0.f};
    Quat r;
    Vec3 s{1.f};

    Mat4 matrix() const { return Mat4::translate(t) * r.matrix() * Mat4::scale(s); }
    /// Decomposes an affine matrix (negative determinant flips the X scale).
    static Trs fromMatrix(const Mat4& m) {
        Trs out;
        out.t = m.translation();
        Vec3 c0{m.at(0, 0), m.at(0, 1), m.at(0, 2)}, c1{m.at(1, 0), m.at(1, 1), m.at(1, 2)}, c2{m.at(2, 0), m.at(2, 1), m.at(2, 2)};
        out.s = {length(c0), length(c1), length(c2)};
        if (dot(cross(c0, c1), c2) < 0.f) {
            out.s.x = -out.s.x;
            c0 = -c0;
        }
        Mat4 rot;
        auto put = [&](int col, Vec3 v, float len) {
            float inv = len > 1e-12f ? 1.f / len : 0.f;
            rot.at(col, 0) = v.x * inv;
            rot.at(col, 1) = v.y * inv;
            rot.at(col, 2) = v.z * inv;
        };
        put(0, c0, std::fabs(out.s.x));
        put(1, c1, out.s.y);
        put(2, c2, out.s.z);
        out.r = Quat::fromMatrix(rot);
        return out;
    }
};

inline Trs lerp(const Trs& a, const Trs& b, float t) { return {lerp(a.t, b.t, t), slerp(a.r, b.r, t), lerp(a.s, b.s, t)}; }

/// Rotation (no scale) of an affine matrix as a quaternion.
inline Quat rotationOf(const Mat4& m) { return Quat::fromMatrix(m); }

}  // namespace sky::anim
