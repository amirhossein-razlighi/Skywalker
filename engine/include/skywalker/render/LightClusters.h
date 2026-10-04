#pragma once
// Clustered forward lighting: the view frustum is split into a grid of tiles x depth slices
// (exponential in depth) and each cluster lists the lights whose influence reaches it, so a
// pixel only evaluates nearby lights — hundreds of lamps, neon signs and fires per frame.
// Built on the CPU (portable, tested); consumed by any GPU backend.

#include <cstdint>
#include <vector>

#include "skywalker/render/Renderer.h"

namespace sky {

struct LightGrid {
    int tilesX = 16, tilesY = 9, slices = 24;
    float zNear = 0.1f, zFar = 500.f;
    uint32_t directionalCount = 0;   // lights[0..directionalCount) affect every pixel
    std::vector<uint32_t> cells;     // per cluster: offset, count (2 x uint32)
    std::vector<uint32_t> indices;   // light indices referenced by `cells`

    size_t clusterCount() const { return static_cast<size_t>(tilesX) * tilesY * slices; }
    /// Cluster index for a pixel (top-left origin) at view depth `z` (meters along the view).
    int clusterAt(float px, float py, int width, int height, float z) const;
};

/// Lights must be ordered with directional lights first (prioritizeLights does this).
LightGrid buildLightGrid(const FrameData& frame, int maxPerCluster = 128);

/// The clusters a world-space sphere may touch (conservative): slices s0..s1, tiles tx0..tx1 x ty0..ty1.
/// False when the sphere is outside the view. Used for lights and reflection probes.
struct ClusterRange {
    int s0 = 0, s1 = 0, tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0;
};
bool sphereClusters(const LightGrid& grid, const FrameData& frame, Vec3 center, float radius, ClusterRange& out);

}  // namespace sky
