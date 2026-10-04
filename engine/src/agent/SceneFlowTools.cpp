// Runtime scene flow tools (docs/SCENE_FLOW.md): scene_flow_info, scene_change, scene_additive_load,
// scene_additive_unload.

#include "ToolHelpers.h"
#include "skywalker/game/SceneFlow.h"

namespace sky::tools {

namespace {

using namespace schema;

/// Entity references in tool arguments (names or ids) as ids, for the option parsers.
Status resolveEntities(Engine& engine, Json& args, std::initializer_list<const char*> fields) {
    for (const char* f : fields) {
        Json* v = args.find(f);
        if (!v || v->isNull()) continue;
        auto id = resolve(engine, *v);
        if (!id) return id.error();
        *v = Json(*id);
    }
    return {};
}

}  // namespace

void addSceneFlowTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"scene_flow_info", "Scene flow state",
             "The running game's scene flow: the current scene (alias or path), the pending change (target, preloaded assets, "
             "loading scene), loading progress, the transition (kind, phase out|loading|in|idle, alpha), loaded sub-scenes "
             "with their entity counts, the entities that persist across changes, game.json scene aliases and every scene "
             "file. Use it before and after scene_change / scene_additive_load, and to explain why something did not carry over.",
             "scene", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 Json out = engine.sceneFlow().info();
                 out["warnings"] = Json::array();
                 if (engine.playState() == PlayState::Editing) out["warnings"].push("not playing: scene changes happen in play mode (sim_control play)");
                 return ToolResult::json(out, "scene " + (out.get("current").asString().empty() ? std::string("(editing)") : out.get("current").asString()) +
                                                  (out.get("pending").isNull() ? "" : ", changing to " + out.get("pending").get("target").asString()));
             }});

    reg.add({"scene_change", "Change scene (play)",
             "Moves the running game to another scene, as Wander's change_scene does: the scene is a game.json \"scenes\" alias "
             "or a .sky.json path (unknown names get a did-you-mean). Entities with a `persistent` component, game.json "
             "sceneFlow.persistent names and `keep` carry over with their behaviors running; the rest is replaced. The change "
             "runs over the next ticks (transition out, preloading, swap, transition in); step the simulation to see it, or "
             "pass immediate: true to swap now. Stopping play returns the editor to the scene play started in. Example: "
             "{\"scene\": \"level2\", \"transition\": \"fade\", \"duration\": 0.4, \"spawn_at\": \"Door_West\"}.",
             "scene",
             object({{"scene", string("Alias or .sky.json path")},
                     {"transition", any("\"none\" | \"fade\" | \"crossfade\", or {kind, duration, color}")},
                     {"duration", number("Transition seconds (0..10)")},
                     {"color", string("Fade color, e.g. \"#000000\"")},
                     {"keep", array(string("Entity name"), "More entities to carry over this time")},
                     {"spawn_at", string("Entity of the new scene the player moves to (persistent.spawn, else tag \"player\")")},
                     {"loading", string("Loading scene shown while the target preloads (\"\" = none; default game.json sceneFlow.loadingScene)")},
                     {"immediate", boolean("Swap now, without transition or preloading (default false)")}},
                    {"scene"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 Json opts = a;
                 opts.erase("scene");
                 auto options = game::ChangeOptions::fromJson(opts);
                 if (!options) return ToolResult::error(options.error());
                 if (Status s = engine.sceneFlow().requestChange(a.get("scene").asString(), *options); !s) return fail(s);
                 Json out = engine.sceneFlow().info();
                 out["warnings"] = Json::array();
                 return ToolResult::json(out, options->immediate ? "now in " + engine.sceneFlow().current()
                                                                  : "changing to " + a.get("scene").asString() + " over the next ticks");
             }});

    reg.add({"scene_additive_load", "Load a sub-scene (play)",
             "Adds a scene's entities to the running game (a room, a streaming chunk, a UI overlay) with fresh entity ids and "
             "returns its handle. The sub-scene owns exactly the entities it created, so scene_additive_unload removes those "
             "and nothing else. Options: id (handle; default the alias or file name), parent (entity to load under), offset "
             "(added to its root positions). Loads now (between ticks); `on scene_loaded` runs next tick. Example: "
             "{\"scene\": \"rooms/cellar\", \"offset\": [40, 0, 0]}.",
             "scene",
             object({{"scene", string("Alias or .sky.json path")},
                     {"id", string("Handle for unloading (default: alias or file name, made unique)")},
                     {"parent", entity("Entity to load under (default: the root)")},
                     {"offset", vec3("Added to the sub-scene's root positions")}},
                    {"scene"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 Json opts = a;
                 opts.erase("scene");
                 if (Status s = resolveEntities(engine, opts, {"parent"}); !s) return fail(s);
                 auto options = game::AdditiveOptions::fromJson(opts);
                 if (!options) return ToolResult::error(options.error());
                 auto handle = engine.sceneFlow().loadAdditive(a.get("scene").asString(), *options, true);
                 if (!handle) return ToolResult::error(handle.error());
                 Json out = Json::object({{"handle", *handle}, {"warnings", Json::array()}});
                 const Json info = engine.sceneFlow().info();
                 for (const auto& sub : info.get("additive").elements()) {
                     if (sub.get("id").asString() == *handle) out["entities"] = sub.get("entities");
                 }
                 return ToolResult::json(out, "loaded sub-scene " + *handle + " (" + std::to_string(out.get("entities").asInt()) + " entities)");
             }});

    reg.add({"scene_additive_unload", "Unload a sub-scene (play)",
             "Removes a sub-scene loaded with scene_additive_load or Wander's load_additive: exactly the entities it created. "
             "Entities the game added under them at run time are kept and moved to the root (listed as orphans). Removes now "
             "(between ticks). Example: {\"handle\": \"cellar\"}.",
             "scene", object({{"handle", string("Sub-scene handle (scene_flow_info lists them)")}}, {"handle"}), true, false,
             [&engine](const Json& a, ToolContext&) {
                 auto r = engine.sceneFlow().unload(a.get("handle").asString(), true);
                 if (!r) return ToolResult::error(r.error());
                 Json orphans = Json::array();
                 for (uint64_t o : r->orphans) orphans.push(o);
                 Json warnings = Json::array();
                 if (!r->orphans.empty()) warnings.push(std::to_string(r->orphans.size()) + " entities added at run time under the sub-scene were kept at the root");
                 return ToolResult::json(Json::object({{"handle", a.get("handle")}, {"removed", r->removed}, {"orphans", orphans}, {"warnings", warnings}}),
                                         "unloaded " + a.get("handle").asString() + " (" + std::to_string(r->removed) + " entities)");
             }});
}

}  // namespace sky::tools
