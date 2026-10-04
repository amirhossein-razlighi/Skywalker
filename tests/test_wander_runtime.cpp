#include <doctest/doctest.h>

#include "skywalker/wander/Runtime.h"

using namespace sky;
using namespace sky::wander;

namespace {

EntityId withScript(Scene& s, const std::string& name, const std::string& source) {
    EntityId e = s.create(name);
    (void)s.patchComponent(e, "mesh", Json::object());
    (void)s.setBehaviors(e, Json::array({Json::object({{"name", name + "Script"}, {"source", source}})}));
    return e;
}

void run(Runtime& rt, int ticks, const InputState& in = {}) {
    for (int i = 0; i < ticks; ++i) rt.tick(1.f / 60.f, in);
}

}  // namespace

TEST_CASE("runtime: tick moves and rotates deterministically") {
    Scene s;
    EntityId e = withScript(s, "Mover", "on tick\n move self by (1 * dt, 0, 0)\n rotate self by (0, 90 * dt, 0)\nend");
    Runtime rt(s);
    run(rt, 60);
    CHECK(s.get<Transform>(e)->position.x == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(s.get<Transform>(e)->rotation.y == doctest::Approx(90.0).epsilon(1e-3));
    CHECK(rt.drainMessages().empty());
}

TEST_CASE("runtime: vars, start, every and after") {
    Scene s;
    EntityId e = withScript(s, "Counter", R"(
behavior Counter
  var count = 0
  var once = false
  on start
    log "hello"
  end
  on tick
    every 0.5 seconds
      count = count + 1
    end
    after 1 seconds
      once = true
    end
  end
end)");
    Runtime rt(s);
    run(rt, 120);  // 2 seconds
    CHECK(s.record(e)->vars["count"].asInt() == 4);
    CHECK(s.record(e)->vars["once"].asBool());
    auto msgs = rt.drainMessages();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].text == "hello");
}

TEST_CASE("runtime: events are delivered next tick, broadcast and targeted") {
    Scene s;
    withScript(s, "Sender", "on start\n emit \"ping\"\n emit \"direct\" to find(\"B\")\nend");
    EntityId a = withScript(s, "A", "on event \"ping\"\n self.got = true\nend\non event \"direct\"\n self.direct = true\nend");
    EntityId b = withScript(s, "B", "on event \"direct\"\n self.direct = true\nend");
    Runtime rt(s);
    run(rt, 1);
    CHECK_FALSE(s.record(a)->vars.contains("got"));
    run(rt, 1);
    CHECK(s.record(a)->vars["got"].asBool());
    CHECK_FALSE(s.record(a)->vars.contains("direct"));
    CHECK(s.record(b)->vars["direct"].asBool());
}

TEST_CASE("runtime: same seed => identical results (determinism)") {
    auto simulate = [] {
        Scene s;
        s.seed = 42;
        EntityId e = withScript(s, "R", "on tick\n move self by (random(-1, 1), random(), 0)\nend");
        Runtime rt(s);
        run(rt, 100);
        return s.get<Transform>(e)->position;
    };
    CHECK(simulate() == simulate());
}

TEST_CASE("runtime: properties, components, colors and look") {
    Scene s;
    EntityId target = s.create("Target");
    s.get<Transform>(target)->position = {10, 0, 0};
    EntityId e = withScript(s, "Looker", R"(
on start
  self.color = #ff0000
  self.mesh.roughness = 0.25
  self.position.y = 3
  look self at find("Target")
  self.target = find("Target")
  self.target.scale = (2, 2, 2)
end)");
    Runtime rt(s);
    run(rt, 1);
    CHECK(s.get<MeshRenderer>(e)->color.x == doctest::Approx(1));
    CHECK(s.get<MeshRenderer>(e)->roughness == doctest::Approx(0.25));
    CHECK(s.get<Transform>(e)->position.y == doctest::Approx(3));
    CHECK(s.get<Transform>(target)->scale == Vec3{2, 2, 2});
    // facing +X means yaw -90 degrees (forward is -Z)
    CHECK(s.get<Transform>(e)->rotation.y == doctest::Approx(-90).epsilon(1e-3));
    CHECK(rt.drainMessages().empty());
}

TEST_CASE("runtime: spawn and destroy") {
    Scene s;
    withScript(s, "Spawner", "on start\n repeat 3 times\n  spawn(\"sphere\", (0, 1, 0), \"Ball\")\n end\nend");
    Runtime rt(s);
    run(rt, 1);
    CHECK(s.size() == 4);
    EntityId ball = s.find("Ball");
    (void)s.setBehaviors(ball, Json::array({Json::object({{"source", "on tick\n destroy self\nend"}})}));
    run(rt, 1);
    CHECK(s.size() == 3);
}

TEST_CASE("runtime: errors are reported and repeated failures disable the script") {
    Scene s;
    EntityId e = withScript(s, "Bad", "on tick\n let x = 1 / 0\nend");
    Runtime rt(s);
    run(rt, 10);
    auto msgs = rt.drainMessages();
    REQUIRE(msgs.size() >= 5);
    CHECK(msgs[0].kind == RuntimeMessage::Kind::Error);
    CHECK(msgs[0].text.find("division by zero") != std::string::npos);
    CHECK_FALSE(s.get<Behavior>(e)->scripts[0].enabled);
}

TEST_CASE("runtime: execution budget stops runaway handlers") {
    Scene s;
    withScript(s, "Heavy", "on tick\n repeat 1000 times\n  repeat 1000 times\n   let x = 1\n  end\n end\nend");
    Runtime rt(s);
    run(rt, 1);
    auto msgs = rt.drainMessages();
    REQUIRE_FALSE(msgs.empty());
    CHECK(msgs[0].text.find("budget") != std::string::npos);
}

TEST_CASE("runtime: compile errors surface as messages and the script does not run") {
    Scene s;
    EntityId e = withScript(s, "Broken", "on tick\n move self by (oops, 0, 0)\nend");
    Runtime rt(s);
    run(rt, 1);
    auto msgs = rt.drainMessages();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].kind == RuntimeMessage::Kind::Compile);
    CHECK(s.get<Transform>(e)->position == Vec3{0, 0, 0});
}

TEST_CASE("runtime: key, click and input queries") {
    Scene s;
    EntityId e = withScript(s, "Input", R"(
on key "space"
  self.jumped = true
end
on click
  self.clicked = true
end
on tick
  if key("w") then
    move self by (0, 0, -1)
  end
end)");
    Runtime rt(s);
    InputState in;
    in.pressed = {"space"};
    in.held = {"w"};
    in.clicked = {e};
    run(rt, 1, in);
    CHECK(s.record(e)->vars["jumped"].asBool());
    CHECK(s.record(e)->vars["clicked"].asBool());
    CHECK(s.get<Transform>(e)->position.z == doctest::Approx(-1));
}

TEST_CASE("runtime: locals in a loop body survive temporaries of the loop header and nested conditions") {
    // Regression: `for i in 0..counts[k]` left the range bound's scratch registers allocated, so the body's
    // `let u` was placed above the locals area and the next condition's temporaries overwrote it.
    Scene s;
    EntityId x = s.create("X");
    EntityId y = s.create("Y");
    withScript(s, "Ctl", R"(
behavior Ctl
  var counts = [1, 1]
  fn show(s: number)
    for k in 0..2
      for i in 0..counts[k]
        let u = find(["X", "Y"][k])
        if k == s then
          if not u.enabled then
            u.enabled = true
            u.position = (u.position.x, 3, u.position.z)
          end
        else
          u.enabled = false
        end
      end
    end
  end
  on start
    show(0)
  end
end)");
    Runtime rt(s);
    run(rt, 2);
    CHECK(rt.drainMessages().empty());
    CHECK(s.record(x)->enabled);
    CHECK_FALSE(s.record(y)->enabled);
}
