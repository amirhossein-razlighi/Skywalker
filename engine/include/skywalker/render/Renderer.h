#pragma once
// Rendering front end.
//
//   Scene --(FrameBuilder, CPU, portable, tested)--> FrameData --(Renderer backend)--> pixels
//
// The backend interface is deliberately tiny so new GPUs/OSes (Vulkan, D3D12, WebGPU)
// only implement `Renderer`. Everything agents rely on — what is visible, where it is on
// screen, picking — is computed on the CPU from FrameData, so it behaves identically on
// every backend (and in headless tests).

#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"
#include "skywalker/scene/Scene.h"

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

struct DrawItem {
    EntityId entity = kNoEntity;
    std::string mesh;
    std::string texture;
    Mat4 model;
    Vec4 color;
    Vec4 emissive;
    float metallic = 0;
    float roughness = 0.5f;
    bool selected = false;
    Aabb worldBounds;
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

struct FrameData {
    int width = 0;
    int height = 0;
    ViewCamera camera;
    Mat4 view;
    Mat4 projection;
    Environment environment;
    std::vector<DrawItem> draws;
    std::vector<LightItem> lights;  // up to kMaxLights are used
    bool drawGrid = true;
    float time = 0;

    static constexpr size_t kMaxLights = 16;
    Mat4 viewProjection() const { return projection * view; }
};

struct BuildOptions {
    bool editorOverlays = true;  // grid, selection highlight
    std::vector<EntityId> selection;
    float time = 0;
};

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
    /// Replaces the shader source at runtime; returns compiler diagnostics on failure.
    virtual Status reloadShaders(const std::string& source) = 0;
    virtual std::string shaderSource() const = 0;
};

enum class RendererBackend { Auto, Metal, Null };
std::unique_ptr<Renderer> createRenderer(RendererBackend backend = RendererBackend::Auto);

/// Software fallback: draws the sky gradient and flat-shaded bounding silhouettes.
/// Useful on platforms without a GPU backend yet, and in CI.
std::unique_ptr<Renderer> createNullRenderer();

}  // namespace sky
