#include "skywalker/render/LightClusters.h"

#include <algorithm>
#include <cmath>

namespace sky {

int LightGrid::clusterAt(float px, float py, int width, int height, float z) const {
    int tx = std::clamp(static_cast<int>(px / std::max(width, 1) * tilesX), 0, tilesX - 1);
    int ty = std::clamp(static_cast<int>(py / std::max(height, 1) * tilesY), 0, tilesY - 1);
    float zc = std::clamp(z, zNear, zFar);
    int s = std::clamp(static_cast<int>(std::log(zc / zNear) / std::log(zFar / zNear) * slices), 0, slices - 1);
    return (s * tilesY + ty) * tilesX + tx;
}

LightGrid buildLightGrid(const FrameData& f, int maxPerCluster) {
    LightGrid g;
    const ViewCamera& cam = f.camera;
    g.zNear = std::max(cam.nearPlane, 0.05f);
    g.zFar = std::max(std::min(cam.farPlane, 2000.f), g.zNear * 2.f);
    const size_t n = g.clusterCount();
    std::vector<std::vector<uint32_t>> lists(n);
    for (size_t i = 0; i < f.lights.size(); ++i) {
        const LightItem& l = f.lights[i];
        if (l.kind == LightItem::Kind::Directional) {
            if (i == g.directionalCount) ++g.directionalCount;  // directional lights come first
            continue;
        }
        ClusterRange r;
        if (!sphereClusters(g, f, l.position, l.range, r)) continue;
        for (int s = r.s0; s <= r.s1; ++s) {
            for (int ty = r.ty0; ty <= r.ty1; ++ty) {
                for (int tx = r.tx0; tx <= r.tx1; ++tx) {
                    auto& list = lists[static_cast<size_t>((s * g.tilesY + ty) * g.tilesX + tx)];
                    if (static_cast<int>(list.size()) < maxPerCluster) list.push_back(static_cast<uint32_t>(i));
                }
            }
        }
    }
    g.cells.resize(n * 2);
    for (size_t k = 0; k < n; ++k) {
        g.cells[k * 2] = static_cast<uint32_t>(g.indices.size());
        g.cells[k * 2 + 1] = static_cast<uint32_t>(lists[k].size());
        g.indices.insert(g.indices.end(), lists[k].begin(), lists[k].end());
    }
    if (g.indices.empty()) g.indices.push_back(0);  // GPU buffers must not be empty
    return g;
}

bool sphereClusters(const LightGrid& g, const FrameData& f, Vec3 center, float radius, ClusterRange& out) {
    const ViewCamera& cam = f.camera;
    const float logRange = std::log(g.zFar / g.zNear);
    auto sliceOf = [&](float z) {
        return std::clamp(static_cast<int>(std::log(std::clamp(z, g.zNear, g.zFar) / g.zNear) / logRange * g.slices), 0, g.slices - 1);
    };
    // View space: camera looks down -Z.
    Vec3 c = f.view.transformPoint(center);
    float depth = -c.z, r = radius;
    if (depth + r < g.zNear || depth - r > g.zFar) return false;
    out = ClusterRange{sliceOf(depth - r), sliceOf(depth + r), 0, g.tilesX - 1, 0, g.tilesY - 1};
    if (depth - r > g.zNear && !cam.orthographic) {
        // Screen bounds of the sphere's view-space box (conservative).
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (int k = 0; k < 8; ++k) {
            Vec3 p{c.x + ((k & 1) ? r : -r), c.y + ((k & 2) ? r : -r), c.z + ((k & 4) ? r : -r)};
            Vec4 h = f.projection * Vec4(p, 1.f);
            if (h.w <= 1e-4f) {
                minx = miny = -1e9f;
                maxx = maxy = 1e9f;
                break;
            }
            minx = std::min(minx, h.x / h.w), maxx = std::max(maxx, h.x / h.w);
            miny = std::min(miny, h.y / h.w), maxy = std::max(maxy, h.y / h.w);
        }
        if (maxx < -1.f || minx > 1.f || maxy < -1.f || miny > 1.f) return false;
        // NDC -> tiles (y flipped: tile 0 is the top row).
        out.tx0 = std::clamp(static_cast<int>((minx * 0.5f + 0.5f) * g.tilesX), 0, g.tilesX - 1);
        out.tx1 = std::clamp(static_cast<int>((maxx * 0.5f + 0.5f) * g.tilesX), 0, g.tilesX - 1);
        out.ty0 = std::clamp(static_cast<int>((0.5f - maxy * 0.5f) * g.tilesY), 0, g.tilesY - 1);
        out.ty1 = std::clamp(static_cast<int>((0.5f - miny * 0.5f) * g.tilesY), 0, g.tilesY - 1);
    }
    return true;
}

}  // namespace sky
