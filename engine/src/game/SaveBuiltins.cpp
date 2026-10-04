// Wander builtins for save games (docs/SAVE_GAMES.md): save_game, load_game, has_save, list_saves,
// delete_save, game_var, and the `saved` / `loaded` triggers. Saving and loading are requests that
// run at the end of the current tick, so a save never captures half a tick and a load never pulls
// the scene from under a running handler; `on saved` / `on loaded` arrive on the next tick.

#include "skywalker/engine/Engine.h"
#include "skywalker/game/SaveGame.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

game::SaveSystem& saves(CallContext& c) {
    Engine* engine = c.service<Engine>();
    if (!engine) c.fail(c.def().name + "(): save games are not available here (no running game)");
    return engine->saves();
}

const std::string& slotArg(CallContext& c, int i) {
    const std::string& slot = c.string(i);
    if (Status s = game::validateSlotName(slot); !s) c.fail(c.def().name + "(): " + s.error().message + " - " + s.error().hint);
    return slot;
}

void def(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, TypeSet ret, const char* doc, const char* example,
         BuiltinImpl impl) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = ret;
    d.category = "save";
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.owner = "engine";
    r.add(std::move(d));
}

}  // namespace

void registerSaveBuiltins(BuiltinRegistry& reg) {
    const TypeSet S = kTString, B = kTBool, M = kTMap;
    def(reg, "save_game", {{"slot", S}, {"meta", M, true}}, kTNone,
        "Saves the game into a slot at the end of this tick: every entity with a `persist` component, game_var values, "
        "the scene and the play time. `meta` is shown in save menus (title, chapter, thumbnail). `on saved` follows "
        "next tick (`on event \"save_failed\"` on error). Slots: \"slot1\", \"autosave\", \"quicksave\"...",
        "save_game(\"autosave\", {title: \"Forest gate\", chapter: 2})", [](CallContext& c) -> Value {
            const std::string& slot = slotArg(c, 0);
            Json meta = c.argc() > 1 ? toJson(c.arg(1)) : Json::object();
            saves(c).requestSave(slot, meta);
            return {};
        });
    def(reg, "load_game", {{"slot", S}}, B,
        "Loads a slot at the end of this tick: persisted entities, game vars, the exact behavior state and the scene "
        "return to the moment of the save. Returns false (and does nothing) when the slot is empty. `on loaded` "
        "follows next tick.",
        "if not load_game(\"autosave\") then log \"no save yet\" end", [](CallContext& c) -> Value {
            const std::string& slot = slotArg(c, 0);
            game::SaveSystem& s = saves(c);
            if (!s.has(slot)) return Value::boolean(false);
            s.requestLoad(slot);
            return Value::boolean(true);
        });
    def(reg, "has_save", {{"slot", S}}, B, "Whether a slot holds a save (a \"Continue\" button shows only then).",
        "find(\"Continue\").ui.visible = has_save(\"autosave\")",
        [](CallContext& c) -> Value { return Value::boolean(saves(c).has(slotArg(c, 0))); });
    def(reg, "list_saves", {}, kTList,
        "Every save, sorted by slot: maps {slot, meta, play_time, saved_at, version, scene}; damaged files have an "
        "`error` instead. For load / save menus.",
        "for s in list_saves() log s.slot + \": \" + s.meta.get(\"title\", s.slot) end", [](CallContext& c) -> Value {
            std::vector<Value> items;
            for (const auto& info : saves(c).list()) {
                Value m = Value::map();
                m.mutMap().set("slot", Value::string(info.slot));
                if (!info.error.empty()) {
                    m.mutMap().set("error", Value::string(info.error));
                } else {
                    m.mutMap().set("meta", fromJson(info.meta));
                    m.mutMap().set("play_time", Value::number(info.playTime));
                    m.mutMap().set("saved_at", Value::string(info.savedAt));
                    m.mutMap().set("version", Value::number(info.version));
                    m.mutMap().set("scene", Value::string(info.scene));
                }
                items.push_back(std::move(m));
            }
            return Value::list(std::move(items));
        });
    def(reg, "delete_save", {{"slot", S}}, B, "Deletes a slot. Returns whether there was a save to delete.",
        "delete_save(\"slot3\")", [](CallContext& c) -> Value { return Value::boolean(saves(c).remove(slotArg(c, 0)).ok()); });
    def(reg, "game_var", {{"name", S}, {"value", kTAny, true}}, kTAny,
        "Reads (or with a value, sets) a global game variable: state that belongs to the game, not to one entity "
        "(chapter, flags, unlocked levels, settings). Saved with every save; reset when play stops. Setting none removes it.",
        "game_var(\"chapter\", game_var(\"chapter\") + 1)", [](CallContext& c) -> Value {
            game::SaveSystem& s = saves(c);
            const std::string& name = c.string(0);
            if (c.argc() > 1) {
                if (Status st = s.setGlobal(name, toTaggedJson(c.arg(1))); !st) c.fail("game_var(): " + st.error().message);
                return c.arg(1);
            }
            Json all = s.globalsJson();
            const Json* v = all.find(name);
            return v ? fromTaggedJson(*v) : Value();
        });
    reg.addTrigger({"saved", "A save_game() finished: on saved — data.slot, data.meta.", "{slot: string, meta: map}", "save"});
    reg.addTrigger({"loaded", "A save was loaded (load_game or the load_game tool): on loaded — data.slot, data.version, data.meta. "
                              "Refresh what is derived from saved state (HUD, music).",
                    "{slot: string, version: number, meta: map}", "save"});
}

}  // namespace sky
