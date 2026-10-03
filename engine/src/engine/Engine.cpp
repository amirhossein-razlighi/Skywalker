#include "skywalker/engine/Engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

#include "skywalker/agent/SocketServer.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/assets/Prefab.h"
#include "skywalker/render/Gltf.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/ui/World2D.h"

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
      renderer_(createRenderer(config_.renderer)),
      assets_(std::make_unique<AssetDatabase>(config_.projectDir)) {
    assets_->refresh();
    runtime_->spawnPrefab = [this](const std::string& ref, Vec3 position, const std::string& name) -> Result<EntityId> {
        PrefabPlacement placement;
        placement.hasPosition = true;
        placement.position = position;
        placement.name = name;
        return instantiatePrefabAsset(ref, placement);
    };
    runtime_->burst = [this](EntityId e, int count) { particles_.burst(e, count); };
    runtime_->waterHeight = [this](float x, float z) {
        float h = 0;
        return waterHeight(x, z, h) ? h : 0.f;
    };
    world2d_ = std::make_unique<World2D>(config_.projectDir);
    runtime_->external = [this](const std::string& fn, const std::vector<Json>& args, EntityId self) {
        return world2d_->callBuiltin(*scene_, *runtime_, fn, args, self);
    };
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
        failQueuedJobsLocked("the engine is shutting down");
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
    // Nested edit() calls (tools inside `batch`) join the outermost one. Only edit() itself
    // counts as nesting: an interactive drag transaction is a different thing and is never
    // joined (jobs are not pumped while one is open, see update()).
    if (editDepth_ > 0) return fn();

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depthGuard(editDepth_);

    if (playState_ != PlayState::Editing) {
        // Live edits during play affect the running simulation only (they revert on stop),
        // but must still be atomic: restore the snapshot if anything fails.
        Json snapshot = scene_->toJson();
        Status s;
        try {
            s = fn();
        } catch (...) {
            (void)scene_->loadJson(snapshot);
            throw;
        }
        if (!s) (void)scene_->loadJson(snapshot);
        return s;
    }

    if (history_->inTransaction()) commitEditTransaction();  // never absorb a stray open transaction
    history_->begin(actor, label);
    // Roll back if fn() throws, so a failure can never leave a dangling transaction.
    struct RollbackGuard {
        History& history;
        bool armed = true;
        ~RollbackGuard() {
            if (armed) history.rollback();
        }
    } rollbackGuard{*history_};
    Status s = fn();
    if (!s) return s;  // guard rolls back
    rollbackGuard.armed = false;
    commitEditTransaction();
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
        particles_.reset();  // play sessions replay exactly
        world2d_->reset();
        world2d_->onPlay(*scene_, *runtime_);  // auto-start dialogues
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
    particles_.reset();
    world2d_->reset();
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
        world2d_->preTick(*scene_, input_, *runtime_, kFixedDt);  // UI input, dialogue
        runtime_->tick(kFixedDt, input_);
        world2d_->postTick(*scene_, *runtime_, kFixedDt);  // sprite animation, 2D cameras
        particles_.update(*scene_, static_cast<float>(kFixedDt));
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
    // Hot reload: rescan the project for changed assets every couple of seconds while editing.
    assetScanTimer_ += seconds;
    if (assetScanTimer_ >= 2.0 && playState_ == PlayState::Editing && !drag_.entity && !gizmoDrag_) {
        assetScanTimer_ = 0;
        refreshAssets();
    }
    // While the user drags an object or a gizmo handle, an undo transaction is open;
    // agent jobs wait until it is committed so their edits are never attributed to it.
    if (!drag_.entity && !gizmoDrag_) pump();
    if (playState_ == PlayState::Editing) {  // live preview of fire, rain, waves while building
        float dt = static_cast<float>(std::min(seconds, 0.1));
        previewTime_ += dt;
        particles_.update(*scene_, dt);
        world2d_->preview(*scene_, dt);
    }
    if (playState_ != PlayState::Playing) return;
    accumulator_ += std::min(seconds, 0.25);  // avoid spiral of death after stalls
    int ticks = 0;
    while (accumulator_ >= kFixedDt) {
        accumulator_ -= kFixedDt;
        ++ticks;
    }
    if (ticks) step(ticks);
}

double Engine::effectsTime() const { return playState_ == PlayState::Editing ? previewTime_ : runtime_->time(); }

fx::Ocean& Engine::oceanFor(EntityId e, const Water& w) {
    fx::Ocean& ocean = oceans_[e];
    ocean.configure(w);
    ocean.evaluate(effectsTime());
    return ocean;
}

bool Engine::waterHeight(float x, float z, float& height, Vec3* normal) {
    for (EntityId e : scene_->entities()) {
        const Water* w = scene_->get<Water>(e);
        if (!w || !scene_->isActive(e)) continue;
        Vec3 c = scene_->worldMatrix(e).translation();
        if (w->size > 0.f && (std::fabs(x - c.x) > w->size * 0.5f || std::fabs(z - c.z) > w->size * 0.5f)) continue;
        fx::Ocean& ocean = oceanFor(e, *w);
        height = c.y + ocean.height(x, z);
        if (normal) *normal = ocean.normal(x, z);
        return true;
    }
    return false;
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
    const MeshData* mesh = cpuMesh(meshKey);
    if (!mesh) {
        scene_->assetBounds[meshKey] = {Vec3(-0.5f), Vec3(0.5f)};  // don't retry every frame
        return;
    }
    (void)renderer_->uploadMesh(meshKey, *mesh);
    scene_->assetBounds[meshKey] = mesh->bounds;
}

FrameData Engine::frame(const CaptureOptions& opts) {
    ViewCamera view = camera_.toView();
    EntityId viewCamera = kNoEntity;  // the scene camera entity looked through (camera2d applies to it)
    if (opts.hasCustomView) {
        view = opts.customView;
    } else if (opts.useSceneCamera || opts.cameraEntity) {
        ViewCamera sc;
        if (sceneCamera(*scene_, sc, opts.cameraEntity)) {
            view = sc;
            viewCamera = render2d::activeCamera(*scene_, opts.cameraEntity);
        }
    } else if (playState_ != PlayState::Editing || viewSceneCamera_) {
        ViewCamera sc;
        if (sceneCamera(*scene_, sc)) {
            view = sc;
            viewCamera = render2d::activeCamera(*scene_);
        }
    }
    const float texelSnap = viewCamera ? world2d_->adjustView(*scene_, viewCamera, view, opts.width, opts.height) : 0.f;
    BuildOptions bo;
    bo.editorOverlays = opts.editorOverlays && playState_ == PlayState::Editing;
    bo.selection = selection_;
    bo.time = static_cast<float>(effectsTime());  // animates water, fluids and skies while editing too
    bo.material = [this](const std::string& path) { return resolveMaterial(path); };
    for (EntityId e : scene_->entities()) {
        if (const auto* m = scene_->get<MeshRenderer>(e)) ensureMeshUploaded(m->mesh);
    }
    FrameData f = buildFrame(*scene_, view, opts.width, opts.height, bo);
    if (!opts.fog) f.environment.fogDensity = 0;
    world2d_->gather(*scene_, f, bo.editorOverlays ? selection_ : std::vector<EntityId>{}, bo.time, texelSnap);  // 2D + UI
    // Effects: simulated particles (+ the light fires cast) and FFT water.
    particles_.gather(*scene_, view, f.particles, f.lights);
    prioritizeLights(f);
    std::vector<EntityId> waterIds;
    for (EntityId e : scene_->entities()) {
        const Water* w = scene_->get<Water>(e);
        if (!w) continue;
        waterIds.push_back(e);
        if (!scene_->isActive(e)) continue;
        fx::Ocean& ocean = oceanFor(e, *w);
        Mat4 m = scene_->worldMatrix(e);
        WaterItem wi;
        wi.entity = e;
        wi.level = m.translation().y;
        wi.center = {m.translation().x, m.translation().z};
        wi.size = w->size;
        wi.deepColor = w->deepColor;
        wi.shallowColor = w->shallowColor;
        wi.clarity = w->clarity;
        wi.foam = w->foam;
        wi.reflections = w->reflections;
        wi.refraction = w->refraction;
        wi.roughness = w->roughness;
        wi.ocean = ocean.cascades();
        f.water.push_back(std::move(wi));
    }
    std::erase_if(oceans_, [&](const auto& kv) { return std::find(waterIds.begin(), waterIds.end(), kv.first) == waterIds.end(); });
    resolveTexturePaths(f);
    if (bo.editorOverlays && selection_.size() == 1 && scene_->exists(selection_[0]) && !opts.annotate) {
        GizmoFrame gf = Gizmo::frameFor(scene_->worldMatrix(selection_[0]), view, gizmo_.local);
        f.overlays = gizmo_.overlays(gf, gizmoHot_, gizmoDrag_ ? gizmoDrag_->axis : -1);
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
    world2d_->refineVisible(c.frame, c.visible, *scene_);  // real boxes for sprites, tiles, text, UI
    if (opts.annotate) annotate(c.image, c.visible);
    return c;
}

Status Engine::renderToSurface(void* surface, int width, int height) {
    auto start = std::chrono::steady_clock::now();
    CaptureOptions opts;
    opts.width = width;
    opts.height = height;
    FrameData f = frame(opts);
    Status s = renderer_->render(f);
    if (s) s = renderer_->present(surface);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stats_.cpuMs = stats_.cpuMs * 0.9 + ms * 0.1;  // smoothed
    stats_.draws = f.draws.size();
    stats_.lights = f.lights.size();
    stats_.entities = scene_->size();
    return s;
}

// ---------------------------------------------------------------------------
// Gizmo
// ---------------------------------------------------------------------------

bool Engine::gizmoTarget(EntityId& id, GizmoFrame& gf, int width, int height) {
    (void)width;
    (void)height;
    if (playState_ != PlayState::Editing || selection_.size() != 1 || !scene_->exists(selection_[0])) return false;
    id = selection_[0];
    gf = Gizmo::frameFor(scene_->worldMatrix(id), camera_.toView(), gizmo_.local);
    return true;
}

int Engine::gizmoHover(float x, float y, int width, int height) {
    EntityId id;
    GizmoFrame gf;
    gizmoHot_ = gizmoTarget(id, gf, width, height) ? gizmo_.hitTest(gf, camera_.toView().rayAt(x, y, width, height)) : -1;
    return gizmoHot_;
}

bool Engine::gizmoBegin(float x, float y, int width, int height) {
    EntityId id;
    GizmoFrame gf;
    if (!gizmoTarget(id, gf, width, height)) return false;
    const Transform* t = scene_->get<Transform>(id);
    auto start = gizmo_.begin(gf, camera_.toView().rayAt(x, y, width, height), gf.center, t->rotation, t->scale);
    if (!start) return false;
    endDrag();
    gizmoDrag_ = start;
    gizmoEntity_ = id;
    const char* verb = gizmo_.mode == GizmoMode::Rotate ? "Rotate " : gizmo_.mode == GizmoMode::Scale ? "Scale " : "Move ";
    history_->begin("user", verb + scene_->record(id)->name);
    return true;
}

void Engine::gizmoDrag(float x, float y, int width, int height, bool snapping) {
    if (!gizmoDrag_ || !scene_->exists(gizmoEntity_)) return;
    auto r = gizmo_.drag(*gizmoDrag_, camera_.toView().rayAt(x, y, width, height), snapping);
    const EntityRecord* rec = scene_->record(gizmoEntity_);
    Vec3 local = rec->parent ? scene_->worldMatrix(rec->parent).inverse().transformPoint(r.worldPosition) : r.worldPosition;
    (void)scene_->patchComponent(gizmoEntity_, "transform",
                                 Json::object({{"position", reflect::vec3ToJson(local)},
                                               {"rotation", reflect::vec3ToJson(r.rotation)},
                                               {"scale", reflect::vec3ToJson(r.scale)}}));
}

void Engine::gizmoEnd() {
    if (!gizmoDrag_) return;
    gizmoDrag_.reset();
    gizmoEntity_ = kNoEntity;
    commitEditTransaction();
}

void Engine::commitEditTransaction() {
    if (history_->inTransaction() && history_->commit()) {
        Json ev = history_->lastCommitted()->summary();
        ev["type"] = "edit";
        emitEvent(std::move(ev));
    }
}

EntityId Engine::pickAt(float x, float y, int width, int height) {
    CaptureOptions opts;
    opts.width = width;
    opts.height = height;
    FrameData f = frame(opts);
    // UI and sprites first. While playing, UI under the pointer consumes the click (no 3D pick).
    const bool playing = playState_ != PlayState::Editing;
    if (EntityId ui = world2d_->pick(*scene_, f, x, y, true)) return playing ? kNoEntity : ui;
    if (EntityId sprite = world2d_->pick(*scene_, f, x, y, false)) return sprite;
    return pick(*scene_, f, x, y);
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
    commitEditTransaction();
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
    auto r = importMeshAsset(path);
    if (!r) return r.error();
    return r->get("mesh").asString();
}

AssetRequest& Engine::addAssetRequest(AssetRequest req) {
    req.id = nextAssetRequest_++;
    assetRequests_.push_back(std::move(req));
    emitEvent(Json::object({{"type", "asset_request"}, {"request", assetRequests_.back().toJson()}}));
    return assetRequests_.back();
}

// ---------------------------------------------------------------------------
// Asset system
// ---------------------------------------------------------------------------

namespace {
size_t gltfMaterialCount(const std::vector<int>& parts, size_t materials) {
    size_t n = 0;
    for (int p : parts) n += p >= 0 && static_cast<size_t>(p) < materials;
    return n;
}

std::string stripAssetPrefix(const std::string& ref) {
    for (const char* p : {"asset:", "prefab:"}) {
        if (ref.rfind(p, 0) == 0) return ref.substr(std::strlen(p));
    }
    return ref;
}
}  // namespace

std::vector<std::string> Engine::refreshAssets() {
    std::vector<std::string> changed = assets_->refresh();
    for (const auto& path : changed) {
        world2d_->invalidate(resolvePath(path));  // images, atlases, tilesets, dialogue scripts
        switch (assetTypeForPath(path)) {
            case AssetType::Mesh: {
                std::string key = "asset:" + path;
                std::vector<std::string> keys{key};
                for (const auto& [k, v] : cpuMeshes_) {
                    if (str::startsWith(k, key + "#")) keys.push_back(k);  // parts of a multi-material model
                }
                for (const auto& [k, v] : scene_->assetBounds) {
                    if (str::startsWith(k, key + "#")) keys.push_back(k);
                }
                for (const auto& k : keys) {
                    renderer_->invalidate(k);
                    cpuMeshes_.erase(k);
                    scene_->assetBounds.erase(k);
                }
                break;
            }
            case AssetType::Texture: renderer_->invalidate(resolvePath(path)); break;
            case AssetType::Material: materials_.erase(path); break;
            case AssetType::Prefab: prefabs_.erase(path); break;
            default: break;
        }
    }
    if (!changed.empty()) {
        Json arr = Json::array();
        for (size_t i = 0; i < changed.size() && i < 50; ++i) arr.push(changed[i]);
        emitEvent(Json::object({{"type", "assets"}, {"changed", arr}, {"count", changed.size()}}));
        scene_->markDirty();
    }
    return changed;
}

const MeshData* Engine::cpuMesh(const std::string& key) {
    if (auto it = cpuMeshes_.find(key); it != cpuMeshes_.end()) return it->second.get();
    Result<MeshData> data = Error::make("not_found", "no mesh");
    if (key.rfind("asset:", 0) == 0) {
        // Re-import with the options recorded when the asset was imported.
        mesh::LoadOptions lo;
        auto [file, part] = mesh::splitPart(key.substr(6));
        lo.part = part;
        if (const AssetRecord* rec = assets_->find(file)) {
            lo.normalize = rec->importSettings.get("normalize").asBool(true);
            lo.zUp = rec->importSettings.get("zUp").asBool(false);
        }
        data = mesh::loadMeshFile(resolvePath(file), lo);
    } else {
        data = mesh::primitive(key);
    }
    if (!data) {
        log::warn("asset", data.error().message);
        cpuMeshes_[key] = nullptr;
        return nullptr;
    }
    auto ptr = std::make_shared<MeshData>(std::move(data.value()));
    cpuMeshes_[key] = ptr;
    return ptr.get();
}

Result<Json> Engine::importMeshAsset(const std::string& path, const MeshImportOptions& options) {
    std::string rel = assets_->relative(resolvePath(path));
    if (rel.empty()) return Error::make("invalid_path", "mesh must be inside the project: " + path);
    std::string lower = str::lower(rel);
    Json result = Json::object();
    MeshData mesh;
    std::string materialPath;
    if (lower.size() > 4 && (lower.rfind(".glb") == lower.size() - 4 || lower.rfind(".gltf") == lower.size() - 5)) {
        auto g = loadGltf(resolvePath(rel), false);
        if (!g) return g.error();
        mesh = std::move(g->mesh);
        // glTF materials become project materials next to the mesh. External images are
        // referenced where they are; embedded ones are extracted once.
        fs::path base = fs::path(rel);
        std::string stem = base.stem().string();
        std::string dir = base.parent_path().generic_string();
        auto join = [&](const std::string& name) { return dir.empty() ? name : dir + "/" + name; };
        std::unordered_map<int, std::string> texPaths;
        auto texPath = [&](int image) -> std::string {
            if (image < 0 || static_cast<size_t>(image) >= g->images.size()) return "";
            if (auto it = texPaths.find(image); it != texPaths.end()) return it->second;
            const GltfImport::ImageData& im = g->images[static_cast<size_t>(image)];
            std::string out;
            if (!im.uri.empty()) {
                std::string u = fs::path(im.uri).lexically_normal().generic_string();
                if (!u.empty() && u[0] != '/' && u.rfind("..", 0) != 0) out = join(u);
            } else if (!im.empty()) {
                out = join(stem + "_img" + std::to_string(image) + im.extension());
                std::ofstream tf(resolvePath(out), std::ios::binary);
                tf.write(reinterpret_cast<const char*>(im.bytes.data()), static_cast<std::streamsize>(im.bytes.size()));
            }
            return texPaths[image] = out;
        };
        auto toMaterial = [&](const GltfImport::Material& gm) {
            MaterialAsset m;
            m.color = gm.baseColor;
            m.metallic = gm.metallic;
            m.roughness = std::max(0.02f, gm.roughness);
            m.emissive = gm.emissive;
            m.texture = texPath(gm.baseColorImage);
            m.normalMap = texPath(gm.normalImage);
            m.normalStrength = gm.normalScale;
            m.ormMap = texPath(gm.metallicRoughnessImage);
            m.occlusionStrength = gm.occlusionStrength;
            m.emissiveMap = texPath(gm.emissiveImage);
            m.doubleSided = gm.doubleSided;
            if (gm.alphaMode == "MASK") m.alphaCutoff = std::clamp(gm.alphaCutoff, 0.01f, 1.f);
            // BLEND with an opaque factor: textured transparency (decals, foliage cards) -> soft cutout.
            if (gm.alphaMode == "BLEND" && gm.baseColor.w >= 0.999f && gm.baseColorImage >= 0) m.alphaCutoff = 0.3f;
            return m;
        };
        std::vector<int> parts = g->parts;
        if (parts.size() <= 1) {
            if (!parts.empty() && parts[0] >= 0 && static_cast<size_t>(parts[0]) < g->materials.size()) {
                materialPath = join(stem + ".mat.json");
                if (Status st = saveMaterial(resolvePath(materialPath), toMaterial(g->materials[static_cast<size_t>(parts[0])])); !st) {
                    return st.error();
                }
            }
        } else {
            // One entity per material: write the materials, upload each part, save a prefab.
            Json children = Json::array();
            Json partList = Json::array();
            std::set<std::string> usedNames;
            Aabb reference = mesh.bounds;
            if (options.zUp) reference = mesh::zUpToYUp(reference);
            for (int part : parts) {
                const GltfImport::Material* gm =
                    part >= 0 && static_cast<size_t>(part) < g->materials.size() ? &g->materials[static_cast<size_t>(part)] : nullptr;
                std::string name = gm ? gm->name : "";
                // "ship_2k" + "ship_hull" -> "hull": drop the model-name prefix (any shorter "_" prefix of the stem).
                for (std::string pre = stem; !pre.empty();) {
                    if (str::startsWith(name, pre + "_") && name.size() > pre.size() + 1) {
                        name = name.substr(pre.size() + 1);
                        break;
                    }
                    size_t cut = pre.rfind('_');
                    pre = cut == std::string::npos ? std::string() : pre.substr(0, cut);
                }
                for (char& c : name) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
                }
                if (name.empty()) name = part >= 0 ? "material" + std::to_string(part) : "default";
                while (!usedNames.insert(name).second) name += "_" + std::to_string(part);
                std::string partMat;
                if (gm) {
                    partMat = join(stem + "_" + name + ".mat.json");
                    if (Status st = saveMaterial(resolvePath(partMat), toMaterial(*gm)); !st) return st.error();
                }
                auto pm = loadGltf(resolvePath(rel), false, part);
                if (!pm) return pm.error();
                MeshData partMesh = std::move(pm->mesh);
                if (options.zUp) mesh::zUpToYUp(partMesh);
                if (options.normalize) mesh::normalizeToUnit(partMesh, reference);
                std::string key = "asset:" + rel + "#" + std::to_string(part);
                if (Status s = renderer_->uploadMesh(key, partMesh); !s) return s.error();
                scene_->assetBounds[key] = partMesh.bounds;
                cpuMeshes_[key] = std::make_shared<MeshData>(std::move(partMesh));
                Json meshJ = Json::object({{"mesh", key}});
                if (!partMat.empty()) meshJ["material"] = partMat;
                children.push(Json::object({{"name", name},
                                            {"enabled", true},
                                            {"components", Json::object({{"mesh", meshJ}})}}));
                partList.push(Json::object({{"name", name}, {"mesh", key}, {"material", partMat}}));
            }
            Json prefab = Json::object(
                {{"format", "skywalker.prefab"},
                 {"version", 1},
                 {"name", stem},
                 {"root", Json::object({{"name", stem},
                                        {"enabled", true},
                                        {"components", Json::object({{"transform", Json::object()}})},
                                        {"children", children}})}});
            std::string prefabPath = join(stem + ".prefab.json");
            if (Status st = savePrefab(resolvePath(prefabPath), prefab); !st) return st.error();
            prefabs_.erase(prefabPath);
            result["prefab"] = prefabPath;
            result["parts"] = partList;
            if (gltfMaterialCount(parts, g->materials.size()) > 0) {
                materialPath = partList[size_t{0}].get("material").asString();
            }
        }
        result["primitives"] = g->primitiveCount;
    } else if (lower.size() > 4 && lower.rfind(".obj") == lower.size() - 4) {
        auto loaded = mesh::loadObjWithMaterial(resolvePath(rel), false);
        if (!loaded) return loaded.error();
        mesh = std::move(loaded->mesh);
        const mesh::ImportedMaterial& im = loaded->material;
        if (im.present) {  // .mtl -> project material next to the mesh
            MaterialAsset m;
            m.color = im.color;
            m.roughness = im.roughness;
            m.metallic = im.metallic;
            m.emissive = im.emissive;
            m.texture = im.texture.empty() ? "" : assets_->relative(im.texture);
            m.normalMap = im.normalMap.empty() ? "" : assets_->relative(im.normalMap);
            fs::path base = fs::path(rel);
            std::string dir = base.parent_path().generic_string();
            materialPath = (dir.empty() ? "" : dir + "/") + base.stem().string() + ".mat.json";
            if (Status st = saveMaterial(resolvePath(materialPath), m); !st) return st.error();
        }
    } else {
        auto loaded = mesh::loadMeshFile(resolvePath(rel), false);
        if (!loaded) return loaded.error();
        mesh = std::move(loaded.value());
    }
    if (options.zUp) mesh::zUpToYUp(mesh);
    if (options.normalize) mesh::normalizeToUnit(mesh);
    std::string key = "asset:" + rel;
    if (Status s = renderer_->uploadMesh(key, mesh); !s) return s.error();
    scene_->assetBounds[key] = mesh.bounds;
    cpuMeshes_[key] = std::make_shared<MeshData>(mesh);
    refreshAssets();
    Json settings = Json::object({{"normalize", options.normalize}, {"zUp", options.zUp}, {"vertexColors", mesh.hasVertexColors}});
    if (!materialPath.empty()) settings["material"] = materialPath;
    if (result.contains("prefab")) settings["prefab"] = result.get("prefab");
    if (auto reg = assets_->registerFile(resolvePath(rel)); reg) (void)assets_->updateMeta(rel, Json::object({{"import", settings}}));
    result["mesh"] = key;
    result["triangles"] = mesh.indices.size() / 3;
    result["vertexColors"] = mesh.hasVertexColors;
    {
        Vec3 e = mesh.bounds.max - mesh.bounds.min;
        result["size"] = reflect::vec3ToJson(e);
        result["bounds"] = Json::object({{"min", reflect::vec3ToJson(mesh.bounds.min)}, {"max", reflect::vec3ToJson(mesh.bounds.max)}});
    }
    if (!materialPath.empty()) result["material"] = materialPath;
    return result;
}

void Engine::resolveTexturePaths(FrameData& f) const {
    if (!f.environment.hdri.empty()) f.environment.hdri = resolvePath(f.environment.hdri);
    for (auto& d : f.draws) {
        for (std::string* p : {&d.surface.texture, &d.surface.normalMap, &d.surface.ormMap, &d.surface.emissiveMap}) {
            if (!p->empty()) *p = resolvePath(*p);
        }
    }
}

const ResolvedMaterial* Engine::resolveMaterial(const std::string& path) {
    std::string rel = assets_->relative(resolvePath(stripAssetPrefix(path)));
    if (rel.empty()) rel = path;
    std::error_code ec;
    auto t = fs::last_write_time(resolvePath(rel), ec);
    int64_t mtime = ec ? -2 : std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    CachedMaterial& c = materials_[rel];
    if (c.mtime != mtime) {
        c.mtime = mtime;
        auto m = loadMaterial(resolvePath(rel));
        c.ok = m.ok();
        if (m) {
            c.material = toSurface(*m);
        } else if (mtime != -2) {
            log::warn("asset", "material " + rel + ": " + m.error().message);
        }
    }
    return c.ok ? &c.material : nullptr;
}

Result<Json> Engine::loadPrefabAsset(const std::string& path) {
    std::string rel = assets_->relative(resolvePath(stripAssetPrefix(path)));
    if (rel.empty()) rel = stripAssetPrefix(path);
    std::error_code ec;
    auto t = fs::last_write_time(resolvePath(rel), ec);
    if (ec) return Error::make("not_found", "no prefab " + rel, "use asset_list type=prefab to see prefabs");
    int64_t mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    CachedPrefab& c = prefabs_[rel];
    if (c.mtime != mtime) {
        auto p = loadPrefab(resolvePath(rel));
        if (!p) return p.error();
        c.prefab = std::move(p.value());
        c.mtime = mtime;
    }
    return c.prefab;
}

Result<EntityId> Engine::instantiatePrefabAsset(const std::string& path, const PrefabPlacement& placement) {
    auto prefab = loadPrefabAsset(path);
    if (!prefab) return prefab.error();
    return instantiatePrefab(*scene_, prefab.value(), placement);
}

size_t Engine::rewriteAssetReferences(const std::string& from, const std::string& to) {
    size_t n = 0;
    for (EntityId e : std::vector<EntityId>(scene_->entities())) {
        const MeshRenderer* m = scene_->get<MeshRenderer>(e);
        if (!m) continue;
        Json patch = Json::object();
        if (m->mesh == "asset:" + from) patch["mesh"] = "asset:" + to;
        if (str::startsWith(m->mesh, "asset:" + from + "#")) patch["mesh"] = "asset:" + to + m->mesh.substr(6 + from.size());
        if (m->texture == from) patch["texture"] = to;
        if (m->material == from) patch["material"] = to;
        if (patch.size()) {
            (void)scene_->patchComponent(e, "mesh", patch);
            ++n;
        }
    }
    return n;
}

std::vector<EntityId> Engine::assetUsage(const std::string& path) const {
    std::vector<EntityId> out;
    for (EntityId e : scene_->entities()) {
        const MeshRenderer* m = scene_->get<MeshRenderer>(e);
        if (m && (m->mesh == "asset:" + path || str::startsWith(m->mesh, "asset:" + path + "#") || m->texture == path ||
                  m->material == path)) {
            out.push_back(e);
        }
    }
    return out;
}

Result<Image> Engine::assetPreview(const std::string& ref, int size) {
    const AssetRecord* rec = assets_->find(ref);
    if (!rec) return Error::make("not_found", "no asset " + ref, "use asset_list to find assets");
    Scene tmp;
    tmp.assetBounds = scene_->assetBounds;
    (void)tmp.patchEnvironment(Json::parse(R"({"skyTop":"#3a3d48","skyHorizon":"#5a5e6c","ground":"#2a2c33","ambient":0.6,
        "sunElevation":40,"sunAzimuth":35,"sunIntensity":2.2,"fogDensity":0,"showGrid":false,"vignette":0,"bloomIntensity":0.3})").value());
    bool front = false;
    switch (rec->type) {
        case AssetType::Mesh: {
            EntityId e = tmp.create("Preview");
            (void)tmp.patchComponent(e, "mesh", Json::object({{"mesh", "asset:" + rec->path}}));
            ensureMeshUploaded("asset:" + rec->path);
            tmp.assetBounds = scene_->assetBounds;
            if (!rec->importSettings.get("material").asString().empty()) {
                (void)tmp.patchComponent(e, "mesh", Json::object({{"material", rec->importSettings.get("material")}}));
            }
            break;
        }
        case AssetType::Material: {
            EntityId e = tmp.create("Preview");
            (void)tmp.patchComponent(e, "mesh", Json::object({{"mesh", "sphere"}, {"material", rec->path}}));
            break;
        }
        case AssetType::Texture: {
            EntityId e = tmp.create("Preview");
            (void)tmp.patchComponent(e, "mesh", Json::object({{"mesh", "quad"}, {"texture", rec->path}, {"unlit", true}, {"color", "#ffffff"}}));
            front = true;
            break;
        }
        case AssetType::Prefab: {
            auto prefab = loadPrefabAsset(rec->path);
            if (!prefab) return prefab.error();
            auto root = instantiatePrefab(tmp, prefab.value(), {});
            if (!root) return root.error();
            for (EntityId e : tmp.entities()) {
                if (const auto* m = tmp.get<MeshRenderer>(e)) ensureMeshUploaded(m->mesh);
            }
            tmp.assetBounds = scene_->assetBounds;
            break;
        }
        default:
            return Error::make("unsupported", std::string("no preview for ") + toString(rec->type) + " assets");
    }
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    for (EntityId e : tmp.entities()) {
        if (!tmp.get<MeshRenderer>(e)) continue;
        Aabb b = tmp.localBounds(e).transformed(tmp.worldMatrix(e));
        box.min = vmin(box.min, b.min);
        box.max = vmax(box.max, b.max);
    }
    if (box.min.x > box.max.x) box = {Vec3(-0.5f), Vec3(0.5f)};
    Vec3 c = box.center();
    float radius = std::max(length(box.extents()), 0.1f);
    ViewCamera cam;
    cam.fovDeg = 35;
    Vec3 dir = front ? Vec3{0, 0, 1} : normalize(Vec3{1.0f, 0.75f, 1.25f});
    cam.eye = c + dir * (radius / std::sin(radians(cam.fovDeg * 0.5f)) * 1.05f);
    cam.target = c;
    cam.nearPlane = radius * 0.01f;
    cam.farPlane = radius * 20.f;
    BuildOptions bo;
    bo.editorOverlays = false;
    bo.material = [this](const std::string& p) { return resolveMaterial(p); };
    FrameData f = buildFrame(tmp, cam, size, size, bo);
    resolveTexturePaths(f);
    if (Status st = renderer_->render(f); !st) return st.error();
    return renderer_->readback();
}

// ---------------------------------------------------------------------------
// Spatial queries
// ---------------------------------------------------------------------------

std::optional<Engine::Hit> Engine::raycast(const Ray& rayIn, const std::vector<EntityId>& exclude) {
    Ray ray{rayIn.origin, normalize(rayIn.dir)};
    std::optional<Hit> best;
    for (EntityId e : scene_->entities()) {
        if (std::find(exclude.begin(), exclude.end(), e) != exclude.end()) continue;
        const MeshRenderer* m = scene_->get<MeshRenderer>(e);
        if (!m || !m->visible || !scene_->isActive(e)) continue;
        Mat4 world = scene_->worldMatrix(e);
        if (intersect(ray, scene_->localBounds(e).transformed(world)) < 0) continue;
        const MeshData* mesh = cpuMesh(m->mesh);
        if (!mesh) continue;
        Mat4 inv = world.inverse();
        Vec3 o = inv.transformPoint(ray.origin), d = inv.transformDir(ray.dir);
        const auto& v = mesh->vertices;
        for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
            auto P = [&](uint32_t i) {
                const float* p = &v[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
                return Vec3{p[0], p[1], p[2]};
            };
            Vec3 a = P(mesh->indices[t]), b = P(mesh->indices[t + 1]), c = P(mesh->indices[t + 2]);
            // Moller-Trumbore (two-sided)
            Vec3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
            float det = dot(e1, pv);
            if (std::fabs(det) < 1e-9f) continue;
            float invDet = 1.f / det;
            Vec3 tv = o - a;
            float u = dot(tv, pv) * invDet;
            if (u < 0 || u > 1) continue;
            Vec3 qv = cross(tv, e1);
            float w = dot(d, qv) * invDet;
            if (w < 0 || u + w > 1) continue;
            float tl = dot(e2, qv) * invDet;
            if (tl <= 1e-5f) continue;
            Vec3 hitWorld = world.transformPoint(o + d * tl);
            float dist = distance(ray.origin, hitWorld);
            if (best && dist >= best->distance) continue;
            Vec3 n = normalize(inv.transposed().transformDir(cross(e1, e2)));
            if (dot(n, ray.dir) > 0) n = -n;
            best = Hit{e, hitWorld, n, dist};
        }
    }
    return best;
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
    if (shuttingDown_ || !acceptingJobs_) {
        promise.set_value(ToolResult::error(Error::make("unavailable", "the engine is not accepting requests")).toMcp());
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
            // Called on a connection thread: hop to the main thread and wait. If we give up
            // waiting, the job is marked abandoned so it can never apply changes later.
            auto abandoned = std::make_shared<std::atomic<bool>>(false);
            std::future<Json> f = post([this, tool, args, actor, abandoned] {
                if (abandoned->load()) return ToolResult::error(Error::make("timeout", "request abandoned")).toMcp();
                return callTool(tool, args, actor).toMcp();
            });
            if (f.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
                abandoned->store(true);
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
    if (!server_) return;
    // Connection threads may be blocked waiting for jobs that only this (main) thread
    // would run: refuse new jobs and fail queued ones before joining them.
    {
        std::lock_guard lock(jobsMutex_);
        acceptingJobs_ = false;
        failQueuedJobsLocked("the agent server is stopping");
    }
    server_->stop();
    server_.reset();
    std::lock_guard lock(jobsMutex_);
    acceptingJobs_ = true;
}

void Engine::failQueuedJobsLocked(const std::string& why) {
    for (auto& [job, promise] : jobs_) {
        promise.set_value(ToolResult::error(Error::make("cancelled", why)).toMcp());
    }
    jobs_.clear();
}

bool Engine::agentServerRunning() const { return server_ != nullptr; }

}  // namespace sky
