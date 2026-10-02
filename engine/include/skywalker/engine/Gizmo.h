#pragma once
// Transform gizmo: move / rotate / scale handles for the selected entity.
//
// Pure math + state, independent of any backend: the Engine turns `overlays()` into
// unlit overlay draws, and routes viewport mouse events to hitTest/begin/drag/end.
// Handles keep a constant on-screen size (scaled by distance to the camera).

#include <optional>
#include <vector>

#include "skywalker/math/Math.h"
#include "skywalker/render/Renderer.h"

namespace sky {

enum class GizmoMode { None = 0, Translate = 1, Rotate = 2, Scale = 3 };

struct GizmoFrame {
    Vec3 center;
    Vec3 axes[3];  // unit axes (world or local)
    float size;    // world-space handle length
};

class Gizmo {
public:
    GizmoMode mode = GizmoMode::Translate;
    bool local = false;
    float snap = 0;  // translate: meters, rotate: degrees*15/1 (uses rotateSnap), scale: step
    float rotateSnapDeg = 15.f;

    static GizmoFrame frameFor(const Mat4& world, const ViewCamera& cam, bool local);

    /// Which axis (0..2) is under the ray, or -1.
    int hitTest(const GizmoFrame& f, const Ray& ray) const;

    struct DragStart {
        int axis = -1;
        GizmoFrame frame;
        Vec3 position;  // world position at start
        Vec3 rotation;  // local euler at start
        Vec3 scale;     // local scale at start
        float param = 0;  // axis parameter / angle reference
        Vec3 planeVec;    // rotate: start vector in the ring plane
    };

    std::optional<DragStart> begin(const GizmoFrame& f, const Ray& ray, Vec3 worldPos, Vec3 rotation, Vec3 scale) const;

    struct Result {
        Vec3 worldPosition;
        Vec3 rotation;
        Vec3 scale;
    };
    Result drag(const DragStart& s, const Ray& ray, bool snapping) const;

    /// Overlay geometry for the renderer (unlit, drawn on top).
    std::vector<OverlayItem> overlays(const GizmoFrame& f, int hotAxis, int activeAxis) const;

    /// Closest-point parameter along the line (origin, dir) to the ray.
    static float closestParam(Vec3 origin, Vec3 dir, const Ray& ray);
};

}  // namespace sky
