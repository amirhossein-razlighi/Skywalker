// 2D physics (Box2D): tilemap collision geometry, stacking, determinism, one-way platforms, sensors and
// contact events, joints, the character2d controller (slopes, coyote time, jump buffering), Wander
// builtins, tools and debug drawing.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "skywalker/engine/Engine.h"
#include "skywalker/physics2d/Physics2DSystem.h"
#include "skywalker/physics2d/Physics2DWorld.h"
#include "skywalker/physics2d/TileColliders.h"
#include "skywalker/render2d/Tilemap.h"
#include "skywalker/wander/Compiler.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

constexpr float kDt = 1.f / 60.f;

std::unique_ptr<Engine> make2DEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-physics2d-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Physics2D", false);
    return e;
}

EntityId make(Scene& s, const std::string& doc) {
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = s.create(parsed.value().get("name").asString());
    Status st = s.applyEntityJson(id, parsed.value());
    INFO((st.ok() ? std::string() : st.error().message));
    REQUIRE(st);
    return id;
}

EntityId ground(Scene& s, float width = 40.f) {
    return make(s, R"({"name":"Ground","components":{"transform":{"position":[0,-0.5,0]},"collider2d":{"size":[)" + std::to_string(width) +
                       R"(,1],"layer":"static"}}})");
}

Vec2 pos2(const Scene& s, EntityId id) {
    Vec3 p = s.worldMatrix(id).translation();
    return {p.x, p.y};
}

void addBehavior(Scene& s, EntityId id, const std::string& source) {
    Json b = Json::array({Json::object({{"name", "Test"}, {"source", source}})});
    REQUIRE(s.setBehaviors(id, b));
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

std::string errorCode(Engine& e, const char* tool, const std::string& args) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    REQUIRE(r.isError);
    const Json& err = r.structured.get("error");
    return err.isObject() ? err.get("code").asString() : r.structured.get("code").asString(err.asString());
}

/// A bare world stepping a scene (no engine).
struct World {
    Scene scene;
    physics2d::Physics2DWorld world;
    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.sync(scene, kDt);
            world.step(scene, kDt);
        }
    }
};

tiles::Grid gridFrom(int w, int h, const std::vector<uint32_t>& cells, const std::string& solid = "all") {
    Tilemap map;
    map.width = w;
    map.height = h;
    Json data = Json::array();
    for (uint32_t c : cells) data.push(static_cast<double>(c));
    map.layers = Json::array({Json::object({{"name", "ground"}, {"data", data}, {"solid", solid == "all" ? Json(true) : Json(solid)}})});
    auto g = tiles::Grid::fromComponent(map);
    REQUIRE(g);
    return g.value();
}

float signedArea(const std::vector<Vec2>& p) {
    float a = 0;
    for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return a * 0.5f;
}

}  // namespace

TEST_CASE("physics2d: tilemap collision merges full tiles into outlines or boxes, with per-tile shapes") {
    // Rows top to bottom; 1 = full ground, 2 = slope tile, 3 = one-way top tile.
    const std::vector<uint32_t> cells = {0, 3, 3, 3, 0, 0,  //
                                         0, 0, 0, 0, 1, 1,  //
                                         0, 2, 0, 0, 1, 1,  //
                                         1, 1, 1, 1, 1, 1};
    tiles::Grid g = gridFrom(6, 4, cells);
    physics2d::TileCollision tc;
    auto shapes = physics2d::parseTileShapes(Json::parse(R"({"2": "slope_up", "3": "top"})").value(), 16);
    REQUIRE(shapes);
    tc.shapes = shapes.value();

    physics2d::TileGeometry chains = physics2d::buildTileGeometry(g, tc, 1.f, true);
    CHECK(chains.fullCells == 10);
    CHECK(chains.solidCells == 14);
    REQUIRE(chains.loops.size() == 1);  // one connected region -> one outline
    const auto& loop = chains.loops[0];
    CHECK(loop.size() == 6);  // an L: collinear tile corners are merged away
    CHECK(signedArea(loop) > 0.f);  // counter-clockwise around the solid (Box2D chains collide on the outside)
    CHECK(std::abs(signedArea(loop)) == doctest::Approx(10.f));
    // The slope stays one polygon; the three one-way tops merge into one band.
    REQUIRE(chains.polygons.size() == 2);
    int oneWays = 0;
    for (const auto& p : chains.polygons) {
        if (p.oneWay) {
            ++oneWays;
            CHECK(p.points.size() == 4);
            float minX = 1e9f, maxX = -1e9f;
            for (Vec2 q : p.points) {
                minX = std::min(minX, q.x);
                maxX = std::max(maxX, q.x);
            }
            CHECK(minX == doctest::Approx(1.f));
            CHECK(maxX == doctest::Approx(4.f));
        } else {
            CHECK(p.points.size() == 3);
            CHECK(std::abs(signedArea(p.points)) == doctest::Approx(0.5f));
        }
    }
    CHECK(oneWays == 1);

    physics2d::TileGeometry boxes = physics2d::buildTileGeometry(g, tc, 0.5f, false);
    CHECK(boxes.loops.empty());
    REQUIRE(boxes.boxes.size() == 2);  // the 2x2 block and the bottom row
    float area = 0;
    for (const auto& b : boxes.boxes) area += (b.max.x - b.min.x) * (b.max.y - b.min.y);
    CHECK(area == doctest::Approx(10.f * 0.25f));

    // A ring has an outer outline and a clockwise hole; diagonal neighbours stay separate loops.
    tiles::Grid ring = gridFrom(3, 3, {1, 1, 1, 1, 0, 1, 1, 1, 1});
    auto r = physics2d::buildTileGeometry(ring, {}, 1.f, true);
    REQUIRE(r.loops.size() == 2);
    CHECK(((signedArea(r.loops[0]) > 0) != (signedArea(r.loops[1]) > 0)));
    tiles::Grid diag = gridFrom(2, 2, {1, 0, 0, 1});
    CHECK(physics2d::buildTileGeometry(diag, {}, 1.f, true).loops.size() == 2);

    // Flipped tiles flip their shape; "tiles" layers only collide on listed tiles.
    tiles::Grid flipped = gridFrom(1, 1, {2u | tiles::kFlipX});
    auto f = physics2d::buildTileGeometry(flipped, tc, 1.f, true);
    REQUIRE(f.polygons.size() == 1);
    bool highLeft = false;
    for (Vec2 q : f.polygons[0].points) highLeft = highLeft || (q.x < 0.01f && q.y > -0.01f);
    CHECK(highLeft);  // slope_up mirrored: the high corner is on the left
    tiles::Grid listed = gridFrom(3, 1, {1, 5, 7}, "tiles");
    physics2d::TileCollision only7;
    only7.solidIds = {7};
    CHECK(physics2d::buildTileGeometry(listed, only7, 1.f, true).fullCells == 1);

    auto bad = physics2d::parseTileShapes(Json::parse(R"({"4": "slope_upp"})").value(), 16);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("did you mean 'slope_up'") != std::string::npos);
    auto poly = physics2d::parseTileShapes(Json::parse(R"({"9": {"points": [[0, 16], [16, 16], [16, 8]], "oneWay": true}})").value(), 16);
    REQUIRE(poly);
    CHECK(poly->at(9).oneWay);
    CHECK(poly->at(9).points.size() == 3);
}

TEST_CASE("physics2d: a stack of boxes comes to rest without drifting") {
    World w;
    ground(w.scene);
    std::vector<EntityId> boxes;
    for (int i = 0; i < 8; ++i) {
        boxes.push_back(make(w.scene, R"({"name":"Box)" + std::to_string(i) + R"(","components":{"transform":{"position":[0,)" +
                                          std::to_string(0.5 + i * 1.01) + R"(,0]},"body2d":{},"collider2d":{"size":[1,1]}}})"));
    }
    w.step(360);
    for (int i = 0; i < 8; ++i) {
        Vec2 p = pos2(w.scene, boxes[static_cast<size_t>(i)]);
        INFO("box ", i, " at ", p.x, ", ", p.y);
        CHECK(std::abs(p.x) < 0.02f);
        CHECK(p.y == doctest::Approx(0.5f + static_cast<float>(i)).epsilon(0.02));
        CHECK(std::abs(w.scene.get<Transform>(boxes[static_cast<size_t>(i)])->rotation.z) < 1.f);
    }
    CHECK(w.world.allAsleep());
    CHECK(w.scene.get<Body2D>(boxes.back())->sleeping);
}

TEST_CASE("physics2d: play replays identically and stop restores the scene") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    ground(s);
    std::vector<EntityId> ids;
    for (int row = 0; row < 4; ++row) {
        for (int i = 0; i <= 3 - row; ++i) {
            ids.push_back(make(s, R"({"name":"B","components":{"transform":{"position":[)" + std::to_string(i - (3 - row) * 0.5) + "," +
                                      std::to_string(0.5 + row) + R"(,0]},"body2d":{},"collider2d":{"size":[0.95,0.95]}}})"));
        }
    }
    ids.push_back(make(s, R"({"name":"Ball","components":{"transform":{"position":[-8,1,0]},
        "body2d":{"velocity":[14,4],"angularVelocity":-360},"collider2d":{"shape":"circle","radius":0.4,"density":4,"restitution":0.3}}})"));
    auto snapshot = [&] {
        std::vector<float> out;
        for (EntityId id : ids) {
            const Transform* t = s.get<Transform>(id);
            out.insert(out.end(), {t->position.x, t->position.y, t->rotation.z});
        }
        return out;
    };
    const std::vector<float> before = snapshot();
    e->play();
    e->step(200);
    const std::vector<float> first = snapshot();
    CHECK(first != before);  // the ball knocked the pyramid over
    e->stop();
    CHECK(snapshot() == before);
    e->play();
    e->step(200);
    CHECK(snapshot() == first);  // bit-identical replay
    e->stop();
}

TEST_CASE("physics2d: one-way platforms let bodies and characters through from below") {
    World w;
    ground(w.scene);
    make(w.scene, R"({"name":"Platform","components":{"transform":{"position":[0,3,0]},
        "collider2d":{"size":[6,0.5],"oneWay":true,"layer":"static"}}})");
    EntityId rising = make(w.scene, R"({"name":"Rising","components":{"transform":{"position":[-1.5,1,0]},
        "body2d":{"velocity":[0,11],"fixedRotation":true},"collider2d":{"size":[0.5,0.5]}}})");
    EntityId dropped = make(w.scene, R"({"name":"Dropped","components":{"transform":{"position":[1.5,6,0]},
        "body2d":{"fixedRotation":true},"collider2d":{"size":[0.5,0.5]}}})");
    EntityId hero = make(w.scene, R"({"name":"Hero","components":{"transform":{"position":[0,0,0]},
        "character2d":{"height":1,"radius":0.3,"offset":[0,0.5]}}})");
    w.step(30);
    REQUIRE(w.world.grounded(hero).value_or(false));
    CHECK(w.world.jump(hero, 17.f));  // apex 17^2 / (2 * 35) = 4.1 above the feet
    w.step(150);
    CHECK(pos2(w.scene, rising).y == doctest::Approx(3.5f).epsilon(0.02));   // passed up through it, landed on top
    CHECK(pos2(w.scene, dropped).y == doctest::Approx(3.5f).epsilon(0.02));  // fell onto it from above
    CHECK(pos2(w.scene, hero).y == doctest::Approx(3.25f).epsilon(0.03));    // jumped through, stands on it
    CHECK(w.world.grounded(hero).value_or(false));
    // Dropping through: the character falls back to the ground.
    CHECK(w.world.dropThrough(hero));
    w.step(90);
    CHECK(pos2(w.scene, hero).y == doctest::Approx(0.f).epsilon(0.05));
}

TEST_CASE("physics2d: sensors and contacts fire Wander handlers with `other` (collide, collide_end, impact)") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    EntityId g = ground(s);
    make(s, R"({"name":"Settings","components":{"physics2d_world":{"impactSpeed":2}}})");
    EntityId zone = make(s, R"({"name":"Zone","components":{"transform":{"position":[0,3.5,0]},
        "collider2d":{"size":[3,1],"sensor":true,"layer":"trigger"}}})");
    addBehavior(s, zone, R"(behavior Zone
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
    EntityId ball = make(s, R"({"name":"Ball","tags":["falling"],"components":{"transform":{"position":[0,6,0]},
        "body2d":{},"collider2d":{"shape":"circle","radius":0.3,"restitution":0.5}}})");
    addBehavior(s, ball, R"(behavior Ball
  var bumps = 0
  var ends = 0
  var hit = ""
  var speed = 0
  var hard = 0
  var hardest = 0
  on collide "Ground"
    bumps = bumps + 1
    hit = other.name
    speed = max(speed, impact)
  end
  on collide_end
    ends = ends + 1
  end
  on impact
    hard = hard + 1
    hardest = max(hardest, data.speed)
  end
end)");
    e->play();
    e->step(240);
    const Json& zv = s.record(zone)->vars;
    CHECK(zv.get("entered").asInt() == 1);
    CHECK(zv.get("exited").asInt() == 1);
    CHECK(zv.get("who").asString() == "Ball");
    const Json& bv = s.record(ball)->vars;
    CHECK(bv.get("bumps").asInt() >= 2);  // it bounces
    CHECK(bv.get("ends").asInt() >= 1);
    CHECK(bv.get("hit").asString() == "Ground");
    CHECK(bv.get("speed").asFloat() > 6.f);  // fell ~5.7 units at 20 units/s^2
    CHECK(bv.get("hard").asInt() >= 1);
    CHECK(bv.get("hardest").asFloat() > 6.f);
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() != "runtime_error");
    (void)g;
}

TEST_CASE("physics2d: joints swing, keep their length, slide, pull and break") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    // Revolute pendulum pinned to the world at (0, 5).
    EntityId bob = make(s, R"({"name":"Bob","components":{"transform":{"position":[2,5,0]},"body2d":{},
        "collider2d":{"shape":"circle","radius":0.2},"joint2d":{"kind":"revolute","anchor":[-2,0]}}})");
    // Distance rod between two bodies.
    EntityId a = make(s, R"({"name":"A","components":{"transform":{"position":[10,10,0]},"body2d":{"motion":"static"},"collider2d":{"size":[0.2,0.2]}}})");
    EntityId b = make(s, R"({"name":"B","components":{"transform":{"position":[13,10,0]},"body2d":{},"collider2d":{"size":[0.4,0.4]},
        "joint2d":{"kind":"distance","other":"A"}}})");
    // Prismatic slider with a motor and limits, along x.
    EntityId slider = make(s, R"({"name":"Slider","components":{"transform":{"position":[20,5,0]},"body2d":{"gravityScale":0},
        "collider2d":{"size":[0.5,0.5]},"joint2d":{"kind":"prismatic","axis":[1,0],"limitMin":-1,"limitMax":1,"motorSpeed":3,"motorForce":200}}})");
    // Target joint pulls toward a point.
    EntityId pulled = make(s, R"({"name":"Pulled","components":{"transform":{"position":[30,0,0]},"body2d":{"gravityScale":0},
        "collider2d":{"shape":"circle","radius":0.25},"joint2d":{"kind":"target","target":[33,2],"maxForce":500,"stiffness":4}}})");
    // A weak weld that breaks under the weight.
    EntityId heavy = make(s, R"({"name":"Heavy","components":{"transform":{"position":[40,5,0]},"body2d":{"mass":50},
        "collider2d":{"size":[1,1]},"joint2d":{"kind":"weld","breakForce":100}}})");
    addBehavior(s, heavy, R"(behavior Heavy
  var broke = 0
  on event "joint_broken"
    broke = broke + 1
  end
end)");
    // A wheel joint on a chassis.
    make(s, R"({"name":"Chassis","components":{"transform":{"position":[50,5,0]},"body2d":{"motion":"kinematic"},"collider2d":{"size":[2,0.5]}}})");
    make(s, R"({"name":"Wheel","components":{"transform":{"position":[50,4.5,0]},"body2d":{},"collider2d":{"shape":"circle","radius":0.3},
        "joint2d":{"kind":"wheel","other":"Chassis","axis":[0,1],"stiffness":5}}})");
    e->play();
    e->step(20);
    Vec2 p = pos2(s, bob);
    CHECK(p.y < 4.9f);  // swinging down
    CHECK(std::hypot(p.x, p.y - 5.f) == doctest::Approx(2.f).epsilon(0.02));
    e->step(100);
    CHECK(std::hypot(pos2(s, b).x - pos2(s, a).x, pos2(s, b).y - pos2(s, a).y) == doctest::Approx(3.f).epsilon(0.02));
    CHECK(pos2(s, slider).x == doctest::Approx(21.f).epsilon(0.01));  // motor drove it to the upper limit
    CHECK(pos2(s, slider).y == doctest::Approx(5.f).epsilon(0.001));
    CHECK(std::hypot(pos2(s, pulled).x - 33.f, pos2(s, pulled).y - 2.f) < 0.2f);
    CHECK_FALSE(s.get<Joint2D>(heavy)->enabled);
    CHECK(s.record(heavy)->vars.get("broke").asInt() == 1);
    CHECK(pos2(s, heavy).y < 3.f);  // falling once free
    Json info = call(*e, "physics2d_info", "{}");
    CHECK(info.get("stats").get("joints").asInt() == 5);
    CHECK(info.get("warnings").size() == 0);
}

TEST_CASE("physics2d: character2d walks up slopes, stops at walls, has coyote time and buffers jumps") {
    SUBCASE("slopes and walls") {
        World w;
        make(w.scene, R"({"name":"Flat","components":{"transform":{"position":[-5,-0.5,0]},"collider2d":{"size":[10,1]}}})");
        make(w.scene, R"({"name":"Slope","components":{"collider2d":{"shape":"polygon","points":[[0,0],[6,0],[6,3]]}}})");
        make(w.scene, R"({"name":"Plateau","components":{"transform":{"position":[11,1.5,0]},"collider2d":{"size":[10,3]}}})");
        make(w.scene, R"({"name":"Wall","components":{"transform":{"position":[15,5,0]},"collider2d":{"shape":"polygon","points":[[0,-2],[1,-2],[1,1]]}}})");
        EntityId hero = make(w.scene, R"({"name":"Hero","components":{"transform":{"position":[-3,0,0]},
            "character2d":{"height":1,"radius":0.3,"offset":[0,0.5],"moveSpeed":5}}})");
        int airborneOnSlope = 0;
        for (int i = 0; i < 240; ++i) {
            w.world.move(hero, 1.f);
            w.step();
            Vec2 p = pos2(w.scene, hero);
            if (p.x > 0.5f && p.x < 5.5f && !w.world.grounded(hero).value_or(false)) ++airborneOnSlope;
        }
        Vec2 p = pos2(w.scene, hero);
        CHECK(airborneOnSlope == 0);  // glued to the slope all the way up
        CHECK(p.y == doctest::Approx(3.f).epsilon(0.02));
        CHECK(p.x > 12.f);
        CHECK(p.x < 14.8f);  // the 71 degree wall (steeper than maxSlope) stops it
        CHECK(w.scene.get<Character2D>(hero)->grounded);
    }
    SUBCASE("coyote time") {
        for (float coyote : {0.1f, 0.f}) {
            World w;
            make(w.scene, R"({"name":"Ledge","components":{"transform":{"position":[-5,-0.5,0]},"collider2d":{"size":[10,1]}}})");
            EntityId hero = make(w.scene, R"({"name":"Hero","components":{"transform":{"position":[-1,0,0]},
                "character2d":{"height":1,"radius":0.3,"offset":[0,0.5],"moveSpeed":4,"coyoteTime":)" + std::to_string(coyote) + "}}}");
            w.step(5);
            int airTicks = 0;
            bool jumped = false;
            for (int i = 0; i < 120 && !jumped; ++i) {
                w.world.move(hero, 1.f);
                w.step();
                if (!w.world.grounded(hero).value_or(true)) ++airTicks;
                if (airTicks == 3) {
                    jumped = w.world.jump(hero, 0.f);
                    w.step();
                    break;
                }
            }
            INFO("coyote ", coyote);
            CHECK(airTicks == 3);
            CHECK(jumped == (coyote > 0.f));
            CHECK((w.world.velocity(hero)->y > 0.f) == (coyote > 0.f));
        }
    }
    SUBCASE("jump buffering") {
        for (float buffer : {0.15f, 0.f}) {
            World w;
            ground(w.scene);
            EntityId hero = make(w.scene, R"({"name":"Hero","components":{"transform":{"position":[0,1.5,0]},
                "character2d":{"height":1,"radius":0.3,"offset":[0,0.5],"jumpBuffer":)" + std::to_string(buffer) + "}}}");
            // Fall until just above the ground, press jump in the air, then land.
            bool pressed = false;
            float maxRise = 0.f;
            for (int i = 0; i < 90; ++i) {
                if (!pressed && pos2(w.scene, hero).y < 0.3f) {
                    CHECK_FALSE(w.world.jump(hero, 0.f));  // airborne: not now
                    pressed = true;
                }
                w.step();
                if (pressed) maxRise = std::max(maxRise, w.world.velocity(hero)->y);
            }
            INFO("buffer ", buffer);
            CHECK(pressed);
            CHECK((maxRise > 5.f) == (buffer > 0.f));  // jumped on landing only with a buffer
        }
    }
}

TEST_CASE("physics2d: Wander builtins move bodies and characters and query the world") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    ground(s);
    make(s, R"({"name":"Target","tags":["enemy"],"components":{"transform":{"position":[0,4,0]},"collider2d":{"size":[1,1]}}})");
    EntityId puck = make(s, R"({"name":"Puck","components":{"transform":{"position":[-5,0.25,0]},"body2d":{"fixedRotation":true},
        "collider2d":{"size":[0.5,0.5],"friction":0}}})");
    addBehavior(s, puck, R"(behavior Probe
  var speed = 0
  var up = ""
  var dist = 0
  var floor = 1
  var near = ""
  var found = ""
  var vy = 0
  on start
    impulse2d(self, (1, 0, 0))
  end
  on tick
    if frame == 5 then
      speed = velocity2d(self).x
      let h = raycast2d((0, 1, 0), (0, 1, 0), 20)
      if exists(h) then
        up = h.name
        dist = hit_distance
      end
      let g = raycast2d(self, (0, -1, 0))
      floor = hit_point.y
      let n = overlap2d((0, 4, 0), 1.5, "enemy")
      if exists(n) then
        near = n.name
      end
      let p = point2d((0.2, 4.2, 0))
      if exists(p) then
        found = p.name
      end
      set_velocity2d(self, (0, 5, 0))
    end
    if frame == 6 then
      vy = velocity2d(self).y
    end
  end
end)");
    EntityId hero = make(s, R"({"name":"Hero","components":{"transform":{"position":[5,0,0]},
        "character2d":{"height":1,"radius":0.3,"offset":[0,0.5],"moveSpeed":4,"acceleration":1000}}})");
    addBehavior(s, hero, R"(behavior Hero
  var ground = false
  var jumped = false
  on tick
    move2d(self, -1)
    if frame == 20 then
      ground = grounded2d(self)
      jumped = jump2d(self)
    end
  end
end)");
    e->play();
    e->step(30);
    const Json& v = s.record(puck)->vars;
    CHECK(v.get("speed").asFloat() == doctest::Approx(4.f).epsilon(0.05));  // 1 N s on 0.25 kg
    CHECK(v.get("up").asString() == "Target");
    CHECK(v.get("dist").asFloat() == doctest::Approx(2.5f).epsilon(0.01));
    CHECK(v.get("floor").asFloat() == doctest::Approx(0.f).epsilon(0.01));
    CHECK(v.get("near").asString() == "Target");
    CHECK(v.get("found").asString() == "Target");
    CHECK(v.get("vy").asFloat() > 4.f);
    const Json& hv = s.record(hero)->vars;
    CHECK(hv.get("ground").asBool());
    CHECK(hv.get("jumped").asBool());
    CHECK(pos2(s, hero).x < 4.f);  // ran left
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() != "runtime_error");

    // Helpful errors: a body without dynamic motion, a missing character.
    EntityId wall = make(s, R"({"name":"Wall","components":{"collider2d":{}}})");
    addBehavior(s, wall, R"(behavior Bad
  on tick
    if frame == 32 then
      move2d(self, 1)
    end
  end
end)");
    e->step(5);
    bool reported = false;
    for (const auto& m : e->recentMessages(100)) {
        reported = reported || m.get("text").asString().find("has no character2d component") != std::string::npos;
    }
    CHECK(reported);
    e->stop();
    auto ok = wander::compile(R"(behavior B
  on collide_end
    log(other.name)
  end
  on impact
    log(data.speed)
  end
end)",
                              {"body2d", "collider2d"});
    CHECK(ok.ok());
}

TEST_CASE("physics2d tools: presets, info, query, settle and validation") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    EntityId player = make(s, R"({"name":"Player","components":{"transform":{"position":[0,3,0]},"sprite":{"size":[1,2],"pivot":[0.5,0]}}})");
    EntityId crate = make(s, R"({"name":"Crate","components":{"transform":{"position":[3,4,0]},"sprite":{"size":[1,1]}}})");
    EntityId plat = make(s, R"({"name":"Plat","components":{"transform":{"position":[0,6,0]},"sprite":{"size":[4,0.5]}}})");
    EntityId level = make(s, R"({"name":"Level","components":{"transform":{"position":[-5,1,0]},
        "tilemap":{"width":10,"height":2,"layers":[{"name":"ground","data":"rle:10*0,10*1","solid":true}]}}})");

    Json r = call(*e, "physics2d_add", R"({"entity":"Player","preset":"platformer_player"})");
    CHECK(r.get("warnings").size() == 0);
    const Character2D* ch = s.get<Character2D>(player);
    REQUIRE(ch);
    CHECK(ch->height == doctest::Approx(1.8f));
    CHECK(ch->offset.y == doctest::Approx(0.9f));
    call(*e, "physics2d_add", R"({"entity":"Crate","preset":"crate","overrides":{"collider2d":{"friction":0.9}}})");
    REQUIRE(s.get<Body2D>(crate));
    CHECK(s.get<Collider2D>(crate)->friction == doctest::Approx(0.9f));
    call(*e, "physics2d_add", R"({"entity":"Plat","preset":"one_way_platform"})");
    CHECK(s.get<Collider2D>(plat)->oneWay);
    CHECK(s.get<Collider2D>(plat)->size.x == doctest::Approx(4.f));
    call(*e, "physics2d_add", R"({"entity":"Level","preset":"tilemap_collision"})");
    CHECK(s.get<Collider2D>(level)->shape == "tilemap");

    CHECK(errorCode(*e, "physics2d_add", R"({"entity":"Crate","preset":"tilemap_collision"})") == "no_tilemap");
    CHECK(errorCode(*e, "physics2d_add", R"({"preset":"crate"})") == "invalid_arguments");
    CHECK(errorCode(*e, "physics2d_query", R"({"type":"raycast","origin":[0,0],"direction":[0,0]})") == "invalid_arguments");
    CHECK(errorCode(*e, "physics2d_query", R"({"type":"raycast","origin":[0,5],"layers":["statik"]})") == "unknown_layer");

    Json info = call(*e, "physics2d_info", "{}");
    CHECK(info.get("stats").get("characters").asInt() == 1);
    REQUIRE(info.get("tilemaps").size() == 1);
    CHECK(info.get("tilemaps")[0].get("loops").asInt() == 1);
    CHECK(info.get("tilemaps")[0].get("solidCells").asInt() == 10);

    // Queries work while editing.
    Json hit = call(*e, "physics2d_query", R"({"type":"raycast","origin":[0,10],"direction":[0,-1]})");
    CHECK(hit.get("hit").get("name").asString() == "Plat");
    Json down = call(*e, "physics2d_query", R"({"type":"raycast_all","origin":[0,10],"direction":[0,-1],"exclude":["Player"]})");
    CHECK(down.get("hits").size() == 2);  // platform, then the tilemap ground
    Json free = call(*e, "physics2d_query", R"({"type":"overlap_circle","origin":[10,10],"radius":1})");
    CHECK(free.get("free").asBool());
    Json at = call(*e, "physics2d_query", R"({"type":"point","origin":[3,4]})");
    CHECK(at.get("entities")[0].get("name").asString() == "Crate");

    // Settle drops the crate onto the tilemap (top at y = 0) as one undo step.
    size_t undoBefore = e->history().cursor();
    Json settled = call(*e, "physics2d_settle", R"({"entity":"Crate","seconds":3})");
    CHECK(settled.get("atRest").asBool());
    CHECK(pos2(s, crate).y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(e->history().cursor() == undoBefore + 1);
    e->play();
    CHECK(errorCode(*e, "physics2d_settle", R"({"entity":"Crate"})") == "not_editing");
    e->stop();
    call(*e, "physics2d_add", R"({"entity":"Crate","preset":"remove"})");
    CHECK_FALSE(s.get<Body2D>(crate));
    CHECK_FALSE(s.get<Collider2D>(crate));
}

TEST_CASE("physics2d: debug draw puts shapes, contacts and joints into the frame") {
    auto e = make2DEngine();
    Scene& s = e->scene();
    ground(s);
    make(s, R"({"name":"Box","components":{"transform":{"position":[0,0.5,0]},"body2d":{},"collider2d":{"size":[1,1]}}})");
    make(s, R"({"name":"Bob","components":{"transform":{"position":[3,3,0]},"body2d":{},"collider2d":{"shape":"circle","radius":0.3},
        "joint2d":{"kind":"distance","otherAnchor":[3,5]}}})");
    CaptureOptions o;
    o.width = 64;
    o.height = 64;
    CHECK(e->frame(o).render2d.debugLines.empty());  // off by default
    EntityId settings = make(s, R"({"name":"Settings","components":{"physics2d_world":{"debugDraw":true}}})");
    e->play();
    e->step(10);
    FrameData f = e->frame(o);
    CHECK(f.render2d.debugLines.size() > 30);  // box + ground edges, circle segments, a contact normal, a joint line
    auto cap = e->capture(o);
    REQUIRE(cap);
    e->stop();
    (void)settings;
}

TEST_CASE("physics2d: the Wander examples in docs/PHYSICS.md and docs/2D_AND_UI.md compile") {
    int blocks = 0;
    for (const char* file : {"/docs/PHYSICS.md", "/docs/2D_AND_UI.md"}) {
        std::ifstream in(std::string(SKY_SOURCE_DIR) + file);
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string doc = ss.str();
        size_t section = doc.find("2D physics");
        REQUIRE(section != std::string::npos);
        for (size_t at = doc.find("```wander", section); at != std::string::npos; at = doc.find("```wander", at)) {
            size_t start = doc.find('\n', at) + 1;
            size_t end = doc.find("```", start);
            REQUIRE(end != std::string::npos);
            std::string code = doc.substr(start, end - start);
            if (code.find("2d(") == std::string::npos) {  // only the 2D physics samples
                at = end + 3;
                continue;
            }
            auto r = wander::compile(code, {"body2d", "collider2d", "character2d", "joint2d", "transform", "sprite", "tilemap"});
            INFO(file, "\n", code, "\n", r.toJson().dump(2));
            CHECK(r.ok());
            ++blocks;
            at = end + 3;
        }
    }
    CHECK(blocks >= 2);
}
