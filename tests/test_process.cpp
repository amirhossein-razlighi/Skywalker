// Game pause, process modes, time scale (WS3) and render interpolation, `on frame` (WS4):
// resolution through the hierarchy, systems honouring modes, deterministic pause/resume,
// in-between frames, teleports, cosmetic handlers and the agent tools.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/engine/Interpolation.h"
#include "skywalker/scene/Process.h"
#include "skywalker/wander/Compiler.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-process-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Process", false);
    return e;
}

EntityId make(Engine& e, const std::string& doc) {
    Scene& s = e.scene();
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = s.create(parsed.value().get("name").asString());
    Status st = s.applyEntityJson(id, parsed.value());
    INFO((st.ok() ? std::string() : st.error().message));
    REQUIRE(st);
    return id;
}

void behave(Engine& e, EntityId id, const std::string& source) {
    REQUIRE(e.scene().setBehaviors(id, Json::array({Json::object({{"name", "Test"}, {"source", source}})})));
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

double var(Engine& e, EntityId id, const char* name) { return e.scene().record(id)->vars.get(name).asFloat(-999.f); }
Vec3 pos(Engine& e, EntityId id) { return e.scene().get<Transform>(id)->position; }

const DrawItem* drawOf(const FrameData& f, EntityId id) {
    for (const auto& d : f.draws) {
        if (d.entity == id) return &d;
    }
    return nullptr;
}

std::string compileErrors(const std::string& source) {
    auto r = wander::compile(source, std::vector<std::string>{"transform", "mesh", "light", "body", "ui"});
    std::string codes;
    for (const auto& d : r.diagnostics) {
        if (d.severity == wander::Severity::Error) codes += (codes.empty() ? "" : ",") + d.code;
    }
    return codes;
}

}  // namespace

TEST_CASE("process: modes resolve through the hierarchy; UI canvases default to always on the real clock") {
    auto engine = makeEngine();
    Scene& s = engine->scene();
    EntityId root = make(*engine, R"({"name":"Enemies","components":{"process":{"mode":"disabled","interpolation":"off"}}})");
    EntityId child = make(*engine, R"({"name":"Grunt"})");
    REQUIRE(s.setParent(child, root));
    EntityId alone = make(*engine, R"({"name":"Alone"})");
    EntityId canvas = make(*engine, R"({"name":"Menu","components":{"ui_canvas":{}}})");
    EntityId button = make(*engine, R"({"name":"Resume","components":{"ui":{"widget":"button"}}})");
    REQUIRE(s.setParent(button, canvas));
    EntityId hud = make(*engine, R"({"name":"Hud","components":{"ui_canvas":{},"process":{"mode":"pausable"}}})");

    CHECK((resolveProcess(s, alone).mode == ProcessMode::Pausable));
    CHECK(resolveProcess(s, alone).modeFrom == kNoEntity);
    ResolvedProcess c = resolveProcess(s, child);
    CHECK((c.mode == ProcessMode::Disabled));
    CHECK(c.modeFrom == root);
    CHECK_FALSE(c.interpolate);
    CHECK((resolveProcess(s, button).mode == ProcessMode::Always));
    CHECK(resolveProcess(s, button).realClock);
    CHECK((resolveProcess(s, hud).mode == ProcessMode::Pausable));  // an explicit setting beats the canvas default
    CHECK(resolveProcess(s, hud).realClock);

    // The per-tick gate agrees with the uncached resolver (and its memo works for chains).
    ProcessGate gate;
    gate.update(s, /*paused=*/true, 0.5f);
    CHECK_FALSE(gate.uniform());
    CHECK_FALSE(gate.runs(alone));
    CHECK(gate.runs(button));
    CHECK(gate.scale(button) == doctest::Approx(1.f));  // real clock
    CHECK_FALSE(gate.runs(child));
    CHECK_FALSE(gate.runs(hud));
    gate.update(s, /*paused=*/false, 0.5f);
    CHECK(gate.runs(alone));
    CHECK(gate.scale(alone) == doctest::Approx(0.5f));
    CHECK_FALSE(gate.runs(child));  // disabled never runs
    CHECK_FALSE(gate.interpolates(child));

    // processRuns table
    CHECK(processRuns(ProcessMode::WhenPaused, true));
    CHECK_FALSE(processRuns(ProcessMode::WhenPaused, false));
    CHECK(processRuns(ProcessMode::Always, true));
    CHECK_FALSE(processRuns(ProcessMode::Disabled, false));

    // Invalid enum values are rejected with the allowed list.
    CHECK_FALSE(s.patchComponent(alone, "process", Json::parse(R"({"mode":"sometimes"})").value()));
}

TEST_CASE("process: pause_game freezes pausable entities, when_paused/always/UI keep running, on pause/resume fire") {
    auto engine = makeEngine();
    EntityId mover = make(*engine, R"({"name":"Mover","vars":{"ticks":0,"pauses":0,"resumes":0}})");
    behave(*engine, mover, R"(
var ticks = 0
var pauses = 0
var resumes = 0
on tick
  ticks += 1
  move self by (1 * dt, 0, 0)
end
on pause
  pauses += 1
end
on resume
  resumes += 1
end
)");
    EntityId menu = make(*engine, R"({"name":"MenuLogic","vars":{"ticks":0},"components":{"process":{"mode":"when_paused"}}})");
    behave(*engine, menu, "var ticks = 0\non tick\n  ticks += 1\nend\n");
    EntityId music = make(*engine, R"({"name":"Music","vars":{"ticks":0},"components":{"process":{"mode":"always"}}})");
    behave(*engine, music, "var ticks = 0\non tick\n  ticks += 1\nend\n");
    EntityId hud = make(*engine, R"({"name":"Hud","vars":{"ticks":0},"components":{"ui_canvas":{}}})");
    behave(*engine, hud, "var ticks = 0\non tick\n  ticks += 1\nend\n");
    EntityId pauser = make(*engine, R"({"name":"Pauser","components":{"process":{"mode":"always"}}})");
    behave(*engine, pauser, R"(
on event "toggle"
  if is_paused() then
    resume_game()
  else
    pause_game()
  end
end
)");

    engine->play();
    engine->step(10);
    CHECK(var(*engine, mover, "ticks") == 10);
    CHECK(var(*engine, menu, "ticks") == 0);
    CHECK(var(*engine, music, "ticks") == 10);
    const float x10 = pos(*engine, mover).x;
    CHECK(x10 == doctest::Approx(10.f / 60.f));

    engine->runtime().emit("toggle", pauser);  // delivered on tick 11, the pause applies from tick 12
    engine->step(1);
    CHECK(engine->gamePaused());
    CHECK_FALSE(engine->runtime().gamePaused());
    CHECK(var(*engine, mover, "ticks") == 11);
    engine->step(20);
    CHECK(engine->runtime().gamePaused());
    CHECK(var(*engine, mover, "ticks") == 11);  // frozen
    CHECK(pos(*engine, mover).x == doctest::Approx(11.f / 60.f));
    CHECK(var(*engine, mover, "pauses") == 1);  // a paused entity still hears `on pause`
    CHECK(var(*engine, menu, "ticks") == 20);   // when_paused runs only now
    CHECK(var(*engine, music, "ticks") == 31);
    CHECK(var(*engine, hud, "ticks") == 31);    // UI canvases run always by default
    const double frozen = engine->runtime().time();
    engine->step(5);
    CHECK(engine->runtime().time() == doctest::Approx(frozen));  // game time stands still
    CHECK(engine->runtime().unscaledTime() > frozen);

    engine->runtime().emit("toggle", pauser);
    engine->step(10);
    CHECK_FALSE(engine->runtime().gamePaused());
    CHECK(var(*engine, mover, "resumes") == 1);
    CHECK(var(*engine, mover, "ticks") == 11 + 9);
    CHECK(var(*engine, menu, "ticks") == 26);  // stopped again

    engine->stop();
    CHECK_FALSE(engine->gamePaused());
}

TEST_CASE("process: events sent to a paused entity wait until it runs again") {
    auto engine = makeEngine();
    EntityId target = make(*engine, R"({"name":"Target","vars":{"hits":0}})");
    behave(*engine, target, "var hits = 0\non event \"hit\"\n  hits += 1\nend\n");
    engine->play();
    engine->step(1);
    engine->setGamePaused(true);
    engine->step(2);
    engine->runtime().emit("hit", target);
    engine->runtime().emit("hit");  // broadcast
    engine->step(3);
    CHECK(var(*engine, target, "hits") == 0);
    engine->setGamePaused(false);
    engine->step(1);
    CHECK(var(*engine, target, "hits") == 2);
}

TEST_CASE("process: time scale halves distance; real clock ignores it; priorities order behaviors") {
    auto engine = makeEngine();
    EntityId slow = make(*engine, R"({"name":"Slow"})");
    behave(*engine, slow, "on tick\n  move self by (1 * dt, 0, 0)\nend\n");
    EntityId real = make(*engine, R"({"name":"Real","components":{"process":{"clock":"real"}}})");
    behave(*engine, real, "on tick\n  move self by (1 * dt, 0, 0)\nend\n");
    EntityId rec = make(*engine, R"({"name":"Rec","vars":{"seq":""}})");
    EntityId a = make(*engine, R"({"name":"A","components":{"process":{"priority":10}}})");
    behave(*engine, a, "on tick\n  let r = find(\"Rec\")\n  if frame == 0 then r.seq = r.seq + \"A\" end\nend\n");
    EntityId b = make(*engine, R"({"name":"B","components":{"process":{"priority":-5}}})");
    behave(*engine, b, "on tick\n  let r = find(\"Rec\")\n  if frame == 0 then r.seq = r.seq + \"B\" end\nend\n");
    (void)a;
    (void)b;

    engine->play();
    engine->setTimeScale(0.5);
    engine->step(60);
    CHECK(pos(*engine, slow).x == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(pos(*engine, real).x == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(engine->runtime().time() == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(engine->runtime().unscaledTime() == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(engine->scene().record(rec)->vars.get("seq").asString() == "BA");  // lower priority first

    // Wander's time_scale() and the clamp
    engine->setTimeScale(50.0);
    engine->step(1);
    CHECK(engine->runtime().timeScale() == doctest::Approx(10.0));
}

TEST_CASE("process: physics and particles hold while paused") {
    auto engine = makeEngine();
    EntityId ball = make(*engine, R"({"name":"Ball","components":{"transform":{"position":[0,10,0]},"mesh":{"mesh":"sphere"},
                                     "body":{},"collider":{"shape":"sphere"}}})");
    EntityId fx = make(*engine, R"({"name":"Sparks","components":{"particles":{"rate":200}}})");
    engine->play();
    engine->step(20);
    const float y = pos(*engine, ball).y;
    CHECK(y < 10.f);
    const size_t live = engine->particles().liveCount(fx);
    CHECK(live > 0);
    engine->setGamePaused(true);
    engine->step(30);
    CHECK(pos(*engine, ball).y == doctest::Approx(y));
    CHECK(engine->particles().liveCount(fx) == live);
    engine->setGamePaused(false);
    engine->step(10);
    CHECK(pos(*engine, ball).y < y);
}

TEST_CASE("process: pausing and resuming at tick N replays identically (and equals not pausing, shifted)") {
    auto run = [](int pauseAt, int pauseFor) {
        auto engine = makeEngine();
        make(*engine, R"({"name":"Ground","components":{"transform":{"scale":[40,1,40]},"mesh":{"mesh":"plane"},"collider":{}}})");
        for (int i = 0; i < 4; ++i) {
            EntityId box = make(*engine, R"({"name":"Box","components":{"transform":{"position":[0,4,0]},"mesh":{"mesh":"cube"},
                                            "body":{},"collider":{}}})");
            engine->scene().get<Transform>(box)->position = {static_cast<float>(i) * 0.3f, 3.f + static_cast<float>(i), 0.f};
            behave(*engine, box, R"(
var wander = 0
on tick
  wander += random() - 0.5
  push(self, (wander * 2, 0, 0))
end
)");
        }
        engine->play();
        int t = 0;
        auto stepTo = [&](int target) {
            if (target > t) engine->step(target - t);
            t = target;
        };
        if (pauseFor > 0) {
            stepTo(pauseAt);
            engine->setGamePaused(true);
            stepTo(pauseAt + pauseFor);
            engine->setGamePaused(false);
        }
        stepTo(180 + pauseFor);
        std::string trace;
        for (EntityId e : engine->scene().entities()) {
            if (const Transform* tr = engine->scene().get<Transform>(e)) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%.5f %.5f %.5f|", tr->position.x, tr->position.y, tr->position.z);
                trace += buf;
            }
        }
        return trace;
    };
    const std::string a = run(60, 45);
    const std::string b = run(60, 45);
    CHECK(a == b);  // the same pause replays exactly
    const std::string none = run(0, 0);
    CHECK(a == none);  // a pause is invisible to the simulation: the same world, 45 ticks later
}

TEST_CASE("interpolation: frames between ticks show in-between transforms; captures and the scene stay tick-exact") {
    auto engine = makeEngine();
    EntityId cube = make(*engine, R"({"name":"Cube","components":{"transform":{},"mesh":{"mesh":"cube"}}})");
    behave(*engine, cube, "on tick\n  move self by (6 * dt, 0, 0)\nend\n");  // 0.1 m per tick
    EntityId snap = make(*engine, R"({"name":"Snap","components":{"mesh":{"mesh":"cube"},"process":{"interpolation":"off"}}})");
    behave(*engine, snap, "on tick\n  move self by (6 * dt, 0, 0)\nend\n");
    engine->play();
    engine->advance(1.0 / 60.0 * 3 + 1.0 / 120.0);  // 3 ticks and half of the next
    CHECK(engine->interpolationAlpha() == doctest::Approx(0.5f).epsilon(0.02));
    const float tickX = pos(*engine, cube).x;
    CHECK(tickX == doctest::Approx(0.3f).epsilon(1e-3));

    CaptureOptions o;
    o.width = 64;
    o.height = 64;
    o.interpolationAlpha = 0.5f;
    FrameData f = engine->frame(o);
    const DrawItem* d = drawOf(f, cube);
    REQUIRE(d);
    CHECK(d->model.translation().x == doctest::Approx(0.25f).epsilon(1e-3));  // halfway between ticks 2 and 3
    CHECK(pos(*engine, cube).x == doctest::Approx(tickX));  // restored
    const DrawItem* ds = drawOf(f, snap);
    REQUIRE(ds);
    CHECK(ds->model.translation().x == doctest::Approx(0.3f).epsilon(1e-3));  // opted out
    CHECK(engine->frameFlowStats().interpolated == 1);

    // Default captures (alpha 1) show the tick state.
    FrameData exact = engine->frame(CaptureOptions{});
    CHECK(drawOf(exact, cube)->model.translation().x == doctest::Approx(0.3f).epsilon(1e-3));

    // Interpolation off: the in-between frame is the tick state.
    engine->setInterpolation(false);
    CHECK(drawOf(engine->frame(o), cube)->model.translation().x == doctest::Approx(0.3f).epsilon(1e-3));
    engine->setInterpolation(true);

    // Teleport: no smear until the next tick.
    engine->teleport(cube);
    CHECK(drawOf(engine->frame(o), cube)->model.translation().x == doctest::Approx(0.3f).epsilon(1e-3));
    engine->step(1);
    CHECK(drawOf(engine->frame(o), cube)->model.translation().x == doctest::Approx(0.35f).epsilon(1e-3));

    // A jump longer than 25 m in one tick is never smeared.
    Transform a, b;
    b.position = {100, 0, 0};
    CHECK(interpolateTransform(a, b, 0.5f).position.x == doctest::Approx(100.f));
    b.position = {1, 0, 0};
    CHECK(interpolateTransform(a, b, 0.5f).position.x == doctest::Approx(0.5f));
}

TEST_CASE("interpolation: a 120 Hz display moves smoothly with interpolation and stutters without") {
    auto engine = makeEngine();
    EntityId cube = make(*engine, R"({"name":"Cube","components":{"mesh":{"mesh":"cube"}}})");
    behave(*engine, cube, "on tick\n  move self by (3 * dt, 0, 0)\nend\n");
    Json on = call(*engine, "sim_trace",
                   R"({"entities":["Cube"],"properties":["transform.position"],"ticks":120,"display_hz":120,"every":4})");
    const Json& smooth = on.get("display").get("smoothness").get(std::to_string(cube) + ".transform.position");
    CHECK(smooth.get("stepJitter").asFloat(9.f) < 0.05f);
    CHECK(smooth.get("stepJitterWithoutInterpolation").asFloat() > 0.8f);  // every other frame shows the same tick
    const Json& pacing = on.get("display").get("pacing");
    CHECK(pacing.get("jitterMs").asFloat(9.f) < 0.5f);
    CHECK(pacing.get("jitterMsWithoutInterpolation").asFloat() > 3.f);
    CHECK(on.get("samples").size() > 10);
    CHECK(on.get("samples")[1].contains("alpha"));
    CHECK((engine->playState() == PlayState::Editing));  // restored

    Json off = call(*engine, "sim_trace",
                    R"({"entities":["Cube"],"properties":["transform.position"],"ticks":60,"display_hz":120,"interpolation":false})");
    CHECK(off.get("display").get("smoothness").get(std::to_string(cube) + ".transform.position").get("stepJitter").asFloat() > 0.8f);
    CHECK(engine->interpolation());  // the trace put the setting back
}

TEST_CASE("on frame: cosmetic writes show in the frame and are undone; simulation unaffected") {
    auto engine = makeEngine();
    EntityId cam = make(*engine, R"({"name":"Cam","vars":{"shake":0.5},"components":{"mesh":{"mesh":"cube"}}})");
    behave(*engine, cam, R"(
on tick
  move self by (1 * dt, 0, 0)
end
on frame
  self.position = self.position + (0, self.shake, 0)
  self.mesh.color = #ff0000
  self.mesh.roughness = 0.2
end
)");
    engine->play();
    engine->step(5);
    const Vec3 tickPos = pos(*engine, cam);
    const Vec4 color = engine->scene().get<MeshRenderer>(cam)->color;
    CaptureOptions o;
    o.width = o.height = 32;
    o.frameHandlers = true;
    o.frameDt = 1.f / 120.f;
    FrameData f = engine->frame(o);
    const DrawItem* d = drawOf(f, cam);
    REQUIRE(d);
    CHECK(d->model.translation().y == doctest::Approx(tickPos.y + 0.5f));
    CHECK(d->surface.color.x == doctest::Approx(1.f));
    CHECK(d->surface.color.y == doctest::Approx(0.f));
    CHECK(engine->frameFlowStats().frameHandlerRuns == 1);
    CHECK(pos(*engine, cam).y == doctest::Approx(tickPos.y));  // undone
    CHECK(engine->scene().get<MeshRenderer>(cam)->color.y == doctest::Approx(color.y));

    // Frames (any number) never change what the simulation computes.
    auto traceWith = [&](int framesPerTick) {
        auto e2 = makeEngine();
        EntityId c2 = make(*e2, R"({"name":"Cam","vars":{"shake":0.5},"components":{"mesh":{"mesh":"cube"}}})");
        behave(*e2, c2, "on tick\n  move self by (1 * dt, 0, 0)\nend\non frame\n  self.position = self.position + (0, self.shake, 0)\nend\n");
        e2->play();
        for (int t = 0; t < 30; ++t) {
            e2->step(1);
            for (int k = 0; k < framesPerTick; ++k) (void)e2->frame(o);
        }
        return pos(*e2, c2);
    };
    const Vec3 p0 = traceWith(0), p3 = traceWith(3);
    CHECK(p0.x == doctest::Approx(p3.x));
    CHECK(p0.y == doctest::Approx(p3.y));
}

TEST_CASE("on frame: the compiler and the runtime reject anything that changes the simulation") {
    CHECK(compileErrors("var hp = 3\non frame\n  hp = 2\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  wait 1\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  emit \"boom\"\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  let r = random()\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  self.body.mass = 2\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  self.score = 2\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  move self by (1, 0, 0)\nend\n") == "frame_not_cosmetic");
    CHECK(compileErrors("on frame\n  let d = distance(self, find(\"Player\"))\n  self.light.intensity = 2 + sin(time * 8) * d\n  "
                        "self.rotation = (0, time * 30, 0)\nend\n")
              .empty());
    // The examples in docs/WANDER.md and docs/ARCHITECTURE.md compile.
    CHECK(compileErrors(R"(behavior CameraShake
  var trauma = 0
  on event "hit"
    trauma = min(1, trauma + 0.5)
  end
  on tick
    trauma = max(0, trauma - dt * 1.5)
  end
  on frame
    let k = trauma * trauma * 0.3
    self.position = self.position + (noise(time * 40) - 0.5, noise(time * 40 + 9) - 0.5, 0) * k
  end
end
)")
              .empty());
    CHECK(compileErrors(R"(on action "pause"
  if is_paused() then resume_game() else pause_game() end
end
on pause
  find("PausePanel").ui.visible = true
end
on resume
  find("PausePanel").ui.visible = false
end
on ui "Resume"
  resume_game()
end
)")
              .empty());

    // A user fn called from on frame is checked at run time: the handler stops, the error is reported once.
    auto engine = makeEngine();
    EntityId e = make(*engine, R"({"name":"Sneaky","vars":{"n":0}})");
    behave(*engine, e, "var n = 0\nfn bump()\n  n += 1\nend\non frame\n  bump()\nend\n");
    engine->play();
    engine->step(1);
    CaptureOptions o;
    o.width = o.height = 16;
    o.frameHandlers = true;
    for (int i = 0; i < 4; ++i) (void)engine->frame(o);
    engine->step(1);
    CHECK(var(*engine, e, "n") == 0);
    int errors = 0;
    for (const auto& m : engine->recentMessages()) {
        if (m.get("text").asString().find("on frame is cosmetic") != std::string::npos) ++errors;
    }
    CHECK(errors == 1);
}

TEST_CASE("process tools: sim_control game pause and time scale, process_info, sim_teleport") {
    auto engine = makeEngine();
    EntityId mover = make(*engine, R"({"name":"Mover"})");
    behave(*engine, mover, "on tick\n  move self by (1 * dt, 0, 0)\nend\n");

    Json st = call(*engine, "sim_control", R"({"action":"pause_game"})");
    CHECK(st.get("state").asString() == "playing");
    CHECK(st.get("game").get("paused").asBool());
    Json info = call(*engine, "process_info", R"({"entity":"Mover"})");
    CHECK(info.get("entity").get("mode").asString() == "pausable");
    CHECK_FALSE(info.get("entity").get("runs").asBool());  // from the next tick on
    CHECK(info.get("warnings").size() == 1);  // nothing can resume

    call(*engine, "sim_control", R"({"action":"step","ticks":30})");
    CHECK(pos(*engine, mover).x == doctest::Approx(0.f));
    call(*engine, "sim_control", R"({"action":"resume_game"})");
    call(*engine, "sim_control", R"({"action":"time_scale","scale":2})");
    call(*engine, "sim_control", R"({"action":"step","ticks":30})");
    CHECK(pos(*engine, mover).x == doctest::Approx(1.f).epsilon(1e-3));
    call(*engine, "sim_control", R"({"action":"time_scale"})", false);  // needs scale

    Json tp = call(*engine, "sim_teleport", R"({"entity":"Mover","position":[5,0,0]})");
    CHECK(tp.get("position")[0].asFloat() == doctest::Approx(5.f));
    CHECK(engine->transformHistory().previous(mover) == nullptr);

    Json perf = call(*engine, "perf_stats", "{}");
    CHECK(perf.get("frameFlow").get("interpolation").asBool());
    call(*engine, "sim_control", R"({"action":"stop"})");
    CHECK(pos(*engine, mover).x == doctest::Approx(0.f));
}

TEST_CASE("process: a pause menu button works while the world is frozen; a pausable HUD takes no clicks") {
    auto engine = makeEngine();
    EntityId mover = make(*engine, R"({"name":"Mover"})");
    behave(*engine, mover, "on tick\n  move self by (1 * dt, 0, 0)\nend\n");
    EntityId menu = make(*engine, R"({"name":"PauseMenu","components":{"ui_canvas":{}}})");
    EntityId resume = make(*engine, R"({"name":"Resume","components":{"ui":{"widget":"button","position":[0,0],"size":[400,200]}}})");
    REQUIRE(engine->scene().setParent(resume, menu));
    behave(*engine, menu, "on ui \"Resume\"\n  resume_game()\nend\n");
    EntityId hud = make(*engine, R"({"name":"Hud","components":{"ui_canvas":{},"process":{"mode":"pausable"}}})");
    EntityId fire = make(*engine, R"({"name":"Fire","components":{"ui":{"widget":"button","position":[0,600],"size":[400,200]}}})");
    REQUIRE(engine->scene().setParent(fire, hud));
    EntityId counter = make(*engine, R"({"name":"Counter","components":{"process":{"mode":"always"}}})");
    behave(*engine, counter, "var fires = 0\non ui \"Fire\"\n  fires += 1\nend\n");

    auto click = [&](double x, double y) {
        call(*engine, "sim_input", "{\"mouse\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"hold\":[\"left\"]}}");
        engine->step(1);
        call(*engine, "sim_input", R"({"mouse":{"release":["left"]}})");
        engine->step(2);
    };
    engine->play();
    engine->step(5);
    click(0.1, 0.65);  // Fire, while playing
    CHECK(var(*engine, counter, "fires") == 1);
    engine->setGamePaused(true);
    engine->step(2);
    const float frozenX = pos(*engine, mover).x;
    click(0.1, 0.65);  // the HUD is paused: no click
    CHECK(var(*engine, counter, "fires") == 1);
    CHECK(engine->runtime().gamePaused());
    click(0.1, 0.1);  // Resume on the pause menu (always): works
    CHECK_FALSE(engine->runtime().gamePaused());
    CHECK(pos(*engine, mover).x > frozenX);
}
