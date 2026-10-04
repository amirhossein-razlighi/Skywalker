#pragma once
// Internal helpers shared by the tool catalogue files (EngineTools / AssetTools / WorldTools).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/engine/Engine.h"

namespace sky::dcc {
class Manager;
}

namespace sky::tools {

/// Entity by numeric id or (case-insensitive) name, with a did-you-mean error.
Result<EntityId> resolve(Engine& engine, const Json& ref);
/// One-line, token-efficient entity description.
std::string describe(const Scene& s, EntityId e);
Json briefJson(const Scene& s, EntityId e);
ToolResult fail(const Status& s);
void duplicateTree(Scene& s, EntityId src, EntityId newParent, const std::string& name, Vec3 offset, bool root,
                   std::vector<EntityId>& created);

void collectSubtree(const Scene& s, EntityId root, std::vector<EntityId>& out);
/// World-space bounds of every mesh in an entity's subtree (nullopt if it has none).
std::optional<Aabb> subtreeBounds(const Scene& s, EntityId root);
/// Moves an entity vertically so its bounds rest on the geometry below (recorded edit).
Status dropToSurface(Engine& engine, EntityId id, float offset = 0.f);

/// Creates an entity for an importMeshAsset() result (a prefab instance for multi-material
/// models). Call inside engine.edit().
Status placeImportedMesh(Engine& engine, const Json& imported, const std::string& name, const Json& position, EntityId& out);

void addAssetTools(Engine& engine, ToolRegistry& reg);
void addWorldTools(Engine& engine, ToolRegistry& reg);
void addNetworkTools(Engine& engine, ToolRegistry& reg);
void addFxTools(Engine& engine, ToolRegistry& reg);
void addTools2D(Engine& engine, ToolRegistry& reg);        // sprites, atlases, tilemaps
void addUiTools(Engine& engine, ToolRegistry& reg);        // ui_create, ui_style, ui_inspect, ui_interact
void addDialogueTools(Engine& engine, ToolRegistry& reg);  // dialogue_check, dialogue_preview, dialogue_control
void addWanderTools(Engine& engine, ToolRegistry& reg);  // Wander 2: check, test, spec, graph, AOT
void addNativeTools(Engine& engine, ToolRegistry& reg);  // native C++ modules
void addWorldBuildTools(Engine& engine, ToolRegistry& reg);
void addAudioTools(Engine& engine, ToolRegistry& reg);
void addInputTools(Engine& engine, ToolRegistry& reg);
/// Applies the action / axis / gamepad / mouse parts of a sim_input call (InputTools.cpp).
Status applySimInput(Engine& engine, const Json& args);
/// DCC bridge tools (Blender, Maya, Houdini, 3ds Max). Pass a manager to use a specific state
/// folder / host (tests); the default talks to the real machine. Returns the manager in use.
std::shared_ptr<dcc::Manager> addDccTools(Engine& engine, ToolRegistry& reg, std::shared_ptr<dcc::Manager> manager = nullptr);
void addStudioTools(Engine& engine, ToolRegistry& reg);
/// physics_* and nav_* tools (PhysicsTools.cpp registers NavTools.cpp too).
void addPhysicsTools(Engine& engine, ToolRegistry& reg);
void addNavTools(Engine& engine, ToolRegistry& reg);
void addAnimationTools(Engine& engine, ToolRegistry& reg);  // AnimationTools.cpp (+ SequenceTools.cpp)
void addHairTools(Engine& engine, ToolRegistry& reg);
void addImpostorTools(Engine& engine, ToolRegistry& reg);  // ImpostorTools.cpp: foliage impostors
void addGameTools(Engine& engine, ToolRegistry& reg);  // GameTools.cpp: game_build, game_run, game_settings
void addMovieTools(Engine& engine, ToolRegistry& reg);  // MovieTools.cpp: movie_render (docs/MOVIE_RENDER.md)
void addRenderLayerTools(Engine& engine, ToolRegistry& reg);  // RenderLayerTools.cpp: render_layers
void addProcessTools(Engine& engine, ToolRegistry& reg);  // ProcessTools.cpp: process_info, sim_teleport, sim_display
/// Reads a property path ("transform.position", "vars.score", "name", "enabled") for traces (WorldTools.cpp).
Json readPropertyPath(const Scene& s, EntityId id, const std::string& path);
void addShadowTools(Engine& engine, ToolRegistry& reg);  // ShadowTools.cpp: shadow_atlas_info, light_shadows
void addPrefabTools(Engine& engine, ToolRegistry& reg);  // PrefabTools.cpp: entity_refs, copy/paste, prefab_overrides...
void addLegalTools(Engine& engine, ToolRegistry& reg);  // LegalTools.cpp: legal_info (docs/legal/)
void addCustomToolTools(Engine& engine, ToolRegistry& reg);  // CustomToolTools.cpp: tool_define, tool_test, ... (docs/CUSTOM_TOOLS.md)
void addAgentLinkTools(Engine& engine, ToolRegistry& reg);  // AgentLinkTools.cpp: events_poll, tool_host_* (docs/PYTHON_AGENTS.md)
void addAuditTools(Engine& engine, ToolRegistry& reg);   // AuditTools.cpp: scene_audit (look-dev quality gate)

}  // namespace sky::tools
