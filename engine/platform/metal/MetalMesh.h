#pragma once
// A mesh uploaded to the GPU (shared by MetalRenderer and MetalFoliage). Objective-C++ only.

#import <Metal/Metal.h>

#include <cstdint>

namespace sky {

struct GpuMesh {
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;  // LOD 0 first, then each coarser level
    uint32_t indexCount = 0;
    static constexpr int kMaxLods = 6;
    int lodCount = 1;
    uint32_t lodOffset[kMaxLods] = {};  // in indices
    uint32_t lodCount_[kMaxLods] = {};
    float lodError[kMaxLods] = {};      // relative to the mesh extent
    float radius = 1.f;                 // bounding-sphere radius (local units)

    /// The coarsest level whose simplification error stays under ~1 pixel at `pixelsPerUnit`.
    int lodFor(float pixelsPerUnit) const {
        int best = 0;
        for (int i = 1; i < lodCount; ++i) {
            if (lodError[i] * radius * 2.f * pixelsPerUnit < 0.8f) best = i;
        }
        return best;
    }
    id<MTLBuffer> skin;  // animation: SkinGpuVertex stream of rigged meshes (nil if static)
    uint32_t vertexCount = 0;
};

}  // namespace sky
