#pragma once
// Scene: the authoritative world state.
//
// Scene layers on top of the raw ECS registry:
//   * stable 64-bit EntityIds that never get reused (safe for agents to hold on to),
//   * names, tags, free-form vars and a parent/child hierarchy,
//   * a component catalogue built from reflection (JSON in / JSON out),
//   * change notification hooks used by History for transactional undo/redo.
//
// All "editing" (from the editor UI, agents or scripts in edit mode) goes through the
// JSON-level API below, so every mutation is validated and recordable.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/ecs/Registry.h"

namespace sky {

using EntityId = uint64_t;
constexpr EntityId kNoEntity = 0;

struct PrefabTemplate;  // scene/PrefabLink.h
/// Resolves a prefab reference ("prefabs/door.prefab.json", "guid:...") to its current template. Set by the
/// engine (asset system); scenes without one keep linked instances expanded but cannot re-expand or diff them.
using PrefabResolver = std::function<Result<std::shared_ptr<const PrefabTemplate>>(const std::string& ref)>;

/// Prefab linkage of an entity (docs/PREFABS.md). A linked instance is a tree of entities expanded from a prefab
/// file. Every member remembers the prefab-local id (pid) of the node it came from, so overrides are addressed by
/// pid and survive renames; the scene file stores only the instance root and its differences from the prefab.
struct PrefabMembership {
    EntityId instance = kNoEntity;  // root entity of the instance (the root itself); kNoEntity = not linked
    uint32_t pid = 0;               // prefab-local id of the node this entity came from
    std::string source;             // root only: project-relative path of the .prefab.json
    std::string guid;               // root only: asset GUID of the source (survives renames and moves)
    Json pending;                   // root only: the saved block, kept verbatim while the source cannot be loaded

    bool linked() const { return instance != kNoEntity; }
};

struct EntityRecord {
    EntityId id = kNoEntity;
    std::string name;
    std::vector<std::string> tags;
    EntityId parent = kNoEntity;
    Json vars = Json::object();  // free-form per-entity state (Wander `var`s live here)
    bool enabled = true;
    ecs::Entity handle;
    // --- Record-level extension points (append new ones here; keep them serialized by Scene::entityToJson) ---
    PrefabMembership prefab;  // linked prefab instance membership (scene/PrefabLink.h)
    bool unique = false;      // unique name in its owner (prefab instance or scene): Wander find("%Name")
};

/// Receives notifications *before* a piece of state changes (used by History).
class ChangeObserver {
public:
    virtual ~ChangeObserver() = default;
    virtual void beforeEntityChange(EntityId id) = 0;
    virtual void beforeEnvironmentChange() = 0;
};

/// Type-erased description of a component kind.
struct ComponentKind {
    std::string name;
    const TypeInfo* info = nullptr;  // null for non-reflected kinds (behaviors)
    std::function<bool(const class Scene&, EntityId)> has;
    std::function<void(Scene&, EntityId)> add;
    std::function<void(Scene&, EntityId)> remove;
    std::function<Json(const Scene&, EntityId)> toJson;
    std::function<Status(Scene&, EntityId, const Json&)> apply;
    /// Raw component pointer (null if absent; reflected kinds only). Wander reads and
    /// writes fields through it without a JSON round trip.
    std::function<void*(Scene&, EntityId)> ptr;
    /// Typed copy of the component from another scene's entity (removes it when the source has none).
    std::function<void(Scene& dst, EntityId, const Scene& src, EntityId)> copy;
};

/// One entity link stored in a component field (FieldType::Entity / EntityList), resolved.
struct LinkInfo {
    EntityId from = kNoEntity;  // entity holding the link
    std::string component;
    std::string field;
    int index = -1;             // element of an EntityList, -1 for single links
    EntityLink link;            // as stored
    EntityId target = kNoEntity;  // what it resolves to now (kNoEntity = dangling / unresolved)
    bool byName = false;        // resolved through the name fallback (not bound to an id yet)
};

class Scene {
public:
    Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // --- Identity & lifetime ------------------------------------------------
    EntityId create(std::string name, EntityId parent = kNoEntity, EntityId forcedId = kNoEntity,
                    size_t orderHint = SIZE_MAX);
    /// Destroys the entity and all its descendants. Returns the number destroyed.
    size_t destroy(EntityId id);
    bool exists(EntityId id) const { return records_.count(id) != 0; }
    void clear();

    EntityRecord* record(EntityId id);
    const EntityRecord* record(EntityId id) const;
    ecs::Entity handle(EntityId id) const;

    /// Entities in stable, deterministic order (creation / hierarchy order).
    const std::vector<EntityId>& entities() const { return order_; }
    /// The id the next create() hands out. Save games restore it so spawns after a load get the same ids.
    EntityId nextEntityId() const { return nextId_; }
    /// Sets the next id (never at or below an existing entity's id).
    void setNextEntityId(EntityId next);
    std::vector<EntityId> children(EntityId id) const;
    /// Reorders entities to follow `order` (ids that don't exist are skipped; existing ids
    /// missing from `order` keep their relative order at the end). Used by undo/redo.
    void setOrder(const std::vector<EntityId>& order);
    size_t size() const { return order_.size(); }

    /// Finds by exact id ("#12"/"12"), exact name, then case-insensitive name.
    EntityId find(std::string_view nameOrId) const;
    std::vector<EntityId> findTagged(std::string_view tag) const;

    // --- Entity links and scoped names ------------------------------------------
    /// The entity a link points at: its id when that entity exists, else its name looked up near `from`
    /// (findNear). kNoEntity when it is empty or dangling.
    EntityId resolve(const EntityLink& link, EntityId from = kNoEntity) const;
    /// Name lookup that prefers entities close to `from`: its prefab instance, its own subtree, its parent's
    /// subtree, then the whole scene (find). "%Name" is findUnique. "#12" is an id.
    EntityId findNear(std::string_view name, EntityId from) const;
    /// "%Name" lookups: the entity named `name` marked `unique` in the owner scope of `from` (its prefab instance,
    /// or the scene's top level when `from` is not part of an instance).
    EntityId findUnique(std::string_view name, EntityId from) const;
    /// Owner scope of an entity: its prefab instance root, or kNoEntity for the scene itself.
    EntityId ownerOf(EntityId id) const;
    /// Binds name-only links on `id` to entity ids when the name resolves unambiguously (scoped lookup, or a
    /// globally unique name). Called after edits and loads, so links then follow renames.
    void bindLinks(EntityId id);
    /// Every link stored on `id`, resolved.
    std::vector<LinkInfo> linksFrom(EntityId id) const;
    /// Links anywhere in the scene that resolve to `target`.
    std::vector<LinkInfo> linksTo(EntityId target) const;
    /// Rewrites links on `ids` whose stored id is a key of `map` (duplicate, paste, prefab expansion).
    void remapLinks(const std::vector<EntityId>& ids, const std::unordered_map<EntityId, EntityId>& map);

    // --- Copying ------------------------------------------------------------------
    /// Replaces `dst`'s name, flags, tags, vars, components and behaviors with those of `srcId` in `src` (which may
    /// be this scene). Parent and prefab membership are kept. Recorded like any edit.
    void copyEntityData(EntityId dst, const Scene& src, EntityId srcId);
    /// Copies whole subtrees from `src` (may be this scene) under `parent`. Links and prefab instances inside the
    /// copied set point inside the copy; `idMap` receives source id -> new id. Returns the new roots.
    /// `wantedIds` (source id -> id) gives copies the ids they should get (save games restore a scene with the
    /// ids it was saved with); an id that is taken, or a source missing from the map, gets a fresh id.
    std::vector<EntityId> cloneTrees(const Scene& src, const std::vector<EntityId>& roots, EntityId parent,
                                     std::unordered_map<EntityId, EntityId>* idMap = nullptr,
                                     const std::unordered_map<EntityId, EntityId>* wantedIds = nullptr);

    // --- Edits (validated, observable) --------------------------------------
    Status rename(EntityId id, std::string name);
    Status setParent(EntityId id, EntityId parent);
    Status setTags(EntityId id, std::vector<std::string> tags);
    Status setEnabled(EntityId id, bool enabled);
    Status patchVars(EntityId id, const Json& patch);
    /// Patch a component (adds it if missing). `null` patch removes the component.
    Status patchComponent(EntityId id, std::string_view component, const Json& patch);
    Status setBehaviors(EntityId id, const Json& behaviors);
    Status setUnique(EntityId id, bool unique);
    /// Prefab membership (scene/PrefabLink.h maintains it; recorded for undo).
    Status setPrefabMembership(EntityId id, PrefabMembership membership);
    Status patchEnvironment(const Json& patch);

    /// Applies an entity-level JSON document: {name, tags, parent, enabled, vars,
    /// components:{...}, behaviors:[...]}. Missing keys are left untouched.
    Status applyEntityJson(EntityId id, const Json& doc);

    // --- Typed access (runtime / rendering; NOT recorded in history) --------
    ecs::Registry& registry() { return registry_; }
    const ecs::Registry& registry() const { return registry_; }

    template <typename T>
    T* get(EntityId id) {
        auto it = records_.find(id);
        return it == records_.end() ? nullptr : registry_.get<T>(it->second.handle);
    }
    template <typename T>
    const T* get(EntityId id) const {
        auto it = records_.find(id);
        return it == records_.end() ? nullptr : registry_.get<T>(it->second.handle);
    }
    template <typename T>
    T& add(EntityId id) {
        auto& rec = records_.at(id);
        if (T* existing = registry_.get<T>(rec.handle)) return *existing;
        return registry_.emplace<T>(rec.handle);
    }

    Environment& environment() { return environment_; }
    const Environment& environment() const { return environment_; }

    /// World transform including all parents.
    Mat4 worldMatrix(EntityId id) const;
    /// Local-space bounds of the entity's mesh (unit primitives), or empty box.
    Aabb localBounds(EntityId id) const;
    bool isActive(EntityId id) const;  // enabled and all ancestors enabled

    // --- Serialization -------------------------------------------------------
    Json entityToJson(EntityId id) const;
    /// Complete, self-contained scene JSON (every entity expanded): snapshots, play restore, sandboxes.
    Json toJson() const;
    /// Scene JSON for files: linked prefab instances are written as their source plus overrides
    /// (docs/PREFABS.md), so editing the prefab updates every scene that uses it.
    Json toFileJson() const;
    /// Loads either form. Problems that do not stop the load (missing prefab sources...) go to loadWarnings().
    Status loadJson(const Json& doc);
    const std::vector<std::string>& loadWarnings() const { return loadWarnings_; }

    // --- Prefab linkage (scene/PrefabLink.h) ----------------------------------------
    void setPrefabResolver(PrefabResolver resolver) { prefabResolver_ = std::move(resolver); }
    const PrefabResolver& prefabResolver() const { return prefabResolver_; }
    /// The template version the instances of `source` were expanded from (kept across clear()).
    std::shared_ptr<const PrefabTemplate> prefabTemplate(const std::string& source) const;
    void setPrefabTemplate(const std::string& source, std::shared_ptr<const PrefabTemplate> t);
    /// Sources with instances expanded from a known template version.
    std::vector<std::string> prefabSources() const;

    /// Snapshot used by History: like entityToJson plus internal ordering.
    Json snapshotEntity(EntityId id) const;
    void restoreEntity(EntityId id, const Json& snapshot);

    const std::vector<ComponentKind>& componentKinds() const { return kinds_; }
    const ComponentKind* componentKind(std::string_view name) const;
    std::vector<std::string> componentNames() const;

    void setObserver(ChangeObserver* observer) { observer_ = observer; }
    ChangeObserver* observer() const { return observer_; }

    std::string name = "Untitled";
    uint64_t seed = 1;
    /// Local bounds of imported mesh assets keyed by "asset:<path>" (filled by the asset loader).
    std::unordered_map<std::string, Aabb> assetBounds;
    /// Bumped on every structural or data change; renderers/editors use it to refresh.
    uint64_t revision() const { return revision_; }
    void markDirty() { ++revision_; }
    /// Bumped whenever any entity's behaviors (Wander scripts) change; the runtime only
    /// rescans scripts for recompilation when this moves.
    uint64_t behaviorsRevision() const { return behaviorsRevision_; }

private:
    void notify(EntityId id) {
        if (observer_) observer_->beforeEntityChange(id);
        ++revision_;
    }
    void registerKinds();
    bool wouldCycle(EntityId id, EntityId newParent) const;

    ecs::Registry registry_;
    std::unordered_map<EntityId, EntityRecord> records_;
    std::vector<EntityId> order_;
    EntityId nextId_ = 1;
    Environment environment_;
    std::vector<ComponentKind> kinds_;
    ChangeObserver* observer_ = nullptr;
    uint64_t revision_ = 0;
    uint64_t behaviorsRevision_ = 0;
    std::vector<std::string> loadWarnings_;
    PrefabResolver prefabResolver_;
    std::unordered_map<std::string, std::shared_ptr<const PrefabTemplate>> prefabTemplates_;
};

std::string formatEntityRef(EntityId id);

}  // namespace sky
