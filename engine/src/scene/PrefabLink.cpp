#include "skywalker/scene/PrefabLink.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <unordered_set>

#include "skywalker/core/Strings.h"

namespace sky {

std::string PrefabTemplate::nodeName(uint32_t pid) const {
    const EntityRecord* r = scene ? scene->record(pid) : nullptr;
    return r ? r->name : std::string();
}

namespace {

uint64_t hashText(const std::string& s) {  // FNV-1a: stable across runs and platforms
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool isLinkField(const FieldInfo& f) { return f.type == FieldType::Entity || f.type == FieldType::EntityList; }

const FieldInfo* fieldOf(const Scene& scene, const std::string& component, const std::string& field) {
    const ComponentKind* k = scene.componentKind(component);
    return k && k->info ? k->info->field(field) : nullptr;
}

/// Rewrites every entity link inside a component JSON object (or one field value) with `fn`.
void mapLinkValue(const FieldInfo& f, Json& value, const std::function<Json(const Json&)>& fn) {
    if (f.type == FieldType::Entity) {
        value = fn(value);
    } else if (f.type == FieldType::EntityList && value.isArray()) {
        Json out = Json::array();
        for (const auto& e : value.elements()) {
            Json m = fn(e);
            if (!m.isNull()) out.push(std::move(m));
        }
        value = std::move(out);
    }
}

void mapComponentLinks(const Scene& scene, const std::string& component, Json& comp,
                       const std::function<Json(const Json&)>& fn) {
    const ComponentKind* k = scene.componentKind(component);
    if (!k || !k->info || !comp.isObject()) return;
    for (const FieldInfo& f : k->info->fields) {
        if (!isLinkField(f)) continue;
        if (Json* v = comp.find(f.name)) mapLinkValue(f, *v, fn);
    }
}

/// Maps the links inside an override value (a field, a whole component, or nothing).
void mapOverrideLinks(const Scene& scene, const std::string& property, Json& value,
                      const std::function<Json(const Json&)>& fn) {
    auto dot = property.find('.');
    if (property.rfind("vars.", 0) == 0) return;
    if (dot == std::string::npos) {
        if (scene.componentKind(property)) mapComponentLinks(scene, property, value, fn);
        return;
    }
    if (const FieldInfo* f = fieldOf(scene, property.substr(0, dot), property.substr(dot + 1)); f && isLinkField(*f)) {
        mapLinkValue(*f, value, fn);
    }
}

uint64_t linkId(const Json& l) { return l.isObject() ? static_cast<uint64_t>(l.get("id").asInt()) : 0; }

const std::vector<std::string>& recordProps() {
    static const std::vector<std::string> k{"name", "enabled", "tags", "unique", "behaviors"};
    return k;
}

Json recordProp(const Json& entity, const std::string& prop) {
    if (prop == "unique") return entity.get("unique").asBool(false);
    if (prop == "behaviors") return entity.get("behaviors").isArray() ? entity.get("behaviors") : Json::array();
    if (prop == "tags") return entity.get("tags").isArray() ? entity.get("tags") : Json::array();
    return entity.get(prop);
}

}  // namespace

Result<std::shared_ptr<const PrefabTemplate>> buildPrefabTemplate(const Json& doc, std::string source, std::string guid) {
    if (doc.get("format").asString() != "skywalker.prefab" || !doc.get("root").isObject()) {
        return Error::make("invalid_prefab",
                           "not a skywalker prefab (needs \"format\": \"skywalker.prefab\" and a \"root\" node)" +
                               (source.empty() ? std::string() : ": " + source),
                           "create prefabs with prefab_create");
    }
    auto t = std::make_shared<PrefabTemplate>();
    t->source = std::move(source);
    t->guid = std::move(guid);
    t->doc = doc;
    t->version = hashText(doc.dump());
    t->scene = std::make_shared<Scene>();

    // Pids: explicit ones (version 2) are kept; nodes without one get the next free pid in depth-first order,
    // so version 1 files get 1..n.
    std::unordered_set<uint32_t> taken;
    uint32_t maxPid = 0;
    std::function<void(const Json&)> collect = [&](const Json& n) {
        int64_t pid = n.get("pid").asInt(0);
        if (pid > 0 && pid < (1ll << 31) && taken.insert(static_cast<uint32_t>(pid)).second) {
            maxPid = std::max(maxPid, static_cast<uint32_t>(pid));
        }
        for (const auto& c : n.get("children").elements()) collect(c);
    };
    collect(doc.get("root"));
    uint32_t next = maxPid + 1;
    std::unordered_set<uint32_t> used;
    Status failure;
    std::function<void(const Json&, uint32_t, const std::string&)> build = [&](const Json& n, uint32_t parentPid,
                                                                               const std::string& parentPath) {
        if (!failure.ok()) return;
        if (!n.isObject()) {
            failure = Error::make("invalid_prefab", "prefab nodes must be objects (under \"" + parentPath + "\")");
            return;
        }
        int64_t want = n.get("pid").asInt(0);
        uint32_t pid = 0;
        if (want > 0 && taken.count(static_cast<uint32_t>(want)) && !used.count(static_cast<uint32_t>(want))) {
            pid = static_cast<uint32_t>(want);
        } else {
            if (want > 0) t->warnings.push_back("duplicate pid " + std::to_string(want) + " renumbered");
            pid = next++;
        }
        used.insert(pid);
        std::string name = n.get("name").asString("Entity");
        std::string path = parentPath.empty() ? name : parentPath + "/" + name;
        t->scene->create(name, parentPid, pid);
        t->order.push_back(pid);
        t->parent[pid] = parentPid;
        t->path[pid] = parentPid ? path : name;
        if (!parentPid) t->root = pid;
        Json body = n;
        for (const char* k : {"children", "pid", "id", "name", "parent"}) body.erase(k);
        if (body.erase("prefab")) {
            t->warnings.push_back("node \"" + path + "\" was a nested prefab instance; it is flattened (nested prefabs "
                                  "keep their link in a later version)");
        }
        if (Status s = t->scene->applyEntityJson(pid, body); !s) {
            failure = Error::make(s.error().code, "prefab node \"" + path + "\": " + s.error().message, s.error().hint);
            return;
        }
        for (const auto& c : n.get("children").elements()) build(c, pid, parentPid ? path : "");
    };
    build(doc.get("root"), 0, "");
    if (!failure.ok()) return failure.error();
    // Names inside the prefab become pid links; ids that are not nodes of this prefab are meaningless here.
    for (uint32_t pid : t->order) {
        t->scene->bindLinks(pid);
        std::unordered_map<EntityId, EntityId> none;
        for (const LinkInfo& l : t->scene->linksFrom(pid)) {
            if (l.link.id && !t->scene->exists(l.link.id)) none[l.link.id] = 0;
        }
        if (!none.empty()) t->scene->remapLinks({pid}, none);
    }
    return std::shared_ptr<const PrefabTemplate>(std::move(t));
}

namespace prefab {

Json Override::toJson() const {
    Json j = Json::object({{"entity", entity}, {"pid", pid}, {"node", node}, {"property", property}, {"value", value},
                           {"prefab_value", prefabValue}});
    if (placement) j["placement"] = true;
    return j;
}

size_t InstanceDiff::count(bool includePlacement) const {
    size_t n = removed.size() + added.size();
    for (const auto& o : overrides) n += (includePlacement || !o.placement) ? 1 : 0;
    return n;
}

Json InstanceDiff::toJson(const Scene& scene) const {
    const EntityRecord* r = scene.record(root);
    Json ov = Json::array(), place = Json::object(), rem = Json::array(), add = Json::array(), det = Json::array();
    for (const auto& o : overrides) {
        if (o.placement) {
            place[o.property] = o.value;
        } else {
            ov.push(o.toJson());
        }
    }
    for (uint32_t pid : removed) {
        rem.push(Json::object({{"pid", pid}, {"node", tmpl && tmpl->path.count(pid) ? tmpl->path.at(pid) : std::string()}}));
    }
    for (EntityId e : added) {
        const EntityRecord* a = scene.record(e);
        add.push(Json::object({{"entity", e}, {"name", a ? a->name : ""}, {"parent", a ? a->parent : kNoEntity}}));
    }
    for (EntityId e : detached) det.push(e);
    Json j = Json::object({{"instance", root},
                           {"name", r ? r->name : ""},
                           {"source", tmpl ? tmpl->source : (r ? r->prefab.source : "")},
                           {"guid", tmpl ? tmpl->guid : (r ? r->prefab.guid : "")},
                           {"members", members.size()},
                           {"override_count", count(false)},
                           {"overrides", ov},
                           {"removed", rem},
                           {"added", add},
                           {"placement", place}});
    if (!detached.empty()) j["detached"] = det;
    if (!warnings.empty()) {
        Json w = Json::array();
        for (const auto& s : warnings) w.push(s);
        j["warnings"] = w;
    }
    return j;
}

std::vector<EntityId> instances(const Scene& scene, const std::string& source) {
    std::vector<EntityId> out;
    for (EntityId e : scene.entities()) {
        const EntityRecord* r = scene.record(e);
        if (r->prefab.linked() && r->prefab.instance == e && (source.empty() || r->prefab.source == source)) out.push_back(e);
    }
    return out;
}

EntityId instanceOf(const Scene& scene, EntityId id) {
    const EntityRecord* r = scene.record(id);
    if (!r || !r->prefab.linked() || !scene.exists(r->prefab.instance)) return kNoEntity;
    return r->prefab.instance;
}

namespace {

Result<std::shared_ptr<const PrefabTemplate>> resolveTemplate(Scene& scene, const std::string& source, const std::string& guid) {
    if (!scene.prefabResolver()) {
        return Error::make("no_prefabs", "this scene cannot load prefab files (no asset system attached)");
    }
    Error last = Error::make("not_found", "prefab not found");
    if (!guid.empty()) {
        auto t = scene.prefabResolver()("guid:" + guid);
        if (t) return t;
        last = t.error();
    }
    if (!source.empty()) {
        auto t = scene.prefabResolver()(source);
        if (t) return t;
        last = t.error();
    }
    return last;
}

}  // namespace

Result<std::shared_ptr<const PrefabTemplate>> templateOf(Scene& scene, EntityId root) {
    const EntityRecord* r = scene.record(root);
    if (!r || !r->prefab.linked()) {
        return Error::make("not_linked", "entity " + formatEntityRef(root) + " is not part of a prefab instance",
                           "prefab_overrides with no entity lists the instances in the scene");
    }
    if (r->prefab.instance != root) return templateOf(scene, r->prefab.instance);
    if (!r->prefab.pending.isNull()) {
        return Error::make("prefab_missing", "the prefab " + r->prefab.source + " of " + formatEntityRef(root) +
                                                 " could not be loaded; the instance is a placeholder",
                           "restore the file, or delete the instance; asset_list type=prefab lists prefabs");
    }
    if (auto t = scene.prefabTemplate(r->prefab.source)) return t;
    std::string source = r->prefab.source, guid = r->prefab.guid;
    auto t = resolveTemplate(scene, source, guid);
    if (!t) return t.error();
    scene.setPrefabTemplate(source, *t);
    return t;
}

Result<InstanceDiff> diff(Scene& scene, EntityId root, std::shared_ptr<const PrefabTemplate> tmpl) {
    const EntityRecord* rr = scene.record(root);
    if (!rr || !rr->prefab.linked() || rr->prefab.instance != root) {
        EntityId inst = instanceOf(scene, root);
        return Error::make("not_instance", "entity " + formatEntityRef(root) + " is not a prefab instance root",
                           inst ? "its instance root is " + formatEntityRef(inst)
                                : "prefab_overrides with no entity lists the instances in the scene");
    }
    if (!tmpl) {
        auto t = templateOf(scene, root);
        if (!t) return t.error();
        tmpl = *t;
    }
    InstanceDiff d;
    d.root = root;
    d.tmpl = tmpl;
    const PrefabTemplate& T = *tmpl;

    // Members claiming this instance, by pid.
    std::unordered_map<uint32_t, EntityId> live;
    for (EntityId e : scene.entities()) {
        const EntityRecord* r = scene.record(e);
        if (e == root || r->prefab.instance != root) continue;
        if (!T.has(r->prefab.pid) || r->prefab.pid == T.root || live.count(r->prefab.pid)) {
            d.detached.push_back(e);
            d.warnings.push_back(formatEntityRef(e) + " claims prefab node " + std::to_string(r->prefab.pid) +
                                 " which the prefab does not have (in this place); it is saved as a plain entity");
            continue;
        }
        live[r->prefab.pid] = e;
    }
    // Intact members: in place under their prefab parent. Missing nodes are removed (top-most only).
    d.members[T.root] = root;
    for (uint32_t pid : T.order) {
        if (pid == T.root) continue;
        uint32_t pp = T.parent.at(pid);
        auto it = live.find(pid);
        auto parentIt = d.members.find(pp);
        if (parentIt == d.members.end()) {
            if (it != live.end()) d.detached.push_back(it->second);
            continue;
        }
        if (it == live.end()) {
            d.removed.push_back(pid);
            continue;
        }
        if (scene.record(it->second)->parent != parentIt->second) {
            d.detached.push_back(it->second);
            d.removed.push_back(pid);
            continue;
        }
        d.members[pid] = it->second;
    }
    std::unordered_map<EntityId, uint32_t> pidOf;
    for (const auto& [pid, e] : d.members) pidOf[e] = pid;
    for (EntityId e : scene.entities()) {
        const EntityRecord* r = scene.record(e);
        if (!pidOf.count(e) && pidOf.count(r->parent)) d.added.push_back(e);
    }

    // Field-level comparison. Links compare by meaning: a link to a member equals the prefab's link to that node.
    auto canonLive = [&](const Json& l) -> Json {
        if (l.isNull()) return l;
        uint64_t id = linkId(l);
        if (auto it = pidOf.find(id); id && it != pidOf.end()) return Json::object({{"pid", it->second}});
        return Json::object({{"name", l.get("name")}});
    };
    auto canonTmpl = [&](const Json& l) -> Json {
        if (l.isNull()) return l;
        uint64_t id = linkId(l);
        if (id && T.scene->exists(id)) return Json::object({{"pid", id}});
        return Json::object({{"name", l.get("name")}});
    };
    for (uint32_t pid : T.order) {
        auto mit = d.members.find(pid);
        if (mit == d.members.end()) continue;
        EntityId L = mit->second;
        const bool isRoot = pid == T.root;
        Json A = scene.entityToJson(L);
        Json E = T.scene->entityToJson(pid);
        const std::string node = T.path.count(pid) ? T.path.at(pid) : "";
        auto add = [&](std::string prop, Json value, Json prefabValue, bool placement) {
            d.overrides.push_back({pid, L, node, std::move(prop), std::move(value), std::move(prefabValue), placement});
        };
        for (const auto& prop : recordProps()) {
            Json a = recordProp(A, prop), e = recordProp(E, prop);
            if (a != e) add(prop, a, e, isRoot && prop == "name");
        }
        std::vector<std::string> keys;
        for (const auto& [k, v] : A.get("vars").members()) keys.push_back(k);
        for (const auto& [k, v] : E.get("vars").members()) {
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        }
        for (const auto& k : keys) {
            const Json& a = A.get("vars").get(k);
            const Json& e = E.get("vars").get(k);
            if (a != e) add("vars." + k, a, e, false);
        }
        for (const auto& kind : scene.componentKinds()) {
            const Json& a = A.get("components").get(kind.name);
            const Json& e = E.get("components").get(kind.name);
            if (a.isNull() && e.isNull()) continue;
            if (a.isNull() || e.isNull()) {
                add(kind.name, a, e, false);
                continue;
            }
            if (!kind.info) continue;
            for (const FieldInfo& f : kind.info->fields) {
                const Json& av = a.get(f.name);
                const Json& ev = e.get(f.name);
                bool same = av == ev;
                if (!same && isLinkField(f)) {
                    Json ca = av, ce = ev;
                    mapLinkValue(f, ca, canonLive);
                    mapLinkValue(f, ce, canonTmpl);
                    same = ca == ce;
                }
                if (!same) add(kind.name + "." + f.name, av, ev, isRoot && kind.name == "transform");
            }
        }
    }
    return d;
}

Status applyOverride(Scene& scene, EntityId e, const std::string& property, const Json& value) {
    if (property == "name") {
        if (!value.isString()) return Error::make("invalid_value", "name must be a string");
        return scene.rename(e, value.asString());
    }
    if (property == "enabled") return scene.setEnabled(e, value.asBool(true));
    if (property == "unique") return scene.setUnique(e, value.asBool(false));
    if (property == "tags") {
        std::vector<std::string> tags;
        for (const auto& t : value.elements()) tags.push_back(t.asString());
        return scene.setTags(e, std::move(tags));
    }
    if (property == "behaviors") return scene.setBehaviors(e, value.isArray() ? value : Json::array());
    if (property.rfind("vars.", 0) == 0) return scene.patchVars(e, Json::object({{property.substr(5), value}}));
    auto dot = property.find('.');
    if (dot == std::string::npos) {
        if (!scene.componentKind(property)) {
            return Error::make("unknown_property", "unknown override property \"" + property + "\"");
        }
        if (value.isNull()) {
            if (const ComponentKind* k = scene.componentKind(property); k && !k->has(scene, e)) return {};
        }
        return scene.patchComponent(e, property, value);
    }
    return scene.patchComponent(e, property.substr(0, dot), Json::object({{property.substr(dot + 1), value}}));
}

namespace {

size_t indexOf(const Scene& scene, EntityId id) {
    const auto& order = scene.entities();
    return static_cast<size_t>(std::find(order.begin(), order.end(), id) - order.begin());
}

/// Instances of `source` expanded from `from` are re-expanded from `to` (keeping their overrides).
size_t upgrade(Scene& scene, const std::string& source, const std::shared_ptr<const PrefabTemplate>& from,
               const std::shared_ptr<const PrefabTemplate>& to, EntityId skip = kNoEntity) {
    size_t n = 0;
    for (EntityId root : instances(scene, source)) {
        if (root == skip) continue;
        auto d = diff(scene, root, from);
        if (!d) continue;
        for (EntityId e : d->detached) (void)scene.setPrefabMembership(e, {});
        if (rebuild(scene, root, to, d->overrides, d->removed)) ++n;
    }
    return n;
}

}  // namespace

Status rebuild(Scene& scene, EntityId root, const std::shared_ptr<const PrefabTemplate>& tmpl,
               const std::vector<Override>& keep, const std::vector<uint32_t>& removed) {
    if (!tmpl || !scene.exists(root)) return Error::make("invalid_argument", "rebuild needs a template and a root");
    const PrefabTemplate& T = *tmpl;
    std::unordered_set<uint32_t> removedSet(removed.begin(), removed.end());
    std::unordered_map<uint32_t, EntityId> live;
    for (EntityId e : scene.entities()) {
        const EntityRecord* r = scene.record(e);
        if (e != root && r->prefab.instance == root && !live.count(r->prefab.pid)) live[r->prefab.pid] = e;
    }
    std::unordered_map<uint32_t, EntityId> placed;
    size_t pos = indexOf(scene, root);
    PrefabMembership rootM;
    rootM.instance = root;
    rootM.pid = T.root;
    rootM.source = T.source;
    rootM.guid = T.guid;
    for (uint32_t pid : T.order) {
        EntityId L = kNoEntity;
        if (pid == T.root) {
            L = root;
        } else {
            uint32_t pp = T.parent.at(pid);
            auto parentIt = placed.find(pp);
            if (removedSet.count(pid) || parentIt == placed.end()) continue;
            auto it = live.find(pid);
            if (it != live.end()) {
                L = it->second;
                if (scene.record(L)->parent != parentIt->second) (void)scene.setParent(L, parentIt->second);
                pos = std::max(pos, indexOf(scene, L));
            } else {
                L = scene.create(T.nodeName(pid), parentIt->second, kNoEntity, pos + 1);
                ++pos;
            }
        }
        scene.copyEntityData(L, *T.scene, pid);
        PrefabMembership m;
        if (pid == T.root) {
            m = rootM;
        } else {
            m.instance = root;
            m.pid = pid;
        }
        const PrefabMembership& cur = scene.record(L)->prefab;
        if (cur.instance != m.instance || cur.pid != m.pid || cur.source != m.source || cur.guid != m.guid ||
            !cur.pending.isNull()) {
            (void)scene.setPrefabMembership(L, m);
        }
        placed[pid] = L;
    }
    // Members of nodes the prefab no longer has (or that are now removed): keep added entities, drop the rest.
    std::vector<EntityId> stale;
    for (const auto& [pid, e] : live) {
        auto it = placed.find(pid);
        if (it == placed.end() || it->second != e) stale.push_back(e);
    }
    for (EntityId e : stale) {
        if (!scene.exists(e)) continue;
        for (EntityId c : scene.children(e)) {
            if (scene.record(c)->prefab.instance != root) (void)scene.setParent(c, root);
        }
        scene.destroy(e);
    }
    // Template links use pids: point them at this instance's entities.
    std::unordered_map<EntityId, EntityId> map;
    std::vector<EntityId> members;
    for (const auto& [pid, e] : placed) {
        map[pid] = e;
        members.push_back(e);
    }
    scene.remapLinks(members, map);
    for (const auto& o : keep) {
        auto it = placed.find(o.pid);
        if (it == placed.end()) continue;  // the node is gone or removed
        (void)applyOverride(scene, it->second, o.property, o.value);
    }
    for (EntityId e : members) scene.bindLinks(e);
    if (!T.source.empty()) scene.setPrefabTemplate(T.source, tmpl);
    return {};
}

Result<EntityId> instantiate(Scene& scene, const std::shared_ptr<const PrefabTemplate>& tmpl, EntityId parent,
                             const std::string& name) {
    if (!tmpl) return Error::make("invalid_argument", "no prefab template");
    // Other instances expanded from an older version are brought up to date first, so one version is current.
    if (!tmpl->source.empty()) {
        auto cur = scene.prefabTemplate(tmpl->source);
        if (cur && cur->version != tmpl->version) upgrade(scene, tmpl->source, cur, tmpl);
    }
    EntityId root = scene.create(name.empty() ? tmpl->nodeName(tmpl->root) : name, parent);
    PrefabMembership m;
    m.instance = root;
    m.pid = tmpl->root;
    m.source = tmpl->source;
    m.guid = tmpl->guid;
    (void)scene.setPrefabMembership(root, m);
    if (Status s = rebuild(scene, root, tmpl, {}, {}); !s) return s.error();
    if (!name.empty()) (void)scene.rename(root, name);
    return root;
}

Result<size_t> sync(Scene& scene, const std::string& source) {
    std::vector<std::string> sources = source.empty() ? scene.prefabSources() : std::vector<std::string>{source};
    size_t n = 0;
    for (const auto& src : sources) {
        auto roots = instances(scene, src);
        if (roots.empty()) continue;
        auto latest = resolveTemplate(scene, src, scene.record(roots.front())->prefab.guid);
        if (!latest) continue;  // missing for now: instances stay as they are
        auto cur = scene.prefabTemplate(src);
        if (!cur) {
            scene.setPrefabTemplate(src, *latest);
            continue;
        }
        if (cur->version == (*latest)->version) continue;
        n += upgrade(scene, src, cur, *latest);
        scene.setPrefabTemplate(src, *latest);
    }
    return n;
}

Result<size_t> revert(Scene& scene, EntityId entity, const std::string& property, bool wholeInstance) {
    EntityId root = instanceOf(scene, entity);
    if (!root) {
        return Error::make("not_linked", "entity " + formatEntityRef(entity) + " is not part of a prefab instance",
                           "prefab_overrides with no entity lists the instances in the scene");
    }
    auto d = diff(scene, root);
    if (!d) return d.error();
    std::vector<Override> keep;
    std::vector<uint32_t> removed = d->removed;
    size_t reverted = 0;
    if (wholeInstance) {
        for (const auto& o : d->overrides) {
            if (o.placement) {
                keep.push_back(o);
            } else {
                ++reverted;
            }
        }
        reverted += removed.size();
        removed.clear();  // detached members still claiming the instance are brought back too
    } else {
        std::vector<std::string> props;
        for (const auto& o : d->overrides) {
            bool match = o.entity == entity && (property.empty() ? !o.placement : o.property == property);
            if (o.entity == entity) props.push_back(o.property);
            if (match) {
                ++reverted;
            } else {
                keep.push_back(o);
            }
        }
        if (!property.empty() && !reverted) {
            std::string guess = str::closest(property, props, 4);
            std::string list;
            for (const auto& p : props) list += (list.empty() ? "" : ", ") + p;
            return Error::make("no_override", formatEntityRef(entity) + " has no override \"" + property + "\"",
                               !guess.empty() ? "did you mean \"" + guess + "\"?"
                                              : (list.empty() ? "it matches the prefab" : "its overrides: " + list));
        }
        for (EntityId e : d->detached) (void)scene.setPrefabMembership(e, {});
    }
    if (Status s = rebuild(scene, root, d->tmpl, keep, removed); !s) return s.error();
    return reverted;
}

namespace {

/// Writes a node tree; `pidOf` maps entities of the subtree to their pids.
Json writeNode(const Scene& scene, EntityId e, const std::unordered_map<EntityId, uint32_t>& pidOf) {
    Json doc = scene.entityToJson(e);
    Json node = Json::object({{"pid", pidOf.at(e)}});
    for (auto& [k, v] : doc.members()) {
        if (k == "id" || k == "parent" || k == "prefab") continue;
        node[k] = v;
    }
    auto toPid = [&](const Json& l) -> Json {
        if (l.isNull()) return l;
        uint64_t id = linkId(l);
        auto it = id ? pidOf.find(id) : pidOf.end();
        const std::string& n = l.get("name").asString();
        if (it != pidOf.end()) return Json::object({{"pid", it->second}, {"name", n}});
        return n.empty() ? Json() : Json::object({{"name", n}});
    };
    if (Json* comps = node.find("components")) {
        for (auto& [name, comp] : comps->members()) mapComponentLinks(scene, name, comp, toPid);
    }
    Json children = Json::array();
    for (EntityId c : scene.children(e)) children.push(writeNode(scene, c, pidOf));
    if (children.size()) node["children"] = children;
    return node;
}

Json writeDocument(const Scene& scene, EntityId root, const std::unordered_map<EntityId, uint32_t>& pidOf,
                   bool zeroRootPosition) {
    Json node = writeNode(scene, root, pidOf);
    if (zeroRootPosition) {
        if (Json* t = node["components"].find("transform")) (*t)["position"] = Json::array({0, 0, 0});
    }
    return Json::object({{"format", "skywalker.prefab"}, {"version", 2}, {"name", node.get("name")}, {"root", node}});
}

}  // namespace

Json documentFromScene(const Scene& scene, EntityId root, bool zeroRootPosition) {
    std::unordered_map<EntityId, uint32_t> pidOf;
    std::function<void(EntityId)> gather = [&](EntityId e) {
        pidOf[e] = static_cast<uint32_t>(pidOf.size() + 1);
        for (EntityId c : scene.children(e)) gather(c);
    };
    gather(root);
    return writeDocument(scene, root, pidOf, zeroRootPosition);
}

Result<ApplyResult> apply(Scene& scene, EntityId entity, const std::string& property, const PrefabWriter& write) {
    EntityId root = instanceOf(scene, entity);
    if (!root) {
        return Error::make("not_linked", "entity " + formatEntityRef(entity) + " is not part of a prefab instance",
                           "prefab_overrides with no entity lists the instances in the scene");
    }
    auto d = diff(scene, root);
    if (!d) return d.error();
    const PrefabTemplate& T = *d->tmpl;
    if (T.source.empty()) return Error::make("invalid_state", "this instance has no prefab file to apply to");

    // A template-space copy of the prefab (entity id == pid) receives the changes.
    Scene work;
    for (uint32_t pid : T.order) {
        work.create(T.nodeName(pid), T.parent.at(pid), pid);
        work.copyEntityData(pid, *T.scene, pid);
    }
    std::unordered_map<EntityId, uint32_t> toPid;  // live entity -> pid
    for (const auto& [pid, e] : d->members) toPid[e] = pid;
    auto toPidLink = [&](const Json& l) -> Json {
        if (l.isNull()) return l;
        uint64_t id = linkId(l);
        const std::string& n = l.get("name").asString();
        if (auto it = id ? toPid.find(id) : toPid.end(); it != toPid.end()) return Json::object({{"pid", it->second}, {"name", n}});
        return n.empty() ? Json() : Json::object({{"name", n}});
    };

    ApplyResult result;
    std::vector<Override> applying, keep;
    std::vector<uint32_t> keepRemoved;
    std::unordered_map<EntityId, uint32_t> adopted;  // added live entity -> its new pid
    if (property.empty()) {
        for (const auto& o : d->overrides) (o.placement ? keep : applying).push_back(o);
        for (uint32_t pid : d->removed) {
            if (work.exists(pid)) work.destroy(pid);
            ++result.applied;
        }
        // Added entities (with their subtrees) become prefab nodes with new pids.
        uint32_t nextPid = 1;
        for (uint32_t pid : T.order) nextPid = std::max(nextPid, pid + 1);
        std::vector<EntityId> adoptOrder;
        std::function<void(EntityId)> number = [&](EntityId e) {
            toPid[e] = nextPid;
            adopted[e] = nextPid++;
            adoptOrder.push_back(e);
            for (EntityId c : scene.children(e)) number(c);
        };
        for (EntityId a : d->added) {
            number(a);
            ++result.applied;
        }
        for (EntityId e : adoptOrder) {
            Json body = scene.entityToJson(e);
            for (const char* k : {"id", "parent", "prefab", "name"}) body.erase(k);
            if (Json* comps = body.find("components")) {
                for (auto& [name, comp] : comps->members()) mapComponentLinks(scene, name, comp, toPidLink);
            }
            EntityId w = work.create(scene.record(e)->name, toPid.at(scene.record(e)->parent), adopted.at(e));
            if (Status s = work.applyEntityJson(w, body); !s) result.warnings.push_back(s.error().message);
        }
    } else {
        std::vector<std::string> props;
        bool placement = false;
        for (const auto& o : d->overrides) {
            if (o.entity == entity) props.push_back(o.property);
            if (o.entity == entity && o.property == property) {
                placement = placement || o.placement;
                if (!o.placement) {
                    applying.push_back(o);
                    continue;
                }
            }
            keep.push_back(o);
        }
        if (applying.empty()) {
            if (placement) {
                return Error::make("placement", "\"" + property + "\" is the instance's placement (root name/transform)",
                                   "placement is never applied to the prefab; edit the prefab's root through another "
                                   "instance's child or the prefab file");
            }
            std::string guess = str::closest(property, props, 4);
            std::string list;
            for (const auto& p : props) list += (list.empty() ? "" : ", ") + p;
            return Error::make("no_override", formatEntityRef(entity) + " has no override \"" + property + "\"",
                               !guess.empty() ? "did you mean \"" + guess + "\"?"
                                              : (list.empty() ? "it matches the prefab" : "its overrides: " + list));
        }
        keepRemoved = d->removed;
    }
    for (const auto& o : applying) {
        if (!work.exists(o.pid)) continue;
        Json v = o.value;
        mapOverrideLinks(scene, o.property, v, toPidLink);
        if (Status s = applyOverride(work, o.pid, o.property, v); !s) {
            result.warnings.push_back(o.node + " " + o.property + ": " + s.error().message);
            continue;
        }
        ++result.applied;
    }
    std::unordered_map<EntityId, uint32_t> pidOf;
    for (EntityId e : work.entities()) pidOf[e] = static_cast<uint32_t>(e);
    Json written = writeDocument(work, T.root, pidOf, false);
    Json full = T.doc;
    full["version"] = 2;
    full["root"] = written.get("root");
    if (!full.contains("name")) full["name"] = written.get("name");
    auto newT = write(full);
    if (!newT) return newT.error();
    result.document = std::move(full);

    // The adopted entities join this instance; then it and every other instance re-expand from the new version.
    for (const auto& [e, pid] : adopted) {
        PrefabMembership m;
        m.instance = root;
        m.pid = pid;
        (void)scene.setPrefabMembership(e, m);
    }
    if (property.empty()) {
        for (EntityId e : d->detached) (void)scene.setPrefabMembership(e, {});
    }
    if (Status s = rebuild(scene, root, *newT, keep, keepRemoved); !s) return s.error();
    result.instancesUpdated = upgrade(scene, T.source, d->tmpl, *newT, root);
    scene.setPrefabTemplate(T.source, *newT);
    return result;
}

size_t unpack(Scene& scene, EntityId root) {
    size_t n = 0;
    for (EntityId e : std::vector<EntityId>(scene.entities())) {
        const EntityRecord* r = scene.record(e);
        if ((r->prefab.linked() && r->prefab.instance == root) || (e == root && !r->prefab.pending.isNull())) {
            (void)scene.setPrefabMembership(e, {});
            ++n;
        }
    }
    return n;
}

bool match(const Scene& scene, EntityId candidate, const PrefabTemplate& T, std::unordered_map<uint32_t, EntityId>& out) {
    out.clear();
    std::function<bool(EntityId, uint32_t)> walk = [&](EntityId e, uint32_t pid) {
        if (scene.record(e)->prefab.linked()) return false;
        out[pid] = e;
        auto lk = scene.children(e);
        auto tk = T.scene->children(pid);
        if (lk.size() != tk.size()) return false;
        for (size_t i = 0; i < lk.size(); ++i) {
            if (scene.record(lk[i])->name != T.scene->record(tk[i])->name) return false;
            if (!walk(lk[i], static_cast<uint32_t>(tk[i]))) return false;
        }
        return true;
    };
    bool ok = scene.exists(candidate) && walk(candidate, T.root);
    if (!ok) out.clear();
    return ok;
}

Status link(Scene& scene, EntityId root, const std::shared_ptr<const PrefabTemplate>& tmpl,
            const std::unordered_map<uint32_t, EntityId>& members) {
    if (!tmpl) return Error::make("invalid_argument", "no prefab template");
    if (auto cur = scene.prefabTemplate(tmpl->source); cur && cur->version != tmpl->version) {
        upgrade(scene, tmpl->source, cur, tmpl, root);
    }
    for (const auto& [pid, e] : members) {
        PrefabMembership m;
        m.instance = root;
        m.pid = pid;
        if (e == root) {
            m.source = tmpl->source;
            m.guid = tmpl->guid;
        }
        if (Status s = scene.setPrefabMembership(e, m); !s) return s;
    }
    if (!tmpl->source.empty()) scene.setPrefabTemplate(tmpl->source, tmpl);
    return {};
}

void expandSaved(Scene& scene, EntityId root, const Json& block, std::vector<std::string>& warnings) {
    const std::string source = block.get("source").asString();
    const std::string guid = block.get("guid").asString();
    const std::string who = formatEntityRef(root) + " \"" + scene.record(root)->name + "\"";
    auto t = resolveTemplate(scene, source, guid);
    if (t) {
        if (auto cur = scene.prefabTemplate((*t)->source); !cur || cur->version != (*t)->version) {
            scene.setPrefabTemplate((*t)->source, *t);
        }
    }
    if (!t) {
        warnings.push_back(who + ": prefab " + source + " could not be loaded (" + t.error().message +
                           "); it is kept as a placeholder and its overrides are saved unchanged");
        PrefabMembership m;
        m.instance = root;
        m.source = source;
        m.guid = guid;
        m.pending = block;
        (void)scene.setPrefabMembership(root, m);
        return;
    }
    const PrefabTemplate& T = **t;
    std::vector<uint32_t> removed;
    for (const auto& r : block.get("removed").elements()) removed.push_back(static_cast<uint32_t>(r.asInt()));
    std::unordered_set<uint32_t> skipped(removed.begin(), removed.end());
    // Members get the ids they were saved with (so links, Wander handles and traces stay stable).
    PrefabMembership rm;
    rm.instance = root;
    rm.pid = T.root;
    rm.source = T.source;
    rm.guid = T.guid;
    (void)scene.setPrefabMembership(root, rm);
    const Json& ids = block.get("ids");
    size_t pos = indexOf(scene, root);
    for (uint32_t pid : T.order) {
        if (pid == T.root) continue;
        if (skipped.count(pid) || skipped.count(T.parent.at(pid))) {
            skipped.insert(pid);
            continue;
        }
        auto id = static_cast<EntityId>(ids.get(std::to_string(pid)).asInt());
        if (!id || scene.exists(id)) continue;
        scene.create(T.nodeName(pid), root, id, ++pos);
        PrefabMembership m;
        m.instance = root;
        m.pid = pid;
        (void)scene.setPrefabMembership(id, m);
    }
    std::vector<Override> overrides;
    for (const auto& o : block.get("overrides").elements()) {
        Override ov;
        ov.pid = static_cast<uint32_t>(o.get("pid").asInt());
        ov.property = o.get("property").asString();
        ov.value = o.get("value");
        if (!T.has(ov.pid)) {
            warnings.push_back(who + ": override " + ov.property + " on prefab node " + std::to_string(ov.pid) + " (" +
                               o.get("node").asString() + ") was dropped: " + T.source + " no longer has that node");
            continue;
        }
        overrides.push_back(std::move(ov));
    }
    for (const auto& [pidText, id] : ids.members()) {
        if (!T.has(static_cast<uint32_t>(std::atoi(pidText.c_str())))) {
            warnings.push_back(who + ": prefab node " + pidText + " no longer exists in " + T.source + "; entity " +
                               formatEntityRef(static_cast<EntityId>(id.asInt())) + " is gone");
        }
    }
    if (Status s = rebuild(scene, root, *t, overrides, removed); !s) warnings.push_back(who + ": " + s.error().message);
}

Json savedRecord(Scene& scene, EntityId root, std::vector<EntityId>& implicit) {
    const EntityRecord* r = scene.record(root);
    Json transform = scene.entityToJson(root).get("components").get("transform");
    auto record = [&](Json block) {
        return Json::object({{"id", root},
                             {"name", r->name},
                             {"parent", r->parent},
                             {"components", Json::object({{"transform", transform}})},
                             {"prefab", std::move(block)}});
    };
    if (!r->prefab.pending.isNull()) return record(r->prefab.pending);
    auto d = diff(scene, root);
    if (!d) return scene.entityToJson(root);  // cannot diff (no asset system): saved expanded, still linked
    const PrefabTemplate& T = *d->tmpl;
    Json ids = Json::object();
    for (uint32_t pid : T.order) {
        auto it = d->members.find(pid);
        if (pid == T.root || it == d->members.end()) continue;
        ids[std::to_string(pid)] = it->second;
        implicit.push_back(it->second);
    }
    Json block = Json::object({{"source", T.source.empty() ? r->prefab.source : T.source}, {"guid", T.guid}, {"ids", ids}});
    Json ov = Json::array();
    for (const auto& o : d->overrides) {
        if (o.placement) continue;
        ov.push(Json::object({{"pid", o.pid}, {"node", o.node}, {"property", o.property}, {"value", o.value}}));
    }
    if (ov.size()) block["overrides"] = ov;
    if (!d->removed.empty()) {
        Json rem = Json::array();
        for (uint32_t pid : d->removed) rem.push(pid);
        block["removed"] = rem;
    }
    return record(std::move(block));
}

}  // namespace prefab
}  // namespace sky
