#include "skywalker/engine/Gizmo.h"

#include <cmath>

namespace sky {

namespace {

const Vec4 kAxisColors[3] = {{0.94f, 0.30f, 0.33f, 1}, {0.45f, 0.82f, 0.30f, 1}, {0.28f, 0.55f, 0.98f, 1}};
const Vec4 kHotColor{1.0f, 0.82f, 0.25f, 1};

/// Basis whose Y column is `dir` (for local-space gizmos).
Mat4 basisFromY(Vec3 dir) {
    Vec3 y = normalize(dir);
    Vec3 helper = std::fabs(y.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 x = normalize(cross(helper, y));
    Vec3 z = cross(x, y);
    Mat4 m;
    m.at(0, 0) = x.x; m.at(0, 1) = x.y; m.at(0, 2) = x.z;
    m.at(1, 0) = y.x; m.at(1, 1) = y.y; m.at(1, 2) = y.z;
    m.at(2, 0) = z.x; m.at(2, 1) = z.y; m.at(2, 2) = z.z;
    return m;
}

float distanceRaySegment(const Ray& ray, Vec3 a, Vec3 b, float& rayT) {
    // Closest points between a ray and a segment (Ericson, RTCD 5.1.9).
    Vec3 d1 = ray.dir, d2 = b - a, r = ray.origin - a;
    float aa = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float c = dot(d1, r), bb = dot(d1, d2);
    float denom = aa * e - bb * bb;
    float s = denom > 1e-8f ? std::clamp((bb * f - c * e) / denom, 0.f, 1e9f) : 0.f;
    float t = (bb * s + f) / e;
    if (t < 0) {
        t = 0;
        s = std::max(-c / aa, 0.f);
    } else if (t > 1) {
        t = 1;
        s = std::max((bb - c) / aa, 0.f);
    }
    rayT = s;
    return length((ray.origin + d1 * s) - (a + d2 * t));
}

bool rayPlane(const Ray& ray, Vec3 point, Vec3 normal, Vec3& hit, float& t) {
    float denom = dot(ray.dir, normal);
    if (std::fabs(denom) < 1e-6f) return false;
    t = dot(point - ray.origin, normal) / denom;
    if (t < 0) return false;
    hit = ray.origin + ray.dir * t;
    return true;
}

}  // namespace

GizmoFrame Gizmo::frameFor(const Mat4& world, const ViewCamera& cam, bool local) {
    GizmoFrame f;
    f.center = world.translation();
    for (int i = 0; i < 3; ++i) {
        Vec3 unit{i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f};
        f.axes[i] = local ? normalize(world.transformDir(unit)) : unit;
        if (length(f.axes[i]) < 0.5f) f.axes[i] = unit;  // zero scale fallback
    }
    float dist = cam.orthographic ? cam.orthoSize * 2.f : distance(cam.eye, f.center);
    f.size = std::max(0.05f, dist * 0.16f);
    return f;
}

float Gizmo::closestParam(Vec3 origin, Vec3 dir, const Ray& ray) {
    // Parameter u on line origin + dir*u closest to the ray.
    Vec3 w = origin - ray.origin;
    float b = dot(dir, ray.dir), d = dot(dir, w), e = dot(ray.dir, w);
    float denom = 1.f - b * b;
    if (std::fabs(denom) < 1e-6f) return 0.f;  // parallel: no well-defined parameter
    return (b * e - d) / denom;
}

int Gizmo::hitTest(const GizmoFrame& f, const Ray& ray) const {
    if (mode == GizmoMode::None) return -1;
    int best = -1;
    float bestT = 1e30f;
    const float tolerance = f.size * 0.09f;
    for (int i = 0; i < 3; ++i) {
        if (mode == GizmoMode::Rotate) {
            Vec3 hit;
            float t;
            if (!rayPlane(ray, f.center, f.axes[i], hit, t)) continue;
            if (std::fabs(distance(hit, f.center) - f.size) < tolerance * 1.3f && t < bestT) {
                bestT = t;
                best = i;
            }
        } else {
            float t;
            float d = distanceRaySegment(ray, f.center, f.center + f.axes[i] * (f.size * 1.25f), t);
            if (d < tolerance && t < bestT) {
                bestT = t;
                best = i;
            }
        }
    }
    return best;
}

std::optional<Gizmo::DragStart> Gizmo::begin(const GizmoFrame& f, const Ray& ray, Vec3 worldPos, Vec3 rotation,
                                             Vec3 scale) const {
    int axis = hitTest(f, ray);
    if (axis < 0) return std::nullopt;
    DragStart s;
    s.axis = axis;
    s.frame = f;
    s.position = worldPos;
    s.rotation = rotation;
    s.scale = scale;
    if (mode == GizmoMode::Rotate) {
        Vec3 hit;
        float t;
        if (!rayPlane(ray, f.center, f.axes[axis], hit, t)) return std::nullopt;
        s.planeVec = normalize(hit - f.center);
    } else {
        s.param = closestParam(f.center, f.axes[axis], ray);
        if (mode == GizmoMode::Scale && std::fabs(s.param) < 1e-4f) s.param = 1e-4f;
    }
    return s;
}

Gizmo::Result Gizmo::drag(const DragStart& s, const Ray& ray, bool snapping) const {
    Result r{s.position, s.rotation, s.scale};
    const Vec3 axis = s.frame.axes[s.axis];
    switch (mode) {
        case GizmoMode::Translate: {
            float delta = closestParam(s.frame.center, axis, ray) - s.param;
            if (snapping && snap > 0) delta = std::round(delta / snap) * snap;
            r.worldPosition = s.position + axis * delta;
            break;
        }
        case GizmoMode::Rotate: {
            Vec3 hit;
            float t;
            if (!rayPlane(ray, s.frame.center, axis, hit, t)) break;
            Vec3 v = normalize(hit - s.frame.center);
            float angle = degrees(std::atan2(dot(axis, cross(s.planeVec, v)), dot(s.planeVec, v)));
            if (snapping) angle = std::round(angle / rotateSnapDeg) * rotateSnapDeg;
            // Euler components map to axes: x = pitch, y = yaw, z = roll.
            float* comp = s.axis == 0 ? &r.rotation.x : (s.axis == 1 ? &r.rotation.y : &r.rotation.z);
            *comp = std::remainder(*comp + angle, 360.f);
            break;
        }
        case GizmoMode::Scale: {
            float ratio = closestParam(s.frame.center, axis, ray) / s.param;
            if (snapping) ratio = std::round(ratio * 10.f) / 10.f;
            ratio = std::max(ratio, 0.01f);
            float* comp = s.axis == 0 ? &r.scale.x : (s.axis == 1 ? &r.scale.y : &r.scale.z);
            *comp = std::max(0.001f, *comp * ratio);
            break;
        }
        case GizmoMode::None: break;
    }
    return r;
}

std::vector<OverlayItem> Gizmo::overlays(const GizmoFrame& f, int hotAxis, int activeAxis) const {
    std::vector<OverlayItem> out;
    if (mode == GizmoMode::None) return out;
    const float s = f.size;
    const Mat4 T = Mat4::translate(f.center);
    for (int i = 0; i < 3; ++i) {
        Vec4 color = (i == activeAxis || (activeAxis < 0 && i == hotAxis)) ? kHotColor : kAxisColors[i];
        if (activeAxis >= 0 && i != activeAxis) color.w = 0.35f;
        Mat4 R = basisFromY(f.axes[i]);
        if (mode == GizmoMode::Rotate) {
            // gizmo_ring lies in XZ (normal +Y) with radius 0.5.
            out.push_back({"gizmo_ring", T * R * Mat4::scale(Vec3(s * 2.f)), color});
            continue;
        }
        out.push_back({"cylinder", T * R * Mat4::translate({0, s * 0.5f, 0}) * Mat4::scale({s * 0.025f, s, s * 0.025f}), color});
        if (mode == GizmoMode::Translate) {
            out.push_back({"cone", T * R * Mat4::translate({0, s * 1.1f, 0}) * Mat4::scale({s * 0.11f, s * 0.24f, s * 0.11f}), color});
        } else {
            out.push_back({"cube", T * R * Mat4::translate({0, s * 1.06f, 0}) * Mat4::scale(Vec3(s * 0.13f)), color});
        }
    }
    out.push_back({"sphere", T * Mat4::scale(Vec3(s * 0.09f)), {0.92f, 0.92f, 0.95f, 1}});
    return out;
}

}  // namespace sky
