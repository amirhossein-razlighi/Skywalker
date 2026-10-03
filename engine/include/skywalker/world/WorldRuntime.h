#pragma once
// Runtime side of world building: owns loaded/generated terrain data and the foliage chunk
// caches, and turns Terrain / Foliage components into render items every frame.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "skywalker/render/Renderer.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/world/Foliage.h"
#include "skywalker/world/Terrain.h"

namespace sky::world {

class WorldRuntime {
public:
    struct Hooks {
        std::function<std::string(const std::string&)> resolvePath;                     // project-relative -> absolute
        std::function<const Surface*(const std::string&)> material;                     // material asset -> surface
        std::function<std::optional<Aabb>(const std::string&)> meshBounds;              // mesh key -> local bounds
        std::function<bool(const std::string&)> meshReady;  // false while a mesh is still streaming in
        std::function<bool(float x, float z, float top, float bottom, float& y, Vec3& n)> sceneSurface;  // ray down
        struct Part {
            std::string mesh, material;
            Mat4 local;
        };
        std::function<std::vector<Part>(const std::string&)> prefabParts;  // prefab asset -> mesh parts
    };
    explicit WorldRuntime(Hooks hooks) : hooks_(std::move(hooks)) {}

    /// The terrain data of an entity with a Terrain component: loaded from its `data` file or
    /// generated (deterministically) from `generator` + `layers`. nullptr if not a terrain.
    std::shared_ptr<TerrainData> terrain(const Scene& scene, EntityId e);
    /// Marks edited data (sculpt/paint) as current so it is not reloaded from disk.
    void adopt(EntityId e, std::shared_ptr<TerrainData> data, const std::string& sourceKey);
    std::string sourceKey(const Scene& scene, EntityId e) const;
    void forget(EntityId e);

    /// Adds terrain and foliage render items for `view`. `realtime` limits how many foliage
    /// chunks are generated per call (captures generate everything in range).
    void gather(const Scene& scene, const ViewCamera& view, FrameData& frame, bool realtime);

    /// Highest terrain surface under (x, z) in world space.
    bool terrainHeight(const Scene& scene, float x, float z, float& y, Vec3* normal = nullptr, EntityId* which = nullptr);
    struct Hit {
        EntityId entity;
        Vec3 point, normal;
        float distance;
    };
    std::optional<Hit> raycast(const Scene& scene, const Ray& ray, float maxDist);

    struct Stats {
        size_t terrains = 0, foliageChunks = 0, foliageInstances = 0;
    };
    Stats stats() const { return stats_; }

private:
    struct TerrainEntry {
        std::shared_ptr<TerrainData> data;
        std::string key;
    };
    struct FoliageEntry {
        FoliageCache cache;
        std::vector<FoliageLayer> layers;
        std::string layersKey;
    };
    EntityId terrainFor(const Scene& scene, EntityId e) const;

    Hooks hooks_;
    std::unordered_map<EntityId, TerrainEntry> terrains_;
    std::unordered_map<EntityId, FoliageEntry> foliage_;
    Stats stats_;
};

/// Default material layers for a terrain preset (procedural textures are referenced by
/// kind and generated into the project on demand by the terrain tools).
Json defaultTerrainLayers(const std::string& preset);

}  // namespace sky::world
