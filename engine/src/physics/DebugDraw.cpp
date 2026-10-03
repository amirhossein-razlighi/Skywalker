#include "skywalker/physics/DebugDraw.h"

#include <algorithm>
#include <cmath>

namespace sky::debugdraw {

namespace {
constexpr float kNearW = 1e-3f;
}

Canvas::Canvas(Image& image, const ViewCamera& camera) : image_(image) {
    float aspect = image.height > 0 ? static_cast<float>(image.width) / static_cast<float>(image.height) : 1.f;
    viewProj_ = camera.projection(aspect) * camera.view();
}

Vec4 Canvas::clip(Vec3 p) const { return viewProj_ * Vec4(p, 1.f); }

bool Canvas::project(Vec3 p, float& x, float& y) const {
    Vec4 c = clip(p);
    if (c.w <= kNearW) return false;
    x = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(image_.width);
    y = (1.f - (c.y / c.w * 0.5f + 0.5f)) * static_cast<float>(image_.height);
    return true;
}

void Canvas::blend(int x, int y, Rgba c) {
    if (x < 0 || y < 0 || x >= image_.width || y >= image_.height) return;
    uint8_t* px = image_.at(x, y);
    unsigned a = c.a;
    px[0] = static_cast<uint8_t>((c.r * a + px[0] * (255 - a)) / 255);
    px[1] = static_cast<uint8_t>((c.g * a + px[1] * (255 - a)) / 255);
    px[2] = static_cast<uint8_t>((c.b * a + px[2] * (255 - a)) / 255);
    px[3] = 255;
}

void Canvas::line2d(float x0, float y0, float x1, float y1, Rgba c, int thickness) {
    // Reject lines far outside the image (projection blow-ups near the camera).
    const float limit = 1e5f;
    if (std::fabs(x0) > limit || std::fabs(y0) > limit || std::fabs(x1) > limit || std::fabs(y1) > limit) return;
    float dx = x1 - x0, dy = y1 - y0;
    int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    steps = std::clamp(steps, 1, 8192);
    int half = thickness / 2;
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        int x = static_cast<int>(std::lround(x0 + dx * t)), y = static_cast<int>(std::lround(y0 + dy * t));
        for (int oy = -half; oy <= half; ++oy) {
            for (int ox = -half; ox <= half; ++ox) blend(x + ox, y + oy, c);
        }
    }
}

void Canvas::line(Vec3 a, Vec3 b, Rgba color, int thickness) {
    Vec4 ca = clip(a), cb = clip(b);
    if (ca.w <= kNearW && cb.w <= kNearW) return;
    // Clip against the plane w = kNearW so segments crossing behind the camera stay correct.
    if (ca.w <= kNearW || cb.w <= kNearW) {
        float t = (kNearW - ca.w) / (cb.w - ca.w);
        Vec4 m = ca + (cb + ca * -1.f) * t;
        if (ca.w <= kNearW) ca = m;
        else cb = m;
    }
    auto toPx = [&](Vec4 c, float& x, float& y) {
        x = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(image_.width);
        y = (1.f - (c.y / c.w * 0.5f + 0.5f)) * static_cast<float>(image_.height);
    };
    float x0, y0, x1, y1;
    toPx(ca, x0, y0);
    toPx(cb, x1, y1);
    line2d(x0, y0, x1, y1, color, thickness);
}

void Canvas::lines(const std::vector<Vec3>& segments, Rgba color, int thickness) {
    for (size_t i = 0; i + 1 < segments.size(); i += 2) line(segments[i], segments[i + 1], color, thickness);
}

void Canvas::polygon(const std::vector<Vec3>& points, Rgba color) {
    if (points.size() < 3) return;
    std::vector<float> xs, ys;
    for (const Vec3& p : points) {
        float x, y;
        if (!project(p, x, y)) return;
        xs.push_back(x);
        ys.push_back(y);
    }
    int y0 = std::max(0, static_cast<int>(std::floor(*std::min_element(ys.begin(), ys.end()))));
    int y1 = std::min(image_.height - 1, static_cast<int>(std::ceil(*std::max_element(ys.begin(), ys.end()))));
    const size_t n = xs.size();
    std::vector<float> hits;
    for (int y = y0; y <= y1; ++y) {
        float sy = static_cast<float>(y) + 0.5f;
        hits.clear();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            float ya = ys[i], yb = ys[j];
            if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy)) hits.push_back(xs[i] + (sy - ya) / (yb - ya) * (xs[j] - xs[i]));
        }
        std::sort(hits.begin(), hits.end());
        for (size_t k = 0; k + 1 < hits.size(); k += 2) {
            int xa = std::max(0, static_cast<int>(std::ceil(hits[k] - 0.5f)));
            int xb = std::min(image_.width - 1, static_cast<int>(std::floor(hits[k + 1] - 0.5f)));
            for (int x = xa; x <= xb; ++x) blend(x, y, color);
        }
    }
}

void Canvas::dot(Vec3 p, float r, Rgba color) {
    float cx, cy;
    if (!project(p, cx, cy)) return;
    int ir = static_cast<int>(std::ceil(r));
    for (int y = -ir; y <= ir; ++y) {
        for (int x = -ir; x <= ir; ++x) {
            if (static_cast<float>(x * x + y * y) <= r * r) {
                blend(static_cast<int>(std::lround(cx)) + x, static_cast<int>(std::lround(cy)) + y, color);
            }
        }
    }
}

ViewCamera topDown(const Aabb& b, float aspect, float margin) {
    ViewCamera v;
    Vec3 c = b.center();
    aspect = std::max(aspect, 0.05f);
    float half = std::max({(b.max.x - b.min.x) / aspect, b.max.z - b.min.z, 1.f}) * 0.5f * margin;
    v.orthographic = true;
    v.orthoSize = half;
    v.target = c;
    float height = std::max(b.max.y - b.min.y, 1.f);
    v.eye = c + Vec3{0.f, height + half + 10.f, 0.f};
    v.up = {0.f, 0.f, -1.f};
    v.nearPlane = 0.05f;
    v.farPlane = (height + half + 10.f) * 2.f + 10.f;
    return v;
}

}  // namespace sky::debugdraw
