#pragma once
// Renderer-facing descriptions of GPU-simulated effects: GPU particle emitters and hair
// grooms. Built on the CPU each frame (fx::ParticleSystem::gatherGpu, fx::GroomSystem::gather)
// and consumed by backends that support them (Metal); other backends ignore them.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/ecs/Components.h"
#include "skywalker/math/Math.h"

namespace sky {

using EntityId = uint64_t;

/// Simple collision proxy (world space).
struct FxCollider {
    enum class Kind : uint8_t { Sphere = 0, Capsule = 1, Plane = 2 } kind = Kind::Sphere;
    Vec3 a{0, 0, 0};       // sphere center / capsule start / point on plane
    Vec3 b{0, 0, 0};       // capsule end / plane normal
    float radius = 0.5f;   // sphere / capsule radius
};

/// Triangles of a mesh with an area CDF, for emitting particles from a surface.
struct MeshSurface {
    std::vector<Vec3> positions;  // 3 per triangle (mesh local space)
    std::vector<Vec3> normals;    // 3 per triangle
    std::vector<float> cdf;       // cumulative area, normalized to 1 (one per triangle)
    float area = 0.f;
    size_t triangles() const { return cdf.size(); }
};

/// A 3D vector field (velocities), e.g. from an .fga file.
struct VectorField {
    int nx = 0, ny = 0, nz = 0;
    Vec3 boundsMin{-1, -1, -1}, boundsMax{1, 1, 1};  // as authored (file units)
    std::vector<float> data;                         // nx*ny*nz*4 (xyz, 0), x fastest
    uint64_t version = 0;
};

/// Color (rgb linear + opacity) and size multipliers over life, sampled at 32 points.
struct GpuCurves {
    static constexpr int kSamples = 32;
    std::array<Vec4, kSamples> color{};
    std::array<float, kSamples> size{};
};

/// A particles component with simulation "gpu", as of this frame.
struct GpuEmitterItem {
    EntityId entity = 0;
    Mat4 world;
    ParticleEmitter params;
    uint64_t burstSerial = 0;      // cumulative particles requested by bursts (the GPU emits the difference)
    EntityId subEmitter = 0;       // resolved `subEmitter`
    bool isSubEmitter = false;     // another emitter spawns into this one
    Vec3 wind{0, 0, 0};            // environment wind (m/s, world)
    std::string particleMesh;      // facing "mesh": mesh key
    std::string texture;           // sprite / flipbook (absolute path)
    std::shared_ptr<const MeshSurface> surface;  // shape "mesh"
    std::shared_ptr<const VectorField> field;    // field "texture"
    std::vector<FxCollider> colliders;
    GpuCurves curves;
};

/// Generated hair: guide strands (simulated) and children (rendered), all with the same
/// number of points. Children are reconstructed on the GPU every frame:
///   base(u)  = root + sum_i w_i * (guide_i(u) - guide_i(0))
///   point_k  = base(u_k) + Frame_k * offset_k
/// where u_k = k / (P - 1) * lengthScale and Frame_k is the parallel-transport frame of
/// the base curve (see fx::reconstructStrands for the CPU reference).
struct GroomData {
    struct Child {
        Vec3 root;               // mesh local space
        float lengthScale = 1;   // fraction of the guide curve this strand spans
        uint32_t guide[3] = {0, 0, 0};
        float weight[3] = {1, 0, 0};
        float random = 0;        // 0..1: color / width variation
        float width = 1;         // width multiplier
    };
    uint32_t points = 0;                // points per strand (P)
    std::vector<Vec3> guideRest;        // guides * P, mesh local space
    std::vector<Child> children;
    std::vector<Vec3> offsets;          // children * P (tangent, normal, binormal components)
    Aabb bounds;                        // rest bounds (local)
    float spacing = 0.01f;              // average distance between guide roots (cards width)
    FxCollider proxy;                   // collision proxy of the scalp mesh (local space)
    bool hasProxy = false;
    uint64_t hash = 0;                  // generation parameters hash (changes => re-upload)

    size_t guideCount() const { return points ? guideRest.size() / points : 0; }
    size_t strandCount() const { return children.size(); }
    size_t memoryBytes() const;
};

/// A groom to simulate and draw this frame.
struct GroomItem {
    EntityId entity = 0;
    Mat4 model;                                  // the hair's mesh-local -> world transform
    std::shared_ptr<const GroomData> data;
    Groom params;
    std::vector<FxCollider> colliders;           // world space (proxy + named colliders)
    Vec3 wind{0, 0, 0};                          // environment wind (m/s, world)
};

}  // namespace sky
