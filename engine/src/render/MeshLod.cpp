// Automatic levels of detail with meshoptimizer: attribute-aware simplification (normals and
// UVs keep shading and texturing intact), each level optimized for the GPU vertex cache.

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

#include "skywalker/render/MeshData.h"

namespace sky::mesh {

void buildLods(MeshData& m, int maxLods, size_t minTriangles) {
    m.lods.clear();
    m.lodErrors.clear();
    const size_t triangles = m.indices.size() / 3;
    const size_t vertexCount = m.vertexCount();
    if (triangles < minTriangles || vertexCount == 0) return;
    const size_t stride = sizeof(float) * MeshData::kFloatsPerVertex;
    meshopt_optimizeVertexCache(m.indices.data(), m.indices.data(), m.indices.size(), vertexCount);
    // Attributes that must survive simplification: normal (3) and uv (2), weighted.
    std::vector<float> attrs(vertexCount * 5);
    for (size_t v = 0; v < vertexCount; ++v) {
        const float* p = &m.vertices[v * MeshData::kFloatsPerVertex];
        attrs[v * 5 + 0] = p[3], attrs[v * 5 + 1] = p[4], attrs[v * 5 + 2] = p[5];
        attrs[v * 5 + 3] = p[6], attrs[v * 5 + 4] = p[7];
    }
    const float weights[5] = {0.5f, 0.5f, 0.5f, 1.f, 1.f};
    const float ratios[5] = {0.3f, 0.1f, 0.03f, 0.01f, 0.003f};
    const std::vector<uint32_t>* source = &m.indices;
    for (int level = 0; level < std::min(maxLods, 5); ++level) {
        size_t target = static_cast<size_t>(static_cast<float>(m.indices.size()) * ratios[level]) / 3 * 3;
        if (target < 3 * 64) break;
        std::vector<uint32_t> out(source->size());
        float error = 0.f;
        size_t n = meshopt_simplifyWithAttributes(out.data(), source->data(), source->size(), m.vertices.data(), vertexCount,
                                                  stride, attrs.data(), sizeof(float) * 5, weights, 5, nullptr, target, 0.2f,
                                                  meshopt_SimplifyLockBorder, &error);
        if (n == 0 || n >= source->size() * 9 / 10) break;  // no meaningful reduction left
        out.resize(n);
        meshopt_optimizeVertexCache(out.data(), out.data(), out.size(), vertexCount);
        m.lods.push_back(std::move(out));
        m.lodErrors.push_back((m.lodErrors.empty() ? 0.f : m.lodErrors.back()) + error);  // errors accumulate
        source = &m.lods.back();
    }
}

}  // namespace sky::mesh
