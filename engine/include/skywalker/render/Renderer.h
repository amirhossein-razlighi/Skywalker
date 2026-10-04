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
#include "skywalker/render/FxItems.h"
#include "skywalker/render/Image.h"
#include "skywalker/render/ProbeItems.h"
#include "skywalker/render/Render2D.h"
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
    float tiltShift = 0.f;      // 0..1 miniature blur outside a sharp horizontal band
    uint32_t cullMask = 0xFFFFF;  // render layers this view draws (Camera.cullMask; editor views see all)

    Mat4 view() const { return Mat4::lookAt(eye, target, up); }
    /// Points the camera from `eye` at `target` with clip planes suited to any view distance
    /// (custom captures and cinematics: close-ups and kilometer-scale vistas alike).
    void lookFrom(Vec3 e, Vec3 t);
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

enum class Shading : uint8_t { Pbr = 0, Toon = 1, Unlit = 2, Water = 3, Skin = 4, Eye = 5, Cloth = 6, HairCard = 7 };
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
    // Car paint (material assets): the lacquer's roughness and metallic flakes under it.
    float clearcoatRoughness = 0.06f;
    float flakes = 0.f;          // 0..1 share of the base reflection from flakes
    float flakeSize = 0.0015f;   // meters (object space)
    bool textureAlphaOnly = false;  // use the base-color texture for its alpha (cut-out) only (clay renders)
    /// Character material models (skin, eye, cloth, hair_card), packed as the shaders read them
    /// (DrawUniforms.character[3]; colors already linear). See toSurface() in assets/Material.cpp.
    Vec4 model[3] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    Vec2 uvScroll{0, 0};      // UV units per second, applied with the frame time (flowing lava, rivers)
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
    uint32_t layers = 1;  // render layers (MeshRenderer.layers); lights only light draws sharing a bit with their mask
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
    // Light v2 (Light component fields, resolved): see docs/RENDERING.md "Lights".
    uint32_t mask = 0xFFFFF;  // render layers lit (Light.cullMask)
    float specular = 1.f;
    float indirect = 1.f;
    float volumetric = 1.f;
    float cosInner = 0.f;    // spot inner cone (0 = automatic soft edge)
    bool inverseSquare = false;
    float size = 0.1f;       // emitter radius (inverse square)
    bool negative = false;   // subtracts light
    // Local shadows (point / spot; render/ShadowAtlas.h).
    uint64_t id = 0;                // stable identity across frames (lightId); 0 = none (no shadow caching)
    bool shadows = false;
    int shadowMode = 0;             // point lights: 0 cube, 1 dual paraboloid
    float shadowBias = 0.02f;       // m
    float shadowNormalBias = 1.f;   // shadow-map texels
    int shadowResolution = 0;       // slot size hint (px), 0 = from screen coverage
    float shadowMaxDistance = 0.f;  // no shadow beyond this camera distance (m), 0 = no limit
};
/// Stable light identity: the entity, tagged by what cast the light (0 light component, 1 fluid
/// fire, 2 CPU particles, 3 GPU particles).
constexpr uint64_t lightId(EntityId entity, int source) { return (static_cast<uint64_t>(source) << 56) | entity; }

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
    float macroVariation = 0.f;
    float detail = 1.f;
    bool castShadows = true;
    bool selected = false;
    std::string overlay;          // absolute path of the draped map image (empty = none)
    float overlayOpacity = 1.f;
    int overlayBlend = 0;         // 0 = mix, 1 = multiply, 2 = glow
};

/// One drawable part of an instanced model: a mesh, its surface and its transform inside the
/// model (identity for single meshes; trunk, branches, leaves... of a prefab).
struct InstancePart {
    std::string mesh;
    Surface surface;  // absolute texture paths
    Mat4 local;
};

/// A model drawn as an octahedral impostor beyond its transition distance (see
/// render/Impostor.h). Baked lazily by the backend, cached in the project under `cachePath`.
struct ImpostorModel {
    std::string key;        // impostor::cacheKey: stable across runs and machines
    std::string cachePath;  // absolute .skyimp file ("" = memory only)
    std::string label;      // what it is (layer name, prefab), for tools and logs
    std::shared_ptr<const std::vector<InstancePart>> parts;
    std::string source;     // parts as named in the project (mesh keys, materials, transforms): key input
    std::string stamp;      // size/mtime of the source files: key input (edits rebake)
    Aabb bounds;            // model space, all parts
    int frames = 12;        // views per atlas side
    int resolution = 1024;  // requested atlas edge (px); see impostor::tileSize
    bool hemi = true;       // hemi-octahedral (upright vegetation) or full octahedral
};

/// A chunk of GPU-instanced foliage: every part of one model, sharing the instance transforms.
/// The backend culls and picks a level of detail per instance on the GPU: mesh LODs near the
/// camera, a dithered crossfade, then the impostor out to the cull distance.
struct InstanceBatch {
    EntityId entity = kNoEntity;
    uint64_t id = 0;  // stable while the instance data is unchanged (GPU buffer caching)
    std::shared_ptr<const std::vector<InstancePart>> parts;
    std::shared_ptr<const std::vector<world::FoliageInstance>> instances;
    Aabb bounds;
    bool castShadows = true;
    float wind = 1.f;           // bend strength
    float cullDistance = 100.f;
    float meshHeight = 1.f;     // height of the model (m) for wind bending
    float maxScale = 1.f;       // largest instance scale (LOD selection)
    Aabb modelBounds{Vec3(-0.5f, 0.f, -0.5f), Vec3(0.5f, 1.f, 0.5f)};  // model space, all parts (instance culling)
    int impostor = -1;          // index into FrameData::impostors (-1 = meshes only)
    float impostorDistance = 0.f;  // camera distance where instances become impostors (0 = never)
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
    std::vector<ImpostorModel> impostors;  // models referenced by InstanceBatch::impostor
    std::vector<SkinItem> skins;  // animation: skinned draws (see SkinItem)
    std::vector<GpuEmitterItem> gpuEmitters;  // GPU-simulated particles (hair & VFX workstream)
    std::vector<GroomItem> grooms;            // strand hair and fur
    std::vector<ProbeItem> probes;            // reflection probes (render/ReflectionProbes.h)
    bool drawGrid = true;
    float time = 0;
    Frame2D render2d;  // sprites, tilemaps, world text, 2D lights and UI (see Render2D.h)
    /// Scene transition over the whole picture, UI included (docs/SCENE_FLOW.md): blend toward `color` by
    /// `alpha` (fades); `crossfade` > 0 blends the last frame of the previous scene over this one by that amount
    /// (Metal keeps the frame it showed when crossfade first goes above 0; the CPU renderer cuts).
    struct ScreenFade {
        Vec4 color{0.f, 0.f, 0.f, 1.f};
        float alpha = 0.f;
        float crossfade = 0.f;
    } fade;
    /// Jittered sub-samples accumulated into this frame (stills and cinematics: supersampling,
    /// noise-free GI). 1 = real-time (temporal anti-aliasing across frames).
    int samples = 1;
    /// Discards temporal history (camera cuts). Large camera jumps are detected automatically.
    bool resetHistory = false;
    /// Buffer visualization / shading override instead of the final image (render/DebugViews.h:
    /// 0 off, 1 albedo, 2 normals, 3 roughness/metallic, 4 GI, 5 reflections, 6 AO, 7 depth,
    /// 8 lighting before GI, 9 sketch, 10 impostors, 11 wireframe, 12 overdraw, 13 unshaded,
    /// 14 lighting_only, 15 shadow_cascades, 16 light_complexity, 17 lod, 18 emission, 19 specular,
    /// 20 uv_checker, 21 texel_density, 22 motion (velocity buffer: hue = direction, brightness = speed),
    /// 23 shadow_atlas (the local point / spot shadow atlas, one outline per light view), 24 reflection_probes
    /// (which probe lights each pixel, influence volumes and capture points)).
    int debugView = 0;
    /// Viewport quality: 0 full (play, captures), 1 balanced, 2 fast (editing a heavy world).
    /// Lower tiers pick coarser LODs and cheaper shadows; the engine also trims the environment.
    int quality = 0;
    /// Offline (movie) rendering, see docs/MOVIE_RENDER.md. Each render() is one independent
    /// sub-frame accumulated by the caller: no TAA history even at samples = 1, the sub-pixel
    /// jitter continues the sequence at `sampleOffset` (so sub-frames cover distinct positions),
    /// and auto exposure adapts by `exposureDt` seconds per render (temporally stable, no
    /// pumping) instead of converging instantly like a still. `resetHistory` (a cut) re-meters.
    struct Offline {
        bool enabled = false;
        int sampleOffset = 0;
        float exposureDt = 0.f;
    } offline;

    static constexpr int kDebugViewShadowAtlas = 23;  // debugView: the local shadow atlas (debugview::kShadowAtlas)
    static constexpr int kDebugViewReflectionProbes = 24;  // debugView: reflection probes (debugview::kReflectionProbes)
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
    /// Shader library and pipeline cache: {library: "metallib"|"source", shaderCompileMs, startupMs,
    /// pipelineCache: {...}} (see docs/RENDERING.md "Shader library and pipeline cache").
    Json shaders = Json::object();
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
    /// Lights cast by GPU effects (glowing GPU particles), from a recent frame (no stall).
    virtual std::vector<LightItem> effectLights() const { return {}; }
    /// Backend statistics of the last completed frame (GPU time in ms, items drawn, effects...).
    virtual Json stats() const { return Json::object(); }
    /// Per-pass GPU timing over the last frames (rolling 60-frame window): {supported, mode,
    /// frames, spanMs, sumMs, passes: [{pass, group, ms, avgMs, minMs, maxMs, count}], groups}.
    virtual Json passProfile() const { return Json::object({{"supported", false}, {"mode", "unsupported"}}); }
    /// Clears the pass timeline (e.g. before a benchmark).
    virtual void resetPassProfile() {}
    /// Local (point / spot) shadows of the last frame: atlas, slots, shadowed lights, budgets
    /// (shadow_atlas_info). `invalidate` makes every shadow re-render on the next frame.
    virtual Json localShadowInfo() const { return Json(); }
    virtual void invalidateLocalShadows() {}
    /// Reflection probes of the last frame: atlas, budgets, per-probe state, warnings (probe_info). The
    /// frame adds staleness and setup warnings. `invalidateReflectionProbes` re-captures one probe (0 = all).
    virtual Json reflectionProbeInfo(const FrameData* frame) const {
        (void)frame;
        return Json();
    }
    virtual void invalidateReflectionProbes(EntityId entity) { (void)entity; }
    /// The six faces of a probe's captured cubemap as one image (a horizontal cross, tonemapped), `mip`
    /// levels below its sharpest (rougher reflections). Needs a frame rendered with the probe ready.
    virtual Result<Image> reflectionProbeImage(EntityId entity, int mip) {
        (void)entity, (void)mip;
        return Error::make("unsupported", "probe images need a GPU renderer backend", "run on macOS (Metal)");
    }
    /// Bakes the impostors of these models now (or loads them from their cache files unless
    /// `force`). Returns one entry per model: key, label, atlas size, frames, tile, bake time,
    /// whether it came from the cache, and the cache path.
    virtual Result<Json> bakeImpostors(const std::vector<ImpostorModel>& models, bool force) {
        (void)models, (void)force;
        return Error::make("unsupported", "impostor baking needs a GPU renderer backend", "run on macOS (Metal)");
    }
};

enum class RendererBackend { Auto, Metal, Null };
std::unique_ptr<Renderer> createRenderer(RendererBackend backend = RendererBackend::Auto);

/// Software fallback: draws the sky gradient and flat-shaded bounding silhouettes.
/// Useful on platforms without a GPU backend yet, and in CI.
std::unique_ptr<Renderer> createNullRenderer();

}  // namespace sky
