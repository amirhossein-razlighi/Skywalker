#pragma once
// CPU-side mesh data: procedural primitives and mesh file import (OBJ + MTL, PLY, STL;
// glTF lives in Gltf.h). Backend-independent so it can be unit tested.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky {

struct MeshData {
    static constexpr int kFloatsPerVertex = 12;  // position(3) normal(3) uv(2) color(4)
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    Aabb bounds;
    bool hasVertexColors = false;
    /// Coarser levels of detail (indices into the same vertices), from mesh::buildLods. LOD 0
    /// is `indices`. `lodErrors[i]` is the simplification error of lods[i] relative to the
    /// mesh's extent (e.g. 0.01 = 1% of its size).
    std::vector<std::vector<uint32_t>> lods;
    std::vector<float> lodErrors;

    size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
    void addVertex(Vec3 p, Vec3 n, Vec2 uv, Vec4 color = {1, 1, 1, 1});
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
/// Builds up to `maxLods` coarser LODs (meshoptimizer, attribute-aware) and optimizes every
/// level for the vertex cache. No-op for small meshes (< `minTriangles`).
void buildLods(MeshData& m, int maxLods = 4, size_t minTriangles = 3000);

// Vegetation and ground detail (Vegetation.cpp), standing on y = 0.
MeshData grass();
MeshData grassTall();
MeshData fern();
MeshData flowers();
MeshData pebbles();
MeshData shell();
MeshData rock();

/// Parses Wavefront OBJ (v/vt/vn/f, polygons triangulated, negative indices supported).
/// If `normalize` is true the mesh is recentred and scaled to fit a unit cube.
Result<MeshData> parseObj(const std::string& text, bool normalize = true);
Result<MeshData> loadObj(const std::string& path, bool normalize = true);

/// Fills zero-length normals with smooth normals accumulated from faces.
void computeMissingNormals(MeshData& m);
/// Recentres and scales to fit a unit cube.
void normalizeToUnit(MeshData& m);
/// Same fit, computed from `reference` (e.g. the whole model when `m` is one of its parts).
void normalizeToUnit(MeshData& m, const Aabb& reference);
/// Bounds after zUpToYUp.
Aabb zUpToYUp(const Aabb& b);
/// Material found next to an imported mesh (OBJ .mtl). Texture paths are absolute.
struct ImportedMaterial {
    bool present = false;
    Vec4 color{1, 1, 1, 1};
    float roughness = 0.6f;
    float metallic = 0.f;
    Vec4 emissive{0, 0, 0, 1};
    std::string texture, normalMap;
};

struct MeshImport {
    MeshData mesh;
    ImportedMaterial material;
};

/// OBJ with its .mtl (first material: Kd/Ks/Ns/Ke/d/map_Kd/map_Bump) and `v x y z r g b`
/// vertex colors.
Result<MeshImport> loadObjWithMaterial(const std::string& path, bool normalize = true);
/// Stanford PLY: ascii / binary little / big endian; positions, normals, UVs, vertex colors,
/// polygon faces (triangulated). Point clouds without faces are rejected.
Result<MeshData> parsePly(const std::vector<uint8_t>& bytes, bool normalize = true);
/// STL: ascii or binary, flat-shaded facets.
Result<MeshData> parseStl(const std::vector<uint8_t>& bytes, bool normalize = true);
/// Rotates a Z-up mesh (CAD, scans, Blender exports without conversion) to Y-up.
void zUpToYUp(MeshData& m);

struct LoadOptions {
    bool normalize = true;
    bool zUp = false;
    int part = -2;  // glTF: one material's triangles (see Gltf.h); -2 = the whole model
};
/// "model.gltf#3" -> {"model.gltf", 3}; no fragment -> part -2.
std::pair<std::string, int> splitPart(const std::string& ref);
/// Loads .obj / .ply / .stl / .glb / .gltf by extension (geometry only; see Gltf.h and
/// loadObjWithMaterial for materials).
Result<MeshData> loadMeshFile(const std::string& path, bool normalize = true);
Result<MeshData> loadMeshFile(const std::string& path, const LoadOptions& options);
/// Extensions loadMeshFile understands.
bool isMeshFile(const std::string& path);

}  // namespace mesh
}  // namespace sky
