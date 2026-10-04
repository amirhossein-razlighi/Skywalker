// Wander builtins for runtime scene flow (docs/SCENE_FLOW.md): change_scene, load_additive,
// unload_scene, current_scene, loading_progress, and the scene_loaded / scene_unloading triggers.

#include "skywalker/engine/Engine.h"
#include "skywalker/game/SceneFlow.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

game::SceneFlow& flow(CallContext& c) {
    Engine* engine = c.service<Engine>();
    if (!engine) c.fail(c.def().name + "(): scene changes are not available here (no running game)");
    return engine->sceneFlow();
}

[[noreturn]] void failWith(CallContext& c, const Error& e) {
    c.fail(c.def().name + "(): " + e.message + (e.hint.empty() ? "" : " - " + e.hint));
}

void def(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, TypeSet ret, const char* doc, const char* example,
         BuiltinImpl impl) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = ret;
    d.category = "scene";
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.owner = "engine";
    r.add(std::move(d));
}

}  // namespace

void registerSceneFlowBuiltins(BuiltinRegistry& reg) {
    def(reg, "change_scene", {{"scene", kTString}, {"options", kTMap, true}}, kTNone,
        "Moves the game to another scene (a game.json \"scenes\" alias or a .sky.json path) after this tick: the "
        "current scene hears `on scene_unloading`, entities with a `carry` component (and options.keep) carry "
        "over with their running behaviors, the rest is replaced. Options: transition (\"fade\" | \"crossfade\" | "
        "\"none\" or {kind, duration, color}), duration, keep: [names], spawn_at: \"EntityName\" (the player moves "
        "there), loading: a loading scene shown while assets preload. `on scene_loaded` follows. Stopping play returns "
        "the editor to the scene play started in.",
        "change_scene(\"level2\", {transition: \"fade\", duration: 0.5, spawn_at: \"Door_West\"})", [](CallContext& c) -> Value {
            auto options = game::ChangeOptions::fromJson(c.argc() > 1 ? toJson(c.arg(1)) : Json());
            if (!options) failWith(c, options.error());
            options->immediate = false;  // scripts always change at the end of the tick
            if (Status s = flow(c).requestChange(c.string(0), *options); !s) failWith(c, s.error());
            return {};
        });
    def(reg, "load_additive", {{"scene", kTString}, {"options", kTMap, true}}, kTString,
        "Adds a scene's entities to the running one at the end of this tick (a room, a streaming chunk, a UI overlay) "
        "and returns its handle for unload_scene. Options: id (handle), parent (entity to load under), offset (vector "
        "added to its root positions). It owns exactly the entities it created.",
        "let room = load_additive(\"rooms/cellar\", {offset: (40, 0, 0)})", [](CallContext& c) -> Value {
            auto options = game::AdditiveOptions::fromJson(c.argc() > 1 ? toJson(c.arg(1)) : Json());
            if (!options) failWith(c, options.error());
            auto handle = flow(c).loadAdditive(c.string(0), *options, false);
            if (!handle) failWith(c, handle.error());
            return Value::string(*handle);
        });
    def(reg, "unload_scene", {{"handle", kTString}}, kTNone,
        "Removes a sub-scene loaded with load_additive: its entities hear `on scene_unloading` next tick and are removed "
        "the tick after. Entities the game added under it are kept (moved to the root).",
        "unload_scene(room)", [](CallContext& c) -> Value {
            if (auto r = flow(c).unload(c.string(0), false); !r) failWith(c, r.error());
            return {};
        });
    def(reg, "current_scene", {}, kTString, "The scene the game is in: its game.json alias, else its path.",
        "if current_scene() == \"menu\" then music(\"audio/menu.ogg\") end",
        [](CallContext& c) -> Value { return Value::string(flow(c).current()); });
    def(reg, "loading_progress", {}, kTNumber,
        "How far the next scene has preloaded, 0..1 (1 when nothing is loading): for a loading bar in a loading scene.",
        "find(\"Bar\").ui.value = loading_progress()",
        [](CallContext& c) -> Value { return Value::number(flow(c).progress()); });
    reg.addTrigger({"scene_loaded", "A scene finished loading: on scene_loaded — data.id (alias or path, or a sub-scene's handle), "
                                    "data.path, data.additive.",
                    "{id: string, path: string, additive: bool}", "scene"});
    reg.addTrigger({"scene_unloading", "A scene is about to go: on scene_unloading — data.id, data.to (the next scene), data.additive. "
                                       "Delivered the tick before the change, to the leaving scene too.",
                    "{id: string, to: string, additive: bool}", "scene"});
}

}  // namespace sky
