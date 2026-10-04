#pragma once
// Prefabs (`*.prefab.json`): a saved entity tree (components, behaviors, vars, tags and
// children) that can be instantiated any number of times — by tools, the editor, or from
// Wander with spawn("prefab:path"). Agents use prefabs to build once and reuse everywhere.
//
// Instances are *linked* (scene/PrefabLink.h, docs/PREFABS.md): scenes store only what differs from the prefab,
// and editing the prefab updates every instance.

#include <memory>
#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/scene/PrefabLink.h"
#include "skywalker/scene/Scene.h"

namespace sky {

/// Captures `root` and its descendants as a prefab document (version 2: every node has a pid; links between its
/// entities are stored as pids). The root's position is stored relative to zero so instances can be placed anywhere.
Json prefabFromEntity(const Scene& scene, EntityId root);

struct PrefabPlacement {
    EntityId parent = kNoEntity;
    bool hasPosition = false;
    Vec3 position;
    bool hasYaw = false;
    float yaw = 0;
    float scale = 1.f;
    std::string name;  // overrides the root name if not empty
};

/// Creates the entity tree from a prefab document (plain entities, not linked); returns the new root.
Result<EntityId> instantiatePrefab(Scene& scene, const Json& prefab, const PrefabPlacement& placement);
/// Creates an instance from a template. `linked` keeps it linked to its source file (when it has one).
Result<EntityId> instantiatePrefab(Scene& scene, const std::shared_ptr<const PrefabTemplate>& tmpl,
                                   const PrefabPlacement& placement, bool linked = true);

Result<Json> loadPrefab(const std::string& absolutePath);
Status savePrefab(const std::string& absolutePath, const Json& prefab);

}  // namespace sky
