#include "skywalker/engine/Engine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/agent/SocketServer.h"
#include "skywalker/core/Log.h"
#include "skywalker/render/MeshData.h"

namespace sky {

namespace fs = std::filesystem;

const char* toString(PlayState s) {
    switch (s) {
        case PlayState::Editing: return "editing";
        case PlayState::Playing: return "playing";
        case PlayState::Paused: return "paused";
    }
    return "?";
}

Json AssetRequest::toJson() const {
    Json j = Json::object({{"id", id},
                           {"kind", kind},
                           {"prompt", prompt},
                           {"status", status},
                           {"requestedBy", requestedBy}});
    if (!style.empty()) j["style"] = style;
    if (target) j["target"] = target;
    if (!path.empty()) j["path"] = path;
    return j;
}

Engine::Engine(EngineConfig config)
    : config_(std::move(config)),
      scene_(std::make_unique<Scene>()),
      history_(std::make_unique<History>(*scene_)),
      runtime_(std::make_unique<wander::Runtime>(*scene_)),
      renderer_(createRenderer(config_.renderer)) {
    registerEngineTools(*this);
    log::info("engine", std::string("Skywalker ") + SKY_VERSION_STRING + " ready (renderer: " + renderer_->info().backend +
                            " " + renderer_->info().device + ")");
}

Engine::~Engine() {
    {
        // Refuse new jobs and release any thread waiting on a queued one, *then* join the
        // server threads; otherwise a connection thread could wait on a job never pumped.
        std::lock_guard lock(jobsMutex_);
        shuttingDown_ = true;
        for (auto& [job, promise] : jobs_) {
            promise.set_value(ToolResult::error(Error::make("engine_shutdown", "the engine is shutting down")).toMcp());
        }
        jobs_.clear();
    }
    stopAgentServer();
}

// ---------------------------------------------------------------------------
// Tools & transactions
// ---------------------------------------------------------------------------

ToolResult Engine::callTool(std::string_view name, const Json& args, const std::string& actor) {
    ToolContext ctx{actor};
    ToolResult result = tools_.call(name, args, ctx);
    const ToolDef* def = tools_.find(name);
    std::string summary = result.content.empty() ? "" : result.content.front().text.substr(0, 160);
    emitEvent(Json::object({{"type", "tool"},
                            {"actor", actor},
                            {"tool", std::string(name)},
                            {"ok", !result.isError},
                            {"mutates", def && def->mutates},
                            {"summary", summary}}));
    return result;
}

Status Engine::edit(const std::string& actor, const std::string& label, const std::function<Status()>& fn) {
    if (playState_ != PlayState::Editing) {
        // Live edits during play affect the running simulation only; they revert on stop.
        return fn();
    }
    if (history_->inTransaction()) return fn();  // joins the enclosing batch
    history_->begin(actor, label);
    Status s = fn();
    if (!s) {
        history_->rollback();
        return s;
    }
    if (history_->commit()) {
        Json ev = history_->lastCommitted()->summary();
        ev["type"] = "edit";
        emitEvent(std::move(ev));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void Engine::play() {
    if (playState_ == PlayState::Playing) return;
    if (playState_ == PlayState::Editing) {
        playSnapshot_ = scene_->toJson();
        runtime_->reset(/*keepQueuedEvents=*/true);
        accumulator_ = 0;
    }
    playState_ = PlayState::Playing;
    emitEvent(Json::object({{"type", "play_state"}, {"state", "playing"}}));
}

void Engine::pause() {
    if (playState_ != PlayState::Playing) return;
    playState_ = PlayState::Paused;
    emitEvent(Json::object({{"type", "play_state"}, {"state", "paused"}}));
}

void Engine::stop() {
    if (playState_ == PlayState::Editing) return;
    playState_ = PlayState::Editing;
    ChangeObserver* obs = scene_->observer();
    (void)scene_->loadJson(playSnapshot_);
    scene_->setObserver(obs);
    playSnapshot_ = Json();
    runtime_->reset();
    input_ = {};
    std::erase_if(selection_, [&](EntityId id) { return !scene_->exists(id); });
    emitEvent(Json::object({{"type", "play_state"}, {"state", "editing"}}));
}

void Engine::step(int ticks) {
    if (playState_ == PlayState::Editing) {
        play();
        pause();
    }
    for (int i = 0; i < ticks; ++i) {
        runtime_->tick(kFixedDt, input_);
        input_.pressed.clear();
        input_.clicked.clear();
    }
    for (auto& m : runtime_->drainMessages()) {
        Json j = m.toJson();
        messages_.push_back(j);
        j["type"] = "log";
        emitEvent(std::move(j));
    }
    while (messages_.size() > 500) messages_.pop_front();
}

void Engine::update(double seconds) {
    pump();
    if (playState_ != PlayState::Playing) return;
    accumulator_ += std::min(seconds, 0.25);  // avoid spiral of death after stalls
    int ticks = 0;
    while (accumulator_ >= kFixedDt) {
        accumulator_ -= kFixedDt;
        ++ticks;
    }
    if (ticks) step(ticks);
}

std::vector<Json> Engine::recentMessages(size_t max) const {
    std::vector<Json> out;
    size_t start = messages_.size() > max ? messages_.size() - max : 0;
    for (size_t i = start; i < messages_.size(); ++i) out.push_back(messages_[i]);
    return out;
}

void Engine::setSelection(std::vector<EntityId> ids, const std::string& actor) {
    std::erase_if(ids, [&](EntityId id) { return !scene_->exists(id); });
    if (ids == selection_) return;
    selection_ = std::move(ids);
    Json arr = Json::array();
    for (EntityId id : selection_) arr.push(id);
    emitEvent(Json::object({{"type", "selection"}, {"actor", actor}, {"entities", arr}}));
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Engine::ensureMeshUploaded(const std::string& meshKey) {
    if (meshKey.rfind("asset:", 0) != 0 || scene_->assetBounds.count(meshKey)) return;
    auto mesh = mesh::loadObj(resolvePath(meshKey.substr(6)));
    if (!mesh) {
        log::warn("asset", mesh.error().message);
        scene_->assetBounds[meshKey] = {Vec3(-0.5f), Vec3(0.5f)};  // don't retry every frame
        return;
    }
    (void)renderer_->uploadMesh(meshKey, mesh.value());
    scene_->assetBounds[meshKey] = mesh.value().bounds;
}

FrameData Engine::frame(const CaptureOptions& opts) {
    ViewCamera view = camera_.toView();
    if (opts.hasCustomView) {
        view = opts.customView;
    } else if (opts.useSceneCamera || opts.cameraEntity) {
        ViewCamera sc;
        if (sceneCamera(*scene_, sc, opts.cameraEntity)) view = sc;
    } else if (playState_ != PlayState::Editing) {
        ViewCamera sc;
        if (sceneCamera(*scene_, sc)) view = sc;
    }
    BuildOptions bo;
    bo.editorOverlays = opts.editorOverlays && playState_ == PlayState::Editing;
    bo.selection = selection_;
    bo.time = static_cast<float>(runtime_->time());
    for (EntityId e : scene_->entities()) {
        if (const auto* m = scene_->get<MeshRenderer>(e)) ensureMeshUploaded(m->mesh);
    }
    FrameData f = buildFrame(*scene_, view, opts.width, opts.height, bo);
    for (auto& d : f.draws) {
        if (!d.texture.empty()) d.texture = resolvePath(d.texture);
    }
    return f;
}

Result<Capture> Engine::capture(const CaptureOptions& opts) {
    Capture c;
    c.frame = frame(opts);
    if (Status s = renderer_->render(c.frame); !s) return s.error();
    auto img = renderer_->readback();
    if (!img) return img.error();
    c.image = std::move(img.value());
    c.visible = visibleEntities(*scene_, c.frame);
    if (opts.annotate) annotate(c.image, c.visible);
    return c;
}

Status Engine::renderToSurface(void* surface, int width, int height) {
    CaptureOptions opts;
    opts.width = width;
    opts.height = height;
    FrameData f = frame(opts);
    if (Status s = renderer_->render(f); !s) return s;
    return renderer_->present(surface);
}

EntityId Engine::pickAt(float x, float y, int width, int height) {
    CaptureOptions opts;
    opts.width = width;
    opts.height = height;
    return pick(*scene_, frame(opts), x, y);
}

void Engine::beginDrag(EntityId id, float x, float y, int width, int height) {
    if (!scene_->exists(id) || playState_ != PlayState::Editing) return;
    endDrag();
    Vec3 pos = scene_->worldMatrix(id).translation();
    Ray ray = camera_.toView().rayAt(x, y, width, height);
    drag_.entity = id;
    drag_.planeY = pos.y;
    float t = std::fabs(ray.dir.y) > 1e-5f ? (pos.y - ray.origin.y) / ray.dir.y : -1.f;
    drag_.offset = t > 0 ? pos - (ray.origin + ray.dir * t) : Vec3{0, 0, 0};
    history_->begin("user", "Move " + scene_->record(id)->name);
}

void Engine::dragTo(float x, float y, int width, int height) {
    if (!drag_.entity || !scene_->exists(drag_.entity)) return;
    Ray ray = camera_.toView().rayAt(x, y, width, height);
    if (std::fabs(ray.dir.y) < 1e-5f) return;
    float t = (drag_.planeY - ray.origin.y) / ray.dir.y;
    if (t <= 0) return;
    Vec3 world = ray.origin + ray.dir * t + drag_.offset;
    const EntityRecord* rec = scene_->record(drag_.entity);
    Vec3 local = rec->parent ? scene_->worldMatrix(rec->parent).inverse().transformPoint(world) : world;
    (void)scene_->patchComponent(drag_.entity, "transform", Json::object({{"position", reflect::vec3ToJson(local)}}));
}

void Engine::endDrag() {
    if (!drag_.entity) return;
    drag_ = {};
    if (history_->inTransaction() && history_->commit()) {
        Json ev = history_->lastCommitted()->summary();
        ev["type"] = "edit";
        emitEvent(std::move(ev));
    }
}

// ---------------------------------------------------------------------------
// Scene files & assets
// ---------------------------------------------------------------------------

std::string Engine::resolvePath(const std::string& path) const {
    if (path.empty()) return path;
    fs::path p(path);
    if (p.is_absolute()) return p.string();
    if (path[0] == '~') {
        const char* home = std::getenv("HOME");
        return (fs::path(home ? home : "") / path.substr(path.size() > 1 ? 2 : 1)).string();
    }
    return (fs::path(config_.projectDir) / p).lexically_normal().string();
}

Status Engine::newScene(const std::string& name, bool withDefaults) {
    if (playState_ != PlayState::Editing) stop();
    scene_->clear();
    history_->clear();
    selection_.clear();
    scene_->name = name.empty() ? "Untitled" : name;
    scenePath_.clear();
    if (withDefaults) {
        EntityId ground = scene_->create("Ground");
        (void)scene_->applyEntityJson(ground, Json::parse(R"({"tags":["static"],"components":{
            "transform":{"scale":[40,1,40]},
            "mesh":{"mesh":"plane","color":"#8a9a80","roughness":0.9}}})").value());
        EntityId cube = scene_->create("Cube");
        (void)scene_->applyEntityJson(cube, Json::parse(R"({"components":{
            "transform":{"position":[0,0.5,0]},
            "mesh":{"mesh":"cube","color":"#f2a65a","roughness":0.45}}})").value());
        EntityId cam = scene_->create("Main Camera");
        (void)scene_->applyEntityJson(cam, Json::parse(R"({"components":{
            "transform":{"position":[0,3,8],"rotation":[-15,0,0]},
            "camera":{"fov":55}}})").value());
    }
    camera_ = OrbitCamera{};
    emitEvent(Json::object({{"type", "scene"}, {"action", "new"}, {"name", scene_->name}}));
    return {};
}

Status Engine::loadScene(const std::string& path) {
    std::string full = resolvePath(path);
    std::ifstream f(full);
    if (!f) return Error::make("io_error", "cannot open scene " + full);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc) return doc.error();
    if (playState_ != PlayState::Editing) stop();
    Status s = scene_->loadJson(doc.value());
    history_->clear();
    selection_.clear();
    scenePath_ = path;
    emitEvent(Json::object({{"type", "scene"}, {"action", "load"}, {"name", scene_->name}, {"path", path}}));
    return s;
}

Status Engine::saveScene(const std::string& path) {
    if (playState_ != PlayState::Editing) {
        return Error::make("invalid_state", "stop the simulation before saving", "call sim_control with action \"stop\"");
    }
    std::string target = path.empty() ? scenePath_ : path;
    if (target.empty()) return Error::make("invalid_arguments", "no path given and the scene was never saved");
    std::string full = resolvePath(target);
    std::error_code ec;
    fs::create_directories(fs::path(full).parent_path(), ec);
    std::ofstream f(full);
    if (!f) return Error::make("io_error", "cannot write " + full);
    f << scene_->toJson().dump(2) << "\n";
    scenePath_ = target;
    emitEvent(Json::object({{"type", "scene"}, {"action", "save"}, {"path", target}}));
    return {};
}

Result<std::string> Engine::importMesh(const std::string& path) {
    auto mesh = mesh::loadObj(resolvePath(path));
    if (!mesh) return mesh.error();
    std::string key = "asset:" + path;
    if (Status s = renderer_->uploadMesh(key, mesh.value()); !s) return s.error();
    scene_->assetBounds[key] = mesh.value().bounds;
    return key;
}

AssetRequest& Engine::addAssetRequest(AssetRequest req) {
    req.id = nextAssetRequest_++;
    assetRequests_.push_back(std::move(req));
    emitEvent(Json::object({{"type", "asset_request"}, {"request", assetRequests_.back().toJson()}}));
    return assetRequests_.back();
}

// ---------------------------------------------------------------------------
// Events & jobs
// ---------------------------------------------------------------------------

void Engine::emitEvent(Json event) {
    events_.push_back(std::move(event));
    while (events_.size() > 2000) events_.pop_front();  // bounded: never grows without a reader
}

std::vector<Json> Engine::drainEvents() {
    std::vector<Json> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_.clear();
    return out;
}

std::future<Json> Engine::post(std::function<Json()> job) {
    std::promise<Json> promise;
    std::future<Json> future = promise.get_future();
    std::lock_guard lock(jobsMutex_);
    if (shuttingDown_) {
        promise.set_value(ToolResult::error(Error::make("engine_shutdown", "the engine is shutting down")).toMcp());
        return future;
    }
    jobs_.emplace_back(std::move(job), std::move(promise));
    return future;
}

void Engine::pump() {
    std::deque<std::pair<std::function<Json()>, std::promise<Json>>> jobs;
    {
        std::lock_guard lock(jobsMutex_);
        jobs.swap(jobs_);
    }
    for (auto& [job, promise] : jobs) promise.set_value(job());
}

// ---------------------------------------------------------------------------
// Agent server
// ---------------------------------------------------------------------------

std::string Engine::defaultSocketPath() {
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : "/tmp") / ".skywalker" / "editor.sock").string();
}

Status Engine::startAgentServer(const std::string& socketPath) {
    stopAgentServer();
    std::error_code ec;
    fs::create_directories(fs::path(socketPath).parent_path(), ec);
    auto server = std::make_unique<SocketServer>(
        tools_, [this](const std::string& tool, const Json& args, const std::string& actor) {
            // Called on a connection thread: hop to the main thread and wait.
            std::future<Json> f = post([this, tool, args, actor] { return callTool(tool, args, actor).toMcp(); });
            if (f.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
                return ToolResult::error(Error::make("timeout", "the editor did not respond in time")).toMcp();
            }
            return f.get();
        });
    if (Status s = server->start(socketPath); !s) return s;
    server_ = std::move(server);
    log::info("agent", "agent server listening on " + socketPath);
    return {};
}

void Engine::stopAgentServer() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

bool Engine::agentServerRunning() const { return server_ != nullptr; }

}  // namespace sky
