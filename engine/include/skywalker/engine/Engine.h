#pragma once
// Engine: the facade that owns every subsystem and defines the threading model.
//
// Threading: the Engine is single-threaded ("main thread"). Other threads — the MCP
// socket server, network callbacks — never touch engine state directly; they `post()`
// jobs which run in `pump()` on the main thread (called from `update()`). This keeps
// the core lock-free and deterministic while still allowing many concurrent agents.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/assets/Material.h"
#include "skywalker/assets/Prefab.h"
#include "skywalker/audio/AudioSystem.h"
#include "skywalker/core/Json.h"
#include "skywalker/engine/Gizmo.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/fx/Ocean.h"
#include "skywalker/fx/Particles.h"
#include "skywalker/input/ActionMap.h"
#include "skywalker/nav/NavSystem.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/History.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Runtime.h"
#include "skywalker/world/WorldRuntime.h"

namespace sky {

class SocketServer;
class World2D;
class NativeModules;
namespace studio {
class Studio;
}
namespace movie {
struct Options;
class Job;
}

/// Registers the engine's Wander builtins (effects, water, and every subsystem's) in the
/// global registry. Idempotent; the Engine constructor calls it.
void registerEngineBuiltins();

struct EngineConfig {
    RendererBackend renderer = RendererBackend::Auto;
    std::string projectDir = ".";
    audio::AudioMode audio = audio::AudioMode::Auto;  // Null for tests/CI; SKYWALKER_AUDIO overrides Auto
};

enum class PlayState { Editing, Playing, Paused };
const char* toString(PlayState s);

/// A request for generated content (3D mesh, texture, sprite, audio, music, video),
/// fulfilled by a generator provider or an agent. See docs/AGENTS.md.
struct AssetRequest {
    uint64_t id = 0;
    std::string kind;    // mesh | texture | sprite | audio | music | video
    std::string prompt;  // natural-language description
    std::string style;
    EntityId target = kNoEntity;
    std::string status = "pending";  // pending | done | failed
    std::string path;                // result file (project-relative)
    std::string requestedBy;
    Json toJson() const;
};

struct CaptureOptions {
    int width = 960;
    int height = 540;
    bool useSceneCamera = false;
    EntityId cameraEntity = kNoEntity;
    bool hasCustomView = false;
    ViewCamera customView;
    bool editorOverlays = true;
    bool annotate = false;  // draw numbered entity boxes ("set-of-mark" prompting)
    bool fog = true;        // false for analysis views (orthographic/top-down)
    /// Jittered sub-samples accumulated per capture (supersampled, noise-free GI/reflections).
    /// 1 = a single real-time frame (temporal AA uses history from previous frames).
    int samples = 4;
    int debugView = 0;  // see FrameData::debugView
    bool clay = false;  // every surface matte white clay (look-dev of form and light; "sketch to fill" films)
    int quality = 0;    // FrameData::quality: 0 full, 1 balanced, 2 fast
    // Movie sub-frames (docs/MOVIE_RENDER.md): FrameData::offline; a real shutter is accumulated by the
    // caller, so the post-process motion blur is off. resetHistory marks a cut.
    FrameData::Offline offline;
    bool resetHistory = false;
    bool listVisible = true;  // compute Capture::visible (skipped by movie frames)
};

/// How the live editor viewport trades quality for responsiveness while editing. Play mode
/// and captures always render at full quality.
enum class ViewportQuality { Full = 0, Balanced = 1, Fast = 2 };

struct Capture {
    Image image;
    FrameData frame;
    std::vector<VisibleEntity> visible;
};

class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Scene& scene() { return *scene_; }
    History& history() { return *history_; }
    wander::Runtime& runtime() { return *runtime_; }
    /// Wander builtins of this engine: the global registry plus native-module builtins.
    wander::BuiltinRegistry& builtins() { return *builtins_; }
    /// Native C++ modules and AOT-compiled behaviors (skywalker/native/NativeModules.h).
    NativeModules& native() { return *native_; }
    Renderer& renderer() { return *renderer_; }
    anim::AnimationSystem& animation() { return *animation_; }
    ToolRegistry& tools() { return tools_; }
    OrbitCamera& camera() { return camera_; }
    const EngineConfig& config() const { return config_; }

    /// Runs a tool as `actor` and records an activity event. The main entry point for
    /// every client (MCP, in-editor agents, editor UI).
    ToolResult callTool(std::string_view name, const Json& args, const std::string& actor);

    /// Non-blocking variant of callTool() for callers that must keep running while a slow tool
    /// works (the editor's agents): beginTool() runs the quick part on the main thread. If the tool
    /// deferred slow work (`call.result.deferred`), run `call.result.deferred->work()` on any thread
    /// and then call finishTool() on the main thread; otherwise `call.result` is already final.
    struct PendingCall {
        std::string tool;
        std::string actor;
        ToolResult result;
    };
    PendingCall beginTool(std::string_view name, const Json& args, const std::string& actor);
    ToolResult finishTool(PendingCall& call);

    /// Runs `fn` as one undoable, attributed transaction. Rolls back on failure. Nested
    /// calls join the outer transaction (used by `batch`).
    Status edit(const std::string& actor, const std::string& label, const std::function<Status()>& fn);

    // --- Simulation -------------------------------------------------------------
    static constexpr float kFixedDt = 1.f / 60.f;
    PlayState playState() const { return playState_; }
    void play();
    void pause();
    void stop();
    void step(int ticks);
    /// Advance real time: pumps posted jobs and runs fixed simulation steps when playing.
    void update(double seconds);
    wander::InputState& input() { return input_; }
    std::vector<Json> recentMessages(size_t max = 50) const;

    // --- Selection ------------------------------------------------------------------
    const std::vector<EntityId>& selection() const { return selection_; }
    void setSelection(std::vector<EntityId> ids, const std::string& actor);

    // --- Rendering --------------------------------------------------------------------
    FrameData frame(const CaptureOptions& opts);
    Result<Capture> capture(const CaptureOptions& opts);
    Status renderToSurface(void* surface, int width, int height);
    EntityId pickAt(float x, float y, int width, int height);
    void beginDrag(EntityId id, float x, float y, int width, int height);
    void dragTo(float x, float y, int width, int height);
    void endDrag();

    // --- Transform gizmo (editor) ---------------------------------------------------
    Gizmo& gizmo() { return gizmo_; }
    /// Editor viewport looks through the scene's primary camera instead of the orbit camera.
    void setViewThroughSceneCamera(bool on) { viewSceneCamera_ = on; }
    void setViewportQuality(ViewportQuality q) { editQuality_ = q; }
    ViewportQuality viewportQuality() const { return editQuality_; }
    /// Debug view of the live editor viewport (render/DebugViews.h id; 0 = final image).
    void setViewportDebugView(int view) { viewportDebugView_ = view; }
    int viewportDebugView() const { return viewportDebugView_; }
    bool viewThroughSceneCamera() const { return viewSceneCamera_; }
    /// Highlights the handle under the cursor. Returns the axis or -1.
    int gizmoHover(float x, float y, int width, int height);
    /// Starts dragging a handle if one is under the cursor (opens an undo transaction).
    bool gizmoBegin(float x, float y, int width, int height);
    void gizmoDrag(float x, float y, int width, int height, bool snapping);
    void gizmoEnd();

    struct FrameStats {
        double cpuMs = 0;
        size_t draws = 0;
        size_t lights = 0;
        size_t entities = 0;
    };
    const FrameStats& stats() const { return stats_; }

    // --- Scene files & assets -------------------------------------------------------
    Status newScene(const std::string& name, bool withDefaults);
    Status loadScene(const std::string& path);
    Status saveScene(const std::string& path);
    const std::string& scenePath() const { return scenePath_; }
    std::string resolvePath(const std::string& path) const;
    Result<std::string> importMesh(const std::string& path);

    // --- Asset system ------------------------------------------------------------------
    AssetDatabase& assets() { return *assets_; }
    /// Rescans the project; drops renderer/material/prefab caches of changed files.
    std::vector<std::string> refreshAssets();
    /// Imports a mesh file (.obj/.glb/.gltf). glTF base color + texture become a material
    /// asset next to the mesh. Returns {"mesh": "asset:...", "material": "...?"}.
    struct MeshImportOptions {
        bool normalize = true;  // fit a 1 m cube (handy for generated models); false keeps real units
        bool zUp = false;       // source is Z-up (CAD, scans, some exporters)
        bool keepRiggedScale = true;  // rigged characters keep their real size unless normalize was asked for explicitly
    };
    Result<Json> importMeshAsset(const std::string& path, const MeshImportOptions& options);
    Result<Json> importMeshAsset(const std::string& path) { return importMeshAsset(path, MeshImportOptions{}); }
    const ResolvedMaterial* resolveMaterial(const std::string& path);
    Result<Json> loadPrefabAsset(const std::string& path);
    Result<EntityId> instantiatePrefabAsset(const std::string& path, const PrefabPlacement& placement);
    /// Renders an isolated preview of an asset (mesh, material, texture, prefab) to an image.
    Result<Image> assetPreview(const std::string& ref, int size = 256);
    /// Rewrites references to `from` in the scene (mesh, texture, material fields).
    size_t rewriteAssetReferences(const std::string& from, const std::string& to);
    /// Entities that reference an asset.
    std::vector<EntityId> assetUsage(const std::string& path) const;

    // --- Spatial queries -------------------------------------------------------------------
    struct Hit {
        EntityId entity = kNoEntity;
        Vec3 point;
        Vec3 normal;
        float distance = 0;
    };
    /// Precise ray cast against rendered meshes (triangle-accurate). `exclude` skips entities.
    std::optional<Hit> raycast(const Ray& ray, const std::vector<EntityId>& exclude = {});
    /// CPU copy of a mesh ("cube", "asset:...") for spatial queries.
    const MeshData* cpuMesh(const std::string& key);
    std::vector<AssetRequest>& assetRequests() { return assetRequests_; }
    AssetRequest& addAssetRequest(AssetRequest req);

    // --- Effects: particles and water -----------------------------------------------
    fx::ParticleSystem& particles() { return particles_; }
    /// 2D, text, UI and dialogue (sprites, tilemaps, 2D lights, canvases, conversations).
    World2D& world2d() { return *world2d_; }
    fx::GroomSystem& grooms() { return grooms_; }  // hair & fur (generated grooms, cached)
    /// Seconds on the effects clock: simulation time while playing, a live preview clock while editing.
    double effectsTime() const;
    /// Height of the water surface at world (x, z), waves included. False if no water covers it.
    bool waterHeight(float x, float z, float& height, Vec3* normal = nullptr);

    // --- World building: terrain and foliage ----------------------------------------
    world::WorldRuntime& world() { return *world_; }
    // --- Audio & input (workstream S) -------------------------------------------------------
    audio::AudioSystem& audio() { return *audio_; }
    /// Project mixer settings (`audio.json`): bus volumes and mutes.
    Status setAudioMix(const audio::MixSettings& mix);
    /// The project's input actions (`input.json`, defaults when the file does not exist).
    const input::ActionMap& actionMap() const { return actionMap_; }
    Status setActionMap(input::ActionMap map);
    /// Re-reads input.json / audio.json if they changed on disk (called periodically).
    void reloadProjectSettings(bool force = false);
    // --- Platform requests from the game (honored by the player runtime, ignored by the editor) ----------
    /// `cursor_lock(true)` in Wander: the game wants a hidden, captured mouse (first-person look). Cleared on stop.
    bool cursorLocked() const { return cursorLocked_; }
    void setCursorLocked(bool on) { cursorLocked_ = on; }
    /// `quit_game()` in Wander: the game asks to close (a "Quit" menu button). The player exits.
    bool quitRequested() const { return quitRequested_; }
    void requestQuit() { quitRequested_ = true; }
    void clearQuitRequest() { quitRequested_ = false; }
    // --- Studio (multi-agent roster, board, feedback, loops; docs/STUDIO.md) -----------
    /// Created on first use from the project's agents/ and studio/ folders. Main thread only.
    studio::Studio& studio();
    bool hasStudio() const { return studio_ != nullptr; }
    // --- Physics & navigation (Jolt, Recast/Detour) -------------------------------------
    physics::PhysicsSystem& physics() { return *physics_; }
    nav::NavSystem& navigation() { return *nav_; }

    // --- Movie render queue (docs/MOVIE_RENDER.md) ------------------------------------------
    /// Renders a movie to completion on this thread; `progress` (and "movie_progress" events) report each frame.
    Result<Json> renderMovie(const movie::Options& options, const std::function<void(const Json&)>& progress = {});
    /// Starts a movie render that update() advances one sub-frame at a time (the editor stays live).
    Status startMovie(const movie::Options& options);
    /// Thread-safe: the running movie render stops after the current sub-frame (its outputs stay valid).
    void cancelMovie() { movieCancel_ = true; }
    bool movieRendering() const { return movie_ != nullptr; }
    /// The running (or last) movie render: progress, or its summary.
    Json movieStatus() const;
    /// Effects clock override for movie sub-frames (water, sky, GPU effects); nullopt = the normal clock.
    void setEffectsTimeOverride(std::optional<double> seconds) { effectsTimeOverride_ = seconds; }

    // --- Events (activity feed) -----------------------------------------------------
    void emitEvent(Json event);
    std::vector<Json> drainEvents();

    // --- Cross-thread jobs ------------------------------------------------------------
    /// Thread-safe. The job runs on the main thread during the next pump().
    std::future<Json> post(std::function<Json()> job);
    void pump();

    // --- Agent server (MCP over a Unix socket, for attaching external agents) ----------
    Status startAgentServer(const std::string& socketPath);
    void stopAgentServer();
    bool agentServerRunning() const;
    static std::string defaultSocketPath();

private:
    void ensureMeshUploaded(const std::string& meshKey);
    void frameSceneView();
    /// Real-time frames stream asset meshes: parsing and LOD building run on background
    /// threads and the mesh appears once uploaded (captures still load synchronously).
    void requestMeshAsync(const std::string& meshKey);
    void drainStreamedMeshes();
    std::optional<audio::ListenerPose> listenerPose();
    void resolveTexturePaths(FrameData& f) const;

    EngineConfig config_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<History> history_;
    std::unique_ptr<wander::BuiltinRegistry> builtins_;  // before runtime_ (it compiles against it)
    std::unique_ptr<wander::Runtime> runtime_;
    std::unique_ptr<NativeModules> native_;  // after builtins_/runtime_: unloads its builtins first
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<AssetDatabase> assets_;
    std::unique_ptr<anim::AnimationSystem> animation_;
    struct CachedMaterial {
        int64_t mtime = -1;
        double checkedAt = -1e9;  // material/prefab files are re-stat'ed at most once a second
        bool ok = false;
        ResolvedMaterial material;
    };
    std::unordered_map<std::string, CachedMaterial> materials_;
    struct CachedPrefab {
        int64_t mtime = -1;
        double checkedAt = -1e9;
        Json prefab;
    };
    std::unordered_map<std::string, CachedPrefab> prefabs_;
    std::unordered_map<std::string, std::shared_ptr<MeshData>> cpuMeshes_;
    struct MeshStream;                      // results handed back from loader threads
    std::shared_ptr<MeshStream> meshStream_;
    std::unordered_set<std::string> pendingMeshes_;
    bool streamMeshes_ = false;
    double assetScanTimer_ = 0;
    ToolRegistry tools_;
    OrbitCamera camera_;

    PlayState playState_ = PlayState::Editing;
    Json playSnapshot_;
    double accumulator_ = 0;
    fx::ParticleSystem particles_;
    std::unique_ptr<world::WorldRuntime> world_;
    fx::GroomSystem grooms_;
    std::unordered_map<EntityId, fx::Ocean> oceans_;
    double previewTime_ = 0;
    fx::Ocean& oceanFor(EntityId e, const Water& w);
    wander::InputState input_;
    input::ActionMap actionMap_ = input::ActionMap::defaults();
    std::unique_ptr<audio::AudioSystem> audio_;
    int64_t inputFileTime_ = -1, audioFileTime_ = -1;
    double settingsTimer_ = 0;
    std::deque<Json> messages_;
    std::unique_ptr<physics::PhysicsSystem> physics_;  // after scene_/runtime_; nav_ refers to it
    std::unique_ptr<nav::NavSystem> nav_;

    std::vector<EntityId> selection_;
    std::string scenePath_;
    std::vector<AssetRequest> assetRequests_;
    uint64_t nextAssetRequest_ = 1;

    bool gizmoTarget(EntityId& id, GizmoFrame& frame, int width, int height);
    void commitEditTransaction();

    Gizmo gizmo_;
    bool viewSceneCamera_ = false;
    ViewportQuality editQuality_ = ViewportQuality::Fast;
    int viewportDebugView_ = 0;  // [debug views] live viewport
    int gizmoHot_ = -1;
    std::optional<Gizmo::DragStart> gizmoDrag_;
    EntityId gizmoEntity_ = kNoEntity;
    FrameStats stats_;

    struct Drag {
        EntityId entity = kNoEntity;
        float planeY = 0;
        Vec3 offset;
    } drag_;

    bool cursorLocked_ = false;
    bool quitRequested_ = false;
    std::deque<Json> events_;
    std::mutex jobsMutex_;
    bool shuttingDown_ = false;   // guarded by jobsMutex_
    bool acceptingJobs_ = true;   // guarded by jobsMutex_
    void failQueuedJobsLocked(const std::string& why);
    void recordToolEvent(std::string_view name, const ToolResult& result, const std::string& actor);
    Json callToolFromConnection(const std::string& tool, const Json& args, const std::string& actor);
    std::mutex workMutex_;
    std::vector<std::shared_ptr<DeferredWork>> activeWork_;  // deferred tool work running on connection threads
    int editDepth_ = 0;
    std::deque<std::pair<std::function<Json()>, std::promise<Json>>> jobs_;
    std::unique_ptr<SocketServer> server_;
    std::unique_ptr<World2D> world2d_;
    std::unique_ptr<studio::Studio> studio_;
    // Movie render queue (Movie.cpp)
    std::unique_ptr<movie::Job> movie_;
    Json lastMovie_;
    std::atomic<bool> movieCancel_{false};
    std::optional<double> effectsTimeOverride_;
};

void registerEngineTools(Engine& engine);

}  // namespace sky
