// Save game tools (docs/SAVE_GAMES.md): save_game, load_game, save_list, save_inspect, save_delete.
// They work on the running game (play mode), between ticks, like Wander's save_game / load_game.

#include <algorithm>

#include "ToolHelpers.h"
#include "skywalker/game/SaveGame.h"

namespace sky::tools {

namespace {

using namespace schema;

Json warningsJson(const std::vector<std::string>& w) {
    Json a = Json::array();
    for (const auto& s : w) a.push(s);
    return a;
}

}  // namespace

void addSaveTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"save_game", "Save the game",
             "Saves the running game into a slot, exactly as Wander's save_game() would at a tick boundary: every entity "
             "with a `persist` component (whole state or chosen fields, Wander vars, spawned subtrees), tombstones for "
             "persisted scene entities that were destroyed, game_var values, the scene, the play time and the exact "
             "behavior state. Use it to make checkpoints while playtesting, then load_game to jump back. Needs play mode "
             "(sim_control play). Slots: 1-64 of [a-z0-9_-]; \"autosave\" and \"quicksave\" do not count toward game.json "
             "saves.maxSlots. Example: {\"slot\": \"checkpoint-1\", \"meta\": {\"title\": \"Before the boss\", \"chapter\": 3}}.",
             "sim",
             object({{"slot", string("Slot name, e.g. \"slot1\", \"autosave\", \"checkpoint-2\"")},
                     {"meta", Json::object({{"type", "object"},
                                            {"description", "Shown in save menus: title, chapter, thumbnail (project-relative "
                                                            "image path), anything else the game wants"}})}},
                    {"slot"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto r = engine.saves().save(a.get("slot").asString(), a.get("meta").isNull() ? Json::object() : a.get("meta"), ctx.actor);
                 if (!r) return ToolResult::error(r.error());
                 Json out = r->toJson();
                 return ToolResult::json(out, "saved slot '" + a.get("slot").asString() + "' (" + std::to_string(r->entities) +
                                                  " persisted entities, " + std::to_string(r->info.bytes) + " bytes)");
             }});

    reg.add({"load_game", "Load a save",
             "Restores a slot into the running game: persisted entities return to their saved state (spawned ones are "
             "recreated with their ids, ones destroyed before the save are removed again), game vars, the scene (a save "
             "from another scene is an immediate scene flow change: persistent entities come along) and the "
             "exact behavior state (state machines, timers, waiting handlers, random generator), then `on loaded` runs. "
             "Older saves are migrated (game.json saves.version / saves.migrate). Stopping play still returns to the "
             "edited scene. After loading, save_inspect on the same slot lists anything that did not restore. Example: "
             "{\"slot\": \"checkpoint-1\"}.",
             "sim", object({{"slot", string("Slot to load (save_list shows them)")}}, {"slot"}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto r = engine.saves().load(a.get("slot").asString(), ctx.actor);
                 if (!r) return ToolResult::error(r.error());
                 Json out = r->toJson();
                 std::string summary = "loaded slot '" + a.get("slot").asString() + "' (" + std::to_string(r->entities) + " entities";
                 if (r->spawned) summary += ", " + std::to_string(r->spawned) + " recreated";
                 if (r->destroyed) summary += ", " + std::to_string(r->destroyed) + " removed";
                 summary += ")";
                 if (!r->warnings.empty()) summary += " with " + std::to_string(r->warnings.size()) + " warning(s)";
                 return ToolResult::json(out, summary);
             }});

    reg.add({"save_list", "List save slots",
             "Lists the save slots of this project: slot, metadata (title, chapter...), file size, format and game "
             "version, play time, scene, tick and when it was saved. Damaged files are listed with an `error`. Also "
             "returns the save folder and game.json `saves` settings. Works while editing too.",
             "sim", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 game::SaveSystem& s = engine.saves();
                 Json slots = Json::array();
                 std::vector<std::string> warnings;
                 for (const auto& info : s.list()) {
                     if (!info.error.empty()) warnings.push_back("slot '" + info.slot + "' is damaged: " + info.error);
                     slots.push(info.toJson());
                 }
                 Json out = Json::object({{"folder", s.directory()},
                                          {"settings", s.settings().toJson()},
                                          {"slots", slots},
                                          {"warnings", warningsJson(warnings)}});
                 return ToolResult::json(out, std::to_string(slots.size()) + " save(s) in " + s.directory());
             }});

    reg.add({"save_inspect", "Compare a save with the game",
             "Diffs a save slot against the running game to debug \"what didn't restore\": per persisted entity, the "
             "fields whose current value differs from the saved one (path, saved, current), entities missing from the "
             "scene, entities the save destroyed or never had, game var differences, the scene and the tick. Right after "
             "load_game everything should read `same`; anything else is what the load could not bring back. Example: "
             "{\"slot\": \"autosave\", \"max_diffs\": 50}.",
             "sim",
             object({{"slot", string("Slot to compare")},
                     {"max_diffs", integer("Most field differences to list (default 200)")}},
                    {"slot"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 const int64_t max = a.get("max_diffs").asInt(200);
                 auto r = engine.saves().inspect(a.get("slot").asString(), static_cast<size_t>(std::clamp<int64_t>(max, 1, 5000)));
                 if (!r) return ToolResult::error(r.error());
                 const Json& sum = r->get("summary");
                 return ToolResult::json(*r, "slot '" + a.get("slot").asString() + "': " + std::to_string(sum.get("same").asInt()) +
                                                 " same, " + std::to_string(sum.get("differs").asInt()) + " differ, " +
                                                 std::to_string(sum.get("missing").asInt()) + " missing");
             }});

    reg.add({"save_delete", "Delete a save slot",
             "Deletes a save slot's file. Cannot be undone. Example: {\"slot\": \"slot3\"}.", "sim",
             object({{"slot", string("Slot to delete")}}, {"slot"}), true, true, [&engine](const Json& a, ToolContext&) {
                 const std::string slot = a.get("slot").asString();
                 if (Status s = engine.saves().remove(slot); !s) return fail(s);
                 return ToolResult::json(Json::object({{"slot", slot}, {"deleted", true}, {"warnings", Json::array()}}),
                                         "deleted slot '" + slot + "'");
             }});
}

}  // namespace sky::tools
