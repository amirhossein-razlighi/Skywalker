// The Wander debugger (docs/WANDER.md "Debugging"): breakpoints with conditions, hit counts and
// logpoints, stepping into and out of functions, watch expressions, setting variables, breaking on
// errors, the zero-cost idle path, pause/resume determinism, and the agent tools (with the game held
// mid-tick while tools keep working).

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Debugger.h"

using namespace sky;
using wander::StepMode;
namespace fs = std::filesystem;

namespace {

// Line numbers matter: the tests set breakpoints on them.
const char* kCalc = R"(var total = 0
var hp = 5
fn add(a, b)
  let s = a + b
  return s
end
on tick
  let i = 0
  while i < 5
    i += 1
    total = add(total, i)
  end
  hp -= 1
end)";

struct Fixture {
    std::unique_ptr<Engine> engine;
    EntityId calc = kNoEntity;
    fs::path dir;

    Fixture() {
        dir = fs::temp_directory_path() / ("skywalker-dbg-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.audio = audio::AudioMode::Null;
        cfg.projectDir = dir.string();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Debug", false);
        calc = engine->scene().create("Calculator");
        REQUIRE(engine->scene().setBehaviors(calc, Json::array({Json::object({{"name", "Calc"}, {"source", kCalc}})})));
        engine->play();
    }
    ~Fixture() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    wander::Debugger& dbg() { return engine->runtime().debugger(); }
    double var(const char* name) { return engine->scene().record(calc)->vars.get(name).asNumber(-999); }
    wander::Breakpoint bp(int line, std::string condition = {}, std::string hits = {}, std::string log = {}) {
        wander::Breakpoint b;
        b.script = "Calc";
        b.line = line;
        b.condition = std::move(condition);
        b.hitCondition = std::move(hits);
        b.log = std::move(log);
        auto r = dbg().setBreakpoint(b);
        REQUIRE(r);
        return *r;
    }
    Json call(const char* tool, const std::string& args, bool expectOk = true) {
        ToolResult r = engine->callTool(tool, Json::parse(args).value(), "agent:test");
        INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
        CHECK(r.isError == !expectOk);
        return r.structured;
    }
};

Json evalJson(wander::Debugger& d, const std::string& expr, int frame = 0) {
    auto r = d.eval(frame, expr);
    REQUIRE(r);
    return r->get("value");
}

}  // namespace

TEST_CASE("wander debugger: breakpoints stop before the line, with the stack, locals and vars") {
    Fixture f;
    auto b = f.bp(11);
    CHECK(b.verified);
    std::vector<Json> stops;
    f.dbg().onStop = [&](const Json& state) {
        stops.push_back(state);
        Json stack = f.dbg().stack();
        REQUIRE(stack.get("frames").size() == 1);
        const Json& top = stack.get("frames")[0];
        CHECK(top.get("function").asString() == "Main.on tick");
        CHECK(top.get("script").asString() == "Calc");
        CHECK(top.get("line").asInt() == 11);
        CHECK(top.get("entity").get("name").asString() == "Calculator");
        CHECK(top.get("locals")[0].get("name").asString() == "i");
        CHECK(top.get("self").get("hp").get("value").asInt() == 5);
        return StepMode::Continue;
    };
    f.engine->step(1);
    REQUIRE(stops.size() == 5);  // once per loop iteration, before `total = add(total, i)` runs
    CHECK(stops[0].get("reason").asString() == "breakpoint");
    CHECK(stops[0].get("where").get("line").asInt() == 11);
    CHECK(f.var("total") == 15);  // the tick ran to the end after each continue
    // Lines without code move to the next line that has some.
    auto moved = f.bp(6);
    CHECK(moved.line == 7);
    CHECK(moved.requestedLine == 6);
    // An unknown script name gets a did-you-mean.
    wander::Breakpoint typo;
    typo.script = "Clac";
    typo.line = 3;
    auto bad = f.dbg().setBreakpoint(typo);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().hint.find("Calc") != std::string::npos);
}

TEST_CASE("wander debugger: conditions, hit counts and logpoints") {
    Fixture f;
    f.bp(11, "i == 3");
    std::vector<double> seen;
    f.dbg().onStop = [&](const Json&) {
        seen.push_back(evalJson(f.dbg(), "i").asNumber());
        return StepMode::Continue;
    };
    f.engine->step(1);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == 3);

    f.dbg().clearBreakpoints();
    seen.clear();
    f.bp(11, "", ">=4");  // the 4th hit and after: i = 4 and 5
    f.engine->step(1);
    CHECK((seen == std::vector<double>{4, 5}));

    f.dbg().clearBreakpoints();
    seen.clear();
    f.bp(11, "", "%2");
    f.engine->step(1);
    CHECK((seen == std::vector<double>{2, 4}));

    // A logpoint logs and never stops.
    f.dbg().clearBreakpoints();
    seen.clear();
    f.bp(11, "", "", "i={i} total={total}");
    f.engine->step(1);
    CHECK(seen.empty());
    std::vector<std::string> logs;
    for (const auto& m : f.engine->recentMessages(50)) {
        if (m.get("script").asString() == "logpoint") logs.push_back(m.get("text").asString());
    }
    REQUIRE(logs.size() == 5);
    CHECK(logs[0] == "i=1 total=45");  // evaluated before the line runs, on the 4th tick
}

TEST_CASE("wander debugger: step into a function, out of it, and over the call") {
    Fixture f;
    f.bp(11, "i == 2");
    std::vector<std::pair<int, size_t>> trail;  // (line, frames)
    int steps = 0;
    f.dbg().onStop = [&](const Json& s) {
        const Json stack = f.dbg().stack(false);
        trail.emplace_back(static_cast<int>(s.get("where").get("line").asInt()), stack.get("frames").size());
        ++steps;
        if (steps == 1) return StepMode::Into;  // into add()
        if (steps == 2) {
            CHECK(stack.get("frames")[0].get("function").asString() == "Main.add");
            CHECK(evalJson(f.dbg(), "a").asNumber() == 1);  // total after i = 1
            CHECK(evalJson(f.dbg(), "b").asNumber() == 2);
            CHECK(evalJson(f.dbg(), "i", 1).asNumber() == 2);  // the caller's frame
            return StepMode::Over;   // line 4 -> line 5
        }
        if (steps == 3) return StepMode::Out;  // back in the handler
        if (steps == 4) return StepMode::Over;
        return StepMode::Continue;
    };
    f.engine->step(1);
    REQUIRE(trail.size() >= 5);
    CHECK((trail[0] == std::make_pair(11, size_t{1})));  // the breakpoint
    CHECK((trail[1] == std::make_pair(4, size_t{2})));   // into: first line of add
    CHECK((trail[2] == std::make_pair(5, size_t{2})));   // over: next line of add
    CHECK(trail[3].second == 1);                       // out: the handler again
    CHECK(trail[3].first == 9);                        // the loop condition, after the call line finished
    CHECK((trail[4] == std::make_pair(10, size_t{1})));  // over
    CHECK(f.var("total") == 15);
}

TEST_CASE("wander debugger: watch expressions are read-only; variables can be set while paused") {
    Fixture f;
    f.bp(13);
    bool checked = false;
    f.dbg().onStop = [&](const Json&) {
        checked = true;
        CHECK(evalJson(f.dbg(), "total * 2 + hp").asNumber() == 35);
        CHECK(evalJson(f.dbg(), "distance(self, self)").asNumber() == 0);
        auto bad = f.dbg().eval(0, "nope + 1");
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == "eval_error");
        // Set a local, then a var: the handler continues with them.
        REQUIRE(f.dbg().setVariable(0, "i", Json(100)));
        CHECK(evalJson(f.dbg(), "i").asNumber() == 100);
        REQUIRE(f.dbg().setVariable(0, "hp", Json(42)));
        Status unknown = f.dbg().setVariable(0, "totl", Json(1));
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().hint.find("total") != std::string::npos);
        return StepMode::Continue;
    };
    f.engine->step(1);
    CHECK(checked);
    CHECK(f.var("hp") == 41);  // 42, then `hp -= 1`
}

TEST_CASE("wander debugger: break on runtime error stops at the failing statement") {
    Fixture f;
    REQUIRE(f.engine->scene().setBehaviors(
        f.calc, Json::array({Json::object({{"name", "Bad"}, {"source", "var items: list = [1, 2]\non tick\n  let k = 7\n  let x = items[k]\nend"}})})));
    f.dbg().setBreakOnError(true);
    Json stop;
    f.dbg().onStop = [&](const Json& s) {
        stop = s;
        CHECK(evalJson(f.dbg(), "k").asNumber() == 7);
        return StepMode::Continue;
    };
    f.engine->step(1);
    REQUIRE(stop.isObject());
    CHECK(stop.get("reason").asString() == "error");
    CHECK(stop.get("where").get("line").asInt() == 4);
    CHECK(stop.get("message").asString().find("index") != std::string::npos);
    // The error is still reported as usual.
    bool logged = false;
    for (const auto& m : f.engine->recentMessages(20)) logged = logged || m.get("kind").asString() == "runtime_error";
    CHECK(logged);
}

TEST_CASE("wander debugger: costs nothing while idle") {
    Fixture f;
    CHECK_FALSE(f.dbg().active());
    f.engine->step(30);  // no debugger involvement: ticks run on the calling thread, nothing stops
    CHECK_FALSE(f.engine->debugHolding());
    auto b = f.bp(11);
    CHECK(f.dbg().active());
    f.dbg().clearBreakpoint(b.id);
    CHECK_FALSE(f.dbg().active());
    f.dbg().setBreakOnError(true);
    CHECK(f.dbg().active());
    f.dbg().setBreakOnError(false);
    CHECK_FALSE(f.dbg().active());
}

TEST_CASE("wander debugger tools: the game holds mid-tick while tools work, then replays identically") {
    Fixture a, b;
    a.call("wander_break_set", R"({"script": "Calc", "line": 13, "condition": "total % 2 == 1"})");
    a.call("sim_control", R"({"action": "step", "ticks": 4})");  // returns at the first stop, mid-tick
    CHECK(a.engine->debugHolding());
    Json state = a.call("wander_debug_state", "{}");
    CHECK(state.get("paused").asBool());
    CHECK(state.get("where").get("line").asInt() == 13);
    const uint64_t frame = a.engine->runtime().frame();
    a.engine->update(1.0);  // real time passes: the simulation does not
    CHECK(a.engine->runtime().frame() == frame);
    Json stack = a.call("wander_stack", "{}");
    CHECK(stack.get("frames").size() == 1);
    Json ev = a.call("wander_eval", R"({"expression": "total + 1"})");
    CHECK(ev.get("value").asNumber() == 16);
    // World-changing tools wait; reading tools work.
    ToolResult refused = a.engine->callTool("entity_create", Json::parse(R"({"name": "X"})").value(), "agent:test");
    REQUIRE(refused.isError);
    CHECK(refused.structured.get("error").asString() == "paused_in_debugger");
    a.call("entity_get", R"({"entity": "Calculator"})");
    // Step over, then continue until the ticks are done (every odd total stops).
    Json stepped = a.call("wander_step", R"({"mode": "over"})");
    CHECK(stepped.get("paused").asBool());
    int guard = 0;
    while (a.engine->debugHolding() && guard++ < 50) a.call("wander_continue", "{}");
    CHECK_FALSE(a.engine->debugHolding());
    CHECK(a.call("wander_debug_state", "{}").get("paused").asBool() == false);
    // The same 4 ticks without a debugger: identical.
    b.engine->step(4);
    CHECK(a.var("total") == b.var("total"));
    CHECK(a.var("hp") == b.var("hp"));
    CHECK(a.engine->runtime().frame() == b.engine->runtime().frame());
    // wander.paused / wander.resumed went to the event log (events_poll, the Python layer).
    int paused = 0, resumed = 0;
    for (const auto& e : a.engine->drainEvents()) {
        paused += e.get("type").asString() == "wander.paused";
        resumed += e.get("type").asString() == "wander.resumed";
    }
    CHECK(paused >= 2);
    CHECK(paused == resumed);
    // Clearing every breakpoint makes the debugger idle again.
    Json cleared = a.call("wander_break_clear", "{}");
    CHECK(cleared.get("remaining").asInt() == 0);
    CHECK_FALSE(a.engine->runtime().debugger().active());
}

TEST_CASE("wander debugger tools: pause, set variables, stop while paused") {
    Fixture f;
    f.engine->step(1);  // vars initialized (the first tick also runs the var initializers)
    f.call("wander_pause", "{}");
    f.call("sim_control", R"({"action": "step", "ticks": 2})");
    REQUIRE(f.engine->debugHolding());
    Json s = f.call("wander_debug_state", "{}");
    CHECK(s.get("reason").asString() == "pause");
    CHECK(s.get("where").get("line").asInt() == 8);  // the first statement of on tick
    f.call("wander_set_var", R"({"name": "total", "value": 1000})");
    f.call("wander_set_var", R"({"name": "nope", "value": 1})", false);
    f.call("wander_eval", R"({"expression": "total"})");
    // wander_step without a stop is an error with a hint.
    f.call("wander_continue", "{}");
    CHECK_FALSE(f.engine->debugHolding());
    CHECK(f.var("total") == 1000 + 15 + 15);  // set at the start of tick 2, then two loops
    f.call("wander_continue", "{}", false);
    // Stopping play while paused abandons the tick and restores the edited scene.
    f.call("wander_pause", "{}");
    f.call("sim_control", R"({"action": "step", "ticks": 1})");
    REQUIRE(f.engine->debugHolding());
    f.call("sim_control", R"({"action": "stop"})");
    CHECK_FALSE(f.engine->debugHolding());
    CHECK((f.engine->playState() == PlayState::Editing));
    CHECK_FALSE(f.engine->scene().record(f.calc)->vars.contains("total"));
    f.call("wander_break_list", "{}");
}
