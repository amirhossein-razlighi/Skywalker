#pragma once
// Software overlay drawing for debug captures (collider wireframes, navmesh, paths): projects
// world-space geometry with a ViewCamera and rasterizes it onto a rendered Image. Renderer
// independent, so it works the same with Metal and the headless null renderer.

#include <cstdint>
#include <vector>

#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"
#include "skywalker/render/Renderer.h"

namespace sky::debugdraw {

struct Rgba {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

class Canvas {
public:
    Canvas(Image& image, const ViewCamera& camera);

    /// World-space segments as pairs (a0, b0, a1, b1, ...), clipped at the near plane.
    void lines(const std::vector<Vec3>& segments, Rgba color, int thickness = 1);
    void line(Vec3 a, Vec3 b, Rgba color, int thickness = 1);
    /// Filled convex polygon (world space), alpha blended.
    void polygon(const std::vector<Vec3>& points, Rgba color);
    /// Filled screen-space disc around a world point (agents, path points).
    void dot(Vec3 p, float radiusPixels, Rgba color);
    /// Projects to pixels; false when behind the camera.
    bool project(Vec3 p, float& x, float& y) const;

private:
    void blend(int x, int y, Rgba c);
    void line2d(float x0, float y0, float x1, float y1, Rgba c, int thickness);
    Vec4 clip(Vec3 p) const;

    Image& image_;
    Mat4 viewProj_;
};

/// A top-down orthographic camera framing `bounds` (x/z) in an image of the given aspect
/// (width / height), for maps of levels and navmeshes. +X is right, -Z is up.
ViewCamera topDown(const Aabb& bounds, float aspect = 1.f, float margin = 1.08f);

}  // namespace sky::debugdraw
