#pragma once
// Strand hair and fur: groom generation (guides + interpolated children with clumping,
// curls, waves and frizz), file formats and the per-scene groom cache.
//
// Everything here is CPU-side, deterministic and backend independent. The GPU simulates
// the guides and rebuilds the children every frame from the data generated here (see
// GroomData in render/FxItems.h for the representation).
//
// File formats
//   .hair       Cem Yuksel's HAIR format (binary, little endian; www.cemyuksel.com/research/hairmodels)
//   .groom.json engine JSON: {"format":"skywalker-groom","version":1,"strands":[[x,y,z, ...], ...],
//               "widths":[mm per strand]?}
//   .skygroom   engine binary: "SKYGROOM" magic, u32 version (1), u32 strands, u32 points per strand,
//               u32 flags (bit 0: per-strand widths), then strands*points*3 floats, then widths

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/FxItems.h"
#include "skywalker/render/MeshData.h"

namespace sky {
class Scene;
}

namespace sky::fx {

/// Raw strands as polylines (any point count per strand), e.g. from a file.
struct StrandSet {
    std::vector<uint32_t> counts;  // points per strand
    std::vector<Vec3> points;      // all strands, concatenated
    std::vector<float> widths;     // optional, per strand (mm)
    size_t strandCount() const { return counts.size(); }
};

/// Cem Yuksel's .hair format.
Result<StrandSet> parseHairFile(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> writeHairFile(const StrandSet& strands);
/// Engine formats (.groom.json / .skygroom).
Result<StrandSet> parseGroomJson(const Json& doc);
Json writeGroomJson(const StrandSet& strands);
Result<StrandSet> parseSkyGroom(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> writeSkyGroom(const StrandSet& strands);
/// Loads any supported groom file by extension (scale and Z-up conversion applied).
Result<StrandSet> loadStrands(const std::string& path, float scale = 1.f, bool zUp = false);

/// Grows a groom on a mesh (mesh local space). `mesh` may be null for imported grooms. `vertexMask`
/// (optional, one 0..1 value per vertex) scales the density (bone masks); the maskCenter / maskRadius
/// region is evaluated exactly per root, around `regionCenter` (default: params.maskCenter).
/// Deterministic: the same inputs give bit-identical output. Roots are bound to their triangles.
Result<GroomData> generateGroom(const Groom& params, const MeshData* mesh, const std::vector<float>* vertexMask = nullptr,
                                const Vec3* regionCenter = nullptr);
/// Builds a groom from explicit strands (every strand rendered; a subset becomes guides).
Result<GroomData> groomFromStrands(const Groom& params, const StrandSet& strands, const MeshData* mesh);

/// CPU reference of the GPU child reconstruction. `guides` = guideCount * P points in the
/// space the result should be in (rest: data.guideRest); `model` maps local -> that space
/// (identity for rest). Writes children * P points.
void reconstructStrands(const GroomData& data, const std::vector<Vec3>& guides, const Mat4& model,
                        std::vector<Vec3>& out);
/// Rest-pose strands of a groom (for export and inspection).
StrandSet restStrands(const GroomData& data);

/// Parameters that change the generated geometry (not shading or motion), hashed.
uint64_t groomHash(const Groom& params, const std::string& meshKey, const MeshData* mesh, int64_t sourceStamp);

/// Preset patches for the groom component.
Json groomPreset(const std::string& name);

/// Physically based hair color: absorption coefficient (per unit of hair diameter) from
/// melanin concentration (eumelanin + pheomelanin, d'Eon 2011 / Chiang 2016 mapping).
Vec3 hairAbsorption(float melanin, float redness);
/// Approximate perceived color for an absorption (for UI swatches and tests).
Vec3 hairColorFromAbsorption(Vec3 sigma);

/// Per-scene cache: one generated groom per entity, regenerated when its hash changes.
class GroomSystem {
public:
    using MeshProvider = std::function<const MeshData*(const std::string& key)>;
    using PathResolver = std::function<std::string(const std::string& path)>;

    /// Character tech (set by the engine): what skinned grooms need from the animation system.
    struct Hooks {
        /// Joint palette (mesh space) of a rigged mesh drawn by `entity` this frame; null = not animated.
        std::function<std::shared_ptr<const std::vector<Mat4>>(EntityId entity, const std::string& meshKey)> palette;
        /// CPU-skinned copy of that mesh (the fallback when it is not drawn).
        std::function<std::shared_ptr<const MeshData>(EntityId entity, const std::string& meshKey)> posed;
        /// World capsules fitted to the skeleton that animates `entity` (head, neck, torso, limbs).
        std::function<std::vector<FxCollider>(EntityId entity)> bodyColliders;
        /// A bone and its descendants, by name, in the skeleton of a rigged mesh (maskBone).
        std::function<std::vector<std::string>(const std::string& meshKey, const std::string& bone)> boneFamily;
    };
    Hooks hooks;

    /// The current guide roots of a groom in world space (CPU skinned when it follows a skin).
    struct RootsView {
        std::vector<Vec3> roots, normals;
        bool skinned = false;
        size_t children = 0, bound = 0;
        float bindError = 0.f;
    };
    Result<RootsView> roots(const Scene& scene, EntityId e, const MeshProvider& meshes, const PathResolver& resolve);

    /// Builds GroomItems for every visible groom (generating as needed).
    void gather(const Scene& scene, const MeshProvider& meshes, const PathResolver& resolve,
                std::vector<GroomItem>& out);
    /// The generated data for an entity (generating if needed); null + error if it can't.
    std::shared_ptr<const GroomData> groomFor(const Scene& scene, EntityId e, const MeshProvider& meshes,
                                              const PathResolver& resolve, std::string* error = nullptr);
    void clear() { cache_.clear(); }
    size_t cached() const { return cache_.size(); }
    /// Milliseconds spent generating the last groom.
    double lastGenerateMs() const { return lastGenerateMs_; }

private:
    /// Per-vertex density from maskBone (skin weights of the bone family); `regionCenter` = maskCenter offset
    /// from the bone's rest position (scaled mesh space).
    std::vector<float> vertexMask(const Groom& g, const MeshData& mesh, const std::string& meshKey, Vec3 scale, Vec3& regionCenter,
                                  std::string& error) const;
    struct Entry {
        uint64_t hash = 0;
        std::shared_ptr<const GroomData> data;
        std::string error;
    };
    std::unordered_map<EntityId, Entry> cache_;
    double lastGenerateMs_ = 0;
};

/// Entity whose mesh a groom grows on (the groom's `target`, else the entity itself).
EntityId groomMeshEntity(const Scene& scene, EntityId e);
/// World-space colliders from entity links (spheres from sphere meshes, planes from plane/quad meshes, capsules
/// from elongated bounds, else bounding spheres). Name-only links resolve near `self` (Scene::findNear).
std::vector<FxCollider> collidersFromLinks(const Scene& scene, EntityId self, const std::vector<EntityLink>& links);

}  // namespace sky::fx
