// Physics (Jolt) integration: bodies, determinism, triggers, characters, joints, queries,
// settle-and-keep and the Wander physics builtins.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "skywalker/engine/Engine.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/wander/Compiler.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makePhysicsEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-physics-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Physics", false);
    return e;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
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

EntityId ground(Engine& e) {
    return make(e, R"({"name":"Ground","components":{"transform":{"scale":[40,1,40]},"mesh":{"mesh":"plane"},
                       "collider":{}}})");
}

Vec3 pos(Engine& e, EntityId id) { return e.scene().worldMatrix(id).translation(); }

void addBehavior(Engine& e, EntityId id, const std::string& source) {
    Json b = Json::array({Json::object({{"name", "Test"}, {"source", source}})});
    REQUIRE(e.scene().setBehaviors(id, b));
}

}  // namespace

TEST_CASE("physics: Euler <-> body pose conversions round-trip through the world") {
    auto e = makePhysicsEngine();
    ground(*e);
    // A tilted static box: the query world must place its collider exactly where the mesh is.
    EntityId box = make(*e, R"({"name":"Ramp","components":{"transform":{"position":[0,1,0],"rotation":[30,45,10],
        "scale":[4,0.5,2]},"mesh":{"mesh":"cube"},"collider":{}}})");
    auto& w = e->physics().queryWorld();
    // Ray straight down through the ramp's center hits its top surface, not the ground.
    auto hit = w.raycast({0, 10, 0}, {0, -1, 0}, 100.f);
    REQUIRE(hit);
    CHECK(hit->entity == box);
    CHECK(hit->point.y > 1.f);
    CHECK(hit->point.y < 2.f);
}

TEST_CASE("physics: bodies fall, collide and come to rest") {
    auto e = makePhysicsEngine();
    EntityId g = ground(*e);
    EntityId crate = make(*e, R"({"name":"Crate","components":{"transform":{"position":[0,5,0]},
        "mesh":{"mesh":"cube"},"body":{"mass":10}}})");
    EntityId ball = make(*e, R"({"name":"Ball","components":{"transform":{"position":[3,8,0],"scale":[0.5,0.5,0.5]},
        "mesh":{"mesh":"sphere"},"body":{"mass":1}}})");
    e->play();
    e->step(30);
    CHECK(pos(*e, crate).y < 5.f);  // falling
    CHECK(e->scene().get<RigidBody>(crate)->velocity.y < -1.f);  // live velocity is written back
    e->step(300);
    CHECK(pos(*e, crate).y == doctest::Approx(0.5f).epsilon(0.02));   // resting on the ground plane (top at y = 0)
    CHECK(pos(*e, ball).y == doctest::Approx(0.25f).epsilon(0.05));
    auto* world = e->physics().playWorld();
    REQUIRE(world);
    CHECK(world->isSleeping(crate));
    physics::Stats st = world->stats();
    CHECK(st.dynamicBodies == 2);
    CHECK(st.staticBodies == 1);
    CHECK(st.sleepingBodies == 2);
    (void)g;
    // Stop restores the pre-play scene.
    e->stop();
    CHECK(pos(*e, crate).y == doctest::Approx(5.f));
    CHECK(e->physics().playWorld() == nullptr);
}

TEST_CASE("physics: simulation replays identically (determinism)") {
    auto e = makePhysicsEngine();
    ground(*e);
    std::vector<EntityId> ids;
    for (int i = 0; i < 12; ++i) {
        char doc[256];
        std::snprintf(doc, sizeof(doc),
                      R"({"name":"P%d","components":{"transform":{"position":[%g,%g,%g],"rotation":[%d,%d,0]},
                      "mesh":{"mesh":"%s"},"body":{"mass":%d,"restitution":0.3}}})",
                      i, (i % 3) * 0.6 - 0.6, 1.0 + i * 0.9, (i % 2) * 0.4, i * 17, i * 31, i % 2 ? "sphere" : "cube", 1 + i);
        ids.push_back(make(*e, doc));
    }
    auto run = [&] {
        e->play();
        e->step(240);
        std::vector<Transform> out;
        for (EntityId id : ids) out.push_back(*e->scene().get<Transform>(id));
        e->stop();
        return out;
    };
    auto a = run();
    auto b = run();
    REQUIRE(a.size() == b.size());
    bool identical = true;
    for (size_t i = 0; i < a.size(); ++i) {
        identical = identical && a[i].position == b[i].position && a[i].rotation == b[i].rotation;
    }
    CHECK(identical);
    // ...and things actually moved and piled up.
    CHECK(a[11].position.y < 10.f);
}

TEST_CASE("physics: triggers and collisions fire Wander handlers with `other`") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId zone = make(*e, R"({"name":"Zone","components":{"transform":{"position":[0,2,0],"scale":[2,1,2]},
        "collider":{"shape":"box","isTrigger":true}}})");
    addBehavior(*e, zone, R"(behavior Zone
  var entered = 0
  var exited = 0
  var who = ""
  on trigger_enter "falling"
    entered = entered + 1
    who = other.name
  end
  on trigger_exit
    exited = exited + 1
  end
end)");
    EntityId ball = make(*e, R"({"name":"Ball","tags":["falling"],"components":{"transform":{"position":[0,5,0],
        "scale":[0.4,0.4,0.4]},"mesh":{"mesh":"sphere"},"body":{}}})");
    addBehavior(*e, ball, R"(behavior Ball
  var bumps = 0
  var hit = ""
  var speed = 0
  on collide "Ground"
    bumps = bumps + 1
    hit = other.name
    speed = impact
  end
end)");
    e->play();
    e->step(200);
    const Json& zv = e->scene().record(zone)->vars;
    CHECK(zv.get("entered").asInt() == 1);
    CHECK(zv.get("exited").asInt() == 1);
    CHECK(zv.get("who").asString() == "Ball");
    const Json& bv = e->scene().record(ball)->vars;
    CHECK(bv.get("bumps").asInt() >= 1);
    CHECK(bv.get("hit").asString() == "Ground");
    CHECK(bv.get("speed").asFloat() > 3.f);  // fell ~4.8 m
    auto msgs = e->recentMessages();
    for (const auto& m : msgs) CHECK(m.get("kind").asString() != "runtime_error");
}

TEST_CASE("physics: character walks up a step, slides along and stops at a wall") {
    auto e = makePhysicsEngine();
    ground(*e);
    make(*e, R"({"name":"Step","components":{"transform":{"position":[0,0.125,-3],"scale":[4,0.25,2]},
        "mesh":{"mesh":"cube"},"collider":{}}})");
    make(*e, R"({"name":"Wall","components":{"transform":{"position":[0,1,-6],"scale":[4,2,0.5]},
        "mesh":{"mesh":"cube"},"collider":{}}})");
    EntityId hero = make(*e, R"({"name":"Hero","components":{"transform":{"position":[0,0.9,0]},
        "mesh":{"mesh":"capsule"},"character":{"height":1.8,"radius":0.35,"stepHeight":0.35,"moveSpeed":3}}})");
    addBehavior(*e, hero, R"(behavior Walker
  var maxY = 0
  var landed = false
  on tick
    walk(self, (0, 0, -1))
    if grounded(self) then
      landed = true
    end
    maxY = max(maxY, self.position.y)
  end
end)");
    e->play();
    e->step(300);
    Vec3 p = pos(*e, hero);
    const Json& v = e->scene().record(hero)->vars;
    CHECK(v.get("landed").asBool());
    CHECK(v.get("maxY").asFloat() > 1.1f);       // climbed the 25 cm step
    CHECK(p.z < -5.f);                           // walked all the way...
    CHECK(p.z > -5.75f + 0.3f);                  // ...but stopped at the wall (radius 0.35)
    CHECK(p.y == doctest::Approx(0.9f).epsilon(0.05));  // back on the ground after the step
    CHECK(e->scene().get<Transform>(hero)->rotation.y == doctest::Approx(0.f).epsilon(0.01));  // facing -Z
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() != "runtime_error");
}

TEST_CASE("physics: characters jump and are detected by triggers") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId pad = make(*e, R"({"name":"Pad","components":{"transform":{"position":[0,0.5,-2],"scale":[2,1,1]},
        "collider":{"isTrigger":true}}})");
    addBehavior(*e, pad, R"(behavior Pad
  var touched = ""
  on trigger_enter
    touched = other.name
  end
end)");
    EntityId hero = make(*e, R"({"name":"Hero","components":{"transform":{"position":[0,0.9,0]},
        "character":{}}})");
    addBehavior(*e, hero, R"(behavior Hop
  var peak = 0
  var jumped = false
  on tick
    walk(self, (0, 0, -1))
    if time > 0.5 and not jumped then
      jumped = jump(self)
    end
    peak = max(peak, self.position.y)
  end
end)");
    e->play();
    e->step(120);
    const Json& v = e->scene().record(hero)->vars;
    CHECK(v.get("jumped").asBool());
    CHECK(v.get("peak").asFloat() > 1.5f);
    CHECK(e->scene().record(pad)->vars.get("touched").asString() == "Hero");
}

TEST_CASE("physics: joints hold, swing and break") {
    auto e = makePhysicsEngine();
    ground(*e);
    // Welded to the world: stays put.
    EntityId sign = make(*e, R"({"name":"Sign","components":{"transform":{"position":[-3,3,0]},
        "mesh":{"mesh":"cube"},"body":{"mass":5},"joint":{"kind":"fixed"}}})");
    // Pendulum: ball joint 1 m above the bob.
    EntityId bob = make(*e, R"({"name":"Bob","components":{"transform":{"position":[1,3,0],"scale":[0.3,0.3,0.3]},
        "mesh":{"mesh":"sphere"},"body":{"mass":2},"joint":{"kind":"ball","anchor":[-3.3333,0,0]}}})");
    // Weak weld under a heavy box: breaks immediately.
    EntityId weak = make(*e, R"({"name":"Weak","components":{"transform":{"position":[4,3,0]},
        "mesh":{"mesh":"cube"},"body":{"mass":50},"joint":{"kind":"fixed","breakForce":10}}})");
    addBehavior(*e, weak, R"(behavior W
  var broke = false
  on event "joint_broken"
    broke = true
  end
end)");
    // Chain: a link ball-jointed to the welded sign (body-to-body), and a rope (distance joint) to a world point.
    EntityId link = make(*e, R"({"name":"Link","components":{"transform":{"position":[-3,1.5,0],"scale":[0.2,0.2,0.2]},
        "mesh":{"mesh":"sphere"},"body":{"mass":1},"joint":{"kind":"ball","target":"Sign","anchor":[0,5,0]}}})");
    EntityId lamp = make(*e, R"({"name":"Lamp","components":{"transform":{"position":[8,2,0],"scale":[0.3,0.3,0.3]},
        "mesh":{"mesh":"sphere"},"body":{"mass":1},
        "joint":{"kind":"distance","connectedAnchor":[8,6,0],"limitMin":0,"limitMax":3}}})");
    e->play();
    e->step(120);
    CHECK(distance(pos(*e, link), Vec3{-3, 2.5f, 0}) < 1.1f);  // hangs from the sign's bottom face area
    CHECK(pos(*e, link).y > 1.f);
    CHECK(pos(*e, lamp).y == doctest::Approx(3.f).epsilon(0.03));  // rope of 3 m from y = 6
    CHECK(pos(*e, sign).y == doctest::Approx(3.f).epsilon(0.01));
    Vec3 b = pos(*e, bob);
    CHECK(distance(b, Vec3{0, 3, 0}) == doctest::Approx(1.f).epsilon(0.03));  // rope length kept
    CHECK(b.y < 2.5f);                                                          // swung down
    CHECK_FALSE(e->scene().get<Joint>(weak)->enabled);
    CHECK(e->scene().record(weak)->vars.get("broke").asBool());
    CHECK(pos(*e, weak).y < 1.f);  // fell after breaking
}

TEST_CASE("physics: hinge motor spins a wheel; kinematic platforms carry bodies") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId wheel = make(*e, R"({"name":"Wheel","components":{"transform":{"position":[0,3,0]},
        "mesh":{"mesh":"cylinder"},"body":{"gravityScale":0},
        "joint":{"kind":"hinge","axis":[0,1,0],"motorSpeed":180,"motorForce":1000}}})");
    EntityId lift = make(*e, R"({"name":"Lift","components":{"transform":{"position":[5,0.25,0],"scale":[2,0.5,2]},
        "mesh":{"mesh":"cube"},"body":{"motion":"kinematic"}}})");
    addBehavior(*e, lift, R"(behavior Lift
  on tick
    move self by (0, 1 * dt, 0)
  end
end)");
    EntityId box = make(*e, R"({"name":"Box","components":{"transform":{"position":[5,1,0],"scale":[0.5,0.5,0.5]},
        "mesh":{"mesh":"cube"},"body":{"mass":2}}})");
    EntityId rider = make(*e, R"({"name":"Rider","components":{"transform":{"position":[5.6,1.4,0.6]},
        "character":{"height":1.6,"radius":0.3}}})");
    e->play();
    e->step(120);
    CHECK(pos(*e, rider).y > 2.5f + 0.75f);  // stood on the lift (top 2.5) while it rose
    CHECK(std::fabs(e->scene().get<RigidBody>(wheel)->angularVelocity.y) == doctest::Approx(180.f).epsilon(0.05));
    CHECK(pos(*e, lift).y == doctest::Approx(2.25f).epsilon(0.02));
    CHECK(pos(*e, box).y > 2.4f);  // rode up on the platform
}

TEST_CASE("physics: compound bodies from child colliders; spawned bodies join the simulation") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId cart = make(*e, R"({"name":"Cart","components":{"transform":{"position":[0,3,0]},"body":{"mass":20}}})");
    EntityId left = make(*e, R"({"name":"L","components":{"transform":{"position":[-1,0,0]},"mesh":{"mesh":"cube"},"collider":{}}})");
    EntityId right = make(*e, R"({"name":"R","components":{"transform":{"position":[1,0,0]},"mesh":{"mesh":"cube"},"collider":{}}})");
    REQUIRE(e->scene().setParent(left, cart));
    REQUIRE(e->scene().setParent(right, cart));
    EntityId spawner = make(*e, R"({"name":"Spawner","components":{}})");
    addBehavior(*e, spawner, R"(behavior S
  var made = 0
  on tick
    if made == 0 and time > 0.2 then
      let b = spawn("sphere", (6, 4, 0), "Spawned")
      b.body.mass = 3
      made = 1
    end
  end
end)");
    e->play();
    e->step(240);
    CHECK(pos(*e, cart).y == doctest::Approx(0.5f).epsilon(0.03));  // both child cubes rest on the ground
    CHECK(pos(*e, left).y == doctest::Approx(0.5f).epsilon(0.03));
    EntityId spawned = e->scene().find("Spawned");
    REQUIRE(spawned);
    CHECK(pos(*e, spawned).y == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(e->physics().playWorld()->hasBody(spawned));
    // Destroying an entity removes its body.
    e->scene().destroy(spawned);
    e->step(1);
    CHECK(e->physics().playWorld()->stats().dynamicBodies == 1);
}

TEST_CASE("physics: Wander builtins (impulse, velocity, raycast, overlap_sphere)") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId puck = make(*e, R"({"name":"Puck","components":{"transform":{"position":[0,0.25,0],"scale":[0.5,0.5,0.5]},
        "mesh":{"mesh":"cube"},"body":{"mass":1,"friction":0}}})");
    EntityId target = make(*e, R"({"name":"Target","tags":["enemy"],"components":{"transform":{"position":[0,1,-6]},
        "mesh":{"mesh":"cube"},"collider":{}}})");
    addBehavior(*e, puck, R"(behavior Probe
  var hitName = ""
  var hitDist = 0
  var near = ""
  var speed = 0
  var floor = 0
  on start
    impulse(self, (0, 0, -2))
  end
  on tick
    if frame == 5 then
      speed = length(velocity(self))
      let h = raycast(self.position + (0, 0.75, 0), (0, 0, -1), 50)
      if exists(h) then
        hitName = h.name
        hitDist = hit_distance
      end
      let n = overlap_sphere((0, 1, -6), 2, "enemy")
      if exists(n) then
        near = n.name
      end
      let g = raycast(self.position, (0, -1, 0))
      floor = hit_point.y
    end
  end
end)");
    e->play();
    e->step(10);
    const Json& v = e->scene().record(puck)->vars;
    CHECK(v.get("speed").asFloat() == doctest::Approx(2.f).epsilon(0.05));
    CHECK(v.get("hitName").asString() == "Target");
    CHECK(v.get("hitDist").asFloat() > 4.f);
    CHECK(v.get("near").asString() == "Target");
    CHECK(v.get("floor").asFloat() == doctest::Approx(0.f).epsilon(0.01));
    (void)target;
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() != "runtime_error");
}

TEST_CASE("physics: builtins report helpful errors and compile checks know the new triggers") {
    auto ok = wander::compile(R"(behavior B
  on collide "Wall"
    log other.name
  end
  on trigger_enter
    log contact_point
  end
  on trigger_exit "player"
    stop_navigation(self)
  end
end)");
    CHECK(ok.ok());
    auto bad = wander::compile(R"(behavior B
  on colide
  end
end)");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.toJson().dump().find("collide") != std::string::npos);  // did-you-mean

    auto e = makePhysicsEngine();
    EntityId rock = make(*e, R"({"name":"Rock","components":{"mesh":{"mesh":"cube"}}})");
    addBehavior(*e, rock, R"(behavior R
  on start
    push(self, (0, 1, 0))
  end
end)");
    e->play();
    e->step(2);
    bool sawError = false;
    for (const auto& m : e->recentMessages()) {
        if (m.get("kind").asString() == "runtime_error" && m.get("text").asString().find("no dynamic body") != std::string::npos) {
            sawError = true;
        }
    }
    CHECK(sawError);
}

TEST_CASE("physics: collision layers and the layer matrix") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId hero = make(*e, R"({"name":"Hero","components":{"transform":{"position":[0,0.9,0]},"character":{}}})");
    EntityId debris = make(*e, R"({"name":"Chip","components":{"transform":{"position":[0,3,0],"scale":[0.2,0.2,0.2]},
        "mesh":{"mesh":"cube"},"body":{"layer":"debris"}}})");
    e->play();
    e->step(90);
    // Debris ignores players by default: it fell through the character to the ground.
    CHECK(pos(*e, debris).y == doctest::Approx(0.1f).epsilon(0.05));
    (void)hero;
    e->stop();
    call(*e, "physics_settings", R"({"ignorePairs": "debris-ground"})");
    Json st = call(*e, "physics_settings", "{}");
    CHECK(st.get("warnings").dump().find("unknown layer") != std::string::npos);
}

TEST_CASE("physics tools: physics_add presets, physics_query, physics_debug") {
    auto e = makePhysicsEngine();
    EntityId level = make(*e, R"({"name":"Level","components":{}})");
    EntityId floor = make(*e, R"({"name":"Floor","components":{"transform":{"scale":[20,1,20]},"mesh":{"mesh":"plane"}}})");
    EntityId wall = make(*e, R"({"name":"Wall","components":{"transform":{"position":[0,1,-3],"scale":[6,2,0.3]},"mesh":{"mesh":"cube"}}})");
    REQUIRE(e->scene().setParent(floor, level));
    REQUIRE(e->scene().setParent(wall, level));
    EntityId crate = make(*e, R"({"name":"Crate","components":{"transform":{"position":[2,2,0]},"mesh":{"mesh":"cube"}}})");
    EntityId hero = make(*e, R"({"name":"Hero","components":{"transform":{"position":[0,0.9,2],"scale":[1.4,1.8,1.4]},"mesh":{"mesh":"capsule"}}})");

    call(*e, "physics_add", R"({"entity": "Level", "preset": "static_level"})");
    CHECK(e->scene().get<Collider>(floor));
    CHECK(e->scene().get<Collider>(wall));
    Json r = call(*e, "physics_add", R"({"entities": ["Crate"], "preset": "prop", "overrides": {"body": {"restitution": 0.2}}})");
    REQUIRE(e->scene().get<RigidBody>(crate));
    CHECK(e->scene().get<RigidBody>(crate)->motion == "dynamic");
    CHECK(e->scene().get<RigidBody>(crate)->restitution == doctest::Approx(0.2f));
    CHECK(e->scene().get<RigidBody>(crate)->mass > 100.f);  // ~1 m^3 crate
    call(*e, "physics_add", R"({"entity": "Hero", "preset": "player_character"})");
    const CharacterController* c = e->scene().get<CharacterController>(hero);
    REQUIRE(c);
    CHECK(c->height == doctest::Approx(1.8f).epsilon(0.01));
    CHECK(c->radius == doctest::Approx(0.35f).epsilon(0.02));
    call(*e, "physics_add", R"({"entity": "Hero", "preset": "jetpack"})", false);  // enum validation

    // Queries work while editing (edit-time mirror of the scene).
    Json q = call(*e, "physics_query", R"({"type": "raycast", "origin": [0, 1, 5], "direction": [0, 0, -1]})");
    CHECK(q.get("hit").get("name").asString() == "Hero");
    q = call(*e, "physics_query", R"({"type": "raycast", "origin": [0, 1, 5], "direction": [0, 0, -1], "exclude": ["Hero"]})");
    CHECK(q.get("hit").get("name").asString() == "Wall");
    q = call(*e, "physics_query", R"({"type": "raycast_all", "origin": [0, 1, 5], "direction": [0, 0, -1]})");
    CHECK(q.get("hits").size() == 2);
    q = call(*e, "physics_query", R"({"type": "overlap", "origin": [2, 2, 0], "radius": 0.2})");
    CHECK(q.get("entities").size() == 1);
    q = call(*e, "physics_query", R"({"type": "overlap", "origin": [6, 2, 6], "radius": 0.5})");
    CHECK(q.get("free").asBool());
    q = call(*e, "physics_query", R"({"type": "shapecast", "origin": [2, 6, 0], "direction": [0, -1, 0], "radius": 0.25})");
    CHECK(q.get("hit").get("name").asString() == "Crate");
    q = call(*e, "physics_query", R"({"type": "raycast", "origin": [0, 1, 5], "direction": [0, 0, -1], "layers": ["static"]})");
    CHECK(q.get("hit").get("name").asString() == "Wall");
    call(*e, "physics_query", R"({"type": "raycast", "origin": [0, 1, 5], "layers": ["walls"]})", false);

    ToolResult dbg = e->callTool("physics_debug", Json::parse(R"({"view": "top", "width": 256, "height": 256})").value(), "agent:test");
    CHECK_FALSE(dbg.isError);
    CHECK(dbg.structured.get("stats").get("bodies").asInt() == 3);
    CHECK(dbg.structured.get("stats").get("characters").asInt() == 1);
    bool hasImage = false;
    for (const auto& b : dbg.content) hasImage = hasImage || b.type == ContentBlock::Type::Image;
    CHECK(hasImage);
}

TEST_CASE("physics tools: physics_settle drops props to rest as one undo step") {
    auto e = makePhysicsEngine();
    ground(*e);
    EntityId a = make(*e, R"({"name":"A","components":{"transform":{"position":[0,2,0]},"mesh":{"mesh":"cube"}}})");
    EntityId b = make(*e, R"({"name":"B","components":{"transform":{"position":[0.2,4,0.1],"rotation":[20,10,5]},"mesh":{"mesh":"cube"}}})");
    EntityId held = make(*e, R"({"name":"Held","components":{"transform":{"position":[5,3,0]},"mesh":{"mesh":"cube"},"body":{}}})");
    size_t before = e->history().cursor();
    Json r = call(*e, "physics_settle", R"({"entities": ["A", "B"], "seconds": 6})");
    CHECK(e->history().cursor() == before + 1);
    CHECK(r.get("atRest").asBool());
    CHECK(pos(*e, a).y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(pos(*e, b).y == doctest::Approx(1.5f).epsilon(0.05));  // stacked on A
    CHECK(pos(*e, held).y == doctest::Approx(3.f));               // frozen: not in the list
    CHECK_FALSE(e->scene().get<RigidBody>(a));                    // settle does not add components
    REQUIRE(e->history().undo());
    CHECK(pos(*e, a).y == doctest::Approx(2.f));
    CHECK(pos(*e, b).y == doctest::Approx(4.f));
}

TEST_CASE("physics: heightfields (16-bit heightmap and sampled mesh), convex hulls and triangle meshes") {
    auto e = makePhysicsEngine();
    // A 64 x 64 ramp rising along +X from 0 to 1 (scaled to 2 m by collider.size.y).
    {
        std::vector<unsigned char> bytes;
        for (int z = 0; z < 64; ++z) {
            for (int x = 0; x < 64; ++x) {
                unsigned v = static_cast<unsigned>(std::lround(x / 63.0 * 65535.0));
                bytes.push_back(static_cast<unsigned char>(v & 0xff));
                bytes.push_back(static_cast<unsigned char>(v >> 8));
            }
        }
        fs::create_directories(e->resolvePath("terrain"));
        FILE* f = std::fopen(e->resolvePath("terrain/ramp.r16").c_str(), "wb");
        REQUIRE(f);
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    EntityId ramp = make(*e, R"({"name":"Ramp","components":{"transform":{"position":[0,0,0]},
        "collider":{"shape":"heightfield","heightmap":"terrain/ramp.r16","size":[10,2,10],"resolution":64}}})");
    EntityId flat = make(*e, R"({"name":"Flat","components":{"transform":{"position":[30,-1,0],"scale":[10,1,10]},
        "mesh":{"mesh":"plane"},"collider":{"shape":"heightfield","resolution":32}}})");
    EntityId torus = make(*e, R"({"name":"Ring","components":{"transform":{"position":[-30,0,0],"scale":[2,2,2]},
        "mesh":{"mesh":"torus"},"collider":{}}})");
    auto& w = e->physics().queryWorld();
    auto mid = w.raycast({0, 10, 0}, {0, -1, 0}, 50.f);
    REQUIRE(mid);
    CHECK(mid->entity == ramp);
    CHECK(mid->point.y == doctest::Approx(1.f).epsilon(0.05));
    auto high = w.raycast({2.5f, 10, 0}, {0, -1, 0}, 50.f);
    REQUIRE(high);
    CHECK(high->point.y == doctest::Approx(1.5f).epsilon(0.05));
    auto f = w.raycast({31, 10, 2}, {0, -1, 0}, 50.f);
    REQUIRE(f);
    CHECK(f->entity == flat);
    CHECK(f->point.y == doctest::Approx(-1.f).epsilon(0.02));
    // Triangle-mesh torus (static): the ring is solid, the hole is empty.
    auto ring = w.raycast({-30 + 0.7f, 10, 0}, {0, -1, 0}, 50.f);
    REQUIRE(ring);
    CHECK(ring->entity == torus);
    CHECK(ring->point.y == doctest::Approx(0.3f).epsilon(0.08));
    CHECK_FALSE(w.raycast({-30, 10, 0}, {0, -1, 0}, 50.f));
    CHECK(w.drainWarnings().empty());

    // A dynamic cone (convex hull) lands on the flat heightfield and rests.
    EntityId cone = make(*e, R"({"name":"Cone","components":{"transform":{"position":[30,3,0]},
        "mesh":{"mesh":"cone"},"body":{"mass":4}}})");
    e->play();
    e->step(240);
    CHECK(pos(*e, cone).y == doctest::Approx(-0.5f).epsilon(0.05));  // cone base on the surface at y = -1
}

TEST_CASE("physics: the Wander examples in docs/PHYSICS.md compile") {
    std::ifstream in(std::string(SKY_SOURCE_DIR) + "/docs/PHYSICS.md");
    REQUIRE(in);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string doc = ss.str();
    int blocks = 0;
    for (size_t at = doc.find("```wander"); at != std::string::npos; at = doc.find("```wander", at)) {
        size_t start = doc.find('\n', at) + 1;
        size_t end = doc.find("```", start);
        REQUIRE(end != std::string::npos);
        std::string code = doc.substr(start, end - start);
        auto r = wander::compile(code, {"body", "collider", "character", "joint", "nav_agent", "transform", "mesh"});
        INFO(code, "\n", r.toJson().dump(2));
        CHECK(r.ok());
        ++blocks;
        at = end + 3;
    }
    CHECK(blocks >= 2);
}
