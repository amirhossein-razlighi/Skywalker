// Wander builtins of the 2D/UI/dialogue subsystem (World2D): sprite animation, tiles and dialogue.
// Registered once in the global builtin registry (EngineBuiltins.cpp); implementations reach the
// engine through `c.service<Engine>()` and delegate to World2D::callBuiltin.

#include "skywalker/engine/Engine.h"
#include "skywalker/ui/World2D.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

Value call2D(CallContext& c, const char* fn) {
    Engine* engine = c.service<Engine>();
    if (!engine) c.fail(std::string(fn) + "() is not available in this context");
    std::vector<Json> args;
    args.reserve(static_cast<size_t>(c.argc()));
    for (int i = 0; i < c.argc(); ++i) args.push_back(toJson(c.arg(i)));
    Result<Json> r = engine->world2d().callBuiltin(c.scene(), c.runtime(), fn, args, c.self());
    if (!r) c.fail(std::string(fn) + "(): " + r.error().message + (r.error().hint.empty() ? "" : " (" + r.error().hint + ")"));
    c.scene().markDirty();
    return fromJson(r.value());
}

}  // namespace

void registerUiBuiltins(BuiltinRegistry& reg) {
    auto def = [&](const char* name, const char* category, std::vector<BuiltinParam> params, TypeSet returns, const char* doc,
                   const char* example, BuiltinImpl fn) {
        BuiltinDef d;
        d.name = name;
        d.params = std::move(params);
        d.returns = returns;
        d.category = category;
        d.doc = doc;
        d.example = example;
        d.owner = "engine";
        d.fn = fn;
        reg.add(std::move(d));
    };
    def("play_anim", "2d", {{"entity", kTEntity}, {"clip", kTString}, {"restart", kTBool, true}}, kTNone,
        "Plays a sprite_anim clip (keeps playing if it already is, unless restart). Frame events arrive as `on anim \"name\"`; "
        "a non-looping clip sends `on anim \"finished\"`.",
        "play_anim(self, \"run\")", [](CallContext& c) { return call2D(c, "play_anim"); });
    def("tile_at", "2d", {{"map", kTEntity}, {"position", kTVec | kTEntity}, {"layer", kTString, true}}, kTNumber,
        "Tile id of a tilemap at a world position (topmost non-empty layer, or the named layer); 0 = empty.",
        "if tile_at(find(\"Farm\"), self.position) == 3 then", [](CallContext& c) { return call2D(c, "tile_at"); });
    def("set_tile", "2d", {{"map", kTEntity}, {"position", kTVec | kTEntity}, {"tile", kTNumber | kTString}, {"layer", kTString, true}},
        kTNone, "Sets the tile at a world position: an id (0 clears) or a terrain from tilemap.autotile (auto-tiled).",
        "set_tile(find(\"Farm\"), self.position, \"soil\")", [](CallContext& c) { return call2D(c, "set_tile"); });
    def("start_dialogue", "dialogue", {{"entity_or_node", kTEntity | kTString}, {"node", kTString, true}}, kTNone,
        "Starts a conversation at a node: start_dialogue(\"Intro\") on the first dialogue component, or "
        "start_dialogue(entity, \"Intro\").",
        "start_dialogue(\"Intro\")", [](CallContext& c) { return call2D(c, "start_dialogue"); });
    def("dialogue_var", "dialogue", {{"name", kTString}, {"value", kTAny, true}}, kTAny,
        "Reads (or with a value, sets) a dialogue variable ($trust = \"trust\") of the first dialogue component.",
        "if dialogue_var(\"trust\") > 2 then", [](CallContext& c) { return call2D(c, "dialogue_var"); });
    def("dialogue_choose", "dialogue", {{"index", kTNumber}}, kTNone, "Picks one of the current dialogue choices (0-based).",
        "dialogue_choose(0)", [](CallContext& c) { return call2D(c, "dialogue_choose"); });
    def("dialogue_advance", "dialogue", {}, kTNone, "Shows the rest of the typing line, or moves to the next line.",
        "dialogue_advance()", [](CallContext& c) { return call2D(c, "dialogue_advance"); });
    reg.addTrigger({"ui", "`on ui \"Name\"`: the button/toggle/slider/input named Name was used (event \"ui:Name\").", "", "ui"});
    reg.addTrigger({"dialogue",
                    "`on dialogue \"start\" | \"line\" | \"choice\" | \"end\" | \"<command>\"`: conversation events and "
                    "<<command>>s of .dialogue scripts (event \"dialogue:...\").",
                    "", "dialogue"});
}

}  // namespace sky
