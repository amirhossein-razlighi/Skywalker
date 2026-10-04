// The engine's save system (docs/SAVE_GAMES.md): captures persisted entities, globals and the exact
// Wander play state into a save document, and restores one into the running game.
//
// Entities are matched by key: "id:<persist.id>" when the persist component names one, else "#<entity
// id>" (stable for entities placed in a scene). A load, between two ticks:
//   1. switches scene through the scene flow when the save was made in another one (or in the same scene
//      loaded with other entity ids): an immediate change without a transition that gives the scene's
//      entities their saved ids; `carry` entities come along as on any scene change,
//   2. destroys tombstoned entities and persisted ones spawned after the save,
//   3. restores every saved entity (recreating spawned subtrees with their saved ids),
//   4. restores entity order, the next entity id and the Wander state, rebuilds the physics world,
//   5. emits `loaded`.
// The editor's play snapshot is never touched, so stopping play returns to the edited scene.

#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "skywalker/core/FileTime.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/game/SaveGame.h"
#include "skywalker/game/SceneFlow.h"
#include "skywalker/ui/World2D.h"
#include "skywalker/wander/Compiler.h"

namespace sky::game {

std::string saveTimestamp();  // SaveGame.cpp

namespace fs = std::filesystem;

namespace {

std::string keyOf(const Scene& s, EntityId e) {
    const Persist* p = s.get<Persist>(e);
    if (p && !p->id.empty()) return "id:" + p->id;
    return "#" + std::to_string(e);
}

/// The root of the spawned persisted subtree `e` lives in (its own children are saved with it), or none.
EntityId spawnedRootAbove(const Scene& s, EntityId e) {
    for (const EntityRecord* r = s.record(e); r && r->parent; r = s.record(r->parent)) {
        const Persist* p = s.get<Persist>(r->parent);
        if (p && p->spawned) return r->parent;
    }
    return kNoEntity;
}

void collectSubtree(const Scene& s, EntityId root, std::vector<EntityId>& out) {
    for (EntityId c : s.children(root)) {
        out.push_back(c);
        collectSubtree(s, c, out);
    }
}

Json floatArray(const float* f, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) a.push(static_cast<double>(f[i]));
    return a;
}

/// Scene JSON rounds floats to 4 decimals and colors to 8 bits (readable files, small diffs). A save must
/// restore the exact values or a loaded game drifts from the original run: rewrite every float-based field
/// the component JSON has at full precision (colors as [r, g, b, a]).
void makeExact(Scene& s, EntityId e, Json& components) {
    for (const ComponentKind& kind : s.componentKinds()) {
        Json* c = kind.info ? components.find(kind.name) : nullptr;
        if (!c || !c->isObject() || !kind.has(s, e)) continue;
        const char* raw = static_cast<const char*>(kind.ptr(s, e));
        if (!raw) continue;
        for (const FieldInfo& f : kind.info->fields) {
            if (!c->contains(f.name)) continue;
            const float* v = reinterpret_cast<const float*>(raw + f.offset);
            switch (f.type) {
                case FieldType::Float: (*c)[f.name] = static_cast<double>(*v); break;
                case FieldType::Vec2: (*c)[f.name] = floatArray(v, 2); break;
                case FieldType::Vec3: (*c)[f.name] = floatArray(v, 3); break;
                case FieldType::Vec4:
                case FieldType::Color: (*c)[f.name] = floatArray(v, 4); break;
                default: break;
            }
        }
    }
}

/// Scene::snapshotEntity with exact component values.
Json exactSnapshot(Scene& s, EntityId e) {
    Json snap = s.snapshotEntity(e);
    if (Json* comps = snap.find("components")) makeExact(s, e, *comps);
    return snap;
}

/// Recursive JSON differences ("light.intensity": saved vs current), at most `budget` entries.
void diffJson(const std::string& path, const Json& saved, const Json& current, Json& out, size_t& budget) {
    if (budget == 0 || saved == current) return;
    if (saved.isObject() && current.isObject()) {
        for (const auto& [k, v] : saved.members()) diffJson(path.empty() ? k : path + "." + k, v, current.get(k), out, budget);
        for (const auto& [k, v] : current.members()) {
            if (!saved.contains(k)) diffJson(path.empty() ? k : path + "." + k, Json(), v, out, budget);
        }
        return;
    }
    // Var mirrors hold doubles, components floats: values that agree as floats are the same.
    if (saved.isNumber() && current.isNumber() &&
        static_cast<float>(saved.asNumber()) == static_cast<float>(current.asNumber())) {
        return;
    }
    out.push(Json::object({{"path", path}, {"saved", saved}, {"current", current}}));
    --budget;
}

Result<std::string> readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

struct SaveSystem::Impl {
    Engine& engine;
    std::string dir;  // "" = <project>/.skywalker/saves
    std::map<int, SaveMigrator> migrators;

    // Per play session.
    Json globals = Json::object();       // name -> tagged value (wander::toTaggedJson)
    double playTimeBase = 0;              // play time of the loaded save
    double unscaledMark = 0;              // runtime unscaled time when it was loaded / play started
    std::set<std::string> baseKeys;       // persisted entities of the scene as play started (tombstones)
    std::set<std::string> tombstones;     // destroyed before a loaded save: stay destroyed
    struct Request {
        bool load = false;
        std::string slot;
        Json meta;
    };
    std::vector<Request> requests;

    // game.json `saves`, cached by file time.
    mutable int64_t settingsTime = INT64_MIN + 1;  // never a real stamp: the first call reads game.json
    mutable SaveSettings cached;
    mutable std::string gameId;

    explicit Impl(Engine& e) : engine(e) {}

    SaveStore store() const {
        return SaveStore(dir.empty() ? (fs::path(engine.config().projectDir) / ".skywalker" / "saves").string() : dir);
    }

    void refreshSettings() const {
        const std::string file = (fs::path(engine.config().projectDir) / "game.json").string();
        const int64_t t = fileModifiedNs(file);  // -1 = no game.json
        if (t == settingsTime) return;
        settingsTime = t;
        cached = SaveSettings{};
        gameId.clear();
        auto text = readText(file);
        if (!text) return;
        auto doc = Json::parse(*text);
        if (!doc) return;
        gameId = doc->get("id").asString();
        auto s = SaveSettings::fromJson(doc->get("saves"));
        if (s) cached = *s;
        else log::warn("saves", s.error().message);
    }

    void resetSession() {
        globals = Json::object();
        playTimeBase = 0;
        unscaledMark = 0;
        requests.clear();
        resetBase();
    }

    /// The scene changed (play started, the scene flow swapped scenes): its persisted entities are the base
    /// that tombstones refer to; tombstones of the scene left behind no longer apply.
    void resetBase() {
        tombstones.clear();
        baseKeys.clear();
        const Scene& s = engine.scene();
        for (EntityId e : s.entities()) {
            if (s.get<Persist>(e)) baseKeys.insert(keyOf(s, e));
        }
    }

    /// The scene a save records: the scene flow's id (alias or path) when the scene has a file.
    std::string sceneId() const {
        const SceneFlow& flow = engine.sceneFlow();
        return flow.currentPath().empty() ? std::string() : flow.current();
    }

    static Json idsJson(const std::vector<std::pair<uint64_t, uint64_t>>& ids) {
        Json a = Json::array();
        for (const auto& [from, to] : ids) a.push(Json::array({from, to}));
        return a;
    }
    static std::vector<std::pair<uint64_t, uint64_t>> idsOf(const Json& a) {
        std::vector<std::pair<uint64_t, uint64_t>> ids;
        for (const auto& p : a.elements()) ids.emplace_back(static_cast<uint64_t>(p[0].asInt()), static_cast<uint64_t>(p[1].asInt()));
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    double playTime() const { return playTimeBase + (engine.runtime().unscaledTime() - unscaledMark); }

    bool persisted(EntityId e) const {
        const Scene& s = engine.scene();
        return s.get<Persist>(e) != nullptr || spawnedRootAbove(s, e) != kNoEntity;
    }

    // --- capture ----------------------------------------------------------------------------

    Json captureEntity(EntityId e, std::vector<std::string>& warnings) const {
        Scene& s = engine.scene();
        const Persist& p = *s.get<Persist>(e);
        const EntityRecord* rec = s.record(e);
        Json j = Json::object({{"key", keyOf(s, e)}, {"id", e}, {"name", rec->name}, {"mode", p.mode}});
        if (p.spawned) {
            j["spawned"] = true;
            std::string prefab = p.prefab;
            if (prefab.empty() && rec->prefab.linked()) {
                if (const EntityRecord* root = s.record(rec->prefab.instance)) prefab = root->prefab.source;
            }
            if (!prefab.empty()) j["prefab"] = prefab;
        }
        if (p.mode == "all" || p.spawned) {
            Json snap = exactSnapshot(s, e);
            if (!p.spawned) snap.erase("behaviors");  // code comes from the game, not from the save
            if (!p.vars) snap.erase("vars");
            j["entity"] = std::move(snap);
        } else if (p.mode == "fields") {
            Json fields = Json::object();
            for (const auto& f : p.fields.elements()) {
                const std::string spec = f.asString();
                const size_t dot = spec.find('.');
                const std::string comp = spec.substr(0, dot);
                const ComponentKind* kind = s.componentKind(comp);
                if (!kind || !kind->has(s, e)) {
                    warnings.push_back(rec->name + ": persist field '" + spec + "' names no component of the entity");
                    continue;
                }
                Json all = Json::object({{comp, kind->toJson(s, e)}});
                makeExact(s, e, all);
                const Json& data = all.get(comp);
                if (dot == std::string::npos) {
                    fields[comp] = data;
                } else if (const Json* v = data.find(spec.substr(dot + 1))) {
                    fields[comp][spec.substr(dot + 1)] = *v;
                } else {
                    warnings.push_back(rec->name + ": persist field '" + spec + "' is not a field of " + comp);
                }
            }
            j["fields"] = std::move(fields);
        }
        if (p.vars && !j.contains("entity")) j["vars"] = rec->vars;
        if (p.spawned) {
            std::vector<EntityId> sub;
            collectSubtree(s, e, sub);
            Json subtree = Json::array();
            for (EntityId c : sub) subtree.push(exactSnapshot(s, c));
            j["subtree"] = std::move(subtree);
        }
        return j;
    }

    Json capture(const std::string& slot, const Json& meta, std::vector<std::string>& warnings, size_t& count) const {
        refreshSettings();
        Scene& s = engine.scene();
        Json entities = Json::array();
        std::set<std::string> present;
        for (EntityId e : s.entities()) {
            if (!s.get<Persist>(e) || spawnedRootAbove(s, e) != kNoEntity) continue;
            Json ej = captureEntity(e, warnings);
            const std::string key = ej.get("key").asString();
            if (!present.insert(key).second) warnings.push_back("two persisted entities share the key '" + key + "'; give them distinct persist.id");
            entities.push(std::move(ej));
        }
        count = entities.size();
        std::set<std::string> destroyed = tombstones;
        for (const auto& k : baseKeys) {
            if (!present.count(k)) destroyed.insert(k);
        }
        Json tomb = Json::array();
        for (const auto& k : destroyed) tomb.push(k);
        Json order = Json::array();
        for (EntityId e : s.entities()) order.push(e);
        Json doc = Json::object({{"format", kSaveFormat},
                                 {"formatVersion", kSaveFormatVersion},
                                 {"version", cached.version},
                                 {"game", gameId},
                                 {"slot", slot},
                                 {"savedAt", saveTimestamp()},
                                 {"engine", SKY_VERSION_STRING},
                                 {"tick", engine.runtime().frame()},
                                 {"playTime", playTime()},
                                 {"scene", sceneId()},
                                 {"sceneIds", idsJson(engine.sceneFlow().sceneIds())},
                                 {"meta", meta.isObject() ? meta : Json::object()},
                                 {"globals", globals},
                                 {"entities", std::move(entities)},
                                 {"destroyed", std::move(tomb)},
                                 {"order", std::move(order)},
                                 {"nextId", s.nextEntityId()}});
        doc["runtime"] = engine.runtime().saveState([this](EntityId e) { return persisted(e); });
        doc["hash"] = saveHash(doc);
        return doc;
    }

    // --- migrations -------------------------------------------------------------------------

    Status runWanderMigration(const std::string& file, int from, Json& doc) const {
        auto text = readText(engine.resolvePath(file));
        if (!text) return Error::make("migration_failed", "game.json saves.migrate: cannot read " + file);
        wander::CompileResult cr = wander::compile(*text, engine.runtime().compileOptions());
        if (!cr.ok()) {
            std::string first = cr.diagnostics.empty() ? "" : cr.diagnostics.front().message;
            return Error::make("migration_failed", file + " does not compile: " + first, "check it with wander_check");
        }
        // A scratch scene: migrations transform data, they never touch the game.
        Scene scratch;
        wander::Runtime rt(scratch, &engine.builtins());
        wander::Value data = wander::Value::map();
        for (const char* key : {"meta", "scene", "entities", "destroyed"}) data.mutMap().set(key, wander::fromJson(doc.get(key)));
        data.mutMap().set("globals", wander::fromTaggedJson(doc.get("globals")));
        wander::Runtime::FunctionCall call;
        call.scriptName = file;
        auto r = rt.callFunction(cr.program, "migrate", {wander::Value::number(from), std::move(data)}, call);
        if (!r.ok) {
            return Error::make("migration_failed",
                               file + ": migrate(" + std::to_string(from) + ", data) failed: " + r.error +
                                   (r.loc.line > 0 ? " (line " + std::to_string(r.loc.line) + ")" : ""),
                               "define `fn migrate(from: number, data: map) -> map` returning the upgraded data");
        }
        if (!r.value.isMap()) return Error::make("migration_failed", file + ": migrate() must return the data map");
        const wander::MapObj& out = r.value.mapObj();
        for (const char* key : {"meta", "scene", "entities", "destroyed"}) {
            if (const wander::Value* v = out.find(key)) doc[key] = wander::toJson(*v);
        }
        if (const wander::Value* g = out.find("globals")) doc["globals"] = wander::toTaggedJson(*g);
        return {};
    }

    /// Brings `doc` up to the game's current version. Returns the version it had.
    Result<int> migrate(Json& doc, std::vector<std::string>& warnings) const {
        refreshSettings();
        const int from = static_cast<int>(doc.get("version").asInt(1));
        const int target = cached.version;
        if (from > target) {
            return Error::make("save_too_new",
                               "the save was written by a newer version of the game (save version " + std::to_string(from) +
                                   ", this build reads up to " + std::to_string(target) + ")",
                               "update the game, or raise game.json saves.version if this build does understand it");
        }
        for (int v = from; v < target; ++v) {
            if (auto it = migrators.find(v); it != migrators.end()) {
                if (Status s = it->second(doc); !s) {
                    return Error::make("migration_failed", "migrating the save from version " + std::to_string(v) + " failed: " + s.error().message,
                                       s.error().hint);
                }
            } else if (!cached.migrate.empty()) {
                if (Status s = runWanderMigration(cached.migrate, v, doc); !s) return s.error();
            } else {
                warnings.push_back("no migration from save version " + std::to_string(v) + " to " + std::to_string(v + 1) +
                                   ": loaded as is (register one, or set game.json saves.migrate)");
            }
            doc["version"] = v + 1;
        }
        return from;
    }

    Result<Json> readMigrated(const std::string& slot, std::vector<std::string>& warnings, int& from) const {
        auto doc = store().read(slot);
        if (!doc) return doc.error();
        auto v = migrate(doc.value(), warnings);
        if (!v) return v.error();
        from = *v;
        return doc;
    }

    // --- restore ----------------------------------------------------------------------------

    /// Puts the game in the save's scene with the save's entity ids. Through the scene flow, like any scene
    /// change at run time: an immediate one (no transition, no loading scene), carried entities come along
    /// and keep running. A change under way in the running game is dropped either way.
    Status switchScene(const Json& doc, Outcome& out) {
        SceneFlow& flow = engine.sceneFlow();
        const std::string scene = doc.get("scene").asString();
        if (scene.empty()) {  // saved from an unsaved scene: restore into whatever runs
            flow.cancel();
            return {};
        }
        auto path = flow.resolve(scene);
        if (!path) {
            return Error::make("scene_not_found", "the save was made in " + scene + ", which no longer exists", path.error().hint);
        }
        const auto ids = idsOf(doc.get("sceneIds"));
        if (*path == flow.currentPath() && ids == flow.sceneIds()) {
            flow.cancel();
            return {};
        }
        ChangeOptions options;
        options.immediate = true;
        options.hasTransition = true;  // kind "none": a load cuts
        options.exactIds = true;
        options.ids = ids;
        if (Status st = flow.requestChange(*path, options); !st) return st;
        out.sceneChanged = scene;
        return {};
    }

    Status applyEntity(const Json& ej, EntityId id, Outcome& out) {
        Scene& s = engine.scene();
        if (const Json* snap = ej.find("entity")) {
            Json data = *snap;
            if (!ej.get("spawned").asBool()) {
                // Keep the game's current code; keep vars when the save left them out.
                const Json now = s.entityToJson(id);
                if (const Json* b = now.find("behaviors")) data["behaviors"] = *b;
                else data.erase("behaviors");
                if (!data.contains("vars")) data["vars"] = now.get("vars");
            }
            s.restoreEntity(id, data);
        } else if (const Json* fields = ej.find("fields")) {
            for (const auto& [comp, patch] : fields->members()) {
                if (Status st = s.patchComponent(id, comp, patch); !st) {
                    out.warnings.push_back(ej.get("name").asString() + ": " + comp + " not restored: " + st.error().message);
                }
            }
        }
        if (const Json* vars = ej.find("vars"); vars && vars->isObject()) {
            s.record(id)->vars = *vars;
            s.markDirty();
        }
        return {};
    }

    Status apply(const Json& doc, Outcome& out) {
        Scene& s = engine.scene();
        if (Status st = switchScene(doc, out); !st) return st;
        std::map<std::string, EntityId> current;  // entities inside a spawned subtree come back with their root
        for (EntityId e : s.entities()) {
            if (s.get<Persist>(e) && spawnedRootAbove(s, e) == kNoEntity) current.emplace(keyOf(s, e), e);
        }
        std::set<std::string> saved;
        for (const auto& ej : doc.get("entities").elements()) saved.insert(ej.get("key").asString());

        // 1. Tombstones and persisted entities spawned after the save.
        std::set<std::string> tomb;
        for (const auto& k : doc.get("destroyed").elements()) tomb.insert(k.asString());
        for (const auto& [key, e] : current) {
            if (!s.exists(e)) continue;
            const Persist* p = s.get<Persist>(e);
            const bool after = !saved.count(key) && (p->spawned || !baseKeys.count(key));
            if (tomb.count(key) || after) out.destroyed += s.destroy(e) > 0 ? 1 : 0;
        }
        // 2. Saved entities: restore in place, or recreate spawned subtrees with their saved ids.
        for (const auto& ej : doc.get("entities").elements()) {
            const std::string key = ej.get("key").asString();
            auto it = current.find(key);
            EntityId id = it != current.end() && s.exists(it->second) ? it->second : kNoEntity;
            if (!id) {
                if (!ej.get("spawned").asBool()) {
                    out.warnings.push_back("'" + ej.get("name").asString() + "' (" + key + ") is not in the scene: not restored");
                    continue;
                }
                id = static_cast<EntityId>(ej.get("id").asInt());
                bool clash = s.exists(id);
                for (const auto& c : ej.get("subtree").elements()) clash = clash || s.exists(static_cast<EntityId>(c.get("id").asInt()));
                if (clash) {
                    out.warnings.push_back("'" + ej.get("name").asString() + "' could not be recreated: its id is used by another entity");
                    continue;
                }
                s.restoreEntity(id, ej.get("entity"));
                for (const auto& c : ej.get("subtree").elements()) s.restoreEntity(static_cast<EntityId>(c.get("id").asInt()), c);
                ++out.spawned;
                continue;
            }
            if (Status st = applyEntity(ej, id, out); !st) return st;
            if (ej.get("spawned").asBool()) {
                // Its children are part of the save: rebuild the subtree exactly.
                for (EntityId c : s.children(id)) s.destroy(c);
                for (const auto& c : ej.get("subtree").elements()) s.restoreEntity(static_cast<EntityId>(c.get("id").asInt()), c);
            }
        }
        out.entities = saved.size();
        // 3. Order (behaviors run in scene order) and the next id (spawns replay with the same ids).
        std::vector<EntityId> order;
        std::set<EntityId> placed;
        for (const auto& e : doc.get("order").elements()) {
            EntityId id = static_cast<EntityId>(e.asInt());
            if (s.exists(id) && placed.insert(id).second) order.push_back(id);
        }
        for (EntityId e : s.entities()) {
            if (placed.insert(e).second) order.push_back(e);
        }
        s.setOrder(order);
        s.setNextEntityId(static_cast<EntityId>(doc.get("nextId").asInt(1)));
        tombstones = tomb;
        return {};
    }
};

// --- SaveSystem -------------------------------------------------------------------------------

SaveSystem::SaveSystem(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}
SaveSystem::~SaveSystem() = default;

void SaveSystem::setDirectory(std::string dir) { impl_->dir = std::move(dir); }
std::string SaveSystem::directory() const { return impl_->store().dir(); }

SaveSettings SaveSystem::settings() const {
    impl_->refreshSettings();
    return impl_->cached;
}

void SaveSystem::addMigrator(int fromVersion, SaveMigrator migrator) { impl_->migrators[fromVersion] = std::move(migrator); }

void SaveSystem::beginPlay() { impl_->resetSession(); }

void SaveSystem::endPlay() {
    impl_->resetSession();
    impl_->baseKeys.clear();
}

void SaveSystem::sceneChanged() { impl_->resetBase(); }

void SaveSystem::endTick() {
    if (impl_->requests.empty()) return;
    std::vector<Impl::Request> requests = std::move(impl_->requests);
    impl_->requests.clear();
    wander::Runtime& rt = impl_->engine.runtime();
    for (const auto& r : requests) {
        auto result = r.load ? load(r.slot) : save(r.slot, r.meta);
        if (!result) {
            const std::string what = r.load ? "load_game" : "save_game";
            rt.log(kNoEntity, what + "(\"" + r.slot + "\") failed: " + result.error().message, "saves");
            rt.emitJson(r.load ? "load_failed" : "save_failed", kNoEntity,
                        Json::object({{"slot", r.slot}, {"error", result.error().code}, {"message", result.error().message}}));
        }
        if (r.load && result) break;  // the game is now the loaded one: later requests of the old timeline are dropped
    }
}

const Json& SaveSystem::globalsJson() const { return impl_->globals; }

Status SaveSystem::setGlobal(const std::string& name, const Json& taggedValue) {
    if (name.empty()) return Error::make("invalid_argument", "a game variable needs a name");
    if (taggedValue.isNull()) {
        impl_->globals.erase(name);
    } else {
        impl_->globals[name] = taggedValue;
    }
    return {};
}

double SaveSystem::playTime() const { return impl_->playTime(); }

Json SaveSystem::Outcome::toJson() const {
    Json warn = Json::array();
    for (const auto& w : warnings) warn.push(w);
    Json j = info.toJson();
    j["entities"] = entities;
    if (spawned) j["spawned"] = spawned;
    if (destroyed) j["destroyed"] = destroyed;
    if (migratedFrom) j["migratedFrom"] = migratedFrom;
    if (!sceneChanged.empty()) j["sceneChanged"] = sceneChanged;
    j["warnings"] = warn;
    return j;
}

namespace {
Status requirePlaying(const Engine& engine, const char* what) {
    if (engine.playState() != PlayState::Editing) return {};
    return Error::make("not_playing", std::string(what) + " needs a running game",
                       "start play first (sim_control {\"action\": \"play\"}); save games capture and restore play state");
}
}  // namespace

Result<SaveSystem::Outcome> SaveSystem::save(const std::string& slot, const Json& meta, const std::string& actor) {
    if (Status s = validateSlotName(slot); !s) return s.error();
    if (Status s = requirePlaying(impl_->engine, "saving"); !s) return s.error();
    if (!meta.isObject() && !meta.isNull()) {
        return Error::make("invalid_argument", "save metadata must be an object, e.g. {\"title\": \"Chapter 2\"}");
    }
    const SaveSettings cfg = settings();
    const SaveStore store = impl_->store();
    if (!isReservedSlot(slot) && !store.exists(slot)) {
        size_t named = 0;
        for (const auto& info : store.list()) named += isReservedSlot(info.slot) ? 0 : 1;
        if (named >= static_cast<size_t>(cfg.maxSlots)) {
            return Error::make("slot_limit", "all " + std::to_string(cfg.maxSlots) + " save slots are used",
                               "overwrite or delete a slot (save_delete), or raise game.json saves.maxSlots");
        }
    }
    Outcome out;
    Json doc = impl_->capture(slot, meta, out.warnings, out.entities);
    if (Status s = store.write(slot, doc, cfg.compress); !s) return s.error();
    for (const auto& info : store.list()) {
        if (info.slot == slot) out.info = info;
    }
    impl_->engine.runtime().emitJson("saved", kNoEntity, Json::object({{"slot", slot}, {"meta", doc.get("meta")}}));
    impl_->engine.emitEvent(Json::object({{"type", "save"}, {"action", "saved"}, {"slot", slot}, {"actor", actor}}));
    return out;
}

Result<SaveSystem::Outcome> SaveSystem::load(const std::string& slot, const std::string& actor) {
    if (Status s = requirePlaying(impl_->engine, "loading"); !s) return s.error();
    Outcome out;
    int from = 0;
    auto doc = impl_->readMigrated(slot, out.warnings, from);
    if (!doc) return doc.error();
    const Json& d = doc.value();
    if (from != static_cast<int>(d.get("version").asInt())) out.migratedFrom = from;
    Engine& engine = impl_->engine;
    // The scene part is one atomic play-time edit (rolled back if it fails); the runtime follows it.
    Status st = engine.edit(actor, "Load game: " + slot, [&]() -> Status { return impl_->apply(d, out); });
    if (!st) return st.error();
    if (Status rs = engine.runtime().loadState(d.get("runtime"), out.warnings); !rs) return rs.error();
    engine.physics().endPlay();  // rebuilt from the restored scene (positions, velocities) on the next tick
    engine.physics().beginPlay();
    engine.navigation().endPlay();
    engine.navigation().beginPlay();
    impl_->globals = d.get("globals").isObject() ? d.get("globals") : Json::object();
    impl_->playTimeBase = d.get("playTime").asNumber();
    impl_->unscaledMark = engine.runtime().unscaledTime();
    impl_->requests.clear();
    for (const auto& info : impl_->store().list()) {
        if (info.slot == slot) out.info = info;
    }
    engine.runtime().emitJson("loaded", kNoEntity,
                              Json::object({{"slot", slot}, {"version", d.get("version")}, {"meta", d.get("meta")}}));
    engine.emitEvent(Json::object({{"type", "save"}, {"action", "loaded"}, {"slot", slot}, {"actor", actor}}));
    return out;
}

Result<Json> SaveSystem::inspect(const std::string& slot, size_t maxDiffs) {
    std::vector<std::string> warnings;
    int from = 0;
    auto doc = impl_->readMigrated(slot, warnings, from);
    if (!doc) return doc.error();
    const Json& d = doc.value();
    Scene& s = impl_->engine.scene();
    std::map<std::string, EntityId> current;
    for (EntityId e : s.entities()) {
        if (s.get<Persist>(e) && spawnedRootAbove(s, e) == kNoEntity) current.emplace(keyOf(s, e), e);
    }
    size_t budget = maxDiffs;
    Json entities = Json::array();
    size_t same = 0, differ = 0, missing = 0, extra = 0, tombstoned = 0;
    std::set<std::string> saved;
    for (const auto& ej : d.get("entities").elements()) {
        const std::string key = ej.get("key").asString();
        saved.insert(key);
        Json row = Json::object({{"key", key}, {"name", ej.get("name")}});
        auto it = current.find(key);
        if (it == current.end()) {
            ++missing;
            row["status"] = "missing";
            row["note"] = ej.get("spawned").asBool() ? "spawned at run time: a load recreates it"
                                                     : "not in the scene: a load cannot restore it";
            entities.push(std::move(row));
            continue;
        }
        row["id"] = it->second;
        Json diffs = Json::array();
        const Json now = exactSnapshot(s, it->second);
        if (const Json* snap = ej.find("entity")) {
            for (const char* k : {"name", "enabled", "tags", "parent"}) diffJson(k, snap->get(k), now.get(k), diffs, budget);
            diffJson("components", snap->get("components"), now.get("components"), diffs, budget);
            if (snap->contains("vars")) diffJson("vars", snap->get("vars"), now.get("vars"), diffs, budget);
        }
        if (const Json* fields = ej.find("fields")) {
            for (const auto& [comp, data] : fields->members()) {
                Json cur = Json::object();
                for (const auto& [f, v] : data.members()) cur[f] = now.get("components").get(comp).get(f);
                diffJson("components." + comp, data, cur, diffs, budget);
            }
        }
        if (const Json* vars = ej.find("vars")) diffJson("vars", *vars, now.get("vars"), diffs, budget);
        row["status"] = diffs.elements().empty() ? "same" : "differs";
        (diffs.elements().empty() ? same : differ)++;
        if (!diffs.elements().empty()) row["diffs"] = std::move(diffs);
        entities.push(std::move(row));
    }
    std::set<std::string> tomb;
    for (const auto& k : d.get("destroyed").elements()) tomb.insert(k.asString());
    for (const auto& [key, e] : current) {
        if (tomb.count(key)) {
            ++tombstoned;
            entities.push(Json::object({{"key", key}, {"id", e}, {"name", s.record(e)->name}, {"status", "destroyed_in_save"},
                                        {"note", "destroyed before the save: a load removes it"}}));
        } else if (!saved.count(key)) {
            ++extra;
            entities.push(Json::object({{"key", key}, {"id", e}, {"name", s.record(e)->name}, {"status", "not_in_save"},
                                        {"note", s.get<Persist>(e)->spawned || !impl_->baseKeys.count(key)
                                                     ? "spawned after the save: a load removes it"
                                                     : "persisted after the save was made: a load leaves it as is"}}));
        }
    }
    Json globalDiffs = Json::array();
    size_t gBudget = maxDiffs;
    diffJson("", d.get("globals"), impl_->globals, globalDiffs, gBudget);
    Json warn = Json::array();
    for (const auto& w : warnings) warn.push(w);
    if (budget == 0) warn.push("more than " + std::to_string(maxDiffs) + " differences: the list is cut (raise max_diffs)");
    const std::string& savedScene = d.get("scene").asString();
    const SceneFlow& flow = impl_->engine.sceneFlow();
    auto savedPath = flow.resolve(savedScene);
    const bool sameScene = savedScene.empty() || (savedPath && *savedPath == flow.currentPath() &&
                                                  Impl::idsOf(d.get("sceneIds")) == flow.sceneIds());
    Json result = Json::object({{"slot", slot},
                                {"version", d.get("version")},
                                {"scene", Json::object({{"saved", savedScene}, {"current", impl_->sceneId()}, {"same", sameScene}})},
                                {"tick", Json::object({{"saved", d.get("tick")}, {"current", impl_->engine.runtime().frame()}})},
                                {"summary", Json::object({{"same", same},
                                                          {"differs", differ},
                                                          {"missing", missing},
                                                          {"destroyedInSave", tombstoned},
                                                          {"notInSave", extra}})},
                                {"entities", std::move(entities)},
                                {"globals", std::move(globalDiffs)},
                                {"warnings", std::move(warn)}});
    if (from != static_cast<int>(d.get("version").asInt())) result["migratedFrom"] = from;
    return result;
}

Status SaveSystem::remove(const std::string& slot) { return impl_->store().remove(slot); }
std::vector<SaveInfo> SaveSystem::list() const { return impl_->store().list(); }
bool SaveSystem::has(const std::string& slot) const { return impl_->store().exists(slot); }

void SaveSystem::requestSave(const std::string& slot, const Json& meta) { impl_->requests.push_back({false, slot, meta}); }
void SaveSystem::requestLoad(const std::string& slot) { impl_->requests.push_back({true, slot, Json()}); }

}  // namespace sky::game
