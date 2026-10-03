#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/input/ActionMap.h"

using namespace sky;
using namespace sky::input;
namespace fs = std::filesystem;

namespace {

/// Evaluates the map for one tick and ends it, like the engine's fixed step does.
const ActionState& tick(const ActionMap& map, InputState& in, const char* action) {
    map.evaluate(in);
    return in.actions.at(action);
}

struct Project {
    std::string dir;
    std::unique_ptr<Engine> engine;
    Project() {
        dir = (fs::temp_directory_path() / ("skywalker-input-" + AssetDatabase::newGuid().substr(0, 8))).string();
        fs::create_directories(dir);
        engine = open();
    }
    ~Project() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::unique_ptr<Engine> open() const {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir;
        cfg.audio = audio::AudioMode::Off;
        auto e = std::make_unique<Engine>(cfg);
        (void)e->newScene("Input", false);
        return e;
    }
};

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

}  // namespace

TEST_CASE("input: default action map parses, covers the core actions and round trips") {
    ActionMap map = ActionMap::defaults();
    for (const char* name : {"move", "look", "jump", "fire", "aim", "interact", "sprint", "pause", "cursor"}) {
        INFO(name);
        CHECK(map.find(name) != nullptr);
    }
    CHECK((map.find("move")->type == ActionType::Axis2D));
    CHECK((map.find("jump")->type == ActionType::Button));
    Json j = map.toJson();
    auto again = ActionMap::fromJson(j);
    REQUIRE(again);
    CHECK(again->toJson() == j);
    CHECK(j.get("actions").get("move").get("bindings")[0].asString() == "wasd");  // shorthand survives
}

TEST_CASE("input: key buttons report held, pressed and released on the right ticks") {
    ActionMap map = ActionMap::defaults();
    InputState in;
    CHECK_FALSE(tick(map, in, "jump").held);

    in.held.insert("space");
    in.pressed.insert("space");
    auto st = tick(map, in, "jump");
    CHECK(st.held);
    CHECK(st.pressed);
    CHECK_FALSE(st.released);
    in.endTick();

    st = tick(map, in, "jump");
    CHECK(st.held);
    CHECK_FALSE(st.pressed);  // still down, but not newly pressed
    in.endTick();

    in.held.erase("space");
    st = tick(map, in, "jump");
    CHECK_FALSE(st.held);
    CHECK(st.released);
    in.endTick();
    CHECK_FALSE(tick(map, in, "jump").released);
    in.endTick();

    // A tap that starts and ends within one tick is not lost.
    in.pressed.insert("space");
    st = tick(map, in, "jump");
    CHECK(st.pressed);
    in.endTick();
    st = tick(map, in, "jump");
    CHECK(st.released);
}

TEST_CASE("input: composites build 2D and 1D axes; opposing keys cancel and diagonals are normalized") {
    ActionMap map = ActionMap::defaults();
    InputState in;
    in.held = {"w"};
    auto st = tick(map, in, "move");
    CHECK(st.vec2);
    CHECK(st.x == doctest::Approx(0));
    CHECK(st.y == doctest::Approx(1));

    in.held = {"w", "d"};
    st = tick(map, in, "move");
    CHECK(std::sqrt(st.x * st.x + st.y * st.y) == doctest::Approx(1).epsilon(1e-4));  // not 1.41
    CHECK(st.x > 0);
    CHECK(st.y > 0);

    in.held = {"a", "d"};
    st = tick(map, in, "move");
    CHECK(st.x == doctest::Approx(0));
    CHECK_FALSE(st.held);

    in.held = {"left"};  // arrows are the second binding
    CHECK(tick(map, in, "move").x == doctest::Approx(-1));

    auto custom = ActionMap::fromJson(Json::parse(R"({"actions":{"steer":{"type":"axis","bindings":["ad",{"negative":"pad:dpadLeft","positive":"pad:dpadRight"}]}}})").value());
    REQUIRE(custom);
    InputState in2;
    in2.held = {"a"};
    st = tick(*custom, in2, "steer");
    CHECK_FALSE(st.vec2);
    CHECK(st.x == doctest::Approx(-1));
    in2.held = {};
    in2.pads[0].connected = true;
    in2.pads[0].setButton(PadButton::DpadRight, true);
    CHECK(tick(*custom, in2, "steer").x == doctest::Approx(1));
}

TEST_CASE("input: gamepad sticks use a radial deadzone, triggers act as buttons") {
    ActionMap map = ActionMap::defaults();
    InputState in;
    in.pads[0].connected = true;

    in.pads[0].lx = 0.1f;
    in.pads[0].ly = 0.05f;  // stick drift: inside the 0.15 deadzone
    auto st = tick(map, in, "move");
    CHECK(st.x == 0.f);
    CHECK(st.y == 0.f);
    CHECK_FALSE(st.held);

    in.pads[0].lx = 0.5f;
    in.pads[0].ly = 0.f;
    st = tick(map, in, "move");
    CHECK(st.x == doctest::Approx((0.5f - 0.15f) / 0.85f));  // rescaled so full deflection reaches 1

    in.pads[0].lx = 1.f;
    st = tick(map, in, "move");
    CHECK(st.x == doctest::Approx(1.f));
    in.pads[0].lx = 0.f;
    in.pads[0].ly = -1.f;
    CHECK(tick(map, in, "move").y == doctest::Approx(-1.f));  // stick down = backward

    // Triggers: analog value, "held" past the threshold.
    in.pads[0].ly = 0.f;
    in.pads[0].rt = 0.3f;
    st = tick(map, in, "fire");
    CHECK_FALSE(st.held);
    in.pads[0].rt = 0.9f;
    st = tick(map, in, "fire");
    CHECK(st.held);
    CHECK(st.x > 0.8f);

    // Face buttons; a disconnected pad contributes nothing.
    in.pads[0].setButton(PadButton::South, true);
    CHECK(tick(map, in, "jump").held);
    in.pads[0].connected = false;
    CHECK_FALSE(tick(map, in, "jump").held);
    CHECK_FALSE(tick(map, in, "fire").held);

    // The strongest of several pads wins.
    in.pads[1].connected = true;
    in.pads[1].lx = 0.6f;
    CHECK(tick(map, in, "move").x > 0.4f);

    Vec2 dz = applyRadialDeadzone({0.2f, 0.f}, 0.25f);
    CHECK(dz.x == 0.f);
    Vec2 diag = applyRadialDeadzone({0.4243f, 0.4243f}, 0.1f);  // magnitude 0.6
    CHECK(diag.x == doctest::Approx(diag.y));
    CHECK(std::sqrt(diag.x * diag.x + diag.y * diag.y) == doctest::Approx((0.6 - 0.1) / 0.9).epsilon(0.01));
    Vec2 full = applyRadialDeadzone({0.7071f, 0.7071f}, 0.1f);  // magnitude 1 stays 1
    CHECK(std::sqrt(full.x * full.x + full.y * full.y) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("input: mouse delta, buttons and position are bindable; per-tick values reset") {
    ActionMap map = ActionMap::defaults();
    InputState in;
    in.mouseDX = 30.f;
    in.mouseDY = -20.f;  // mouse moved down: look goes down (y up convention)
    auto st = tick(map, in, "look");
    CHECK(st.x == doctest::Approx(3.f));   // 0.1 per pixel
    CHECK(st.y == doctest::Approx(-2.f));
    in.endTick();
    CHECK(tick(map, in, "look").x == 0.f);  // deltas only last one tick

    in.mouseHeld.insert("left");
    in.mousePressed.insert("left");
    st = tick(map, in, "fire");
    CHECK(st.pressed);
    in.endTick();
    CHECK(tick(map, in, "fire").held);
    in.mouseHeld.clear();
    CHECK(tick(map, in, "fire").released);

    in.mouseX = 0.25f;
    in.mouseY = 0.75f;
    st = tick(map, in, "cursor");
    CHECK(st.x == doctest::Approx(0.25f));
    CHECK(st.y == doctest::Approx(0.75f));

    auto inv = ActionMap::fromJson(Json::parse(R"({"actions":{"turn":{"type":"axis2d","bindings":[{"source":"mouse:delta","scale":0.5,"invertY":true}]},"zoom":{"type":"axis","bindings":["mouse:scroll.y"]}}})").value());
    REQUIRE(inv);
    InputState in2;
    in2.mouseDX = 10.f;
    in2.mouseDY = 10.f;
    in2.scrollY = 2.f;
    inv->evaluate(in2);
    CHECK(in2.actions.at("turn").x == doctest::Approx(5.f));
    CHECK(in2.actions.at("turn").y == doctest::Approx(-5.f));
    CHECK(in2.actions.at("zoom").x == doctest::Approx(2.f));
}

TEST_CASE("input: virtual (agent) input overrides devices for a number of ticks") {
    ActionMap map = ActionMap::defaults();
    InputState in;
    in.virtualActions["jump"] = {{1.f, 0.f}, 2};
    in.virtualActions["move"] = {{0.f, 1.f}, 3};
    auto st = tick(map, in, "jump");
    CHECK(st.held);
    CHECK(st.pressed);
    CHECK(in.actions.at("move").y == doctest::Approx(1));
    in.endTick();
    CHECK(tick(map, in, "jump").held);
    in.endTick();
    st = tick(map, in, "jump");
    CHECK_FALSE(st.held);   // 2 ticks are over
    CHECK(st.released);
    CHECK(in.actions.at("move").y == doctest::Approx(1));  // 3rd tick
    in.endTick();
    map.evaluate(in);
    CHECK(in.actions.at("move").y == doctest::Approx(0));
}

TEST_CASE("input: binding errors name the problem and suggest fixes") {
    auto key = parseSource("key:spcae");
    REQUIRE_FALSE(key);
    CHECK(key.error().hint.find("space") != std::string::npos);
    auto pad = parseSource("pad:suoth");
    REQUIRE_FALSE(pad);
    CHECK(pad.error().hint.find("south") != std::string::npos);
    auto mouse = parseSource("mouse:lef");
    REQUIRE_FALSE(mouse);
    CHECK(mouse.error().hint.find("left") != std::string::npos);
    CHECK_FALSE(parseSource("space"));
    CHECK_FALSE(parseSource("joystick:a"));

    auto badType = ActionMap::fromJson(Json::parse(R"({"actions":{"x":{"type":"trigger","bindings":["key:a"]}}})").value());
    REQUIRE_FALSE(badType);
    CHECK(badType.error().message.find("button, axis or axis2d") != std::string::npos);
    auto badField = ActionMap::fromJson(Json::parse(R"({"actions":{"x":{"type":"button","binding":["key:a"]}}})").value());
    REQUIRE_FALSE(badField);
    CHECK(badField.error().hint.find("bindings") != std::string::npos);
    auto badComposite = ActionMap::fromJson(Json::parse(R"({"actions":{"x":{"type":"axis2d","bindings":["wsad"]}}})").value());
    REQUIRE_FALSE(badComposite);
    CHECK(badComposite.error().hint.find("wasd") != std::string::npos);
    CHECK_FALSE(ActionMap::fromJson(Json::parse(R"({"actions":{"bad name":{"type":"button","bindings":["key:a"]}}})").value()));
    CHECK_FALSE(ActionMap::fromJson(Json::parse(R"({"actions":{"x":{"bindings":["key:a"],"deadzone":2}}})").value()));

    // Aliases and canonical forms.
    PadButton b;
    CHECK(parsePadButton("cross", b));
    CHECK((b == PadButton::South));
    CHECK(parsePadButton("RB", b));
    CHECK((b == PadButton::RightShoulder));
    CHECK(canonicalKey("Esc") == "escape");
    CHECK(canonicalKey("Return") == "enter");
    CHECK(canonicalKey("W") == "w");
    CHECK(canonicalKey(" ") == "space");
    CHECK(isKnownKey("é"));
}

TEST_CASE("input: Wander reads actions, axes and `on action` triggers driven through sim_input") {
    Project proj;
    Engine& e = *proj.engine;
    EntityId id = e.scene().create("Player");
    call(e, "behavior_set", R"({"entity":"Player","name":"Ctl","source":"var jumps = 0\nvar fired = 0\nvar held = 0\nvar moved = 0\nvar tapped = 0\nvar dx = 0\nvar dy = 0\n\non action \"jump\"\n  jumps = jumps + 1\nend\non tick\n  if action(\"fire\") then\n    fired = fired + 1\n  end\n  if pressed(\"interact\") then\n    tapped = tapped + 1\n  end\n  let m = axis(\"move\")\n  dx = m.x\n  dy = m.y\n  if length(m) > 0.1 then\n    moved = moved + 1\n  end\nend"})");

    // Nothing pressed: nothing happens.
    call(e, "sim_control", R"({"action":"step","ticks":3})");
    auto vars = [&]() -> const Json& { return e.scene().record(id)->vars; };
    CHECK(vars().get("jumps").asInt() == 0);
    CHECK(vars().get("fired").asInt() == 0);

    // Hold fire for 5 ticks, tap jump, hold move forward for 4 ticks.
    call(e, "sim_input", R"({"actions":[{"name":"fire","ticks":5},"jump"],"axes":[{"name":"move","x":0,"y":1,"ticks":4}]})");
    call(e, "sim_control", R"({"action":"step","ticks":10})");
    CHECK(vars().get("jumps").asInt() == 1);   // a tap fires `on action` once
    CHECK(vars().get("fired").asInt() == 5);   // held for exactly 5 ticks
    CHECK(vars().get("moved").asInt() == 4);
    CHECK(vars().get("dy").asNumber() == doctest::Approx(0));  // released again

    // Real keys go through the same actions: space = jump, e = interact, WASD = move.
    call(e, "sim_input", R"({"press":["space","e"],"hold":["d"]})");
    call(e, "sim_control", R"({"action":"step","ticks":2})");
    CHECK(vars().get("jumps").asInt() == 2);
    CHECK(vars().get("tapped").asInt() == 1);
    CHECK(vars().get("dx").asNumber() == doctest::Approx(1));
    call(e, "sim_input", R"({"release":["d"]})");
    call(e, "sim_control", R"({"action":"step","ticks":1})");
    CHECK(vars().get("dx").asNumber() == doctest::Approx(0));

    // A simulated gamepad: left stick forward-right, A button.
    call(e, "sim_input", R"({"gamepad":{"leftStick":[0.6,0.8],"buttons":["south"]}})");
    call(e, "sim_control", R"({"action":"step","ticks":1})");
    CHECK(vars().get("dy").asNumber() > 0.5);
    CHECK(vars().get("dx").asNumber() > 0.3);
    CHECK(vars().get("jumps").asInt() == 3);  // south = jump
    call(e, "sim_input", R"({"gamepad":{"leftStick":[0,0],"buttons":[]}})");
    call(e, "sim_control", R"({"action":"step","ticks":1})");
    CHECK(vars().get("dy").asNumber() == doctest::Approx(0));

    // Mouse buttons feed `fire`.
    int before = static_cast<int>(vars().get("fired").asInt());
    call(e, "sim_input", R"({"mouse":{"hold":["left"]}})");
    call(e, "sim_control", R"({"action":"step","ticks":3})");
    CHECK(vars().get("fired").asInt() == before + 3);
    call(e, "sim_input", R"({"mouse":{"release":["left"]}})");
    call(e, "sim_control", R"({"action":"step","ticks":2})");
    CHECK(vars().get("fired").asInt() == before + 3);

    // Mistakes give hints, in the tool and in Wander.
    ToolResult typo = e.callTool("sim_input", Json::parse(R"({"actions":["jmup"]})").value(), "t");
    REQUIRE(typo.isError);
    CHECK(typo.content.front().text.find("jump") != std::string::npos);
    ToolResult notAxis = e.callTool("sim_input", Json::parse(R"({"axes":[{"name":"jump","x":1}]})").value(), "t");
    CHECK(notAxis.isError);
    ToolResult badPad = e.callTool("sim_input", Json::parse(R"({"gamepad":{"buttons":["suoth"]}})").value(), "t");
    CHECK(badPad.isError);
    call(e, "behavior_set", R"({"entity":"Player","name":"Typo","source":"on tick\n if action(\"jmup\") then\n  self.x = 1\n end\nend"})");
    call(e, "sim_control", R"({"action":"step","ticks":1})");
    bool hinted = false;
    for (const auto& m : e.recentMessages(20)) hinted = hinted || m.get("text").asString().find("did you mean 'jump'") != std::string::npos;
    CHECK(hinted);
}

TEST_CASE("input: Wander compiler accepts `on action` and rejects a missing name") {
    Project proj;
    Engine& e = *proj.engine;
    e.scene().create("A");
    call(e, "behavior_set", R"({"entity":"A","name":"Ok","source":"on action \"fire\"\n log \"bang\"\nend"})");
    ToolResult bad = e.callTool("behavior_set", Json::parse(R"({"entity":"A","name":"Bad","source":"on action\n log \"x\"\nend"})").value(), "t");
    CHECK(bad.isError);
    CHECK(bad.content.front().text.find("on action") != std::string::npos);
}

TEST_CASE("tools: input_map edits, persists and reloads the action map") {
    Project proj;
    Engine& e = *proj.engine;
    Json got = call(e, "input_map", R"({"catalog":true})");
    CHECK(got.get("map").get("actions").contains("move"));
    CHECK(got.get("catalog").contains("gamepad"));
    CHECK_FALSE(fs::exists(proj.dir + "/input.json"));  // untouched until edited

    Json set = call(e, "input_map", R"({"operation":"set_action","name":"dash","type":"button","bindings":["key:shift","pad:east",{"source":"mouse:middle"}],"description":"quick roll"})");
    CHECK(set.get("changed").get("set").asString() == "dash");
    REQUIRE(e.actionMap().find("dash"));
    CHECK(e.actionMap().find("dash")->bindings.size() == 3);
    CHECK(fs::exists(proj.dir + "/input.json"));

    // A new engine on the same project reads the file.
    auto reopened = proj.open();
    REQUIRE(reopened->actionMap().find("dash"));
    CHECK(reopened->actionMap().find("dash")->description == "quick roll");
    CHECK(reopened->actionMap().find("move"));  // defaults are kept in the file

    // Rebinding jump and removing actions.
    call(e, "input_map", R"({"operation":"set_action","name":"jump","type":"button","bindings":["key:j"]})");
    InputState in;
    in.held = {"space"};
    e.actionMap().evaluate(in);
    CHECK_FALSE(in.actions.at("jump").held);
    in.held = {"j"};
    e.actionMap().evaluate(in);
    CHECK(in.actions.at("jump").held);
    call(e, "input_map", R"({"operation":"remove_action","name":"dash"})");
    CHECK(e.actionMap().find("dash") == nullptr);
    ToolResult missing = e.callTool("input_map", Json::parse(R"({"operation":"remove_action","name":"dahs"})").value(), "t");
    REQUIRE(missing.isError);
    CHECK(missing.content.front().text.find("actions:") != std::string::npos);

    // Errors: bad bindings are rejected and leave the map untouched.
    ToolResult bad = e.callTool("input_map", Json::parse(R"({"operation":"set_action","name":"x","type":"button","bindings":["key:spcae"]})").value(), "t");
    REQUIRE(bad.isError);
    CHECK(bad.content.front().text.find("space") != std::string::npos);
    CHECK(e.actionMap().find("x") == nullptr);
    CHECK(e.callTool("input_map", Json::parse(R"({"operation":"set_action","name":"x","type":"button","bindings":[]})").value(), "t").isError);

    // Hand-edited files hot reload.
    {
        std::ofstream f(proj.dir + "/input.json");
        f << R"({"actions":{"only":{"type":"button","bindings":["key:o"]}}})";
    }
    e.reloadProjectSettings(true);
    CHECK(e.actionMap().actions().size() == 1);
    // A broken file keeps the previous map instead of crashing.
    {
        std::ofstream f(proj.dir + "/input.json");
        f << R"({"actions":{"only":{"type":"nonsense"}}})";
    }
    e.reloadProjectSettings(true);
    CHECK(e.actionMap().find("only") != nullptr);

    call(e, "input_map", R"({"operation":"reset"})");
    CHECK(e.actionMap().find("move") != nullptr);
    CHECK(e.actionMap().find("only") == nullptr);
}

TEST_CASE("input: stop clears input state; state is deterministic across replays") {
    Project proj;
    Engine& e = *proj.engine;
    EntityId id = e.scene().create("Walker");
    call(e, "behavior_set", R"({"entity":"Walker","name":"W","source":"on tick\n let m = axis(\"move\")\n move self by (m.x * dt * 3, 0, -m.y * dt * 3)\nend"})");
    auto run = [&] {
        call(e, "sim_control", R"({"action":"stop"})");
        call(e, "sim_input", R"({"axes":[{"name":"move","x":0.5,"y":1,"ticks":30}]})");
        call(e, "sim_control", R"({"action":"step","ticks":60})");
        return e.scene().get<Transform>(id)->position;
    };
    Vec3 a = run(), b = run();
    CHECK(a.x == doctest::Approx(b.x));
    CHECK(a.z == doctest::Approx(b.z));
    CHECK(a.z < -1.0f);  // walked forward (-Z) for half a second at ~3 m/s
    CHECK(a.x == doctest::Approx(0.75).epsilon(0.05));
    call(e, "sim_control", R"({"action":"stop"})");
    CHECK(e.input().held.empty());
    CHECK(e.input().virtualActions.empty());
}
