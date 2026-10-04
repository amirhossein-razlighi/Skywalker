// Runtime scene flow (docs/SCENE_FLOW.md): scene changes with persistent entities, additive sub-scenes,
// time-sliced preloading, loading scenes and transitions, all advanced at the end of a tick.
//
// A change goes:  requested -> (activation: `on scene_unloading` goes out) -> Out (fade to color) ->
// Loading (optional loading scene; assets preloaded a few per tick; progress) -> swap -> In (fade back).
// The swap keeps persistent entities (and their running behaviors), removes everything else, and clones
// the new scene in with fresh ids.

#include "skywalker/game/SceneFlow.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/ecs/Reflection.h"

namespace sky::game {

namespace fs = std::filesystem;

namespace {

constexpr float kMaxTransition = 10.f;

Error invalid(const std::string& what, const std::string& hint = {}) { return Error::make("invalid_argument", what, hint); }

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

EntityId entityValue(const Json& j) {
    if (j.isNumber()) return static_cast<EntityId>(j.asInt());
    if (j.isObject() && j.contains("$entity")) return static_cast<EntityId>(j.get("$entity").asInt());
    return kNoEntity;
}

}  // namespace

// --- settings and options -----------------------------------------------------------------------

Result<TransitionSpec> TransitionSpec::fromJson(const Json& j) {
    TransitionSpec t;
    if (j.isNull()) return t;
    if (j.isString()) {
        t.kind = j.asString();
        if (t.kind != "none") t.duration = t.kind == "crossfade" ? 0.6f : 0.4f;
    } else if (j.isObject()) {
        for (const auto& [k, v] : j.members()) {
            if (k != "kind" && k != "duration" && k != "color") {
                std::string guess = str::closest(k, {"kind", "duration", "color"}, 3);
                return invalid("transition: unknown field '" + k + "'", guess.empty() ? "fields: kind, duration, color" : "did you mean '" + guess + "'?");
            }
        }
        t.kind = j.get("kind").asString("fade");
        t.duration = t.kind == "none" ? 0.f : j.get("duration").asFloat(t.kind == "crossfade" ? 0.6f : 0.4f);
        if (const Json* c = j.find("color"); c && !reflect::jsonToColor(*c, t.color)) {
            return invalid("transition.color must be \"#rrggbb\" or [r, g, b]");
        }
    } else {
        return invalid("transition must be \"none\", \"fade\", \"crossfade\" or {kind, duration, color}");
    }
    if (t.kind != "none" && t.kind != "fade" && t.kind != "crossfade") {
        std::string guess = str::closest(t.kind, {"none", "fade", "crossfade"}, 4);
        return invalid("unknown transition '" + t.kind + "'", guess.empty() ? "use none, fade or crossfade" : "did you mean '" + guess + "'?");
    }
    if (!(t.duration >= 0.f && t.duration <= kMaxTransition)) return invalid("transition.duration must be 0..10 seconds");
    return t;
}

Json TransitionSpec::toJson() const {
    return Json::object({{"kind", kind}, {"duration", duration}, {"color", Json::array({color.x, color.y, color.z})}});
}

Result<SceneFlowSettings> SceneFlowSettings::fromJson(const Json& scenes, const Json& flow) {
    SceneFlowSettings s;
    auto bad = [](const std::string& m, const std::string& hint = {}) { return Error::make("invalid_game_json", "game.json " + m, hint); };
    if (!scenes.isNull()) {
        if (!scenes.isObject()) return bad("scenes must map names to scene files, e.g. {\"menu\": \"scenes/menu.sky.json\"}");
        for (const auto& [alias, path] : scenes.members()) {
            if (alias.empty() || !path.isString() || path.asString().empty()) return bad("scenes." + alias + " must be a scene file path");
            s.aliases.emplace_back(alias, path.asString());
        }
    }
    if (!flow.isNull()) {
        if (!flow.isObject()) return bad("sceneFlow must be an object: {persistent, loadingScene, transition, preloadPerTick}");
        static const std::vector<std::string> keys{"persistent", "loadingScene", "transition", "preloadPerTick"};
        for (const auto& [k, v] : flow.members()) {
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
                std::string guess = str::closest(k, keys, 3);
                return bad("sceneFlow: unknown field '" + k + "'",
                           (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + "valid fields: persistent, loadingScene, transition, preloadPerTick");
            }
        }
        for (const auto& n : flow.get("persistent").elements()) {
            if (!n.isString()) return bad("sceneFlow.persistent must list entity names");
            s.persistent.push_back(n.asString());
        }
        s.loadingScene = flow.get("loadingScene").asString();
        auto t = TransitionSpec::fromJson(flow.get("transition"));
        if (!t) return bad("sceneFlow." + t.error().message, t.error().hint);
        s.transition = *t;
        if (const Json* p = flow.find("preloadPerTick")) {
            if (!p->isNumber() || p->asNumber() < 1 || p->asNumber() > 1000) return bad("sceneFlow.preloadPerTick must be 1..1000");
            s.preloadPerTick = static_cast<int>(p->asInt());
        }
    }
    return s;
}

Result<ChangeOptions> ChangeOptions::fromJson(const Json& j) {
    ChangeOptions o;
    if (j.isNull()) return o;
    if (!j.isObject()) return invalid("options must be a map: {transition, duration, keep, spawn_at, loading}");
    static const std::vector<std::string> keys{"transition", "duration", "color", "keep", "spawn_at", "loading", "immediate"};
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            std::string guess = str::closest(k, keys, 3);
            return invalid("unknown option '" + k + "'", guess.empty() ? "options: transition, duration, keep, spawn_at, loading" : "did you mean '" + guess + "'?");
        }
    }
    if (const Json* t = j.find("transition")) {
        auto spec = TransitionSpec::fromJson(*t);
        if (!spec) return spec.error();
        o.transition = *spec;
        o.hasTransition = true;
    }
    if (const Json* d = j.find("duration")) {
        if (!d->isNumber() || d->asNumber() < 0 || d->asNumber() > kMaxTransition) return invalid("duration must be 0..10 seconds");
        if (!o.hasTransition) {
            o.transition.kind = "fade";
            o.hasTransition = true;
        }
        o.transition.duration = d->asFloat();
    }
    if (const Json* c = j.find("color"); c && !reflect::jsonToColor(*c, o.transition.color)) return invalid("color must be \"#rrggbb\"");
    for (const auto& k : j.get("keep").elements()) {
        if (k.isString()) o.keep.push_back(k.asString());
        else if (EntityId e = entityValue(k)) o.keep.push_back("#" + std::to_string(e));
    }
    if (const Json* s = j.find("spawn_at")) {
        if (EntityId e = entityValue(*s)) o.spawnAt = "#" + std::to_string(e);
        else o.spawnAt = s->asString();
    }
    if (const Json* l = j.find("loading")) {
        o.hasLoading = true;
        o.loading = l->asString();
    }
    o.immediate = j.get("immediate").asBool();
    return o;
}

Result<AdditiveOptions> AdditiveOptions::fromJson(const Json& j) {
    AdditiveOptions o;
    if (j.isNull()) return o;
    if (!j.isObject()) return invalid("options must be a map: {id, parent, offset}");
    for (const auto& [k, v] : j.members()) {
        if (k != "id" && k != "parent" && k != "offset") {
            std::string guess = str::closest(k, {"id", "parent", "offset"}, 3);
            return invalid("unknown option '" + k + "'", guess.empty() ? "options: id, parent, offset" : "did you mean '" + guess + "'?");
        }
    }
    o.id = j.get("id").asString();
    o.parent = entityValue(j.get("parent"));
    if (const Json* off = j.find("offset")) {
        if (!reflect::jsonToVec3(*off, o.offset)) return invalid("offset must be [x, y, z]");
        o.hasOffset = true;
    }
    return o;
}

// --- SceneFlow ----------------------------------------------------------------------------------

struct SceneFlow::Impl {
    Engine& engine;
    mutable std::string settingsStamp = "?";
    mutable SceneFlowSettings cached;

    enum class Phase { Idle, Out, Loading, In };
    struct Change {
        std::string id, path;
        ChangeOptions options;
        bool activated = false;
        std::string loadingPath;  // "" = no loading scene
        bool loadingShown = false;
        std::vector<std::pair<char, std::string>> assets;  // ('m'esh | 'a'material | 'p'refab, ref)
        size_t done = 0;
    };
    struct SubScene {
        std::string id, path;
        std::vector<EntityId> owned;  // every entity it created (roots first, depth-first)
        int unloadIn = -1;            // ticks until removal (-1 = loaded)
    };
    struct AdditiveRequest {
        std::string id, path;
        AdditiveOptions options;
    };

    std::string current, currentPath;
    std::optional<Change> pending;
    Phase phase = Phase::Idle;
    double phaseTime = 0;
    TransitionSpec transition;
    std::vector<SubScene> subs;
    std::vector<AdditiveRequest> additiveRequests;

    explicit Impl(Engine& e) : engine(e) {}

    const SceneFlowSettings& settings() const {
        const fs::path file = fs::path(engine.config().projectDir) / "game.json";
        std::error_code ec;
        auto t = fs::last_write_time(file, ec);  // compared for equality only
        const std::string stamp = ec ? "none" : std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        if (stamp == settingsStamp) return cached;
        settingsStamp = stamp;
        cached = SceneFlowSettings{};
        if (ec) return cached;
        auto doc = Json::parse(readText(file));
        if (!doc) return cached;
        if (auto s = SceneFlowSettings::fromJson(doc->get("scenes"), doc->get("sceneFlow"))) cached = *s;
        return cached;
    }

    std::string relative(const std::string& path) const {
        if (path.empty()) return {};
        std::string rel = engine.assets().relative(engine.resolvePath(path));
        return rel.empty() ? path : rel;
    }

    std::vector<std::string> sceneFiles() const {
        std::vector<std::string> out;
        std::error_code ec;
        const fs::path root = engine.config().projectDir;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (!name.empty() && (name[0] == '.' || name == "build" || name == "node_modules")) it.disable_recursion_pending();
                continue;
            }
            if (name.ends_with(".sky.json")) out.push_back(fs::relative(it->path(), root, ec).generic_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    Result<std::string> resolve(const std::string& target) const {
        const std::string t = str::trim(target);
        if (t.empty()) return Error::make("scene_not_found", "no scene given", "pass a scene alias from game.json \"scenes\" or a .sky.json path");
        for (const auto& [alias, path] : settings().aliases) {
            if (alias == t) return relative(path);
        }
        std::error_code ec;
        for (const std::string& candidate : {t, t + ".sky.json", "scenes/" + t + ".sky.json", "scenes/" + t}) {
            if (fs::is_regular_file(engine.resolvePath(candidate), ec) && candidate.ends_with(".sky.json")) return relative(candidate);
        }
        std::vector<std::string> names;
        for (const auto& [alias, path] : settings().aliases) names.push_back(alias);
        for (const auto& f : sceneFiles()) {
            names.push_back(f);
            std::string stem = fs::path(f).filename().string();
            names.push_back(stem.substr(0, stem.find('.')));
        }
        std::string guess = str::closest(t, names, 4);
        std::string list;
        for (const auto& [alias, path] : settings().aliases) list += (list.empty() ? "" : ", ") + alias;
        return Error::make("scene_not_found", "no scene '" + t + "'",
                           (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") +
                               (list.empty() ? "scene_flow_info lists the scene files; game.json \"scenes\" can name them"
                                             : "aliases: " + list));
    }

    std::string idOf(const std::string& path) const {
        for (const auto& [alias, p] : settings().aliases) {
            if (relative(p) == path) return alias;
        }
        return path;
    }

    Result<Json> readScene(const std::string& path) const {
        auto doc = Json::parse(readText(engine.resolvePath(path)));
        if (!doc) return Error::make("invalid_scene", path + ": " + doc.error().message);
        if (doc->get("format").asString() != "skywalker.scene") return Error::make("invalid_scene", path + " is not a scene file");
        return doc;
    }

    void emit(const std::string& name, const Json& payload) { engine.runtime().emitJson(name, kNoEntity, payload); }

    // --- persistence --------------------------------------------------------------------------

    std::string persistentKey(EntityId e) const {
        const Scene& s = engine.scene();
        const Persistent* p = s.get<Persistent>(e);
        return p && !p->id.empty() ? p->id : s.record(e)->name;
    }

    /// Entities that survive a change: `persistent` components, game.json names, the change's `keep`; with descendants.
    std::unordered_set<EntityId> keptEntities(const std::vector<std::string>& keep) const {
        Scene& s = engine.scene();
        std::unordered_set<EntityId> roots;
        std::set<std::string> names(settings().persistent.begin(), settings().persistent.end());
        for (const auto& k : keep) names.insert(k);
        for (EntityId e : s.entities()) {
            const EntityRecord* r = s.record(e);
            if (s.get<Persistent>(e) || names.count(r->name) || names.count("#" + std::to_string(e))) roots.insert(e);
        }
        std::unordered_set<EntityId> all;
        std::function<void(EntityId)> add = [&](EntityId e) {
            if (!all.insert(e).second) return;
            for (EntityId c : s.children(e)) add(c);
        };
        for (EntityId r : roots) add(r);
        return all;
    }

    std::vector<std::string> persistentNames() const {
        std::vector<std::string> out;
        Scene& s = engine.scene();
        std::unordered_set<EntityId> kept = keptEntities({});
        for (EntityId e : s.entities()) {
            if (kept.count(e) && !kept.count(s.record(e)->parent)) out.push_back(s.record(e)->name);
        }
        return out;
    }

    // --- loading ------------------------------------------------------------------------------

    std::vector<std::pair<char, std::string>> assetsOf(const Json& doc) const {
        std::set<std::pair<char, std::string>> set;
        for (const auto& e : doc.get("entities").elements()) {
            const Json& mesh = e.get("components").get("mesh");
            const std::string& key = mesh.get("mesh").asString();
            if (str::startsWith(key, "asset:")) set.insert({'m', key});
            const std::string& mat = mesh.get("material").asString();
            if (!mat.empty()) set.insert({'a', mat});
            const std::string& prefab = e.get("prefab").get("source").asString();
            if (!prefab.empty()) set.insert({'p', prefab});
        }
        return {set.begin(), set.end()};
    }

    /// Loads one asset (true when it is ready). Meshes go through the engine's mesh streaming.
    bool preload(const std::pair<char, std::string>& a) {
        switch (a.first) {
            case 'm': return engine.preloadMesh(a.second);
            case 'a': (void)engine.resolveMaterial(a.second); return true;
            default: (void)engine.prefabTemplateAsset(a.second); return true;
        }
    }

    /// The scene file in a scratch scene (prefab instances expanded with the engine's prefabs).
    Status loadInto(Scene& temp, const std::string& path) const {
        auto doc = readScene(path);
        if (!doc) return doc.error();
        temp.setPrefabResolver(engine.scene().prefabResolver());
        return temp.loadJson(*doc);
    }

    std::vector<EntityId> rootsOf(const Scene& s) const {
        std::vector<EntityId> roots;
        for (EntityId e : s.entities()) {
            if (!s.exists(s.record(e)->parent)) roots.push_back(e);
        }
        return roots;
    }

    /// Replaces the scene: kept entities stay (behaviors keep running), the rest goes, the new scene is cloned in.
    Status swap(const std::string& path, const ChangeOptions& options) {
        Scene temp;
        if (Status st = loadInto(temp, path); !st) return st;
        Scene& s = engine.scene();
        std::unordered_set<EntityId> kept = keptEntities(options.keep);
        // Kept entities under a parent that goes become roots (their local transform is kept).
        for (EntityId e : s.entities()) {
            if (kept.count(e) && s.record(e)->parent && !kept.count(s.record(e)->parent)) (void)s.setParent(e, kNoEntity);
        }
        std::vector<EntityId> doomed;
        for (EntityId e : s.entities()) {
            if (!kept.count(e) && !s.record(e)->parent) doomed.push_back(e);
        }
        for (EntityId e : doomed) s.destroy(e);
        subs.clear();
        engine.runtime().forgetMissingEntities();
        // The next scene's copies of persistent entities give way to the running ones.
        std::set<std::string> keys;
        for (EntityId e : kept) {
            if (s.get<Persistent>(e) && !kept.count(s.record(e)->parent)) keys.insert(persistentKey(e));
        }
        std::vector<EntityId> dupes;
        for (EntityId e : temp.entities()) {
            const Persistent* p = temp.get<Persistent>(e);
            if (p && keys.count(p->id.empty() ? temp.record(e)->name : p->id)) dupes.push_back(e);
        }
        for (EntityId e : dupes) temp.destroy(e);
        std::unordered_map<EntityId, EntityId> map;
        s.cloneTrees(temp, rootsOf(temp), kNoEntity, &map);
        (void)s.patchEnvironment(reflect::toJson(&temp.environment(), Environment::type()));
        s.name = temp.name;
        for (const auto& src : temp.prefabSources()) {
            if (auto t = temp.prefabTemplate(src)) s.setPrefabTemplate(src, t);
        }
        // spawn_at: the player goes to the named entity of the new scene.
        if (!options.spawnAt.empty()) {
            EntityId at = kNoEntity;
            for (const auto& [from, to] : map) {
                const std::string& n = s.record(to)->name;
                if (str::lower(n) == str::lower(options.spawnAt) || "#" + std::to_string(from) == options.spawnAt) at = to;
            }
            if (at) {
                const Mat4 m = s.worldMatrix(at);
                const Vec3 rot = s.get<Transform>(at)->rotation;
                std::vector<EntityId> movers;
                for (EntityId e : kept) {
                    const Persistent* p = s.get<Persistent>(e);
                    if (p && p->spawn) movers.push_back(e);
                }
                if (movers.empty()) {
                    for (EntityId e : kept) {
                        const auto& tags = s.record(e)->tags;
                        if (!kept.count(s.record(e)->parent) && std::find(tags.begin(), tags.end(), "player") != tags.end()) movers.push_back(e);
                    }
                }
                std::sort(movers.begin(), movers.end());
                for (EntityId e : movers) {
                    Transform& t = *s.get<Transform>(e);
                    t.position = m.translation();
                    t.rotation = rot;
                    engine.teleport(e);
                }
            }
        }
        s.markDirty();
        engine.physics().endPlay();  // rebuilt from the new scene on the next tick
        engine.physics().beginPlay();
        engine.navigation().endPlay();
        engine.navigation().beginPlay();
        currentPath = path;
        current = idOf(path);
        emit("scene_loaded", Json::object({{"id", current}, {"path", path}, {"additive", false}}));
        engine.emitEvent(Json::object({{"type", "scene_flow"}, {"action", "changed"}, {"scene", current}}));
        return {};
    }

    Result<std::string> addSub(const AdditiveRequest& r) {
        Scene temp;
        if (Status st = loadInto(temp, r.path); !st) return st.error();
        Scene& s = engine.scene();
        const EntityId parent = r.options.parent && s.exists(r.options.parent) ? r.options.parent : kNoEntity;
        std::unordered_map<EntityId, EntityId> map;
        std::vector<EntityId> roots = s.cloneTrees(temp, rootsOf(temp), parent, &map);
        if (r.options.hasOffset) {
            for (EntityId e : roots) s.get<Transform>(e)->position = s.get<Transform>(e)->position + r.options.offset;
        }
        SubScene sub{r.id, r.path, {}, -1};
        for (EntityId e : temp.entities()) {
            if (auto it = map.find(e); it != map.end()) sub.owned.push_back(it->second);
        }
        subs.push_back(std::move(sub));
        s.markDirty();
        emit("scene_loaded", Json::object({{"id", r.id}, {"path", r.path}, {"additive", true}}));
        engine.emitEvent(Json::object({{"type", "scene_flow"}, {"action", "additive_loaded"}, {"scene", r.id}}));
        return r.id;
    }

    UnloadResult removeSub(size_t index) {
        Scene& s = engine.scene();
        SubScene sub = std::move(subs[index]);
        subs.erase(subs.begin() + static_cast<std::ptrdiff_t>(index));
        std::unordered_set<EntityId> owned(sub.owned.begin(), sub.owned.end());
        UnloadResult out;
        // Exactly what it loaded: entities the game added under it at run time move to the root and stay.
        for (EntityId e : sub.owned) {
            if (!s.exists(e)) continue;
            for (EntityId c : s.children(e)) {
                if (!owned.count(c)) {
                    (void)s.setParent(c, kNoEntity);
                    out.orphans.push_back(c);
                }
            }
        }
        for (EntityId e : sub.owned) {
            if (s.exists(e)) out.removed += s.destroy(e);
        }
        engine.runtime().forgetMissingEntities();
        s.markDirty();
        engine.emitEvent(Json::object({{"type", "scene_flow"}, {"action", "additive_unloaded"}, {"scene", sub.id}}));
        return out;
    }

    std::string uniqueHandle(std::string base) const {
        if (base.empty()) base = "scene";
        auto taken = [&](const std::string& h) {
            for (const auto& s : subs) {
                if (s.id == h) return true;
            }
            for (const auto& r : additiveRequests) {
                if (r.id == h) return true;
            }
            return false;
        };
        std::string h = base;
        for (int n = 2; taken(h); ++n) h = base + "#" + std::to_string(n);
        return h;
    }

    // --- the change state machine -------------------------------------------------------------

    void advance() {
        if (phase == Phase::In) {
            phaseTime += Engine::kFixedDt;
            if (phaseTime >= transition.duration) phase = Phase::Idle;
            return;
        }
        if (!pending) return;
        Change& c = *pending;
        if (!c.activated) {
            c.activated = true;
            transition = c.options.hasTransition ? c.options.transition : settings().transition;
            phaseTime = 0;
            phase = transition.kind == "fade" && transition.duration > 0 ? Phase::Out : Phase::Loading;
            emit("scene_unloading", Json::object({{"id", current}, {"to", c.id}, {"additive", false}}));
            return;  // the leaving scene hears it next tick
        }
        if (phase == Phase::Out) {
            phaseTime += Engine::kFixedDt;
            if (phaseTime >= transition.duration) phase = Phase::Loading;
            return;
        }
        // Loading
        if (!c.loadingPath.empty() && !c.loadingShown) {
            c.loadingShown = true;
            ChangeOptions toLoading = c.options;
            toLoading.spawnAt.clear();  // the player is placed in the real scene
            if (Status st = swap(c.loadingPath, toLoading); !st) log(st);
            return;
        }
        const int budget = std::max(1, settings().preloadPerTick);
        for (int n = 0; n < budget && c.done < c.assets.size(); ++n) {
            if (!preload(c.assets[c.done])) break;  // still streaming: try again next tick
            ++c.done;
        }
        if (c.done < c.assets.size()) return;
        Change finished = std::move(c);
        pending.reset();
        if (Status st = swap(finished.path, finished.options); !st) {
            log(st);
            phase = Phase::Idle;
            return;
        }
        phaseTime = 0;
        phase = transition.kind != "none" && transition.duration > 0 ? Phase::In : Phase::Idle;
    }

    void log(const Status& st) {
        engine.runtime().log(kNoEntity, "scene change failed: " + st.error().message, "scenes");
        engine.emitEvent(Json::object({{"type", "scene_flow"}, {"action", "failed"}, {"message", st.error().message}}));
    }

    void reset() {
        current.clear();
        currentPath.clear();
        pending.reset();
        phase = Phase::Idle;
        phaseTime = 0;
        transition = TransitionSpec{};
        subs.clear();
        additiveRequests.clear();
    }
};

SceneFlow::SceneFlow(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}
SceneFlow::~SceneFlow() = default;

SceneFlowSettings SceneFlow::settings() const { return impl_->settings(); }
Result<std::string> SceneFlow::resolve(const std::string& idOrPath) const { return impl_->resolve(idOrPath); }
std::string SceneFlow::idOf(const std::string& path) const { return impl_->idOf(path); }

void SceneFlow::beginPlay() {
    impl_->reset();
    impl_->currentPath = impl_->relative(impl_->engine.scenePath());
    impl_->current = impl_->currentPath.empty() ? std::string("(unsaved scene)") : impl_->idOf(impl_->currentPath);
}

void SceneFlow::endPlay() { impl_->reset(); }

void SceneFlow::endTick() {
    Impl& m = *impl_;
    // Sub-scenes asked for during the tick.
    std::vector<Impl::AdditiveRequest> requests = std::move(m.additiveRequests);
    m.additiveRequests.clear();
    for (const auto& r : requests) {
        if (auto added = m.addSub(r); !added) m.log(added.error());
    }
    // Sub-scenes leaving: heard `on scene_unloading` last tick, removed now.
    for (size_t i = 0; i < m.subs.size();) {
        if (m.subs[i].unloadIn == 0) {
            (void)m.removeSub(i);
            continue;
        }
        if (m.subs[i].unloadIn > 0) --m.subs[i].unloadIn;
        ++i;
    }
    m.advance();
}

Status SceneFlow::requestChange(const std::string& target, const ChangeOptions& options) {
    Impl& m = *impl_;
    if (m.engine.playState() == PlayState::Editing) {
        return Error::make("not_playing", "scene changes happen while the game runs",
                           "start play (sim_control {\"action\": \"play\"}); while editing use scene_load");
    }
    auto path = m.resolve(target);
    if (!path) return path.error();
    auto doc = m.readScene(*path);
    if (!doc) return doc.error();
    std::string loading;
    const std::string loadingTarget = options.hasLoading ? options.loading : m.settings().loadingScene;
    if (!loadingTarget.empty() && !options.immediate) {
        auto l = m.resolve(loadingTarget);
        if (!l) return Error::make(l.error().code, "loading scene: " + l.error().message, l.error().hint);
        loading = *l;
    }
    if (options.immediate) {
        if (m.engine.runtime().ticking()) return Error::make("invalid_state", "immediate changes are for tools (between ticks)");
        m.pending.reset();
        m.phase = Impl::Phase::Idle;
        m.emit("scene_unloading", Json::object({{"id", m.current}, {"to", m.idOf(*path)}, {"additive", false}}));
        return m.swap(*path, options);
    }
    Impl::Change c;
    c.path = *path;
    c.id = m.idOf(*path);
    c.options = options;
    c.loadingPath = loading;
    c.assets = m.assetsOf(*doc);
    // A change already under way is redirected to the new target (its transition carries on).
    if (m.pending && m.pending->activated) c.activated = true;
    m.pending = std::move(c);
    return {};
}

Result<std::string> SceneFlow::loadAdditive(const std::string& target, const AdditiveOptions& options, bool now) {
    Impl& m = *impl_;
    if (m.engine.playState() == PlayState::Editing) {
        return Error::make("not_playing", "sub-scenes are loaded while the game runs", "start play first; while editing use prefab_instantiate");
    }
    auto path = m.resolve(target);
    if (!path) return path.error();
    if (auto doc = m.readScene(*path); !doc) return doc.error();
    std::string base = options.id;
    if (base.empty()) {
        base = m.idOf(*path);
        if (base == *path) {
            std::string stem = fs::path(*path).filename().string();
            base = stem.substr(0, stem.find('.'));
        }
    }
    Impl::AdditiveRequest r{m.uniqueHandle(base), *path, options};
    if (now) return m.addSub(r);
    m.additiveRequests.push_back(r);
    return r.id;
}

Result<SceneFlow::UnloadResult> SceneFlow::unload(const std::string& handle, bool now) {
    Impl& m = *impl_;
    for (size_t i = 0; i < m.subs.size(); ++i) {
        if (m.subs[i].id != handle) continue;
        m.emit("scene_unloading", Json::object({{"id", handle}, {"path", m.subs[i].path}, {"additive", true}}));
        if (now) return m.removeSub(i);
        if (m.subs[i].unloadIn < 0) m.subs[i].unloadIn = 1;  // removed at the end of the next tick
        return UnloadResult{};
    }
    std::vector<std::string> ids;
    for (const auto& s : m.subs) ids.push_back(s.id);
    std::string guess = str::closest(handle, ids, 3);
    std::string list;
    for (const auto& id : ids) list += (list.empty() ? "" : ", ") + id;
    return Error::make("scene_not_loaded", "no sub-scene '" + handle + "' is loaded",
                       guess.empty() ? (list.empty() ? "nothing is loaded additively" : "loaded: " + list) : "did you mean '" + guess + "'?");
}

const std::string& SceneFlow::current() const { return impl_->current; }

double SceneFlow::progress() const {
    const Impl& m = *impl_;
    if (!m.pending || !m.pending->activated) return m.pending ? 0.0 : 1.0;
    if (m.phase == Impl::Phase::Out) return 0.0;
    const size_t total = m.pending->assets.size();
    return total ? static_cast<double>(m.pending->done) / static_cast<double>(total) : 0.0;
}

bool SceneFlow::busy() const { return impl_->pending.has_value() || impl_->phase != Impl::Phase::Idle; }

FrameData::ScreenFade SceneFlow::fade() const {
    const Impl& m = *impl_;
    FrameData::ScreenFade f;
    f.color = m.transition.color;
    const float d = std::max(1e-6f, m.transition.duration);
    const float t = static_cast<float>(std::min<double>(1.0, m.phaseTime / d));
    switch (m.phase) {
        case Impl::Phase::Idle: break;
        case Impl::Phase::Out: f.alpha = m.transition.kind == "fade" ? t : 0.f; break;
        case Impl::Phase::Loading:
            f.alpha = m.transition.kind == "fade" && !(m.pending && m.pending->loadingShown) ? 1.f : 0.f;
            break;
        case Impl::Phase::In:
            if (m.transition.kind == "fade") f.alpha = 1.f - t;
            if (m.transition.kind == "crossfade") f.crossfade = 1.f - t;
            break;
    }
    return f;
}

Json SceneFlow::info() const {
    const Impl& m = *impl_;
    const SceneFlowSettings& s = m.settings();
    Json aliases = Json::object();
    for (const auto& [alias, path] : s.aliases) aliases[alias] = path;
    Json files = Json::array();
    for (const auto& f : m.sceneFiles()) files.push(f);
    Json subs = Json::array();
    for (const auto& sub : m.subs) {
        size_t alive = 0;
        for (EntityId e : sub.owned) alive += m.engine.scene().exists(e) ? 1 : 0;
        subs.push(Json::object({{"id", sub.id}, {"path", sub.path}, {"entities", alive}, {"unloading", sub.unloadIn >= 0}}));
    }
    Json persistent = Json::array();
    for (const auto& n : m.persistentNames()) persistent.push(n);
    static const char* kPhases[] = {"idle", "out", "loading", "in"};
    Json pending;
    if (m.pending) {
        pending = Json::object({{"target", m.pending->id},
                                {"path", m.pending->path},
                                {"started", m.pending->activated},
                                {"assets", m.pending->assets.size()},
                                {"preloaded", m.pending->done},
                                {"loadingScene", m.pending->loadingPath}});
    }
    const FrameData::ScreenFade f = fade();
    Json settingsJson = Json::object({{"persistent", Json::array()}, {"loadingScene", s.loadingScene}, {"transition", s.transition.toJson()},
                                      {"preloadPerTick", s.preloadPerTick}});
    for (const auto& n : s.persistent) settingsJson["persistent"].push(n);
    return Json::object({{"playing", m.engine.playState() != PlayState::Editing},
                         {"current", m.current},
                         {"path", m.currentPath},
                         {"pending", pending},
                         {"progress", progress()},
                         {"transition", Json::object({{"kind", m.transition.kind},
                                                      {"phase", kPhases[static_cast<int>(m.phase)]},
                                                      {"alpha", f.alpha},
                                                      {"crossfade", f.crossfade}})},
                         {"additive", subs},
                         {"persistent", persistent},
                         {"aliases", aliases},
                         {"scenes", files},
                         {"settings", settingsJson}});
}

}  // namespace sky::game
