#include "skywalker/scene/Scene.h"

#include <algorithm>
#include <charconv>

#include "skywalker/core/Strings.h"

namespace sky {

std::string formatEntityRef(EntityId id) { return "#" + std::to_string(id); }

namespace {

template <typename T>
ComponentKind makeReflectedKind() {
    ComponentKind k;
    k.info = &T::type();
    k.name = k.info->name;
    k.has = [](const Scene& s, EntityId id) { return s.get<T>(id) != nullptr; };
    k.add = [](Scene& s, EntityId id) { s.add<T>(id); };
    k.remove = [](Scene& s, EntityId id) { s.registry().remove<T>(s.handle(id)); };
    k.toJson = [](const Scene& s, EntityId id) -> Json {
        const T* c = s.get<T>(id);
        return c ? reflect::toJson(c, T::type()) : Json();
    };
    k.apply = [](Scene& s, EntityId id, const Json& patch) -> Status {
        T* c = s.get<T>(id);
        if (c) return reflect::applyJson(c, T::type(), patch);
        // Validate on a temporary first so a bad patch does not leave a half-added component.
        T tmp{};
        Status st = reflect::applyJson(&tmp, T::type(), patch);
        if (!st) return st;
        s.add<T>(id) = std::move(tmp);
        return {};
    };
    return k;
}

Json behaviorsToJson(const Behavior* b) {
    Json arr = Json::array();
    if (!b) return arr;
    for (const auto& s : b->scripts) {
        arr.push(Json::object(
            {{"name", s.name}, {"intent", s.intent}, {"source", s.source}, {"enabled", s.enabled}}));
    }
    return arr;
}

}  // namespace

Scene::Scene() { registerKinds(); }

void Scene::registerKinds() {
    kinds_.push_back(makeReflectedKind<Transform>());
    kinds_.push_back(makeReflectedKind<MeshRenderer>());
    kinds_.push_back(makeReflectedKind<Light>());
    kinds_.push_back(makeReflectedKind<Camera>());
    kinds_.push_back(makeReflectedKind<ParticleEmitter>());
    kinds_.push_back(makeReflectedKind<Water>());
    kinds_.push_back(makeReflectedKind<Terrain>());
    kinds_.push_back(makeReflectedKind<Foliage>());
    kinds_.push_back(makeReflectedKind<FluidVolume>());
    kinds_.push_back(makeReflectedKind<AudioSource>());
    kinds_.push_back(makeReflectedKind<AudioListener>());
    kinds_.push_back(makeReflectedKind<RigidBody>());
    kinds_.push_back(makeReflectedKind<Collider>());
    kinds_.push_back(makeReflectedKind<CharacterController>());
    kinds_.push_back(makeReflectedKind<Joint>());
    kinds_.push_back(makeReflectedKind<PhysicsSettings>());
    kinds_.push_back(makeReflectedKind<NavAgent>());
    kinds_.push_back(makeReflectedKind<NavMeshSurface>());
    kinds_.push_back(makeReflectedKind<Animator>());
    kinds_.push_back(makeReflectedKind<BoneAttachment>());
    kinds_.push_back(makeReflectedKind<SequencePlayer>());
    kinds_.push_back(makeReflectedKind<IkTarget>());
}

const ComponentKind* Scene::componentKind(std::string_view n) const {
    for (const auto& k : kinds_) {
        if (k.name == n) return &k;
    }
    return nullptr;
}

std::vector<std::string> Scene::componentNames() const {
    std::vector<std::string> names;
    for (const auto& k : kinds_) names.push_back(k.name);
    return names;
}

EntityId Scene::create(std::string entityName, EntityId parent, EntityId forcedId, size_t orderHint) {
    EntityId id = forcedId != kNoEntity && !exists(forcedId) ? forcedId : nextId_;
    nextId_ = std::max(nextId_, id + 1);
    notify(id);  // "before" state of a new entity is "absent"

    EntityRecord rec;
    rec.id = id;
    rec.name = entityName.empty() ? "Entity " + std::to_string(id) : std::move(entityName);
    rec.parent = exists(parent) ? parent : kNoEntity;
    rec.handle = registry_.create();
    registry_.emplace<Transform>(rec.handle);
    records_.emplace(id, std::move(rec));
    if (orderHint < order_.size()) {
        order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(orderHint), id);
    } else {
        order_.push_back(id);
    }
    return id;
}

size_t Scene::destroy(EntityId id) {
    if (!exists(id)) return 0;
    size_t count = 0;
    // Notify parent-first so History can restore in the same order.
    notify(id);
    for (EntityId child : children(id)) count += destroy(child);
    auto it = records_.find(id);
    registry_.destroy(it->second.handle);
    records_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    return count + 1;
}

void Scene::clear() {
    registry_.clear();
    records_.clear();
    order_.clear();
    nextId_ = 1;
    environment_ = Environment{};
    ++revision_;
}

EntityRecord* Scene::record(EntityId id) {
    auto it = records_.find(id);
    return it == records_.end() ? nullptr : &it->second;
}

const EntityRecord* Scene::record(EntityId id) const {
    auto it = records_.find(id);
    return it == records_.end() ? nullptr : &it->second;
}

ecs::Entity Scene::handle(EntityId id) const {
    auto it = records_.find(id);
    return it == records_.end() ? ecs::Entity{} : it->second.handle;
}

std::vector<EntityId> Scene::children(EntityId id) const {
    std::vector<EntityId> out;
    for (EntityId e : order_) {
        if (records_.at(e).parent == id) out.push_back(e);
    }
    return out;
}

void Scene::setOrder(const std::vector<EntityId>& order) {
    std::vector<EntityId> result;
    result.reserve(order_.size());
    std::unordered_map<EntityId, bool> placed;
    for (EntityId id : order) {
        if (exists(id) && !placed[id]) {
            result.push_back(id);
            placed[id] = true;
        }
    }
    for (EntityId id : order_) {
        if (!placed[id]) result.push_back(id);
    }
    order_ = std::move(result);
    ++revision_;
}

EntityId Scene::find(std::string_view nameOrId) const {
    std::string_view s = nameOrId;
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    uint64_t parsed = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (res.ec == std::errc() && res.ptr == s.data() + s.size() && exists(parsed)) return parsed;
    for (EntityId e : order_) {
        if (records_.at(e).name == nameOrId) return e;
    }
    std::string lowered = str::lower(nameOrId);
    for (EntityId e : order_) {
        if (str::lower(records_.at(e).name) == lowered) return e;
    }
    return kNoEntity;
}

std::vector<EntityId> Scene::findTagged(std::string_view tag) const {
    std::vector<EntityId> out;
    for (EntityId e : order_) {
        const auto& tags = records_.at(e).tags;
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) out.push_back(e);
    }
    return out;
}

static Error notFound(EntityId id) {
    return Error::make("not_found", "entity " + formatEntityRef(id) + " does not exist",
                       "use scene_query to list entities and their ids");
}

Status Scene::rename(EntityId id, std::string newName) {
    if (!exists(id)) return notFound(id);
    if (newName.empty()) return Error::make("invalid_value", "entity name must not be empty");
    notify(id);
    records_[id].name = std::move(newName);
    return {};
}

bool Scene::wouldCycle(EntityId id, EntityId newParent) const {
    for (EntityId p = newParent; p != kNoEntity; p = records_.at(p).parent) {
        if (p == id) return true;
    }
    return false;
}

Status Scene::setParent(EntityId id, EntityId parent) {
    if (!exists(id)) return notFound(id);
    if (parent != kNoEntity && !exists(parent)) return notFound(parent);
    if (wouldCycle(id, parent)) {
        return Error::make("invalid_hierarchy", "cannot parent " + formatEntityRef(id) + " under its own descendant");
    }
    notify(id);
    records_[id].parent = parent;
    return {};
}

Status Scene::setTags(EntityId id, std::vector<std::string> tags) {
    if (!exists(id)) return notFound(id);
    notify(id);
    records_[id].tags = std::move(tags);
    return {};
}

Status Scene::setEnabled(EntityId id, bool enabled) {
    if (!exists(id)) return notFound(id);
    notify(id);
    records_[id].enabled = enabled;
    return {};
}

Status Scene::patchVars(EntityId id, const Json& patch) {
    if (!exists(id)) return notFound(id);
    if (!patch.isObject()) return Error::make("invalid_value", "vars must be an object");
    notify(id);
    records_[id].vars.mergePatch(patch);
    return {};
}

Status Scene::patchComponent(EntityId id, std::string_view component, const Json& patch) {
    if (!exists(id)) return notFound(id);
    const ComponentKind* kind = componentKind(component);
    if (!kind) {
        std::string guess = str::closest(component, componentNames(), 3);
        std::string all;
        for (const auto& n : componentNames()) all += (all.empty() ? "" : ", ") + n;
        return Error::make("unknown_component", "unknown component \"" + std::string(component) + "\" (known: " + all + ")",
                           guess.empty() ? "" : "did you mean \"" + guess + "\"?");
    }
    if (patch.isNull()) {
        if (component == "transform") return Error::make("invalid_value", "transform cannot be removed");
        notify(id);
        kind->remove(*this, id);
        return {};
    }
    notify(id);
    return kind->apply(*this, id, patch);
}

Status Scene::setBehaviors(EntityId id, const Json& behaviors) {
    if (!exists(id)) return notFound(id);
    if (!behaviors.isArray()) {
        return Error::make("invalid_value", "behaviors must be an array of {name, intent, source, enabled}");
    }
    std::vector<Script> scripts;
    for (const auto& b : behaviors.elements()) {
        if (!b.isObject()) return Error::make("invalid_value", "each behavior must be an object");
        Script s;
        s.name = b.get("name").asString("Behavior");
        s.intent = b.get("intent").asString();
        s.source = b.get("source").asString();
        s.enabled = b.get("enabled").asBool(true);
        scripts.push_back(std::move(s));
    }
    notify(id);
    ecs::Entity h = handle(id);
    if (scripts.empty()) {
        registry_.remove<Behavior>(h);
        return {};
    }
    // Keep compiled programs for unchanged sources (avoids needless recompiles).
    if (Behavior* old = registry_.get<Behavior>(h)) {
        for (auto& s : scripts) {
            for (const auto& o : old->scripts) {
                if (o.compiledSource == s.source && o.program) {
                    s.program = o.program;
                    s.compiledSource = o.compiledSource;
                    s.hasErrors = o.hasErrors;
                }
            }
        }
    }
    registry_.emplace<Behavior>(h).scripts = std::move(scripts);
    return {};
}

Status Scene::patchEnvironment(const Json& patch) {
    if (observer_) observer_->beforeEnvironmentChange();
    ++revision_;
    return reflect::applyJson(&environment_, Environment::type(), patch);
}

Status Scene::applyEntityJson(EntityId id, const Json& doc) {
    if (!exists(id)) return notFound(id);
    if (!doc.isObject()) return Error::make("invalid_value", "entity document must be an object");
    static const std::vector<std::string> kKeys{"id", "name", "tags", "parent", "enabled", "vars", "components", "behaviors"};
    for (const auto& [key, value] : doc.members()) {
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) {
            // Be forgiving: a top-level component name is treated as a component patch.
            if (componentKind(key)) continue;
            std::string guess = str::closest(key, kKeys, 3);
            return Error::make("unknown_field", "entity has no field \"" + key + "\"",
                               guess.empty() ? "valid: name, tags, parent, enabled, vars, components, behaviors"
                                             : "did you mean \"" + guess + "\"?");
        }
    }
    if (const Json* v = doc.find("name"); v && v->isString()) {
        if (Status s = rename(id, v->asString()); !s) return s;
    }
    if (const Json* v = doc.find("tags"); v && v->isArray()) {
        std::vector<std::string> tags;
        for (const auto& t : v->elements()) tags.push_back(t.asString());
        if (Status s = setTags(id, std::move(tags)); !s) return s;
    }
    if (const Json* v = doc.find("parent")) {
        EntityId p = v->isNumber() ? static_cast<EntityId>(v->asInt()) : (v->isString() ? find(v->asString()) : kNoEntity);
        if (v->isString() && p == kNoEntity && !v->asString().empty()) {
            return Error::make("not_found", "parent \"" + v->asString() + "\" not found");
        }
        if (Status s = setParent(id, p); !s) return s;
    }
    if (const Json* v = doc.find("enabled"); v && v->isBool()) {
        if (Status s = setEnabled(id, v->asBool()); !s) return s;
    }
    if (const Json* v = doc.find("vars"); v && v->isObject()) {
        if (Status s = patchVars(id, *v); !s) return s;
    }
    if (const Json* v = doc.find("components"); v && v->isObject()) {
        for (const auto& [comp, patch] : v->members()) {
            if (Status s = patchComponent(id, comp, patch); !s) return s;
        }
    }
    for (const auto& [key, value] : doc.members()) {
        if (componentKind(key)) {
            if (Status s = patchComponent(id, key, value); !s) return s;
        }
    }
    if (const Json* v = doc.find("behaviors")) {
        if (Status s = setBehaviors(id, *v); !s) return s;
    }
    return {};
}

Mat4 Scene::worldMatrix(EntityId id) const {
    const EntityRecord* rec = record(id);
    if (!rec) return {};
    const Transform* t = registry_.get<Transform>(rec->handle);
    Mat4 local = t ? t->local() : Mat4{};
    if (rec->parent != kNoEntity && exists(rec->parent)) return worldMatrix(rec->parent) * local;
    return local;
}

Aabb Scene::localBounds(EntityId id) const {
    const MeshRenderer* m = get<MeshRenderer>(id);
    if (!m) return {Vec3(-0.5f), Vec3(0.5f)};
    const std::string& mesh = m->mesh;
    if (mesh == "plane") return {{-0.5f, -0.01f, -0.5f}, {0.5f, 0.01f, 0.5f}};
    if (mesh == "quad") return {{-0.5f, -0.5f, -0.01f}, {0.5f, 0.5f, 0.01f}};
    if (mesh == "torus") return {{-0.5f, -0.15f, -0.5f}, {0.5f, 0.15f, 0.5f}};
    if (mesh == "capsule") return {{-0.25f, -0.5f, -0.25f}, {0.25f, 0.5f, 0.25f}};
    if (auto it = assetBounds.find(mesh); it != assetBounds.end()) return it->second;
    return {Vec3(-0.5f), Vec3(0.5f)};
}

bool Scene::isActive(EntityId id) const {
    for (const EntityRecord* r = record(id); r; r = r->parent ? record(r->parent) : nullptr) {
        if (!r->enabled) return false;
    }
    return true;
}

Json Scene::entityToJson(EntityId id) const {
    const EntityRecord* rec = record(id);
    if (!rec) return {};
    Json tags = Json::array();
    for (const auto& t : rec->tags) tags.push(t);
    Json comps = Json::object();
    for (const auto& k : kinds_) {
        if (k.has(*this, id)) comps[k.name] = k.toJson(*this, id);
    }
    Json doc = Json::object({{"id", rec->id},
                             {"name", rec->name},
                             {"parent", rec->parent},
                             {"enabled", rec->enabled},
                             {"tags", tags},
                             {"vars", rec->vars},
                             {"components", comps}});
    const Behavior* b = registry_.get<Behavior>(rec->handle);
    if (b && !b->scripts.empty()) doc["behaviors"] = behaviorsToJson(b);
    return doc;
}

Json Scene::snapshotEntity(EntityId id) const {
    Json snap = entityToJson(id);
    if (snap.isNull()) return snap;
    auto it = std::find(order_.begin(), order_.end(), id);
    snap["_order"] = static_cast<uint64_t>(it - order_.begin());
    return snap;
}

void Scene::restoreEntity(EntityId id, const Json& snap) {
    ChangeObserver* saved = observer_;
    observer_ = nullptr;  // restoring is not itself a recorded change
    if (snap.isNull()) {
        destroy(id);
        observer_ = saved;
        return;
    }
    if (!exists(id)) {
        create(snap.get("name").asString(), kNoEntity, id, static_cast<size_t>(snap.get("_order").asInt(SIZE_MAX)));
    }
    EntityRecord& rec = records_[id];
    rec.name = snap.get("name").asString();
    EntityId parent = static_cast<EntityId>(snap.get("parent").asInt());
    rec.parent = exists(parent) ? parent : kNoEntity;
    rec.enabled = snap.get("enabled").asBool(true);
    rec.vars = snap.get("vars").isObject() ? snap.get("vars") : Json::object();
    rec.tags.clear();
    for (const auto& t : snap.get("tags").elements()) rec.tags.push_back(t.asString());
    for (const auto& k : kinds_) {
        if (k.name != "transform") k.remove(*this, id);
    }
    for (const auto& [comp, data] : snap.get("components").members()) {
        if (const ComponentKind* k = componentKind(comp)) (void)k->apply(*this, id, data);
    }
    (void)setBehaviors(id, snap.get("behaviors").isArray() ? snap.get("behaviors") : Json::array());
    ++revision_;
    observer_ = saved;
}

Json Scene::toJson() const {
    Json entities = Json::array();
    for (EntityId e : order_) entities.push(entityToJson(e));
    return Json::object({{"format", "skywalker.scene"},
                         {"version", 1},
                         {"name", name},
                         {"seed", seed},
                         {"environment", reflect::toJson(&environment_, Environment::type())},
                         {"entities", entities}});
}

Status Scene::loadJson(const Json& doc) {
    if (!doc.isObject() || doc.get("format").asString() != "skywalker.scene") {
        return Error::make("invalid_scene", "not a skywalker scene (missing \"format\": \"skywalker.scene\")");
    }
    ChangeObserver* saved = observer_;
    observer_ = nullptr;
    clear();
    name = doc.get("name").asString("Untitled");
    seed = static_cast<uint64_t>(doc.get("seed").asInt(1));
    Status result;
    if (const Json* env = doc.find("environment")) {
        if (Status s = reflect::applyJson(&environment_, Environment::type(), *env); !s) result = s;
    }
    // Two passes: create all entities (so parents resolve regardless of order), then apply.
    for (const auto& e : doc.get("entities").elements()) {
        create(e.get("name").asString(), kNoEntity, static_cast<EntityId>(e.get("id").asInt()));
    }
    size_t index = 0;
    for (const auto& e : doc.get("entities").elements()) {
        EntityId id = static_cast<EntityId>(e.get("id").asInt());
        if (id == kNoEntity) id = order_[index];
        ++index;
        Json body = e;
        body.erase("id");
        if (Status s = applyEntityJson(id, body); !s && result.ok()) {
            result = Error::make(s.error().code, "entity " + formatEntityRef(id) + ": " + s.error().message, s.error().hint);
        }
    }
    observer_ = saved;
    ++revision_;
    return result;
}

}  // namespace sky
