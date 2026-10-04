#pragma once
// Local light shadows (point and spot lights): the shadow atlas, the per-frame shadow plan and
// the projection math the GPU shaders mirror (engine/platform/metal/shaders/Shadows.metal).
//
//   FrameData::lights --(LocalShadowPlanner::plan: CPU, deterministic, unit-tested)--> ShadowPlan
//       per light: projection, atlas slot, bias, strength; the faces to (re)render this frame
//   backend: renders those faces (depth only) into the atlas, then shades with PCF
//
// Atlas layout (a quadrant-subdivided shadow atlas): the atlas
// is 4 square quadrants of size/2 (one texture-array slice each). Quadrant q is split into
// subdiv[q] x subdiv[q] slots, by default 1, 4, 16 and 64 slots of 2048, 1024, 512 and 256 px
// for a 4096 atlas. A light gets the slot size that matches its screen coverage (or its
// `shadowResolution` hint), the most important lights first. Inside its slot:
//   * spot light       one perspective view (the cone, plus a guard band for filtering)
//   * point, cube      6 faces of 90 degrees in a 3 x 2 grid (each floor(size/3) x size/2 px)
//   * point, dual par. 2 paraboloid hemispheres side by side (size/2 x size px; 2 renders, not 6)
//
// Static caching: slots persist across frames. A light's slot is re-rendered only when the hash
// of the light and of the shadow casters inside its range changes (casterHash), so a static
// street of lamps costs nothing after its first frame. Budgets keep the GPU work bounded: at most
// `maxLights` shadowed lights and `maxFaceUpdates` face renders per frame (stills render all).

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/math/Math.h"

namespace sky {

struct FrameData;
struct LightItem;
struct DrawItem;
struct Environment;

namespace shadows {

/// How a light's shadow is projected. The numeric values are part of the GPU contract.
enum class Projection : int { None = 0, Spot = 1, Cube = 2, DualParaboloid = 3 };
const char* projectionName(Projection p);

constexpr int kQuadrants = 4;
constexpr int kMaxShadowedLights = 64;  // hard cap of LocalShadowPlanner (GPU work stays bounded)
constexpr float kParaboloidSentinel = -2.f;  // m[15] of an encoded paraboloid "matrix" (see paraboloidMatrix)

// ---------------------------------------------------------------------------------------------
// Projection math (mirrored in Shadows.metal; keep both in sync)
// ---------------------------------------------------------------------------------------------

/// Near plane of a light's shadow views: a fraction of its range, clamped (m).
float shadowNear(float range);
/// Depth stored by a perspective shadow view for a point at distance `d` along the view axis.
float perspectiveDepth(float d, float near, float far);

/// Orthonormal basis of a light view looking along `forward`: right (s), up (u), forward (f).
/// World up is +Y, or +Z when looking (almost) straight up or down.
struct Basis {
    Vec3 s, u, f;
};
Basis lightBasis(Vec3 forward);
/// Cube face basis: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.
Basis cubeFaceBasis(int face);
/// The cube face a direction from the light falls into (major axis).
int cubeFaceOf(Vec3 v);

/// View-projection of one perspective shadow view (spot, or a cube face): `tanHalf` is the tangent
/// of half its field of view (square).
Mat4 perspectiveViewProj(Vec3 position, const Basis& b, float tanHalf, float near, float far);
/// Encodes a paraboloid hemisphere as a "matrix" the caster vertex shaders recognize
/// (m[15] = kParaboloidSentinel): columns 0..2 = basis (w: near, far, uv scale), column 3 = position.
/// hemisphere 0 looks along `forward`, 1 away from it.
Mat4 paraboloidMatrix(Vec3 position, Vec3 forward, int hemisphere, float near, float far, float uvScale);
/// CPU mirror of the shaders' shadowClip(): clip position of a world point for a caster pass.
Vec4 shadowClip(const Mat4& m, Vec3 world);

/// Where a receiver lands in its light's slot: uv inside the face rect (0..1, y down), the depth
/// to compare, the face, and whether it is inside the view at all.
struct Projected {
    Vec2 uv;
    float depth = 1.f;
    int face = 0;
    bool inside = false;
};
Projected projectSpot(Vec3 p, Vec3 lightPos, Vec3 dir, float tanHalf, float near, float far);
Projected projectCube(Vec3 p, Vec3 lightPos, float tanHalf, float near, float far);
Projected projectParaboloid(Vec3 p, Vec3 lightPos, Vec3 dir, float uvScale, float near, float far);

/// A face's rectangle inside a slot of `slotSize` px: x, y, w, h in px (integers).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
Rect faceRect(Projection p, int face, int slotSize);
int faceCount(Projection p);

// ---------------------------------------------------------------------------------------------
// Atlas allocation
// ---------------------------------------------------------------------------------------------

struct AtlasConfig {
    int size = 4096;                        // atlas edge (px); each quadrant slice is size / 2
    std::array<int, kQuadrants> subdiv{1, 2, 4, 8};  // slots per side in each quadrant
    bool operator==(const AtlasConfig& o) const { return size == o.size && subdiv == o.subdiv; }
};

struct AtlasSlot {
    int quadrant = -1;
    int index = -1;
    int x = 0, y = 0, size = 0;  // px inside the quadrant slice
    bool valid() const { return quadrant >= 0; }
    bool operator==(const AtlasSlot& o) const { return quadrant == o.quadrant && index == o.index; }
};

class ShadowAtlas {
public:
    explicit ShadowAtlas(AtlasConfig config = {});
    /// Changes the layout; every slot is released.
    void reset(AtlasConfig config);
    const AtlasConfig& config() const { return config_; }
    int quadrantSize() const { return config_.size / 2; }
    int slotSize(int quadrant) const;
    int slotCount(int quadrant) const;
    /// The quadrant whose slot size best fits `size` px (the largest slots not bigger than it).
    int quadrantFor(int size) const;

    struct Request {
        uint64_t key = 0;     // stable light identity
        int size = 0;         // desired slot size (px)
        float priority = 0;   // higher keeps / gets slots first
    };
    /// Assigns this frame's slots (one per request, same order; an invalid slot = none left). A key
    /// keeps its slot while its size tier matches; slots of keys not requested this frame stay
    /// cached until another light needs the space (least recently used first).
    std::vector<AtlasSlot> allocate(const std::vector<Request>& requests, uint64_t frame);

    /// Static caching: whether the slot holds a render made with this content hash.
    bool contentValid(const AtlasSlot& slot, uint64_t hash) const;
    /// Whether the slot holds any render of its current owner (false right after (re)assignment).
    bool hasContent(const AtlasSlot& slot) const;
    void markRendered(const AtlasSlot& slot, uint64_t hash);
    /// Every slot must re-render (shader reload, atlas recreated, `shadow_atlas_info invalidate`).
    void invalidateAll();
    uint64_t owner(const AtlasSlot& slot) const;
    int used(int quadrant) const;
    /// The slot a key currently owns (invalid if none).
    AtlasSlot slotOf(uint64_t key) const;

private:
    struct Cell {
        uint64_t owner = 0;
        uint64_t lastUse = 0;
        uint64_t hash = 0;
        bool valid = false;  // holds a render of `hash`
    };
    AtlasSlot slotAt(int quadrant, int index) const;
    Cell& cell(const AtlasSlot& s) { return cells_[static_cast<size_t>(s.quadrant)][static_cast<size_t>(s.index)]; }
    const Cell& cell(const AtlasSlot& s) const { return cells_[static_cast<size_t>(s.quadrant)][static_cast<size_t>(s.index)]; }

    AtlasConfig config_;
    std::array<std::vector<Cell>, kQuadrants> cells_;
    std::unordered_map<uint64_t, AtlasSlot> byKey_;
};

// ---------------------------------------------------------------------------------------------
// Per-frame plan
// ---------------------------------------------------------------------------------------------

struct ShadowSettings {
    AtlasConfig atlas;
    int maxLights = 16;            // shadowed point/spot lights per frame (most important first)
    int maxFaceUpdates = 24;       // face renders per frame (spot 1, cube 6, dual paraboloid 2)
    bool unlimitedUpdates = false; // stills and movie frames render every face they need
    float softness = 1.f;          // PCF radius multiplier (Environment::shadowSoftness)
};
/// Settings from the environment (localShadow* fields), the viewport quality tier and whether the
/// frame is a still / offline frame (no update budget).
ShadowSettings settingsFor(const Environment& env, int quality, bool still);

struct LightShadow {
    uint64_t id = 0;  // LightItem::id (entity and source)
    int kind = 1;     // LightItem::Kind
    Projection projection = Projection::None;
    AtlasSlot slot;
    float tanHalf = 1.f;     // spot / cube: tan(half field of view); paraboloid: uv scale
    float near = 0.05f, far = 10.f;
    float bias = 0.02f;      // m, along the light direction
    float normalBias = 1.f;  // shadow-map texels, along the surface normal
    float strength = 1.f;    // 0..1 (distance fade toward shadowMaxDistance)
    int faceResolution = 0;  // px, smallest side of one face
    float priority = 0.f;
    int requestedSize = 0;   // px (slot)
    bool updated = false;    // its faces were rendered this frame
    bool stale = false;      // casters changed but the update waits for budget (old shadow kept)
    uint64_t hash = 0;       // light + casters (static caching)
    std::string reason;      // why it has no shadow: disabled, directional, out_of_view, beyond_max_distance,
                             // over_light_budget, atlas_full, pending (first render waits for the face budget)
};

struct ShadowFace {
    int light = 0;  // index into FrameData::lights
    int face = 0;
    Projection projection = Projection::Spot;
    Mat4 viewProj;  // perspective view-projection, or an encoded paraboloid (paraboloidMatrix)
    int quadrant = 0;
    Rect viewport;  // px inside the quadrant slice
    Vec3 center;    // light position and range: caster culling
    float radius = 0.f;
};

struct ShadowPlan {
    std::vector<LightShadow> lights;  // index-aligned with FrameData::lights
    std::vector<ShadowFace> faces;    // to render this frame
    ShadowSettings settings;
    int requested = 0;     // lights asking for shadows
    int candidates = 0;    // ... of which in view and in range
    int shadowed = 0;      // lights shaded with a shadow this frame
    int overBudget = 0;    // candidates beyond maxLights (lit without shadows)
    int deferredFaces = 0; // face renders postponed by the update budget
    int cachedLights = 0;  // shadowed lights reusing last frame's render
    std::vector<std::string> warnings;
};

/// The GPU parameters of a light's shadow (GPULight::shadow / shadow2, see Shadows.metal):
///   shadow  = (slot u, slot v, slot size (uv of a quadrant slice), projection + 4 * quadrant)
///   shadow2 = (strength, bias (m), normal bias (texels), tanHalf / paraboloid uv scale)
std::array<Vec4, 2> gpuShadowParams(const LightShadow& s, const AtlasConfig& atlas);

/// Whether a draw casts a shadow from a local light: shadow casting, opaque enough, lit, and not
/// a small fixture around or within 0.3 m of the light (lamp heads with their glass, bulbs, sign
/// boxes: the light stands for the glowing fixture, which must not swallow it).
bool castsLocalShadow(const DrawItem& d, Vec3 lightPos, float lightRange);
/// Sphere / box overlap.
bool sphereTouches(Vec3 center, float radius, const Aabb& box);
/// Hash of a light's shadow inputs: the light itself and every caster inside its range (meshes with
/// their transforms and skin poses, terrains, foliage batches). Animated hair and mesh particles in
/// range make the light dynamic: `frame` is mixed in and the hash changes every frame.
uint64_t casterHash(const FrameData& frame, const LightItem& light, uint64_t frameIndex);

class LocalShadowPlanner {
public:
    /// Decides this frame's shadowed lights, their slots and the faces to render (the slots of the
    /// rendered faces are marked valid: the backend renders every face of the plan).
    ShadowPlan plan(const FrameData& frame, const ShadowSettings& settings);
    /// Forces every shadow to re-render on the next plan.
    void invalidate() { atlas_.invalidateAll(); }
    const ShadowAtlas& atlas() const { return atlas_; }
    const ShadowPlan& last() const { return last_; }
    uint64_t frames() const { return frame_; }
    /// Agent-facing description of the last plan (shadow_atlas_info).
    Json info(const FrameData* frame = nullptr) const;

private:
    ShadowAtlas atlas_;
    uint64_t frame_ = 0;
    ShadowPlan last_;
};

}  // namespace shadows
}  // namespace sky
