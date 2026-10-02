#pragma once
// CPU-side mesh data: procedural primitives and OBJ import (the common output format
// of 3D-generation models). Backend-independent so it can be unit tested.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky {

struct MeshData {
    static constexpr int kFloatsPerVertex = 8;  // position(3) normal(3) uv(2)
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    Aabb bounds;

    size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
    void addVertex(Vec3 p, Vec3 n, Vec2 uv);
    void computeBounds();
};

namespace mesh {

/// Unit-sized primitives centred at the origin (a cube spans -0.5..0.5).
Result<MeshData> primitive(const std::string& name);
MeshData cube();
MeshData sphere(int segments = 48, int rings = 24);
MeshData plane();
MeshData quad();
MeshData cylinder(int segments = 48);
MeshData cone(int segments = 48);
MeshData capsule(int segments = 32, int rings = 8);
MeshData torus(int segments = 64, int sides = 24, float major = 0.35f, float minor = 0.15f);

/// Parses Wavefront OBJ (v/vt/vn/f, polygons triangulated, negative indices supported).
/// If `normalize` is true the mesh is recentred and scaled to fit a unit cube.
Result<MeshData> parseObj(const std::string& text, bool normalize = true);
Result<MeshData> loadObj(const std::string& path, bool normalize = true);

}  // namespace mesh
}  // namespace sky
