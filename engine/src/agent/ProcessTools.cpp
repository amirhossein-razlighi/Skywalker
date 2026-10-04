// Process modes, game pause, time scale and render interpolation tools: process_info (what runs
// while paused, why, and how smooth frames are) and sim_teleport (move without smearing).
// docs/ARCHITECTURE.md "Game pause, process modes and time scale" and "Render interpolation".

#include <algorithm>
#include <cmath>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/scene/Process.h"

namespace sky::tools {

namespace {

using namespace schema;

std::string label(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    return r ? "#" + std::to_string(e) + " " + r->name : "#" + std::to_string(e);
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

/// Effective settings of one entity, with the reason, as an agent reads them.
Json describeProcess(Engine& engine, EntityId e) {
    const Scene& s = engine.scene();
    ResolvedProcess r = resolveProcess(s, e);
    const bool runs = processRuns(r.mode, engine.gamePaused());  // as of the next tick (requests apply then)
    std::string from = "default (no process component above it)";
    if (r.modeFrom) {
        from = label(s, r.modeFrom);
        if (!s.get<Process>(r.modeFrom) || s.get<Process>(r.modeFrom)->mode == "inherit") from += " (UI canvas default)";
        else if (r.modeFrom != e) from += " (inherited)";
    }
    const double scale = !runs ? 0.0 : r.realClock ? 1.0 : engine.timeScale();
    return Json::object({{"entity", e},
                         {"name", s.record(e) ? s.record(e)->name : std::string()},
                         {"mode", toString(r.mode)},
                         {"modeFrom", from},
                         {"clock", r.realClock ? "real" : "game"},
                         {"interpolation", r.interpolate ? "on" : "off"},
                         {"priority", r.priority},
                         {"runs", runs},
                         {"dtScale", round3(scale)}});
}

}  // namespace

void addProcessTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"process_info", "Pause, time scale and smoothing",
             "What runs while the game is paused or slowed, and how smooth real-time frames are. Reports the game "
             "clock (paused?, time scale, game vs real time), every entity with a `process` component or a UI canvas "
             "(UI runs `always` by default), and with `entity` that entity's effective settings and WHY (which ancestor "
             "decided its mode). Also render interpolation: alpha, entities smoothed last frame, `on frame` handler "
             "runs and frame pacing jitter (jitterMs vs jitterMsWithoutInterpolation). Warnings flag a paused game "
             "nothing can resume. Example: {\"entity\": \"PauseMenu\"}.",
             "sim", object({{"entity", entity("Entity to explain (effective mode, clock, interpolation, whether it runs from the next tick)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 const bool playing = engine.playState() != PlayState::Editing;
                 wander::Runtime& rt = engine.runtime();
                 Json game = Json::object({{"playState", toString(engine.playState())},
                                           {"paused", engine.gamePaused()},
                                           {"pausedThisTick", rt.gamePaused()},
                                           {"timeScale", engine.timeScale()},
                                           {"timeScaleThisTick", rt.timeScale()},
                                           {"gameTime", round3(rt.time())},
                                           {"unscaledTime", round3(rt.unscaledTime())},
                                           {"tick", rt.frame()}});
                 Json interp = Json::object({{"enabled", engine.interpolation()},
                                             {"alpha", round3(engine.interpolationAlpha())},
                                             {"historyEntities", engine.transformHistory().size()},
                                             {"interpolatedLastFrame", engine.frameFlowStats().interpolated},
                                             {"frameHandlerRunsLastFrame", engine.frameFlowStats().frameHandlerRuns},
                                             {"pacing", engine.pacing().toJson()}});
                 Json listed = Json::array();
                 size_t special = 0, keepRunning = 0, behaviorsRunning = 0, behaviorsStopped = 0;
                 const bool pausedNow = engine.gamePaused();
                 for (EntityId e : s.entities()) {
                     const bool hasProcess = s.get<Process>(e) != nullptr;
                     const bool canvas = s.get<UICanvas>(e) != nullptr;
                     ResolvedProcess r = resolveProcess(s, e);
                     const bool runsPaused = processRuns(r.mode, true);
                     if (s.get<Behavior>(e) && s.isActive(e)) {
                         if (processRuns(r.mode, pausedNow)) ++behaviorsRunning;
                         else ++behaviorsStopped;
                         if (runsPaused) ++keepRunning;
                     }
                     if (canvas && runsPaused && s.isActive(e)) ++keepRunning;
                     if (!hasProcess && !canvas) continue;
                     ++special;
                     if (listed.size() < 100) listed.push(describeProcess(engine, e));
                 }
                 Json warnings = Json::array();
                 if (engine.gamePaused() && keepRunning == 0) {
                     warnings.push("the game is paused and nothing runs while paused (no UI canvas and no behavior with process "
                                   "mode always/when_paused): nothing can call resume_game(). Add a pause menu canvas, or set "
                                   "process.mode = \"always\" on the entity that handles the pause key.");
                 }
                 if (playing && rt.timeScale() == 0.0 && !rt.gamePaused()) {
                     warnings.push("time scale is 0: the game clock is stopped (dt = 0) but nothing is paused; use pause_game "
                                   "for a pause menu");
                 }
                 Json out = Json::object({{"game", game},
                                          {"interpolation", interp},
                                          {"behaviors", Json::object({{"running", behaviorsRunning}, {"stopped", behaviorsStopped}})},
                                          {"processEntities", listed},
                                          {"processEntityCount", special},
                                          {"warnings", warnings}});
                 std::ostringstream os;
                 os << "game " << (engine.gamePaused() ? "PAUSED" : "running") << ", time scale " << engine.timeScale() << "; "
                    << behaviorsRunning << " behavior entities run, " << behaviorsStopped << " stopped; interpolation "
                    << (engine.interpolation() ? "on" : "off") << " (alpha " << round3(engine.interpolationAlpha())
                    << ", jitter " << engine.pacing().jitterMs << " ms vs " << engine.pacing().jitterMsRaw << " ms without)";
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     Json d = describeProcess(engine, *id);
                     out["entity"] = d;
                     os << "\n" << label(s, *id) << ": mode " << d.get("mode").asString() << " from "
                        << d.get("modeFrom").asString() << ", clock " << d.get("clock").asString() << ", "
                        << (d.get("runs").asBool() ? "runs" : "STOPPED");
                 }
                 for (const auto& w : warnings.elements()) os << "\nwarning: " << w.asString();
                 return ToolResult::json(out, os.str());
             }});

    reg.add({"sim_teleport", "Teleport entity",
             "Move an entity instantly — a respawn, a portal, a checkpoint — without render interpolation smearing it "
             "across the screen for a frame (Wander: teleport(entity, position)). position/rotation are local "
             "(like transform). Without position/rotation it only resets the smoothing. Undoable.",
             "sim",
             object({{"entity", entity("Entity to move")}, {"position", vec3("New position")}, {"rotation", vec3("New rotation (Euler degrees)")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Json patch = Json::object();
                 if (a.contains("position")) patch["position"] = a.get("position");
                 if (a.contains("rotation")) patch["rotation"] = a.get("rotation");
                 if (patch.size() > 0) {
                     Status st = engine.edit(ctx.actor, "Teleport " + engine.scene().record(*id)->name,
                                             [&]() -> Status { return engine.scene().patchComponent(*id, "transform", patch); });
                     if (!st) return fail(st);
                 }
                 engine.teleport(*id);
                 const Transform* t = engine.scene().get<Transform>(*id);
                 Json pos = t ? reflect::vec3ToJson(t->position) : Json();
                 return ToolResult::json(Json::object({{"entity", *id}, {"position", pos}}),
                                         "teleported " + label(engine.scene(), *id) + " (no interpolation smear this frame)");
             }});
}

}  // namespace sky::tools
