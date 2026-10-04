#pragma once
// Reflection probes: the CPU side (docs/RENDERING.md "Reflection probes").
//
//   reflection_probe components --(buildFrame)--> FrameData::probes (ProbeItem, world space)
//   probes::Planner::plan (CPU, deterministic, unit-tested) decides each frame
//       * which probes own a slot of the cube-array atlas (fixed budget, least recently used eviction),
//       * which faces to capture this frame (first captures, moved probes, on_change content, realtime
//         intervals) within a per-frame face budget, and
//       * which probes shade this frame, in shading order (priority, then smaller volumes first).
//   backend: renders the planned faces through the regular scene path (no post), GGX-prefilters the
//       roughness mips into the probe's atlas slot, and shades surfaces with the probes of their
//       light cluster (Probes.metal mirrors the math below: influence, box projection, blending).
//
// Shading order per pixel: screen-space reflections first, then the probes (front to back until
// their weights sum to 1), then the sky for the remainder. Inside an `interior` probe there is no
// sky remainder: the probes' weights are renormalized.

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/ProbeItems.h"

namespace sky {

struct FrameData;
struct Environment;
struct LightGrid;
struct ReflectionProbe;

namespace probes {

constexpr int kMaxProbes = 32;         // hard atlas cap: one bit per shaded probe in a cluster mask
constexpr int kMaxPerPixel = 8;        // most probes blended at one pixel (shader loop bound)
constexpr int kMips = 6;               // atlas mips: GGX roughness 0..1; the last one is the diffuse ambient
constexpr int kFaces = 6;
constexpr int kMinResolution = 64;
constexpr int kMaxResolution = 512;
constexpr int kMaxCaptureLights = 128; // point / spot lights a capture evaluates (nearest first)
constexpr double kMaxAtlasMB = 256.0;     // the atlas never grows past this (fewer slots at high resolutions)
constexpr float kInteriorMargin = 0.1f;    // m: surfaces this far outside an interior volume still belong to it
constexpr float kInteriorMinWeight = 1e-3f; // interior volumes never fade to zero inside (renormalized: no sky)

enum class Update : int { Once = 0, OnChange = 1, Realtime = 2 };
enum class Ambient : int { Probe = 0, Sky = 1, Color = 2 };
const char* updateName(int update);
const char* ambientName(int ambient);

/// The nearest power of two in 64..512.
int sanitizeResolution(int px);
/// GPU memory of one atlas slot (a cube with its mips) at `resolution` px.
double slotBytes(int resolution);
/// Resolves a component on an entity with world matrix `world` (its scale multiplies the volume).
ProbeItem makeItem(EntityId entity, const Mat4& world, const ReflectionProbe& p);

// ---------------------------------------------------------------------------------------------
// Math (mirrored in shaders/Probes.metal; keep both in sync)
// ---------------------------------------------------------------------------------------------

/// Direction of texel `uv` (0..1, y down) on cube face `face` (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z),
/// the GPU's cube map convention (Sky.metal cubeDir).
Vec3 cubeDir(int face, Vec2 uv);
struct FaceBasis {
    Vec3 right, up, forward;  // screen right (NDC +x), screen up (NDC +y), view direction
};
FaceBasis faceBasis(int face);
/// View-projection of capture face `face` seen from `eye` (90 degrees, square, depth 0..1). Cube map
/// faces are mirrored images: triangles that face the camera wind clockwise in them.
Mat4 faceViewProj(Vec3 eye, int face, float nearPlane, float farPlane);

/// A world point in the volume's frame (meters, origin at the center, axes of the volume).
Vec3 toLocal(const ProbeItem& p, Vec3 world);
/// Distance (m) from a point to the nearest face of the volume: > 0 inside, < 0 outside.
float edgeDistance(const ProbeItem& p, Vec3 world);
/// Influence 0..1: 1 deeper than blendDistance inside the volume, fading to 0 at its faces. Interior
/// volumes keep a tiny weight up to kInteriorMargin outside their faces (the room's own walls), which
/// the renormalization of interior blends turns into full strength: no sky line along the walls.
float influence(const ProbeItem& p, Vec3 world);
/// The direction to look up in the probe's cubemap for reflection vector `R` at `world`: with box
/// projection, from the capture point toward where R leaves the volume, or the projection box when the
/// probe has one (parallax correction).
Vec3 lookupDir(const ProbeItem& p, Vec3 world, Vec3 R);
float volume(const ProbeItem& p);
float boundingRadius(const ProbeItem& p);
Aabb worldBounds(const ProbeItem& p);
/// Shading order: higher priority first, then the smaller volume (detail probes inside big ones).
bool shadesBefore(const ProbeItem& a, const ProbeItem& b);

/// Front-to-back blend of the probes covering a point (`ordered` in shading order): each probe adds
/// influence x (1 - accumulated); the sky gets the rest. Inside an interior probe the probe weights
/// are renormalized to 1 and the sky gets nothing.
struct Blend {
    int count = 0;
    std::array<int, kMaxPerPixel> probe{};  // indices into `ordered`
    std::array<float, kMaxPerPixel> weight{};
    float sky = 1.f;
    bool interior = false;
};
Blend blend(const std::vector<const ProbeItem*>& ordered, Vec3 world);

/// Sun shadow view-projection for a capture: an orthographic view covering a sphere around it.
Mat4 sunViewProj(Vec3 center, float radius, Vec3 sunDir);

// ---------------------------------------------------------------------------------------------
// GPU contract (GPUProbe / ProbeBlock in Probes.metal)
// ---------------------------------------------------------------------------------------------

struct GpuProbe {
    float worldToLocal[16];  // column-major: world -> volume frame
    float extents[4];        // xyz half extents (sphere: x = radius), w = blend distance
    float capture[4];        // xyz capture point (world), w = intensity
    float params[4];         // x = atlas cube index, y = base mip, z = flags (1 box projection, 2 interior, 4 sphere),
                             // w = ambient mode (0 probe, 1 sky, 2 color)
    float ambient[4];        // rgb = ambient color (linear x energy), w = debug color index
    float projection[4];     // xyz = center of the projection box in the volume frame, w = 1: project onto it (else the volume)
    float projectionHalf[4]; // xyz = its half size
};
static_assert(sizeof(GpuProbe) == 160, "GpuProbe must match GPUProbe in Probes.metal");

struct GpuProbeBlock {
    float info[4];     // x = probe count (0 = probes off), y = atlas max mip, z = flags (1 capture pass, 2 interior capture)
    float ambient[4];  // interior capture: the constant ambient that replaces the sky's light
    GpuProbe probes[kMaxProbes];
};
static_assert(sizeof(GpuProbeBlock) == 32 + 160 * kMaxProbes, "GpuProbeBlock must match ProbeBlock in Probes.metal");

GpuProbe gpuProbe(const ProbeItem& p, int slot, int baseMip, int colorIndex);
/// Debug color of a slot (probe_info reports it; the reflection_probes view draws it).
Vec3 debugColor(int colorIndex);

// ---------------------------------------------------------------------------------------------
// Per-frame plan
// ---------------------------------------------------------------------------------------------

struct Settings {
    int budget = 16;          // probes with an atlas slot (Environment.probeBudget, max kMaxProbes); 0 = off
    int facesPerFrame = 6;    // capture faces rendered per frame (Environment.probeUpdates); stills: all
    bool unlimited = false;   // stills and movie frames capture everything they need now
};
Settings settingsFor(const Environment& env, int quality, bool still);

struct ProbeState {
    EntityId entity = 0;
    int slot = -1;             // atlas cube index (-1 = none)
    int baseMip = 0;           // log2(atlas resolution / probe resolution)
    bool ready = false;        // the slot holds a complete capture
    bool hadCapture = false;   // ... already when the frame started (captures may light their surroundings with it)
    bool shaded = false;       // lights surfaces this frame
    bool inView = false;
    int facesCaptured = 0;     // faces rendered this frame
    int facesDone = 0;         // progress of a capture spread over frames (0 = none in progress)
    uint64_t captures = 0;     // completed captures
    uint64_t lastCapture = 0;  // frame of the last completed capture
    float importance = 0.f;
    std::string reason;        // why it does not shade: disabled, over_budget, pending, out_of_view
};

struct CaptureJob {
    int probe = 0;          // index into FrameData::probes
    int firstFace = 0;      // faces [firstFace, firstFace + faceCount) of the cube
    int faceCount = 0;
    bool completes = false; // the cube is complete after this job: filter it into the atlas
    bool bounce = false;    // then capture the whole cube once more, lit by its own first capture (bounce light)
};

struct Plan {
    std::vector<ProbeState> probes;  // index-aligned with FrameData::probes
    std::vector<int> shaded;         // indices into FrameData::probes, shading order (<= kMaxProbes)
    std::vector<CaptureJob> jobs;    // in capture order
    Settings settings;
    int atlasResolution = 0;         // face size of mip 0 (px; 0 = no atlas needed)
    int atlasSlots = 0;              // cubes allocated in the atlas
    bool atlasReset = false;         // the atlas layout changed: every slot must be re-captured
    int facesCaptured = 0;
    int facesDeferred = 0;
    int overBudget = 0;
    std::vector<std::string> warnings;
};

/// Hash of what a probe sees: the draws, terrains and lights within its capture range (on its cull
/// mask) and the sky. `on_change` probes re-capture when it changes; probe_info reports `stale`.
uint64_t contentHash(const FrameData& frame, const ProbeItem& p);
/// Setup problems agents should hear about: overlapping volumes with equal priority, volumes with no
/// geometry in them, capture points inside a solid mesh, capture planes cutting the volume.
std::vector<std::string> sceneWarnings(const FrameData& frame);
/// Per light cluster (LightGrid order): bit i set when shaded probe i (plan.shaded order) may touch it.
std::vector<uint32_t> clusterMasks(const FrameData& frame, const LightGrid& grid, const Plan& plan);
/// The probe block the surfaces read: the shaded probes with their slots.
GpuProbeBlock gpuBlock(const FrameData& frame, const Plan& plan);
/// Indices into FrameData::lights for a capture of `p`: directional lights first (all of them), then
/// the point / spot lights that reach the capture range, nearest first, at most kMaxCaptureLights.
std::vector<uint32_t> captureLights(const FrameData& frame, const ProbeItem& p, uint32_t& directionalCount);

class Planner {
public:
    Plan plan(const FrameData& frame, const Settings& settings);
    /// Re-captures `entity` (0 = every probe) on the next frames.
    void invalidate(EntityId entity = 0);
    /// The backend lost the atlas contents (allocation failure): every probe re-captures.
    void atlasLost();
    /// GPU time of a completed capture (from the backend's completion handler, main thread).
    void reportCaptureMs(EntityId entity, double ms);
    const Plan& last() const { return last_; }
    uint64_t frames() const { return frame_; }
    /// Agent-facing description of the last plan (probe_info). With the frame: staleness and setup warnings.
    Json info(const FrameData* frame = nullptr) const;

private:
    struct Record {
        int slot = -1;
        int resolution = 0;
        uint64_t captureKey = 0;
        uint64_t contentHash = 0;  // at the start of the last capture
        bool ready = false;
        bool forced = false;
        int facesDone = 0;
        uint64_t captures = 0;
        uint64_t lastCapture = 0;
        uint64_t lastSeen = 0;
        double lastCaptureMs = 0.0;
        uint64_t firstRequest = 0;  // frame the pending capture was first wanted (deferral age)
    };
    std::unordered_map<EntityId, Record> records_;
    std::vector<EntityId> slots_;     // owner per atlas slot (0 = free)
    std::vector<uint64_t> slotUse_;   // last frame each slot shaded or was requested
    EntityId inProgress_ = 0;         // the probe whose capture spans frames (one at a time)
    int atlasResolution_ = 0;
    uint64_t frame_ = 0;
    Plan last_;
};

}  // namespace probes
}  // namespace sky
