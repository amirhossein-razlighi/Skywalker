#pragma once
// Engine: the facade that owns every subsystem and defines the threading model.
//
// Threading: the Engine is single-threaded ("main thread"). Other threads — the MCP
// socket server, network callbacks — never touch engine state directly; they `post()`
// jobs which run in `pump()` on the main thread (called from `update()`). This keeps
// the core lock-free and deterministic while still allowing many concurrent agents.

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/core/Json.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/History.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Runtime.h"

namespace sky {

class SocketServer;

struct EngineConfig {
    RendererBackend renderer = RendererBackend::Auto;
    std::string projectDir = ".";
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
};

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
    Renderer& renderer() { return *renderer_; }
    ToolRegistry& tools() { return tools_; }
    OrbitCamera& camera() { return camera_; }
    const EngineConfig& config() const { return config_; }

    /// Runs a tool as `actor` and records an activity event. The main entry point for
    /// every client (MCP, in-editor agents, editor UI).
    ToolResult callTool(std::string_view name, const Json& args, const std::string& actor);

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

    // --- Scene files & assets -------------------------------------------------------
    Status newScene(const std::string& name, bool withDefaults);
    Status loadScene(const std::string& path);
    Status saveScene(const std::string& path);
    const std::string& scenePath() const { return scenePath_; }
    std::string resolvePath(const std::string& path) const;
    Result<std::string> importMesh(const std::string& path);
    std::vector<AssetRequest>& assetRequests() { return assetRequests_; }
    AssetRequest& addAssetRequest(AssetRequest req);

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

    EngineConfig config_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<History> history_;
    std::unique_ptr<wander::Runtime> runtime_;
    std::unique_ptr<Renderer> renderer_;
    ToolRegistry tools_;
    OrbitCamera camera_;

    PlayState playState_ = PlayState::Editing;
    Json playSnapshot_;
    double accumulator_ = 0;
    wander::InputState input_;
    std::deque<Json> messages_;

    std::vector<EntityId> selection_;
    std::string scenePath_;
    std::vector<AssetRequest> assetRequests_;
    uint64_t nextAssetRequest_ = 1;

    struct Drag {
        EntityId entity = kNoEntity;
        float planeY = 0;
        Vec3 offset;
    } drag_;

    std::deque<Json> events_;
    std::mutex jobsMutex_;
    bool shuttingDown_ = false;  // guarded by jobsMutex_
    std::deque<std::pair<std::function<Json()>, std::promise<Json>>> jobs_;
    std::unique_ptr<SocketServer> server_;
};

void registerEngineTools(Engine& engine);

}  // namespace sky
