#pragma once
// Foliage scattering: deterministic, chunked placement of instanced meshes (grass, plants,
// rocks, trees, shells...) over a surface. Chunks are generated on demand around the camera
// (each chunk is a pure function of the layer, seed and chunk coordinates), so dense grass
// costs memory only near the viewer while sparse trees cover the whole world.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky::world {

/// One instance as the GPU sees it: a 3x4 world matrix (column-major columns 0..2 rotation/
/// scale, column 3 translation, packed as rows of float4) + per-instance variation.
struct FoliageInstance {
    float row0[4];  // world matrix rows (x)
    float row1[4];  // (y)
    float row2[4];  // (z)
    float tint;     // -1..1 color variation
    float phase;    // wind phase 0..1
    float height;   // instance height (m) for wind bending
    float fade;     // 0..1 random, for distance thinning
};
static_assert(sizeof(FoliageInstance) == 16 * sizeof(float));

struct FoliageLayer {
    std::string name;
    std::string mesh = "grass";         // primitive ("grass", "grass_tall", "pebbles", "fern", ...) or "asset:..." (incl. #part)
    std::string prefab;                 // multi-part model (e.g. an imported tree: trunk + alpha-cut leaves)
    std::string material;               // optional material asset; otherwise the fields below
    std::string texture, normalMap, ormMap;  // optional maps (project-relative)
    bool triplanar = false;             // project the maps in world space (rocks)
    float tiling = 1.f;                 // texture repeats (per meter when triplanar)
    Vec4 color{0.32f, 0.5f, 0.18f, 1.f};
    float roughness = 0.7f;
    float subsurface = 0.5f;            // light through leaves / blades
    float density = 4.f;                // instances per square meter
    float scaleMin = 0.8f, scaleMax = 1.2f;
    float slopeMin = 0.f, slopeMax = 35.f;   // degrees
    float heightMin = -1e9f, heightMax = 1e9f;  // world height
    int terrainLayer = -1;              // only where this terrain layer's weight exceeds `layerThreshold` (JSON: index or name)
    float layerThreshold = 0.35f;
    float alignToNormal = 0.6f;         // 0 upright .. 1 follows the ground
    float clumping = 0.4f;              // 0 uniform .. 1 patches
    float sink = 0.02f;                 // push into the ground (m) so bases don't float
    float colorVariation = 0.35f;
    float wind = 1.f;                   // bend strength multiplier
    float cullDistance = 120.f;         // meters; chunks beyond are not generated or drawn
    bool castShadows = true;
    float randomTilt = 4.f;             // degrees
    uint32_t seed = 0;
    // Distance rendering: beyond `impostorDistance` instances are drawn as octahedral impostors
    // (baked atlases of the model seen from many directions) instead of meshes.
    bool impostors = true;              // allow impostors (used when the mesh is heavy enough to pay off)
    float impostorDistance = 0.f;       // meters; 0 = automatic from on-screen size, < 0 = never
    int impostorResolution = 0;         // atlas edge in pixels; 0 = automatic (512..2048 by model size)
    int impostorFrames = 12;            // capture directions per atlas side (4..32)
};

/// Parses foliage layers. `terrainLayerNames` (see terrainLayerNames) lets `terrainLayer` name a
/// terrain material layer instead of giving its index; a name that does not resolve leaves the
/// layer unrestricted (foliage_add rejects it up front with a did-you-mean hint).
std::vector<FoliageLayer> foliageLayersFromJson(const Json& layers, const std::vector<std::string>& terrainLayerNames = {});
/// The names of a terrain's material layers (`Terrain::layers`), in index order.
std::vector<std::string> terrainLayerNames(const Json& terrainLayers);
/// A foliage layer's `terrainLayer` value: an index (a number or a digit string) or a terrain
/// layer name (case-insensitive). Fails with `unknown_terrain_layer` (did-you-mean hint) for an
/// unknown name and `invalid_terrain_layer` for an index outside the terrain's layers. A negative
/// index means "anywhere" (-1). With no names known, any index is accepted and names fail.
Result<int> resolveTerrainLayer(const Json& value, const std::vector<std::string>& names);
/// Named layer presets: meadow_grass, tall_grass, dune_grass, beach_pebbles, shells, flowers,
/// ferns, rocks_small, boulders, pine_trees (with asset meshes when provided).
const std::vector<std::string>& foliagePresets();
Json foliagePreset(const std::string& name);

/// Sample of the surface under (x, z): world height, normal, terrain layer weights (0..1).
struct SurfaceSample {
    float y = 0;
    Vec3 normal{0, 1, 0};
    float layerWeights[8] = {};
};
using SurfaceFn = std::function<bool(float x, float z, SurfaceSample& out)>;

struct FoliageChunkKey {
    int layer, cx, cz;
    bool operator==(const FoliageChunkKey& o) const { return layer == o.layer && cx == o.cx && cz == o.cz; }
};
struct FoliageChunkKeyHash {
    size_t operator()(const FoliageChunkKey& k) const {
        return (static_cast<size_t>(static_cast<uint32_t>(k.layer)) * 73856093u) ^ (static_cast<size_t>(static_cast<uint32_t>(k.cx)) * 19349663u) ^
               (static_cast<size_t>(static_cast<uint32_t>(k.cz)) * 83492791u);
    }
};

struct FoliageChunk {
    std::shared_ptr<const std::vector<FoliageInstance>> instances;
    Aabb bounds;
    uint64_t id = 0;  // stable id for GPU buffer caching
};

/// Generates the instances of one chunk (world-space square [cx*size, (cx+1)*size) x [cz..]).
std::vector<FoliageInstance> scatterChunk(const FoliageLayer& layer, int layerIndex, uint32_t seed, int cx, int cz,
                                          float chunkSize, const SurfaceFn& surface, float meshHeight);

/// Per-entity cache of generated chunks with LRU-ish eviction.
class FoliageCache {
public:
    static constexpr float kChunkSize = 16.f;  // minimum; sparse layers use larger chunks
    /// Chunk edge (m) holding ~256 instances of a layer: dense grass gets small chunks
    /// (fine culling, streaming), sparse trees big ones (few draw calls).
    static float chunkSizeFor(const FoliageLayer& layer);
    /// Returns the chunks of `layerIndex` that are within the layer's cull distance of `eye`
    /// and inside `area` (world xz bounds), generating missing ones (at most `budget` per call).
    std::vector<FoliageChunk> visibleChunks(const FoliageLayer& layer, int layerIndex, uint32_t seed, Vec3 eye,
                                            Vec2 areaMin, Vec2 areaMax, const SurfaceFn& surface, float meshHeight,
                                            int& budget);
    void clear() { chunks_.clear(); }
    size_t chunkCount() const { return chunks_.size(); }
    size_t instanceCount() const;
    uint64_t signature = 0;  // hash of the inputs; a change clears the cache

private:
    struct Entry {
        FoliageChunk chunk;
        uint64_t lastUse = 0;
    };
    std::unordered_map<FoliageChunkKey, Entry, FoliageChunkKeyHash> chunks_;
    uint64_t tick_ = 0;
    uint64_t nextId_ = 1;
};

}  // namespace sky::world
