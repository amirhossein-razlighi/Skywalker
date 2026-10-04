// Scene: entity links (FieldType::Entity), scoped names ("%Name"), copying subtrees, and the file form of
// scenes with linked prefab instances. Kept apart from Scene.cpp so the core stays small.

#include <algorithm>
#include <unordered_set>

#include "skywalker/core/Strings.h"
#include "skywalker/scene/PrefabLink.h"
#include "skywalker/scene/Scene.h"

namespace sky {

namespace {

bool isLinkField(const FieldInfo& f) { return f.type == FieldType::Entity || f.type == FieldType::EntityList; }

bool sameName(const std::string& a, std::string_view b, bool exact) {
    if (exact) return a == b;
    return a.size() == b.size() && str::lower(a) == str::lower(b);
}

/// Calls fn(kind, field, link, index) for every link stored on an entity.
template <typename Fn>
void eachLink(const std::vector<ComponentKind>& kinds, Scene& scene, EntityId id, Fn&& fn) {
    for (const auto& k : kinds) {
        if (!k.info || !k.ptr) continue;
        void* raw = nullptr;
        for (const FieldInfo& f : k.info->fields) {
            if (!isLinkField(f)) continue;
            if (!raw) raw = k.ptr(scene, id);
            if (!raw) break;
            char* p = static_cast<char*>(raw) + f.offset;
            if (f.type == FieldType::Entity) {
                fn(k, f, *reinterpret_cast<EntityLink*>(p), -1);
            } else {
                auto& list = *reinterpret_cast<std::vector<EntityLink>*>(p);
                for (size_t i = 0; i < list.size(); ++i) fn(k, f, list[i], static_cast<int>(i));
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------------------------------------------

EntityId Scene::ownerOf(EntityId id) const {
    const EntityRecord* r = record(id);
    if (!r || !r->prefab.linked() || r->prefab.instance == id) return kNoEntity;
    return exists(r->prefab.instance) ? r->prefab.instance : kNoEntity;
}

EntityId Scene::findUnique(std::string_view wanted, EntityId from) const {
    if (!wanted.empty() && wanted[0] == '%') wanted.remove_prefix(1);
    auto search = [&](EntityId owner) -> EntityId {
        for (bool exact : {true, false}) {
            for (EntityId e : order_) {
                const EntityRecord& r = records_.at(e);
                if (r.unique && ownerOf(e) == owner && sameName(r.name, wanted, exact)) return e;
            }
        }
        return kNoEntity;
    };
    // An instance root sees the unique names inside its own instance first, then its owner's.
    if (const EntityRecord* r = record(from); r && r->prefab.linked() && r->prefab.instance == from) {
        if (EntityId e = search(from)) return e;
    }
    return search(ownerOf(from));
}

EntityId Scene::findNear(std::string_view wanted, EntityId from) const {
    if (wanted.empty()) return kNoEntity;
    if (wanted[0] == '%') return findUnique(wanted, from);
    if (wanted[0] == '#') return find(wanted);
    if (exists(from)) {
        auto isUnder = [&](EntityId e, EntityId root) {
            for (const EntityRecord* r = record(e); r; r = r->parent ? record(r->parent) : nullptr) {
                if (r->id == root) return true;
            }
            return false;
        };
        auto searchWhere = [&](auto&& pred) -> EntityId {
            for (bool exact : {true, false}) {
                for (EntityId e : order_) {
                    if (e != from && pred(e) && sameName(records_.at(e).name, wanted, exact)) return e;
                }
            }
            return kNoEntity;
        };
        // 1. Inside the same prefab instance (so each copy of a prefab links to its own parts).
        EntityId inst = records_.at(from).prefab.linked() ? records_.at(from).prefab.instance : kNoEntity;
        if (inst && exists(inst)) {
            if (EntityId e = searchWhere([&](EntityId e) { return records_.at(e).prefab.instance == inst; })) return e;
        }
        // 2. Its own subtree, then 3. its parent's subtree (siblings and cousins).
        if (EntityId e = searchWhere([&](EntityId e) { return isUnder(e, from); })) return e;
        if (EntityId p = records_.at(from).parent) {
            if (EntityId e = searchWhere([&](EntityId e) { return isUnder(e, p); })) return e;
        }
    }
    return find(wanted);
}

EntityId Scene::resolve(const EntityLink& link, EntityId from) const {
    if (link.id && exists(link.id)) return link.id;
    if (!link.name.empty()) return findNear(link.name, from);
    return kNoEntity;
}

void Scene::bindLinks(EntityId id) {
    if (!exists(id)) return;
    bool changed = false;
    eachLink(kinds_, *this, id, [&](const ComponentKind&, const FieldInfo&, EntityLink& l, int) {
        if (l.name.empty() || (l.id && exists(l.id))) return;
        if (l.name[0] == '%') return;  // scoped by design: resolved on use
        EntityId target = kNoEntity;
        // Scoped first (own instance, subtree, siblings); a scene-wide match only when the name is unique.
        EntityId near = findNear(l.name, id);
        if (near) {
            size_t matches = 0;
            for (EntityId e : order_) matches += records_.at(e).name == l.name ? 1 : 0;
            const EntityRecord& nr = records_.at(near);
            bool scoped = (records_.at(id).prefab.linked() && nr.prefab.instance == records_.at(id).prefab.instance) ||
                          nr.parent == records_.at(id).parent || nr.parent == id;
            if (matches <= 1 || scoped) target = near;
        }
        if (target && target != l.id) {
            if (!changed) notify(id);
            changed = true;
            l.id = target;
            l.name = records_.at(target).name;
        }
    });
}

std::vector<LinkInfo> Scene::linksFrom(EntityId id) const {
    std::vector<LinkInfo> out;
    if (!exists(id)) return out;
    eachLink(kinds_, const_cast<Scene&>(*this), id, [&](const ComponentKind& k, const FieldInfo& f, EntityLink& l, int i) {
        if (l.empty()) return;
        LinkInfo info;
        info.from = id;
        info.component = k.name;
        info.field = f.name;
        info.index = i;
        info.link = l;
        info.target = resolve(l, id);
        info.byName = info.target && !(l.id && exists(l.id));
        if (l.id && exists(l.id)) info.link.name = records_.at(l.id).name;
        out.push_back(std::move(info));
    });
    return out;
}

std::vector<LinkInfo> Scene::linksTo(EntityId target) const {
    std::vector<LinkInfo> out;
    for (EntityId e : order_) {
        for (auto& l : linksFrom(e)) {
            if (l.target == target) out.push_back(std::move(l));
        }
    }
    return out;
}

void Scene::remapLinks(const std::vector<EntityId>& ids, const std::unordered_map<EntityId, EntityId>& map) {
    for (EntityId id : ids) {
        if (!exists(id)) continue;
        bool changed = false;
        eachLink(kinds_, *this, id, [&](const ComponentKind&, const FieldInfo&, EntityLink& l, int) {
            auto it = l.id ? map.find(l.id) : map.end();
            if (it == map.end() || it->second == l.id) return;
            if (!changed) notify(id);
            changed = true;
            l.id = it->second;
            if (const EntityRecord* t = record(l.id)) l.name = t->name;
        });
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------------------------------------------

void Scene::copyEntityData(EntityId dst, const Scene& src, EntityId srcId) {
    const EntityRecord* from = src.record(srcId);
    if (!from || !exists(dst)) return;
    notify(dst);
    // Copy the record fields first (src may be this scene: references can move on rehash).
    std::string newName = from->name;
    bool enabled = from->enabled, unique = from->unique;
    std::vector<std::string> tags = from->tags;
    Json vars = from->vars;
    std::string process = from->process;
    EntityRecord& rec = records_.at(dst);
    rec.name = std::move(newName);
    rec.enabled = enabled;
    rec.unique = unique;
    rec.tags = std::move(tags);
    rec.vars = std::move(vars);
    rec.process = std::move(process);
    for (const auto& k : kinds_) {
        if (k.copy) k.copy(*this, dst, src, srcId);
    }
    const Behavior* b = src.registry().get<Behavior>(src.handle(srcId));
    ecs::Entity h = handle(dst);
    if (b && !b->scripts.empty()) {
        Behavior copy = *b;  // compiled programs are shared, not recompiled
        registry_.emplace<Behavior>(h) = std::move(copy);
        ++behaviorsRevision_;
    } else if (registry_.get<Behavior>(h)) {
        registry_.remove<Behavior>(h);
        ++behaviorsRevision_;
    }
}

std::vector<EntityId> Scene::cloneTrees(const Scene& src, const std::vector<EntityId>& roots, EntityId parent,
                                        std::unordered_map<EntityId, EntityId>* idMap) {
    // Gather sources first (depth first): copies may land inside the source scene.
    std::vector<std::pair<EntityId, bool>> list;  // (source id, is a root)
    std::unordered_set<EntityId> seen;
    std::function<void(EntityId, bool)> gather = [&](EntityId e, bool root) {
        if (!src.exists(e) || !seen.insert(e).second) return;
        list.emplace_back(e, root);
        for (EntityId c : src.children(e)) gather(c, false);
    };
    for (EntityId r : roots) gather(r, true);

    std::unordered_map<EntityId, EntityId> map;
    std::vector<EntityId> created, newRoots;
    for (const auto& [s, isRoot] : list) {
        EntityId p = parent;
        if (!isRoot) {
            auto it = map.find(src.record(s)->parent);
            p = it == map.end() ? parent : it->second;
        }
        EntityId c = create(src.record(s)->name, p);
        copyEntityData(c, src, s);
        map[s] = c;
        created.push_back(c);
        if (isRoot) newRoots.push_back(c);
    }
    // Links and prefab instances inside the copied set point inside the copy.
    remapLinks(created, map);
    for (const auto& [s, isRoot] : list) {
        PrefabMembership m = src.record(s)->prefab;
        if (!m.linked() && m.pending.isNull()) continue;
        auto it = map.find(m.instance);
        if (it == map.end()) continue;  // a member copied without its instance root becomes a plain entity
        m.instance = it->second;
        (void)setPrefabMembership(map[s], std::move(m));
    }
    if (idMap) *idMap = std::move(map);
    return newRoots;
}

// ---------------------------------------------------------------------------------------------------------------
// Prefab templates and the file form
// ---------------------------------------------------------------------------------------------------------------

std::shared_ptr<const PrefabTemplate> Scene::prefabTemplate(const std::string& source) const {
    auto it = prefabTemplates_.find(source);
    return it == prefabTemplates_.end() ? nullptr : it->second;
}

void Scene::setPrefabTemplate(const std::string& source, std::shared_ptr<const PrefabTemplate> t) {
    if (t) {
        prefabTemplates_[source] = std::move(t);
    } else {
        prefabTemplates_.erase(source);
    }
}

std::vector<std::string> Scene::prefabSources() const {
    std::vector<std::string> out;
    for (EntityId e : order_) {
        const EntityRecord& r = records_.at(e);
        if (r.prefab.linked() && r.prefab.instance == e && !r.prefab.source.empty() &&
            std::find(out.begin(), out.end(), r.prefab.source) == out.end()) {
            out.push_back(r.prefab.source);
        }
    }
    return out;
}

Json Scene::toFileJson() const {
    Scene& self = const_cast<Scene&>(*this);  // diffing may cache resolved templates
    std::vector<EntityId> implicit;
    std::unordered_map<EntityId, Json> roots;
    std::unordered_set<EntityId> expanded;  // instances that could not be diffed: saved fully, still linked
    for (EntityId e : order_) {
        const EntityRecord& r = records_.at(e);
        if ((r.prefab.linked() && r.prefab.instance == e) || !r.prefab.pending.isNull()) {
            Json rec = prefab::savedRecord(self, e, implicit);
            if (rec.get("prefab").contains("instance")) expanded.insert(e);
            roots[e] = std::move(rec);
        }
    }
    std::unordered_set<EntityId> skip(implicit.begin(), implicit.end());
    Json entities = Json::array();
    for (EntityId e : order_) {
        if (skip.count(e)) continue;
        if (auto it = roots.find(e); it != roots.end()) {
            entities.push(std::move(it->second));
            continue;
        }
        Json j = entityToJson(e);
        // A member that left its instance is saved as a plain entity.
        if (!expanded.count(records_.at(e).prefab.instance)) j.erase("prefab");
        entities.push(std::move(j));
    }
    return Json::object({{"format", "skywalker.scene"},
                         {"version", 1},
                         {"name", name},
                         {"seed", seed},
                         {"environment", reflect::toJson(&environment_, Environment::type())},
                         {"entities", entities}});
}

}  // namespace sky
