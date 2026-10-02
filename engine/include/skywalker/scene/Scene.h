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

struct EntityRecord {
    EntityId id = kNoEntity;
    std::string name;
    std::vector<std::string> tags;
    EntityId parent = kNoEntity;
    Json vars = Json::object();  // free-form per-entity state (Wander `var`s live here)
    bool enabled = true;
    ecs::Entity handle;
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
    std::vector<EntityId> children(EntityId id) const;
    size_t size() const { return order_.size(); }

    /// Finds by exact id ("#12"/"12"), exact name, then case-insensitive name.
    EntityId find(std::string_view nameOrId) const;
    std::vector<EntityId> findTagged(std::string_view tag) const;

    // --- Edits (validated, observable) --------------------------------------
    Status rename(EntityId id, std::string name);
    Status setParent(EntityId id, EntityId parent);
    Status setTags(EntityId id, std::vector<std::string> tags);
    Status setEnabled(EntityId id, bool enabled);
    Status patchVars(EntityId id, const Json& patch);
    /// Patch a component (adds it if missing). `null` patch removes the component.
    Status patchComponent(EntityId id, std::string_view component, const Json& patch);
    Status setBehaviors(EntityId id, const Json& behaviors);
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
    Json toJson() const;
    Status loadJson(const Json& doc);

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
};

std::string formatEntityRef(EntityId id);

}  // namespace sky
