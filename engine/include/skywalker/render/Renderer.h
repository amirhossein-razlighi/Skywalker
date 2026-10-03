#pragma once
// Rendering front end.
//
//   Scene --(FrameBuilder, CPU, portable, tested)--> FrameData --(Renderer backend)--> pixels
//
// The backend interface is deliberately tiny so new GPUs/OSes (Vulkan, D3D12, WebGPU)
// only implement `Renderer`. Everything agents rely on — what is visible, where it is on
// screen, picking — is computed on the CPU from FrameData, so it behaves identically on
// every backend (and in headless tests).

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/world/Foliage.h"
#include "skywalker/world/Terrain.h"

namespace sky {

/// A resolved camera for one view.
struct ViewCamera {
    Vec3 eye{0, 3, 8};
    Vec3 target{0, 0, 0};
    Vec3 up{0, 1, 0};
    float fovDeg = 55.f;
    float nearPlane = 0.05f;
    float farPlane = 1000.f;
    bool orthographic = false;
    float orthoSize = 5.f;
    float aperture = 0.f;       // f-stop (0 = no depth of field)
    float focusDistance = 0.f;  // 0 = autofocus
    float motionBlur = 0.f;     // shutter fraction

    Mat4 view() const { return Mat4::lookAt(eye, target, up); }
    Mat4 projection(float aspect) const;
    /// Ray through pixel (x,y) of a width x height viewport (y down).
    Ray rayAt(float x, float y, int width, int height) const;
};

/// Orbit camera used by the editor viewport (and by agents unless they pick a scene camera).
struct OrbitCamera {
    Vec3 target{0, 0.5f, 0};
    float yaw = 30.f;     // degrees around +Y
    float pitch = 25.f;   // degrees above horizon
    float distance = 10.f;
    float fovDeg = 55.f;

    void orbit(float dYaw, float dPitch);
    void pan(float dx, float dy);  // in screen-relative units
    void zoom(float factor);
    void frame(const Aabb& box);
    void lookAt(Vec3 eye, Vec3 newTarget);
    ViewCamera toView() const;
    Json toJson() const;
};

enum class Shading : uint8_t { Pbr = 0, Toon = 1, Unlit = 2, Water = 3 };
Shading shadingFromString(std::string_view s);

/// How a surface looks: the inline MeshRenderer fields or a material asset, resolved.
/// Texture paths are project-relative in materials and absolute once a frame is built.
struct Surface {
    Vec4 color{0.8f, 0.8f, 0.82f, 1.f};
    Vec4 emissive{0.f, 0.f, 0.f, 1.f};
    float metallic = 0.f;
    float roughness = 0.55f;
    std::string texture;      // base color (sRGB)
    std::string normalMap;    // tangent space (linear)
    std::string ormMap;       // occlusion / roughness / metallic (linear)
    std::string emissiveMap;  // sRGB
    Vec2 tiling{1, 1};
    float normalStrength = 1.f;
    bool triplanar = false;
    Shading shading = Shading::Pbr;
    float clearcoat = 0.f;
    float subsurface = 0.f;
    float rim = 0.f;
    float outline = 0.f;  // pixels
    Vec4 outlineColor{0.04f, 0.04f, 0.06f, 1.f};
    bool doubleSided = false;
    float occlusionStrength = 1.f;
    float alphaCutoff = 0.f;  // > 0: alpha-tested cutout
};

struct DrawItem {
    EntityId entity = kNoEntity;
    std::string mesh;
    Mat4 model;
    Surface surface;
    bool selected = false;
    bool castShadows = true;
    Aabb worldBounds;
    int skin = -1;  // index into FrameData::skins for skinned (animated) meshes
};

// --- Animation: GPU skinning input ------------------------------------------------------
/// A posed skin: joint matrices (mesh space) for a rigged mesh, and its posed bounds.
struct SkinPose {
    std::shared_ptr<const std::vector<Mat4>> palette;
    Aabb bounds;  // mesh space
};
/// One skinned draw. Backends skin `mesh` (which has a SkinStream) with `palette` into a
/// per-instance vertex buffer registered under `key`; the DrawItem's mesh is `key`, so
/// every pass (shadows, outlines, selection) draws the posed vertices unchanged.
struct SkinItem {
    std::string mesh;  // base mesh key ("asset:models/hero.glb#2")
    std::string key;   // unique per instance ("asset:models/hero.glb#2@skin12")
    std::shared_ptr<const std::vector<Mat4>> palette;
};

struct LightItem {
    enum class Kind { Directional, Point, Spot } kind = Kind::Point;
    Vec3 position;
    Vec3 direction{0, -1, 0};
    Vec3 color{1, 1, 1};
    float intensity = 1;
    float range = 10;
    float cosCone = 0.8f;
};

/// Unlit geometry drawn on top of the scene (gizmos, editor helpers).
struct OverlayItem {
    std::string mesh;  // primitive name (incl. internal "gizmo_ring")
    Mat4 model;
    Vec4 color;
};

/// One particle as the GPU sees it (world space, linear HDR color). 16 floats.
struct ParticleInstance {
    float position[3];
    float size;      // diameter (m)
    float color[4];  // linear rgb (pre-multiplied by intensity for emissive looks), a = opacity
    float velocity[3];
    float rotation;  // radians
    float age;       // 0..1 of its life
    float look;      // ParticleLook
    float seed;      // 0..1, per-particle variation
    float softness;  // soft-particle fade distance (m)
};
static_assert(sizeof(ParticleInstance) == 16 * sizeof(float));

enum class ParticleLook : int { Glow = 0, Flame = 1, Smoke = 2, Spark = 3, Rain = 4, Snow = 5, Mist = 6, Splash = 7 };

/// The engine's FFT ocean simulation for one water body, at the frame's time.
struct OceanCascades {
    static constexpr int kCascades = 3;
    int resolution = 0;                          // N: each cascade is N x N texels
    float patchSize[kCascades] = {};             // meters covered by one tile
    std::vector<float> displacement[kCascades];  // N*N*4: dx, height, dz, jacobian (foam where < 1)
    std::vector<float> slope[kCascades];         // N*N*4: dh/dx, dh/dz, 0, 0
    uint64_t version = 0;                        // changes whenever the data changes
};

struct WaterItem {
    EntityId entity = kNoEntity;
    float level = 0.f;
    Vec2 center{0, 0};
    float size = 0.f;  // 0 = endless
    Vec4 deepColor{0.015f, 0.07f, 0.1f, 1.f};
    Vec4 shallowColor{0.12f, 0.5f, 0.45f, 1.f};
    float clarity = 6.f;
    float foam = 1.f;
    float reflections = 1.f;
    float refraction = 1.f;
    float roughness = 0.04f;
    std::shared_ptr<const OceanCascades> ocean;
};

/// A volumetric fluid to simulate (on the GPU) and ray-march this frame.
struct VolumeItem {
    EntityId entity = kNoEntity;
    Mat4 model;           // bottom-center origin, unscaled (size below)
    FluidVolume params;
    Vec3 wind{0, 0, 0};   // environment wind in world space (m/s), already scaled by params.wind
};

/// A heightfield terrain to draw (continuous LOD from its height texture).
struct TerrainItem {
    EntityId entity = kNoEntity;
    Vec3 origin;  // world position of the terrain center
    std::shared_ptr<const world::TerrainData> data;
    struct Layer {
        Surface surface;  // color, roughness, metallic, texture / normalMap / ormMap (absolute paths)
        float tiling = 0.25f;  // texture repeats per meter
        bool triplanar = false;
    };
    std::vector<Layer> layers;
    float waterLevel = -100000.f;
    float wetBand = 1.2f;
    float detail = 1.f;
    bool castShadows = true;
    bool selected = false;
};

/// A chunk of GPU-instanced foliage (one mesh + surface, many transforms).
struct InstanceBatch {
    EntityId entity = kNoEntity;
    uint64_t id = 0;  // stable while the instance data is unchanged (GPU buffer caching)
    std::string mesh;
    Surface surface;
    std::shared_ptr<const std::vector<world::FoliageInstance>> instances;
    Aabb bounds;
    bool castShadows = true;
    float wind = 1.f;           // bend strength
    float cullDistance = 100.f;
    float meshHeight = 1.f;     // height of the mesh (m) for wind bending
    Mat4 part;                  // transform of this part inside a multi-part model (identity otherwise)
};

struct FrameData {
    int width = 0;
    int height = 0;
    ViewCamera camera;
    Mat4 view;
    Mat4 projection;
    Environment environment;
    std::vector<DrawItem> draws;
    std::vector<LightItem> lights;  // up to kMaxLights are used
    std::vector<OverlayItem> overlays;
    std::vector<ParticleInstance> particles;  // sorted back to front
    std::vector<WaterItem> water;
    std::vector<VolumeItem> volumes;
    std::vector<TerrainItem> terrains;
    std::vector<InstanceBatch> instances;
    std::vector<SkinItem> skins;  // animation: skinned draws (see SkinItem)
    bool drawGrid = true;
    float time = 0;
    /// Jittered sub-samples accumulated into this frame (stills and cinematics: supersampling,
    /// noise-free GI). 1 = real-time (temporal anti-aliasing across frames).
    int samples = 1;
    /// Discards temporal history (camera cuts). Large camera jumps are detected automatically.
    bool resetHistory = false;
    /// Buffer visualization instead of the final image: 0 off, 1 albedo, 2 normals,
    /// 3 roughness/metallic, 4 GI, 5 reflections, 6 AO, 7 depth, 8 lighting before GI.
    int debugView = 0;

    static constexpr size_t kMaxLights = 1024;       // clustered lighting on surfaces
    static constexpr size_t kMaxEffectLights = 16;   // the most important ones also light water, particles, fog
    Mat4 viewProjection() const { return projection * view; }
};

using ResolvedMaterial = Surface;

struct BuildOptions {
    bool editorOverlays = true;  // grid, selection highlight
    std::vector<EntityId> selection;
    float time = 0;
    /// Resolves MeshRenderer::material paths (provided by the engine's asset system).
    std::function<const ResolvedMaterial*(const std::string&)> material;
    /// Animation: the posed skin of a rigged mesh drawn by an entity (null = rest pose).
    std::function<const SkinPose*(EntityId entity, const std::string& mesh)> skin;
};

/// Orders lights by importance for this view (directional first, then the point/spot lights
/// nearest to what the camera looks at) and keeps at most kMaxLights.
void prioritizeLights(FrameData& f);

/// Converts the scene into a renderer-agnostic frame description.
FrameData buildFrame(const Scene& scene, const ViewCamera& camera, int width, int height, const BuildOptions& opts);

/// What an agent "sees": every visible entity with its on-screen rectangle.
struct VisibleEntity {
    EntityId id;
    std::string name;
    float x, y, w, h;  // pixels, top-left origin, clipped to viewport
    float depth;       // distance from camera
    float coverage;    // fraction of viewport area covered (0..1)
};
std::vector<VisibleEntity> visibleEntities(const Scene& scene, const FrameData& frame);

/// Draws numbered outlines for each visible entity onto the image (set-of-mark prompting).
void annotate(Image& image, const std::vector<VisibleEntity>& visible);

/// CPU picking: closest entity hit by the ray through (x, y). Exact for boxes, spheres
/// and planes; bounding-box accurate for other meshes.
EntityId pick(const Scene& scene, const FrameData& frame, float x, float y);

/// Finds the scene's primary camera (if any) as a ViewCamera.
bool sceneCamera(const Scene& scene, ViewCamera& out, EntityId preferred = kNoEntity);

// ---------------------------------------------------------------------------
// Backend interface
// ---------------------------------------------------------------------------

struct RendererInfo {
    std::string backend;  // "metal", "null"
    std::string device;   // GPU name
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual RendererInfo info() const = 0;
    /// Renders into the renderer's offscreen target (sized to frame.width/height).
    virtual Status render(const FrameData& frame) = 0;
    /// Reads back the last rendered frame (blocks until the GPU is done).
    virtual Result<Image> readback() = 0;
    /// Displays the last rendered frame in a platform surface (CAMetalLayer* on Apple).
    virtual Status present(void* surface) = 0;
    /// Uploads a mesh for "asset:<path>" meshes.
    virtual Status uploadMesh(const std::string& key, const struct MeshData& mesh) = 0;
    /// Drops cached GPU data for a mesh key ("asset:...") or texture path so it reloads.
    virtual void invalidate(const std::string& key) = 0;
    /// Replaces the shader source at runtime; returns compiler diagnostics on failure.
    virtual Status reloadShaders(const std::string& source) = 0;
    virtual std::string shaderSource() const = 0;
    /// Backend statistics of the last completed frame (GPU time in ms, items drawn, ...).
    virtual Json stats() const { return Json::object(); }
};

enum class RendererBackend { Auto, Metal, Null };
std::unique_ptr<Renderer> createRenderer(RendererBackend backend = RendererBackend::Auto);

/// Software fallback: draws the sky gradient and flat-shaded bounding silhouettes.
/// Useful on platforms without a GPU backend yet, and in CI.
std::unique_ptr<Renderer> createNullRenderer();

}  // namespace sky
