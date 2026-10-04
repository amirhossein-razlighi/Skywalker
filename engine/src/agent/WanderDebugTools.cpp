// Wander debugger tools (docs/WANDER.md "Debugging"): wander_break_set / _clear / _list,
// wander_debug_state, wander_step, wander_continue, wander_pause, wander_stack, wander_eval,
// wander_set_var. While the game is paused at a breakpoint the simulation is frozen mid-tick and these
// tools run on the main thread; tools that change the world wait (paused_in_debugger).

#include <algorithm>
#include <chrono>

#include "ToolHelpers.h"
#include "skywalker/wander/Debugger.h"

namespace sky::tools {

namespace {

using namespace schema;

Json stateWithHint(Engine& engine) {
    wander::Debugger& d = engine.runtime().debugger();
    Json s = d.state();
    s["warnings"] = Json::array();
    if (!s.get("paused").asBool() && engine.playState() == PlayState::Editing) {
        s["warnings"].push("not playing: breakpoints stop once behaviors run (sim_control play / step)");
    }
    return s;
}

std::string where(const Json& s) {
    if (!s.get("paused").asBool()) return "running";
    const Json& w = s.get("where");
    return "paused (" + s.get("reason").asString() + ") at " + w.get("script").asString() + ":" + std::to_string(w.get("line").asInt()) +
           " in " + w.get("function").asString();
}

/// A tool result with the debugger state; with wait_ms (agent connections) it first waits for the next stop.
ToolResult stateAfter(Engine& engine, const Json& args, uint64_t stopsBefore) {
    const int64_t ms = std::clamp<int64_t>(args.get("wait_ms").asInt(0), 0, 600000);
    wander::Debugger& d = engine.runtime().debugger();
    auto finish = [&engine] {
        Json s = stateWithHint(engine);
        return ToolResult::json(s, where(s));
    };
    if (ms <= 0 || d.paused()) return finish();
    // Waiting happens on the agent's connection thread while the main loop runs the game; a call made on the
    // main thread itself (in-process, headless stdio) cannot wait for ticks that only it could run.
    return ToolResult::defer(
        [&engine, &d, stopsBefore, ms] {
            if (!engine.onMainThread()) d.waitForStop(stopsBefore, std::chrono::milliseconds(ms));
        },
        finish);
}

}  // namespace

void addWanderDebugTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"wander_break_set", "Set a Wander breakpoint",
             "Stops the running game at a line of a behavior script so you can inspect it (wander_stack, wander_eval). "
             "`script` is the behavior's script name as in behavior_set (or a used module path, \"scripts/ai.wander\"); "
             "omit it to stop in any script at that line. The breakpoint moves to the first line with code. Options: "
             "condition (a Wander expression over the frame's locals and vars: \"hp < 3\"), hit_count (\"5\" = the 5th hit, "
             "\">=5\", \"%10\"), log (a logpoint: logs \"hp={hp}\" instead of stopping), entity (only that entity). "
             "on_error: true stops at any runtime error instead (line not needed). Costs nothing until set. Example: "
             "{\"script\": \"Guard\", \"line\": 12, \"condition\": \"hp <= 0\"}.",
             "wander",
             object({{"script", string("Behavior script name (behavior_set name) or module path; omit for any script")},
                     {"line", integer("1-based line")},
                     {"condition", string("Stop only when this Wander expression is true")},
                     {"hit_count", string("\"N\" (the Nth hit), \">=N\", \"%N\"")},
                     {"log", string("Logpoint message with {expressions}; never stops")},
                     {"entity", entity("Only stop for this entity")},
                     {"on_error", boolean("Stop at runtime errors (true) or not (false)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 if (const Json* e = a.find("on_error")) {
                     d.setBreakOnError(e->asBool());
                     if (!a.contains("line")) {
                         return ToolResult::json(Json::object({{"breakOnError", d.breakOnError()}, {"warnings", Json::array()}}),
                                                 std::string("break on error ") + (d.breakOnError() ? "on" : "off"));
                     }
                 }
                 if (!a.contains("line")) {
                     return ToolResult::error(Error::make("invalid_arguments", "give a line (and usually a script), or on_error",
                                                          "e.g. {\"script\": \"Guard\", \"line\": 12}"));
                 }
                 wander::Breakpoint bp;
                 bp.script = a.get("script").asString();
                 bp.line = static_cast<int>(a.get("line").asInt());
                 bp.condition = a.get("condition").asString();
                 bp.hitCondition = a.get("hit_count").isNumber() ? std::to_string(a.get("hit_count").asInt()) : a.get("hit_count").asString();
                 bp.log = a.get("log").asString();
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     bp.entity = *id;
                 }
                 auto set = d.setBreakpoint(bp);
                 if (!set) return ToolResult::error(set.error());
                 Json out = set->toJson();
                 out["warnings"] = Json::array();
                 if (!set->verified) out["warnings"].push(set->message);
                 if (set->line != set->requestedLine) {
                     out["warnings"].push("line " + std::to_string(set->requestedLine) + " has no code: moved to line " + std::to_string(set->line));
                 }
                 return ToolResult::json(out, std::string(bp.log.empty() ? "breakpoint" : "logpoint") + " #" + std::to_string(set->id) + " at " +
                                                  (set->script.empty() ? std::string("any script") : set->script) + ":" + std::to_string(set->line));
             }});

    reg.add({"wander_break_clear", "Clear Wander breakpoints",
             "Removes breakpoints: one by id, every breakpoint of a script, or all (no arguments). on_error: false also stops "
             "breaking on errors. With no breakpoints left the debugger costs nothing again. Example: {\"id\": 2}.",
             "wander",
             object({{"id", integer("Breakpoint id (wander_break_list)")},
                     {"script", string("Every breakpoint of this script")},
                     {"on_error", boolean("false: stop breaking on runtime errors")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 size_t removed = 0;
                 if (a.contains("id")) {
                     if (!d.clearBreakpoint(static_cast<int>(a.get("id").asInt()))) {
                         return ToolResult::error(Error::make("not_found", "no breakpoint #" + std::to_string(a.get("id").asInt()),
                                                              "wander_break_list shows them"));
                     }
                     removed = 1;
                 } else if (!a.contains("on_error") || a.contains("script")) {
                     removed = d.clearBreakpoints(a.get("script").asString());
                 }
                 if (a.contains("on_error")) d.setBreakOnError(a.get("on_error").asBool());
                 return ToolResult::json(Json::object({{"removed", removed},
                                                       {"remaining", d.breakpoints().size()},
                                                       {"breakOnError", d.breakOnError()},
                                                       {"warnings", Json::array()}}),
                                         "removed " + std::to_string(removed) + " breakpoint(s)");
             }});

    reg.add({"wander_break_list", "List Wander breakpoints",
             "Breakpoints and logpoints with their script, line (and the line asked for when it moved), condition, hit "
             "condition, hits so far, whether a compiled script has code there, plus break-on-error.",
             "wander", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 Json list = Json::array();
                 for (const auto& b : d.breakpoints()) list.push(b.toJson());
                 return ToolResult::json(Json::object({{"breakpoints", list}, {"breakOnError", d.breakOnError()}, {"warnings", Json::array()}}),
                                         std::to_string(list.size()) + " breakpoint(s)");
             }});

    reg.add({"wander_debug_state", "Wander debugger state",
             "Whether the game is paused in the Wander debugger, why (breakpoint, step, pause, error with its message) and "
             "where (script, line, function, entity, state), plus the breakpoints. wait_ms waits for the next stop when "
             "running (agent connections; the game must be playing). Example: {\"wait_ms\": 5000}.",
             "wander", object({{"wait_ms", integer("Wait up to this long for the next stop (0 = answer now)")}}), false, false,
             [&engine](const Json& a, ToolContext&) { return stateAfter(engine, a, engine.runtime().debugger().stopCount()); }});

    reg.add({"wander_continue", "Continue (Wander debugger)",
             "Resumes a game paused in the Wander debugger. It runs until the next stop or the end of the ticks in progress, "
             "then answers with the debugger state; wait_ms keeps waiting for the next stop on agent connections. "
             "Example: {\"wait_ms\": 2000}.",
             "wander", object({{"wait_ms", integer("Then wait up to this long for the next stop")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 const uint64_t before = d.stopCount();
                 if (Status s = d.resume(wander::StepMode::Continue); !s) return fail(s);
                 return stateAfter(engine, a, before);
             }});

    reg.add({"wander_step", "Step (Wander debugger)",
             "Steps a paused behavior: over (the next statement of this function; calls run through), into (the next "
             "statement anywhere, entering called functions), out (back in the caller). When the handler ends, the step "
             "stops at the next statement any behavior runs. Answers with where it stopped. Example: {\"mode\": \"into\"}.",
             "wander",
             object({{"mode", enumeration({"over", "into", "out"}, "over (default), into or out")},
                     {"wait_ms", integer("Wait up to this long for the stop")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 const std::string m = a.get("mode").asString("over");
                 const wander::StepMode mode = m == "into" ? wander::StepMode::Into : m == "out" ? wander::StepMode::Out : wander::StepMode::Over;
                 const uint64_t before = d.stopCount();
                 if (Status s = d.resume(mode); !s) return fail(s);
                 return stateAfter(engine, a, before);
             }});

    reg.add({"wander_pause", "Pause behaviors (Wander debugger)",
             "Stops at the next statement any behavior runs (the game is frozen mid-tick there; resume with wander_continue). "
             "Unlike sim_control pause it stops inside the code, so wander_stack shows where each behavior is. wait_ms waits for "
             "the stop on agent connections. Example: {\"wait_ms\": 1000}.",
             "wander", object({{"wait_ms", integer("Wait up to this long for the stop")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 const uint64_t before = d.stopCount();
                 d.requestPause();
                 return stateAfter(engine, a, before);
             }});

    reg.add({"wander_stack", "Wander call stack",
             "The paused behavior's call stack, innermost first: function, script, line and column, entity, behavior and "
             "state, each frame's arguments and locals (value and display text), the entity's vars, and globals (time, frame, "
             "dt). Example: {\"vars\": true}.",
             "wander", object({{"vars", boolean("Include args, locals and vars (default true)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 if (!d.paused()) return ToolResult::error(Error::make("not_paused", "the Wander debugger is not paused", "wander_break_set or wander_pause first"));
                 Json out = d.stack(a.get("vars").asBool(true));
                 out["warnings"] = Json::array();
                 return ToolResult::json(out, std::to_string(out.get("frames").size()) + " frame(s)");
             }});

    reg.add({"wander_eval", "Evaluate in a paused frame",
             "Evaluates a Wander expression in a paused frame (0 = innermost): its locals, arguments, the behavior's vars, "
             "`self` and every read-only builtin are available (\"distance(self, target)\", \"items.length\"). Watch "
             "expressions: call it at each stop. Read-only: assignments to the scene fail (use wander_set_var). Example: "
             "{\"expression\": \"hp * 2 + bonus\", \"frame\": 0}.",
             "wander",
             object({{"expression", string("Wander expression")}, {"frame", integer("Frame index (0 = innermost)")}}, {"expression"}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 auto r = engine.runtime().debugger().eval(static_cast<int>(a.get("frame").asInt(0)), a.get("expression").asString());
                 if (!r) return ToolResult::error(r.error());
                 Json out = *r;
                 out["warnings"] = Json::array();
                 return ToolResult::json(out, a.get("expression").asString() + " = " + out.get("display").asString());
             }});

    reg.add({"wander_set_var", "Set a variable while paused",
             "Changes a local, an argument or one of the entity's vars in a paused frame, then the behavior continues with it "
             "(test a fix without restarting). The value is JSON ([x, y, z] is a vector). Example: {\"name\": \"hp\", "
             "\"value\": 10}.",
             "wander",
             object({{"name", string("Variable name")}, {"value", any("New value (JSON)")}, {"frame", integer("Frame index (0 = innermost)")}},
                    {"name", "value"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 wander::Debugger& d = engine.runtime().debugger();
                 const int frame = static_cast<int>(a.get("frame").asInt(0));
                 if (Status s = d.setVariable(frame, a.get("name").asString(), a.get("value")); !s) return fail(s);
                 auto now = d.eval(frame, a.get("name").asString());
                 Json out = Json::object({{"name", a.get("name")}, {"value", now ? now->get("value") : a.get("value")}, {"warnings", Json::array()}});
                 return ToolResult::json(out, a.get("name").asString() + " set");
             }});
}

}  // namespace sky::tools
