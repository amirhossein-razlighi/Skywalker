// Navigation (Recast/Detour): navmesh bake, paths around obstacles, save/load, agents.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "skywalker/engine/Engine.h"
#include "skywalker/nav/NavMesh.h"
#include "skywalker/nav/NavSystem.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeNavEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-nav-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Nav", false);
    return e;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

EntityId make(Engine& e, const std::string& doc) {
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = e.scene().create(parsed.value().get("name").asString());
    REQUIRE(e.scene().applyEntityJson(id, parsed.value()));
    return id;
}

/// A 20 x 20 m floor with a wall across the middle (a gap at x > 6).
void buildLevel(Engine& e) {
    make(e, R"({"name":"Floor","components":{"transform":{"scale":[20,1,20]},"mesh":{"mesh":"plane"},"collider":{}}})");
    make(e, R"({"name":"Wall","components":{"transform":{"position":[-2,1,0],"scale":[16,2,0.5]},"mesh":{"mesh":"cube"},"collider":{}}})");
}

}  // namespace

TEST_CASE("nav: path goes around an obstacle") {
    auto e = makeNavEngine();
    buildLevel(*e);
    Json r = call(*e, "nav_build", R"({"save": false})");
    CHECK(r.get("report").get("polygons").asInt() > 0);
    Json p = call(*e, "nav_path", R"({"from": [0, 0, 5], "to": [0, 0, -5]})");
    CHECK(p.get("reachable").asBool());
    float straight = p.get("straightDistance").asFloat();
    float length = p.get("length").asFloat();
    CHECK(straight == doctest::Approx(10.f).epsilon(0.01));
    CHECK(length > straight + 4.f);  // detour through the gap at x ~ 6.4
    bool throughGap = false;
    for (const auto& pt : p.get("points").elements()) throughGap = throughGap || pt[size_t{0}].asFloat() > 6.f;
    CHECK(throughGap);

    // Closing the gap makes the goal unreachable (the navmesh refreshes when the level changes).
    make(*e, R"({"name":"Plug","components":{"transform":{"position":[8,1,0],"scale":[4.5,2,0.5]},"mesh":{"mesh":"cube"},"collider":{}}})");
    p = call(*e, "nav_path", R"({"from": [0, 0, 5], "to": [0, 0, -5]})");
    CHECK_FALSE(p.get("reachable").asBool());
    CHECK(p.get("partial").asBool());
}

TEST_CASE("nav: navmesh saves, reloads and answers the same paths") {
    auto e = makeNavEngine();
    buildLevel(*e);
    Json r = call(*e, "nav_build", R"({"agent_radius": 0.5, "path": "levels/arena.navmesh"})");
    std::string file = r.get("file").asString();
    CHECK(file == "levels/arena.navmesh");
    CHECK(fs::exists(e->resolvePath(file)));
    // Settings + file were recorded in the scene's navmesh component (one undoable edit).
    EntityId holder = e->scene().find("Navigation");
    REQUIRE(holder);
    const NavMeshSurface* surf = e->scene().get<NavMeshSurface>(holder);
    REQUIRE(surf);
    CHECK(surf->agentRadius == doctest::Approx(0.5f));
    CHECK(surf->data == file);

    nav::NavMesh loaded;
    REQUIRE(loaded.load(e->resolvePath(file)));
    nav::NavMesh* live = e->navigation().mesh();
    REQUIRE(live);
    CHECK(loaded.sourceHash() == live->sourceHash());
    CHECK(loaded.polygons().size() == live->polygons().size());
    auto a = loaded.findPath({0, 0, 5}, {0, 0, -5});
    auto b = live->findPath({0, 0, 5}, {0, 0, -5});
    CHECK(a.found);
    CHECK(a.length == doctest::Approx(b.length));

    nav::NavMesh garbage;
    CHECK_FALSE(garbage.load(e->resolvePath("levels/missing.navmesh")));
}

TEST_CASE("nav: agents walk to their destination around obstacles (Wander navigate / arrived)") {
    auto e = makeNavEngine();
    buildLevel(*e);
    EntityId walker = make(*e, R"({"name":"Walker","components":{"transform":{"position":[0,0.5,5]},
        "mesh":{"mesh":"cube"},"nav_agent":{"speed":4,"stoppingDistance":0.3}}})");
    Json behaviors = Json::array({Json::object({{"name", "Go"}, {"source", R"(behavior Go
  var done = false
  var len = 0
  on start
    navigate(self, (0, 0, -5))
    len = path_length(self.position, (0, 0, -5))
  end
  on event "arrived"
    done = true
  end
end)"}})});
    REQUIRE(e->scene().setBehaviors(walker, behaviors));
    e->play();
    e->step(60 * 12);
    Vec3 p = e->scene().worldMatrix(walker).translation();
    const Json& v = e->scene().record(walker)->vars;
    CHECK(v.get("done").asBool());
    CHECK(v.get("len").asFloat() > 14.f);
    CHECK(std::hypot(p.x - 0.f, p.z + 5.f) < 0.5f);
    CHECK(p.y == doctest::Approx(0.5f).epsilon(0.05));  // kept its height above the navmesh
    CHECK_FALSE(e->scene().get<NavAgent>(walker)->navigating);
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() != "runtime_error");
}

TEST_CASE("nav: character agents walk through the physics controller; replays are deterministic") {
    auto e = makeNavEngine();
    buildLevel(*e);
    EntityId hero = make(*e, R"({"name":"Hero","components":{"transform":{"position":[-4,0.9,6]},
        "character":{"moveSpeed":4},"nav_agent":{"speed":3.5,"destination":[-4,0,-6],"navigating":true}}})");
    EntityId buddy = make(*e, R"({"name":"Buddy","components":{"transform":{"position":[-3,0.5,6]},
        "nav_agent":{"speed":3,"destination":[-3,0,-6],"navigating":true}}})");
    auto run = [&] {
        e->play();
        e->step(60 * 15);
        std::pair<Vec3, Vec3> out{e->scene().worldMatrix(hero).translation(), e->scene().worldMatrix(buddy).translation()};
        e->stop();
        return out;
    };
    auto a = run();
    auto b = run();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK(std::hypot(a.first.x + 4.f, a.first.z + 6.f) < 0.6f);
    CHECK(std::hypot(a.second.x + 3.f, a.second.z + 6.f) < 0.6f);
}

TEST_CASE("nav: navigate(self, entity) follows a moving target; stop_navigation stops") {
    auto e = makeNavEngine();
    buildLevel(*e);
    EntityId target = make(*e, R"({"name":"Rabbit","components":{"transform":{"position":[-6,0.5,-4]},"mesh":{"mesh":"sphere"}}})");
    Json rabbit = Json::array({Json::object({{"name", "Run"}, {"source", R"(behavior Run
  on tick
    if time < 3 then
      move self by (2 * dt, 0, 0)
    end
  end
end)"}})});
    REQUIRE(e->scene().setBehaviors(target, rabbit));
    EntityId hound = make(*e, R"({"name":"Hound","components":{"transform":{"position":[-6,0.5,5]},"nav_agent":{"speed":5}}})");
    Json chase = Json::array({Json::object({{"name", "Chase"}, {"source", R"(behavior Chase
  on start
    navigate(self, find("Rabbit"))
  end
end)"}})});
    REQUIRE(e->scene().setBehaviors(hound, chase));
    e->play();
    e->step(60 * 10);
    Vec3 r = e->scene().worldMatrix(target).translation();
    Vec3 h = e->scene().worldMatrix(hound).translation();
    CHECK(r.x == doctest::Approx(0.f).epsilon(0.05));      // the rabbit ran 6 m
    CHECK(std::hypot(h.x - r.x, h.z - r.z) < 0.9f);         // the hound followed it to its new spot
    CHECK(e->physics().arrived(hound).value_or(false));
}

TEST_CASE("nav tools: nav_debug draws a top-down map; errors without walkable geometry") {
    auto e = makeNavEngine();
    call(*e, "nav_path", R"({"from": [0, 0, 0], "to": [1, 0, 1]})", false);  // empty scene: no navmesh
    buildLevel(*e);
    make(*e, R"({"name":"Agent","components":{"transform":{"position":[2,0.5,4]},"nav_agent":{}}})");
    ToolResult r = e->callTool("nav_debug", Json::parse(R"({"from": [0,0,5], "to": [0,0,-5], "size": 256})").value(), "agent:test");
    CHECK_FALSE(r.isError);
    CHECK(r.structured.get("path").get("reachable").asBool());
    CHECK(r.structured.get("agents").size() == 1);
    bool hasImage = false;
    for (const auto& b : r.content) hasImage = hasImage || b.type == ContentBlock::Type::Image;
    CHECK(hasImage);
}
