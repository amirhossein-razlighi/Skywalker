#include <doctest/doctest.h>

#include <cstring>

#include "skywalker/anim/Controller.h"

using namespace sky;
using namespace sky::anim;

namespace {

// Bones: Hips (root, rest y = 1), Spine (child), Arm (child of Spine).
// Each clip writes a constant "marker" into Spine.t.x so tests can read which clips play
// and with what weight: Idle 0, Walk 1, Run 2, Jump 3, Wave 9 (and Arm.t.x = 5).
Channel constant(int bone, Path path, Vec3 v, float duration) {
    Channel c;
    c.bone = bone;
    c.path = path;
    c.times = {0.f, duration};
    c.values = {v.x, v.y, v.z, v.x, v.y, v.z};
    return c;
}

Channel moving(int bone, Vec3 a, Vec3 b, float duration) {
    Channel c;
    c.bone = bone;
    c.path = Path::Translation;
    c.times = {0.f, duration};
    c.values = {a.x, a.y, a.z, b.x, b.y, b.z};
    return c;
}

Clip clip(const char* name, float duration, float marker, std::vector<Channel> extra = {}) {
    Clip c;
    c.name = name;
    c.duration = duration;
    c.channels.push_back(constant(1, Path::Translation, {marker, 0.3f, 0}, duration));
    for (auto& e : extra) c.channels.push_back(std::move(e));
    return c;
}

std::shared_ptr<Library> makeLibrary() {
    auto lib = std::make_shared<Library>();
    lib->skeleton.bones.push_back({"Hips", -1, {{0, 1, 0}, {}, {1, 1, 1}}});
    lib->skeleton.bones.push_back({"Spine", 0, {{0, 0.3f, 0}, {}, {1, 1, 1}}});
    lib->skeleton.bones.push_back({"Arm", 1, {{0.2f, 0.2f, 0}, {}, {1, 1, 1}}});
    lib->rootBone = 0;
    lib->clips.push_back(clip("Idle", 2.f, 0.f));
    lib->clips.push_back(clip("Walk", 1.f, 1.f, {moving(0, {0, 1, 0}, {0, 1, 1.5f}, 1.f)}));
    lib->clips.push_back(clip("Run", 0.5f, 2.f, {moving(0, {0, 1, 0}, {0, 1.1f, 2.f}, 0.5f)}));
    lib->clips.push_back(clip("Jump", 0.5f, 3.f));
    lib->clips.push_back(clip("Wave", 1.f, 9.f, {constant(2, Path::Translation, {5, 0.2f, 0}, 1.f)}));
    return lib;
}

const char* kController = R"({
  "format": "skywalker.animctl", "version": 1,
  "parameters": {"speed": "float", "jump": "trigger", "grounded": {"type": "bool", "default": true}, "wave": "float"},
  "layers": [
    {"name": "Base", "default": "Locomotion",
     "states": {
       "Locomotion": {"blend": {"parameter": "speed", "motions": [{"clip": "Idle", "at": 0}, {"clip": "Walk", "at": 1.6}, {"clip": "Run", "at": 4.5}]},
                      "events": [{"time": 0.5, "name": "footstep"}]},
       "Jump": {"clip": "Jump", "loop": false, "events": [{"time": 0, "name": "takeoff"}]},
       "Fall": {"clip": "Idle"}
     },
     "transitions": [
       {"from": "Locomotion", "to": "Jump", "when": "jump and grounded", "duration": 0.1},
       {"from": "Jump", "to": "Locomotion", "exit": 0.8, "duration": 0.2},
       {"from": "any", "to": "Fall", "when": "!grounded", "duration": 0}
     ]},
    {"name": "Upper", "mask": ["Arm"], "weightParameter": "wave", "default": "Wave",
     "states": {"Wave": {"clip": "Wave"}}}
  ]
})";

ClipResolver resolver(const std::shared_ptr<Library>& lib) {
    return [lib](const std::string& ref) -> std::shared_ptr<const Clip> {
        const Clip* c = lib->clip(ref);
        return c ? std::shared_ptr<const Clip>(lib, c) : nullptr;
    };
}

struct Rig {
    std::shared_ptr<Library> lib = makeLibrary();
    std::shared_ptr<ControllerDef> def;
    AnimatorRuntime rt;

    explicit Rig(const char* json = kController) {
        auto parsed = ControllerDef::fromJson(Json::parse(json).value());
        REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
        def = std::make_shared<ControllerDef>(std::move(*parsed));
        REQUIRE(rt.init(lib, def, resolver(lib)).ok());
    }
    Pose pose() const {
        Pose p;
        rt.evaluate(p);
        return p;
    }
    float marker() const { return pose()[1].t.x; }
    void run(float seconds, std::vector<AnimatorRuntime::Event>* events = nullptr, Vec3* motion = nullptr) {
        int ticks = static_cast<int>(std::round(seconds * 60.f));
        for (int i = 0; i < ticks; ++i) {
            Vec3 d;
            rt.update(1.f / 60.f, events, &d);
            if (motion) *motion += d;
        }
    }
};

}  // namespace

TEST_CASE("animctl: parse, serialize, validate") {
    auto parsed = ControllerDef::fromJson(Json::parse(kController).value());
    REQUIRE(parsed.ok());
    const ControllerDef& c = *parsed;
    REQUIRE(c.layers.size() == 2);
    CHECK(c.layers[0].states.size() == 3);
    CHECK(c.layers[0].states[0].kind == StateKind::Blend1D);
    REQUIRE(c.layers[0].transitions[0].conditions.size() == 2);
    CHECK(c.layers[0].transitions[0].conditions[0].op == CondOp::Trigger);
    CHECK(c.layers[0].transitions[0].conditions[1].op == CondOp::IsTrue);
    CHECK(c.layers[0].transitions[2].from == "any");
    CHECK(c.layers[0].transitions[2].conditions[0].op == CondOp::IsFalse);
    CHECK(c.params[2].value == 1.f);  // grounded defaults to true

    // Round trip through the canonical form.
    auto again = ControllerDef::fromJson(c.toJson());
    REQUIRE(again.ok());
    CHECK(again->toJson() == c.toJson());

    auto lib = makeLibrary();
    auto hasClip = [&](const std::string& n) { return lib->clip(n) != nullptr; };
    CHECK(c.validate(hasClip, lib->clipNames(), &lib->skeleton).ok());

    auto broken = [&](const char* json) {
        auto p = ControllerDef::fromJson(Json::parse(json).value());
        if (!p) return p.error();
        Status s = p->validate(hasClip, lib->clipNames(), &lib->skeleton);
        REQUIRE(!s.ok());
        return s.error();
    };
    Error e = broken(R"({"states": {"A": {"clip": "Wlak"}}})");
    CHECK(e.code == "unknown_clip");
    CHECK(e.hint.find("Walk") != std::string::npos);
    e = broken(R"({"parameters": {"speed": "float"}, "states": {"A": {"clip": "Walk"}}, "transitions": [{"from": "A", "to": "A", "when": "sped > 1"}]})");
    CHECK(e.message.find("sped") != std::string::npos);
    CHECK(e.hint.find("speed") != std::string::npos);
    e = broken(R"({"parameters": {"speed": "float"}, "states": {"A": {"clip": "Walk"}}, "transitions": [{"from": "A", "to": "A", "when": "speed"}]})");
    CHECK(e.message.find("is a number") != std::string::npos);
    e = broken(R"({"states": {"A": {"clip": "Walk"}}, "transitions": [{"from": "A", "to": "B"}]})");
    CHECK(e.message.find("no state \"B\"") != std::string::npos);
    e = broken(R"({"layers": [{"mask": ["Neck"], "states": {"A": {"clip": "Walk"}}}]})");
    CHECK(e.code == "unknown_bone");
    e = broken(R"({"states": {"A": {"clip": "Walk", "lop": false}}})");
    CHECK(e.hint.find("loop") != std::string::npos);
}

TEST_CASE("animator: 1D blend space weights and synchronized durations") {
    Rig r;
    auto at = [&](float speed) {
        r.rt.setParam("speed", speed);
        return r.marker();
    };
    CHECK(at(0.f) == doctest::Approx(0.f));
    CHECK(at(0.8f) == doctest::Approx(0.5f));
    CHECK(at(1.6f) == doctest::Approx(1.f));
    CHECK(at(3.05f) == doctest::Approx(1.5f));
    CHECK(at(9.f) == doctest::Approx(2.f));
    r.rt.setParam("speed", 3.05f);
    CHECK(r.rt.stateDuration(0, 0) == doctest::Approx(0.75f));  // 0.5 * 1 s + 0.5 * 0.5 s
}

TEST_CASE("animator: 2D blend space (gradient bands)") {
    Rig r(R"({"parameters": {"x": "float", "y": "float"},
              "states": {"Move": {"blend2d": {"x": "x", "y": "y", "motions": [
                 {"clip": "Idle", "pos": [0, 0]}, {"clip": "Walk", "pos": [0, 1]}, {"clip": "Run", "pos": [1, 0]}, {"clip": "Jump", "pos": [-1, 0]}]}}}})");
    auto at = [&](float x, float y) {
        r.rt.setParam("x", x);
        r.rt.setParam("y", y);
        return r.marker();
    };
    CHECK(at(0, 0) == doctest::Approx(0.f));
    CHECK(at(0, 1) == doctest::Approx(1.f));
    CHECK(at(1, 0) == doctest::Approx(2.f));
    CHECK(at(-1, 0) == doctest::Approx(3.f));
    float mid = at(0, 0.5f);  // halfway Idle -> Walk
    CHECK(mid == doctest::Approx(0.5f));
}

TEST_CASE("animator: triggers, crossfades, exit times and any-state transitions") {
    Rig r;
    std::vector<AnimatorRuntime::Event> events;
    r.rt.setParam("speed", 1.6f);  // pure Walk
    r.run(0.1f, &events);
    CHECK(r.rt.stateName() == "Locomotion");
    r.rt.trigger("jump");
    r.run(1.f / 60.f, &events);
    CHECK(r.rt.stateName() == "Jump");
    CHECK(r.rt.inTransition());
    CHECK(r.rt.param("jump").value() == 0.f);  // consumed
    r.run(3.f / 60.f, &events);                 // halfway through the 0.1 s crossfade
    CHECK(events.back().name == "takeoff");     // the new state's time-0 event
    float during = r.marker();
    CHECK(during > 1.f);
    CHECK(during < 3.f);
    r.run(0.15f, &events);
    CHECK(!r.rt.inTransition());
    CHECK(r.marker() == doctest::Approx(3.f));
    // Exit time 0.8 of a 0.5 s clip: back to locomotion after ~0.4 s, crossfading 0.2 s.
    r.run(0.3f, &events);
    CHECK(r.rt.stateName() == "Locomotion");
    r.run(0.3f, &events);
    CHECK(r.marker() == doctest::Approx(1.f));

    // Unconsumed triggers disarm after a short grace period.
    r.rt.setParam("grounded", 1.f);
    r.rt.trigger("jump");
    r.rt.setParam("grounded", 0.f);  // Locomotion -> Jump needs grounded, so nothing consumes it
    r.run(1.f / 60.f);
    CHECK(r.rt.stateName() == "Fall");  // any-state transition
    r.run(0.5f);
    CHECK(r.rt.param("jump").value() == 0.f);
}

TEST_CASE("animator: events fire once per crossing, also across loops") {
    Rig r;
    r.rt.setParam("speed", 1.6f);  // Walk, 1 s cycle, footstep at 0.5
    std::vector<AnimatorRuntime::Event> events;
    r.run(3.0f, &events);  // crossings at 0.5, 1.5, 2.5
    int steps = 0;
    for (const auto& e : events) steps += e.name == "footstep";
    CHECK(steps == 3);
    events.clear();
    r.rt.update(2.0f, &events);  // one huge step still reports every crossing
    steps = 0;
    for (const auto& e : events) steps += e.name == "footstep";
    CHECK(steps == 2);
}

TEST_CASE("animator: root motion is extracted and the root is pinned") {
    Rig r;
    r.rt.setRootMotion(true, {0, 1, 0});
    r.rt.setParam("speed", 1.6f);  // Walk: hips move 1.5 m per 1 s cycle along +Z
    Vec3 moved{0, 0, 0};
    r.run(2.5f, nullptr, &moved);
    CHECK(moved.z == doctest::Approx(3.75f).epsilon(0.01));
    CHECK(std::fabs(moved.y) < 1e-4f);  // vertical motion stays in the pose
    Pose p = r.pose();
    CHECK(std::fabs(p[0].t.z) < 1e-4f);    // pinned over the start spot
    CHECK(p[0].t.y == doctest::Approx(1.f));
}

TEST_CASE("animator: masked override layer drives only its bones") {
    Rig r;
    r.rt.setParam("speed", 1.6f);
    CHECK(r.pose()[2].t.x == doctest::Approx(0.2f));  // layer weight parameter is 0
    r.rt.setParam("wave", 1.f);
    Pose p = r.pose();
    CHECK(p[2].t.x == doctest::Approx(5.f));  // Arm from the Wave layer
    CHECK(p[1].t.x == doctest::Approx(1.f));  // Spine still from locomotion
    r.rt.setParam("wave", 0.5f);
    CHECK(r.pose()[2].t.x == doctest::Approx(2.6f));
}

TEST_CASE("animator: play() one-shots return to the default state; seek for previews") {
    Rig r;
    r.rt.setParam("speed", 0.f);
    REQUIRE(r.rt.play("Wave", 0.1f).ok());  // a clip, not a base-layer state
    CHECK(r.rt.stateName() == "Wave");
    r.run(0.5f);
    CHECK(r.marker() == doctest::Approx(9.f));
    REQUIRE(r.rt.play("Wave", 0.1f).ok());  // already playing: no restart
    CHECK(r.rt.normalizedTime() > 0.4f);
    r.run(1.0f);
    CHECK(r.rt.stateName() == "Locomotion");
    r.run(0.5f);
    CHECK(r.marker() == doctest::Approx(0.f));
    Error e = r.rt.play("Wavee", 0.1f).error();
    CHECK(e.hint.find("Wave") != std::string::npos);

    REQUIRE(r.rt.seek("Jump", 0.5f).ok());
    CHECK(r.rt.stateName() == "Jump");
    CHECK(!r.rt.inTransition());
    CHECK(r.marker() == doctest::Approx(3.f));
}

TEST_CASE("animator: interrupting a crossfade blends from the visible pose") {
    Rig r(R"({"parameters": {"go": "trigger", "back": "trigger"},
              "states": {"A": {"clip": "Idle"}, "B": {"clip": "Run"}, "C": {"clip": "Jump"}},
              "transitions": [{"from": "A", "to": "B", "when": "go", "duration": 1, "interruptible": true},
                              {"from": "B", "to": "C", "when": "back", "duration": 1}]})");
    r.rt.trigger("go");
    r.run(0.5f);
    float mid = r.marker();
    CHECK(mid > 0.f);
    CHECK(mid < 2.f);
    r.rt.trigger("back");
    r.rt.update(1.f / 60.f);
    CHECK(r.rt.stateName() == "C");
    CHECK(std::fabs(r.marker() - mid) < 0.1f);  // no pop: starts from the frozen pose
    r.run(1.1f);
    CHECK(r.marker() == doctest::Approx(3.f));
}

TEST_CASE("animator: identical inputs give bit-identical poses") {
    Rig a, b;
    a.rt.setRootMotion(true);
    b.rt.setRootMotion(true);
    for (int i = 0; i < 400; ++i) {
        float speed = 2.f + 2.f * std::sin(static_cast<float>(i) * 0.05f);
        a.rt.setParam("speed", speed);
        b.rt.setParam("speed", speed);
        if (i % 97 == 0) {
            a.rt.trigger("jump");
            b.rt.trigger("jump");
        }
        Vec3 da, db;
        a.rt.update(1.f / 60.f, nullptr, &da);
        b.rt.update(1.f / 60.f, nullptr, &db);
        CHECK(std::memcmp(&da, &db, sizeof(Vec3)) == 0);
    }
    Pose pa = a.pose(), pb = b.pose();
    REQUIRE(pa.size() == pb.size());
    CHECK(std::memcmp(pa.data(), pb.data(), pa.size() * sizeof(Trs)) == 0);
}
