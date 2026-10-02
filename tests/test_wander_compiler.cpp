#include <doctest/doctest.h>

#include "skywalker/wander/Compiler.h"

using namespace sky::wander;

static const std::vector<std::string> kComponents{"transform", "mesh", "light", "camera"};

static bool hasCode(const CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

TEST_CASE("wander: full behavior compiles") {
    auto r = compile(R"(
behavior Patrol
  intent "Walk back and forth; turn red near the player."
  var speed = 2
  var home = (0, 0, 0)
  on start
    home = self.position
  end
  on tick
    move self by (sin(time) * speed * dt, 0, 0)
    let player = find("Player")
    if exists(player) and distance(self, player) < 3 then
      self.color = #ff4040
    elif speed > 5 then
      set self.mesh.roughness to 0.2
    else
      set self.color to #40c0ff
    end
    every 2 seconds
      emit "ping"
    end
  end
  on event "alarm"
    speed = speed * 2
  end
  on key "space"
    spawn("sphere", self.position + (0, 1, 0), "Ball")
  end
end
)", kComponents);
    CHECK(r.ok());
    CHECK(r.errorCount() == 0);
    REQUIRE(r.program);
    REQUIRE(r.program->behaviors.size() == 1);
    CHECK(r.program->behaviors[0].intent.find("Walk") == 0);
    CHECK(r.program->behaviors[0].handlers.size() == 4);
}

TEST_CASE("wander: bare handlers are wrapped in an implicit behavior") {
    auto r = compile("on tick\n  rotate self by (0, 90 * dt, 0)\nend", kComponents);
    REQUIRE(r.ok());
    CHECK(r.program->behaviors[0].name == "Main");
}

TEST_CASE("wander: diagnostics carry location, code and did-you-mean hints") {
    auto r = compile("on tick\n  let d = distnace(self, self)\nend", kComponents);
    CHECK_FALSE(r.ok());
    REQUIRE(hasCode(r, "unknown_function"));
    const auto& d = r.diagnostics.front();
    CHECK(d.loc.line == 2);
    CHECK(d.hint.find("distance") != std::string::npos);

    auto arity = compile("on tick\n  let x = clamp(1, 2)\nend");
    CHECK(hasCode(arity, "wrong_arity"));

    auto trig = compile("on tik\nend");
    CHECK(hasCode(trig, "unknown_trigger"));

    auto name = compile("on tick\n  move self by (spd, 0, 0)\nend");
    CHECK(hasCode(name, "unknown_name"));

    auto ro = compile("on tick\n  dt = 3\nend");
    CHECK(hasCode(ro, "readonly"));

    auto noEnd = compile("behavior A\n on tick\n  log 1\n end\n");
    CHECK(hasCode(noEnd, "missing_end"));
}

TEST_CASE("wander: undeclared self vars are warnings, not errors") {
    auto r = compile("on tick\n  self.healht = 3\nend", kComponents);
    CHECK(r.ok());
    REQUIRE(hasCode(r, "undeclared_var"));
}

TEST_CASE("wander: comments and colors coexist") {
    auto r = compile("# a comment\n-- another\n// third\non start\n  self.color = #abc  # trailing\nend", kComponents);
    CHECK(r.ok());
}

TEST_CASE("wander: garbage input never crashes and reports errors") {
    const char* inputs[] = {"", ")))", "behavior", "on", "if then else end", "on tick\n let = \nend",
                            "on tick\n move\nend", "\"unterminated", "on tick\n x = (1,2,3,4,5)\nend",
                            "on tick\n log ((((((1\nend"};
    for (const char* in : inputs) {
        auto r = compile(in, kComponents);
        (void)r.toJson();
    }
    CHECK(compile("").ok());  // empty program is valid (does nothing)
}
