#pragma once
// Linked prefab instances (docs/PREFABS.md).
//
// Placing a prefab creates a *linked instance*: real entities in the scene (so rendering, physics and Wander see
// nothing special) whose records remember the instance root and the prefab-local id ("pid") of the node each one
// came from. Differences between an instance and its prefab are *overrides*. They are computed, never tracked:
// an override is any field whose value differs from the prefab (the way Godot's PackedScene packs only changed
// properties), so every edit path (tools, editor, Wander in edit mode) produces them for free.
//
//   * Scene files store an instance as its root record plus {"source", "guid", "ids", "overrides", "removed"}.
//     Loading expands it from the current prefab, so editing the prefab updates every scene that uses it.
//   * Overrides are addressed by pid, so renaming or reordering nodes inside the prefab keeps them.
//   * The root's name and transform are the instance's *placement*: listed, but never applied to the prefab.
//   * Entities added under an instance are ordinary entities parented to a member; members deleted from an
//     instance are listed as "removed".
//
// Templates are prefab documents expanded into a private Scene whose entity ids ARE the pids; that scene gives
// normalized values to compare against and typed components to copy from (fast spawn).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/scene/Scene.h"

namespace sky {

struct PrefabTemplate {
    std::string source;  // project-relative path of the .prefab.json ("" for in-memory prefabs)
    std::string guid;
    uint64_t version = 0;  // hash of the document: instances re-expand when it changes
    Json doc;              // the prefab document as loaded
    std::shared_ptr<Scene> scene;  // the prefab expanded with entity id == pid
    uint32_t root = 0;             // pid of the root node
    std::vector<uint32_t> order;   // pids, depth first
    std::unordered_map<uint32_t, uint32_t> parent;   // pid -> parent pid (the root maps to 0)
    std::unordered_map<uint32_t, std::string> path;  // pid -> readable path inside the prefab ("Body/Lamp")
    std::vector<std::string> warnings;

    bool has(uint32_t pid) const { return parent.count(pid) != 0; }
    std::string nodeName(uint32_t pid) const;
};

/// Builds a template from a prefab document. Nodes without a "pid" (version 1 files) get depth-first pids 1..n,
/// which stay stable until the file is rewritten (prefab_create / prefab_apply write version 2 with pids).
Result<std::shared_ptr<const PrefabTemplate>> buildPrefabTemplate(const Json& doc, std::string source = {},
                                                                  std::string guid = {});

namespace prefab {

struct Override {
    uint32_t pid = 0;
    EntityId entity = kNoEntity;  // the live member it is set on
    std::string node;             // readable path of the node inside the prefab
    /// "name", "enabled", "tags", "unique", "behaviors", "vars.<key>", "<component>" (added / removed component)
    /// or "<component>.<field>".
    std::string property;
    Json value;        // the instance's value (null = removed); entity links use scene ids
    Json prefabValue;  // the prefab's value
    bool placement = false;  // root name / transform: where this instance sits; never applied to the prefab

    Json toJson() const;
};

struct InstanceDiff {
    EntityId root = kNoEntity;
    std::shared_ptr<const PrefabTemplate> tmpl;
    std::unordered_map<uint32_t, EntityId> members;  // intact members by pid
    std::vector<Override> overrides;                 // includes placement (flagged)
    std::vector<uint32_t> removed;                   // top-most prefab nodes missing from the instance
    std::vector<EntityId> added;                     // non-prefab entities parented to a member
    std::vector<EntityId> detached;                  // members moved out of their place (saved as plain entities)
    std::vector<std::string> warnings;

    size_t count(bool includePlacement = false) const;
    /// Agent-facing summary: {instance, source, overrides[], removed[], added[], placement{}, warnings[]}.
    Json toJson(const Scene& scene) const;
};

/// Instance roots in scene order (all, or those whose source is `source`).
std::vector<EntityId> instances(const Scene& scene, const std::string& source = {});
/// Root of the instance `id` belongs to (kNoEntity if it is not linked).
EntityId instanceOf(const Scene& scene, EntityId id);
/// The template an instance was expanded from (resolved through the scene's resolver when unknown).
Result<std::shared_ptr<const PrefabTemplate>> templateOf(Scene& scene, EntityId root);

/// Compares an instance with its template (default: the version it was expanded from).
Result<InstanceDiff> diff(Scene& scene, EntityId root, std::shared_ptr<const PrefabTemplate> tmpl = nullptr);

/// Creates a linked instance under `parent` (name "" = the prefab root's name). Call inside an edit for undo.
Result<EntityId> instantiate(Scene& scene, const std::shared_ptr<const PrefabTemplate>& tmpl, EntityId parent,
                             const std::string& name = {});

/// Re-expands an instance from `tmpl`: members are reset to the prefab, new nodes are created, nodes the prefab no
/// longer has are destroyed, then `keep` overrides and `removed` nodes are re-applied. Ids of existing members stay.
Status rebuild(Scene& scene, EntityId root, const std::shared_ptr<const PrefabTemplate>& tmpl,
               const std::vector<Override>& keep, const std::vector<uint32_t>& removed);

/// Applies one override value (scene space) to a live entity.
Status applyOverride(Scene& scene, EntityId entity, const std::string& property, const Json& value);

/// Re-expands every instance whose prefab changed since it was expanded (asks the scene's resolver for the
/// current version). `source` limits it to one prefab. Returns how many instances were updated.
Result<size_t> sync(Scene& scene, const std::string& source = {});

/// Reverts overrides. `entity` is a member or the root; `property` "" reverts all of that entity's overrides
/// (placement is kept), `wholeInstance` reverts every member and restores removed nodes (added entities stay).
Result<size_t> revert(Scene& scene, EntityId entity, const std::string& property, bool wholeInstance);

/// Writes a new version of a prefab and returns its template (the engine saves the file and reloads it).
using PrefabWriter = std::function<Result<std::shared_ptr<const PrefabTemplate>>(const Json& document)>;
struct ApplyResult {
    Json document;                // what was written
    size_t applied = 0;           // overrides (+ removed and added nodes) pushed into the prefab
    size_t instancesUpdated = 0;  // other instances in this scene re-expanded from the new version
    std::vector<std::string> warnings;
};
/// Pushes overrides into the prefab file (pids are kept; entities added under the instance become new nodes and
/// stay linked). `property` "" applies everything on the instance of `entity`; otherwise only that override of
/// `entity`. Placement is never applied. Every other instance of the prefab in the scene is re-expanded.
Result<ApplyResult> apply(Scene& scene, EntityId entity, const std::string& property, const PrefabWriter& write);

/// Make Local: the instance becomes plain entities. Returns how many entities were unlinked.
size_t unpack(Scene& scene, EntityId root);

/// Whether the subtree at `candidate` has the prefab's hierarchy node for node (same child names in order).
/// On success fills pid -> entity.
bool match(const Scene& scene, EntityId candidate, const PrefabTemplate& tmpl, std::unordered_map<uint32_t, EntityId>& out);
/// Links an existing copy whose hierarchy matched. Values are left as they are (differences become overrides).
Status link(Scene& scene, EntityId root, const std::shared_ptr<const PrefabTemplate>& tmpl,
            const std::unordered_map<uint32_t, EntityId>& members);

/// Prefab document (version 2) for a subtree. Links inside the subtree become {"pid"}; links outside keep only
/// the name. Nested instances are flattened. `zeroRootPosition` stores the root at the origin.
Json documentFromScene(const Scene& scene, EntityId root, bool zeroRootPosition = true);

// --- Scene file form (used by Scene::toFileJson / loadJson) ---------------------------------------------------
/// Expands a saved instance block onto `root` (created by the loader). Problems become warnings; when the source
/// cannot be loaded the block is kept verbatim (membership.pending) so saving again loses nothing.
void expandSaved(Scene& scene, EntityId root, const Json& block, std::vector<std::string>& warnings);
/// File-form record of an instance root; adds the ids of members it covers to `implicit`.
Json savedRecord(Scene& scene, EntityId root, std::vector<EntityId>& implicit);

}  // namespace prefab
}  // namespace sky
