#pragma once
// Builds Jolt collision shapes from Skywalker colliders and meshes. Expensive shapes (convex
// hulls, triangle meshes, height fields) are cached by content so rebuilding a body, opening
// a query world or settling props never re-cooks unchanged geometry.

#include "JoltCommon.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/ecs/Components.h"
#include "skywalker/physics/PhysicsWorld.h"

namespace sky::physics {

/// Content-keyed cache of cooked shapes and surface materials (shared by every world of an engine).
class ShapeCache {
public:
    JPH::RefConst<JPH::Shape> find(const std::string& key) const;
    void put(const std::string& key, JPH::RefConst<JPH::Shape> shape);
    const SurfaceMaterial* material(float friction, float restitution);
    void clear();
    size_t size() const { return shapes_.size(); }

private:
    std::unordered_map<std::string, JPH::RefConst<JPH::Shape>> shapes_;
    std::unordered_map<uint64_t, JPH::Ref<SurfaceMaterial>> materials_;
};

/// One collider contributing to a body: the owner's own collider or a child's.
struct ShapePart {
    EntityId entity = kNoEntity;
    const Collider* collider = nullptr;    // null = implicit "auto" collider
    const MeshRenderer* mesh = nullptr;    // the part entity's mesh, if any
    Mat4 local;                            // part entity matrix in the owner's unscaled body frame
    bool hasFallbackBounds = false;        // auto collider without a mesh: fit these local bounds
    Aabb fallbackBounds;
    float friction = 0.5f;                 // resolved (collider override or body value)
    float restitution = 0.f;
};

struct ShapeContext {
    MeshProvider meshes;
    PathResolver paths;
    ShapeCache* cache = nullptr;
    std::vector<std::string>* warnings = nullptr;
};

/// The shape for a body made of `parts` (a compound when there are several). `dynamic`
/// selects dynamic-compatible shapes (triangle meshes become convex hulls). Null if nothing
/// could be built.
JPH::RefConst<JPH::Shape> buildBodyShape(const ShapeContext& ctx, const std::vector<ShapePart>& parts, bool dynamic);

/// Capsule standing on the origin (feet at y = 0) for character controllers.
JPH::RefConst<JPH::Shape> characterShape(float height, float radius);

/// Hashing helpers for change detection (FNV-1a over raw bytes).
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {  // 8 bytes per round (hot: runs for every body every tick)
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
        for (; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void pod(const T& v) {
        bytes(&v, sizeof(T));
    }
    void str(const std::string& s) {
        pod(s.size());
        bytes(s.data(), s.size());
    }
    void mat(const Mat4& m) { bytes(m.m, sizeof(m.m)); }
    /// Hashes every reflected field of a component except the listed ones.
    void reflected(const void* object, const TypeInfo& type, std::initializer_list<const char*> skip = {});
};

}  // namespace sky::physics
