#pragma once
// Binding groom roots to the triangles of a (skinned) mesh.
//
// Every guide and child root of a groom grown on a mesh remembers its triangle and barycentric
// coordinates (GroomData::RootBind). On a skinned character the GPU re-evaluates the roots every frame
// on the posed vertices (GPU skinning output), and each guide's rest shape turns with the frame of its
// triangle: hair, beards, brows and fur stay on the skin through any animation. This file is the CPU
// side: binding (and rebinding imported strands to the nearest triangle), the root frame formula the
// shaders mirror, and CPU skinning of the guide roots (bounds, debug views, inspection, fallbacks).

#include <vector>

#include "skywalker/render/FxItems.h"
#include "skywalker/render/MeshData.h"

namespace sky::fx {

/// A root's position and orientation on a mesh: x = tangent (first triangle edge, made orthogonal),
/// y = the interpolated normal, z = x cross y... stored as a quaternion (x, y, z, w).
struct RootFrame {
    Vec3 position{0, 0, 0};
    Vec4 rotation{0, 0, 0, 1};
};

/// Binds points to their nearest triangles (barycentric projection, clamped into the triangle).
/// `maxError` receives the largest point-to-surface distance (0 for points on the mesh).
std::vector<GroomData::RootBind> bindToMesh(const MeshData& mesh, const std::vector<Vec3>& points, float* maxError = nullptr);

/// The bound point and frame on vertex data laid out like MeshData::vertices (12 floats per vertex),
/// positions multiplied by `scale`. Indices are clamped (never reads out of range).
RootFrame evalRoot(const float* vertices, size_t vertexCount, const GroomData::RootBind& bind, Vec3 scale);

/// Fills d.guideFrames (and meshVertices) from the rest mesh the groom grew on.
void computeRestFrames(GroomData& d, const MeshData& rest, Vec3 scale);

/// Current guide roots of a skinned groom: only the vertices the guide binds touch are skinned with
/// `palette` (mesh space; SkinStream bind positions). Result: one frame per guide (scaled mesh space).
std::vector<RootFrame> skinnedGuideRoots(const GroomData& d, const MeshData& mesh, const std::vector<Mat4>& palette, Vec3 scale);

/// Rotation taking a root's rest frame to its current one (quaternion: current * conjugate(rest)).
Vec4 rootRotation(const Vec4& current, const Vec4& rest);

/// Hamilton product helpers on (x, y, z, w) quaternions (tests, CPU reference of the shaders).
Vec4 quatMul(const Vec4& a, const Vec4& b);
Vec3 quatRotate(const Vec4& q, Vec3 v);

}  // namespace sky::fx
