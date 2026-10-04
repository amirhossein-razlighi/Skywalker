#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "skywalker/engine/Engine.h"

using namespace sky;

namespace {

std::unique_ptr<Engine> makeEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;  // deterministic, GPU-free
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Test", true);
    return e;
}

Json call(Engine& e, const char* tool, const char* args, bool expectOk = true, const char* actor = "agent:test") {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), actor);
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

}  // namespace

TEST_CASE("tools: every tool has a valid name, description and object schema") {
    auto e = makeEngine();
    CHECK(e->tools().all().size() >= 30);
    for (const auto& t : e->tools().all()) {
        INFO(t.name);
        // Names must be valid for every major LLM API: ^[a-zA-Z0-9_-]{1,64}$
        CHECK(t.name.size() <= 64);
        for (char c : t.name) CHECK((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'));
        CHECK(t.description.size() > 20);
        CHECK(t.inputSchema.get("type").asString() == "object");
    }
}

TEST_CASE("tools: create, update, transform, query, delete with undo attribution") {
    auto e = makeEngine();
    Json created = call(*e, "entity_create", R"({"name":"Rock","mesh":"sphere","color":"#808080","position":[1,0,2]})");
    EntityId rock = static_cast<EntityId>(created.get("id").asInt());
    REQUIRE(rock != 0);
    CHECK(e->history().lastCommitted()->actor == "agent:test");

    call(*e, "entity_update", R"({"entity":"Rock","components":{"mesh":{"roughness":0.2}},"tags":["stone"]})");
    CHECK(e->scene().get<MeshRenderer>(rock)->roughness == doctest::Approx(0.2));

    call(*e, "transform", R"({"entity":"Rock","translate":[0,1,0],"rotate":[0,45,0]})");
    CHECK(e->scene().get<Transform>(rock)->position == Vec3{1, 1, 2});

    Json q = call(*e, "scene_query", R"({"tag":"stone"})");
    CHECK(q.get("matches").size() == 1);

    call(*e, "entity_delete", R"({"entity":"Rock"})");
    CHECK_FALSE(e->scene().exists(rock));
    call(*e, "history", R"({"action":"undo"})");
    CHECK(e->scene().exists(rock));
}

TEST_CASE("tools: helpful errors for typos and bad arguments") {
    auto e = makeEngine();
    ToolResult r = e->callTool("entity_craete", Json::object(), "a");
    CHECK(r.isError);
    CHECK(r.content.front().text.find("entity_create") != std::string::npos);

    r = e->callTool("entity_update", Json::parse(R"({"entity":"Cub","name":"x"})").value(), "a");
    CHECK(r.isError);
    CHECK(r.content.front().text.find("Cube") != std::string::npos);

    r = e->callTool("entity_create", Json::parse(R"({"name":"X","colour":"#fff"})").value(), "a");
    CHECK(r.isError);
    CHECK(r.content.front().text.find("color") != std::string::npos);

    r = e->callTool("entity_create", Json::parse(R"({"name":"X","position":[1,2]})").value(), "a");
    CHECK(r.isError);
}

TEST_CASE("tools: batch is atomic") {
    auto e = makeEngine();
    size_t before = e->scene().size();
    size_t historyBefore = e->history().entries().size();
    call(*e, "batch", R"({"operations":[
        {"tool":"entity_create","args":{"name":"A"}},
        {"tool":"entity_create","args":{"name":"B"}},
        {"tool":"entity_update","args":{"entity":"DoesNotExist","name":"x"}}
    ]})", false);
    CHECK(e->scene().size() == before);
    CHECK(e->history().entries().size() == historyBefore);

    call(*e, "batch", R"({"label":"Two boxes","operations":[
        {"tool":"entity_create","args":{"name":"A","mesh":"cube"}},
        {"tool":"entity_create","args":{"name":"B","mesh":"cube","position":[2,0,0]}}
    ]})");
    CHECK(e->scene().size() == before + 2);
    CHECK(e->history().entries().size() == historyBefore + 1);
    CHECK(e->history().lastCommitted()->label == "Two boxes");
}

TEST_CASE("tools: behaviors are compiled and rejected on error; simulation steps deterministically") {
    auto e = makeEngine();
    Json bad = call(*e, "behavior_set", R"({"entity":"Cube","name":"Spin","source":"on tick\n rotate self by (0, spd, 0)\nend"})", false);
    CHECK_FALSE(bad.get("ok").asBool());
    CHECK(bad.get("diagnostics")[0].get("code").asString() == "unknown_name");
    CHECK(e->scene().get<Behavior>(e->scene().find("Cube")) == nullptr);

    call(*e, "behavior_set", R"({"entity":"Cube","name":"Spin","intent":"spin 90 deg/s","source":"on tick\n rotate self by (0, 90 * dt, 0)\nend"})");
    Json st = call(*e, "sim_control", R"({"action":"step","ticks":60})");
    CHECK(st.get("state").asString() == "paused");
    EntityId cube = e->scene().find("Cube");
    CHECK(e->scene().get<Transform>(cube)->rotation.y == doctest::Approx(90).epsilon(1e-3));
    call(*e, "sim_control", R"({"action":"stop"})");
    CHECK(e->scene().get<Transform>(cube)->rotation.y == doctest::Approx(0));  // restored
}

TEST_CASE("tools: sim_input drives key and event handlers") {
    auto e = makeEngine();
    call(*e, "behavior_set", R"({"entity":"Cube","name":"Jump","source":"on key \"space\"\n self.jumped = true\nend\non event \"go\"\n self.went = true\nend"})");
    call(*e, "sim_input", R"({"press":["Space"],"event":"go"})");
    call(*e, "sim_control", R"({"action":"step","ticks":2})");
    const Json& vars = e->scene().record(e->scene().find("Cube"))->vars;
    CHECK(vars.get("jumped").asBool());
    CHECK(vars.get("went").asBool());
}

TEST_CASE("tools: capture returns an image and visible entities; environment presets") {
    auto e = makeEngine();
    call(*e, "camera_set", R"({"frame":"Cube"})");
    ToolResult r = e->callTool("viewport_capture", Json::parse(R"({"width":160,"height":90})").value(), "a");
    REQUIRE_FALSE(r.isError);
    bool hasImage = false;
    for (const auto& c : r.content) hasImage = hasImage || c.type == ContentBlock::Type::Image;
    CHECK(hasImage);
    bool sawCube = false;
    for (const auto& v : r.structured.get("visible").elements()) sawCube = sawCube || v.get("name").asString() == "Cube";
    CHECK(sawCube);

    call(*e, "environment_update", R"({"preset":"sunset","fogDensity":0.02})");
    CHECK(e->scene().environment().sunElevation == doctest::Approx(8));
    CHECK(e->scene().environment().fogDensity == doctest::Approx(0.02));
    call(*e, "environment_update", R"({"preset":"midnight"})", false);
}

TEST_CASE("tools: duplicate copies hierarchies; asset requests flow") {
    auto e = makeEngine();
    call(*e, "entity_create", R"({"name":"Child","parent":"Cube","mesh":"sphere"})");
    size_t before = e->scene().size();
    Json d = call(*e, "entity_duplicate", R"({"entity":"Cube","offset":[2,0,0],"count":3})");
    CHECK(d.get("created").size() == 3);
    CHECK(e->scene().size() == before + 6);

    Json req = call(*e, "asset_request", R"({"kind":"texture","prompt":"mossy stone","target":"Cube"})");
    Json done = call(*e, "asset_complete", (R"({"id":)" + std::to_string(req.get("id").asInt()) + R"(,"path":"tex/moss.png"})").c_str());
    CHECK(done.get("status").asString() == "done");
    CHECK(e->scene().get<MeshRenderer>(e->scene().find("Cube"))->texture == "tex/moss.png");
}

TEST_CASE("engine: events record who did what") {
    auto e = makeEngine();
    (void)e->drainEvents();
    call(*e, "entity_create", R"({"name":"X"})", true, "agent:Nimbus");
    auto events = e->drainEvents();
    bool sawEdit = false, sawTool = false;
    for (const auto& ev : events) {
        if (ev.get("type").asString() == "edit" && ev.get("actor").asString() == "agent:Nimbus") sawEdit = true;
        if (ev.get("type").asString() == "tool" && ev.get("tool").asString() == "entity_create") sawTool = true;
    }
    CHECK(sawEdit);
    CHECK(sawTool);
}

TEST_CASE("engine: posted jobs run on pump (cross-thread request path)") {
    auto e = makeEngine();
    std::future<Json> f = e->post([] { return Json(42); });
    CHECK(f.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    e->pump();
    CHECK(f.get().asInt() == 42);
}

TEST_CASE("engine: a throwing edit rolls back and leaves no dangling transaction (regression)") {
    auto e = makeEngine();
    size_t before = e->scene().size();
    CHECK_THROWS((void)e->edit("a", "boom", [&]() -> Status {
        e->scene().create("Temp");
        throw std::runtime_error("boom");
    }));
    CHECK(e->scene().size() == before);
    CHECK_FALSE(e->history().inTransaction());
}

TEST_CASE("engine: batch during play is atomic (regression)") {
    auto e = makeEngine();
    call(*e, "sim_control", R"({"action":"play"})");
    size_t before = e->scene().size();
    call(*e, "batch", R"({"operations":[
        {"tool":"entity_create","args":{"name":"A"}},
        {"tool":"entity_update","args":{"entity":"Nope","name":"x"}}]})", false);
    CHECK(e->scene().size() == before);
    call(*e, "sim_control", R"({"action":"stop"})");
}

TEST_CASE("runtime: replacing a behavior during play restarts its instance (regression)") {
    auto e = makeEngine();
    call(*e, "behavior_set", R"({"entity":"Cube","name":"B","source":"on start\n self.v = 1\nend"})");
    call(*e, "sim_control", R"({"action":"step","ticks":1})");
    call(*e, "behavior_set", R"({"entity":"Cube","name":"B","source":"on start\n self.v = 2\nend"})");
    call(*e, "sim_control", R"({"action":"step","ticks":1})");
    CHECK(e->scene().record(e->scene().find("Cube"))->vars.get("v").asInt() == 2);
    call(*e, "sim_control", R"({"action":"stop"})");
}

TEST_CASE("gizmo: translate drag along X follows the cursor ray; hit testing picks axes") {
    Gizmo g;
    ViewCamera cam;
    cam.eye = {0, 5, 10};
    cam.target = {0, 0, 0};
    GizmoFrame f = Gizmo::frameFor(Mat4{}, cam, false);
    // A ray aimed at the middle of the X handle hits axis 0.
    Vec3 onX = f.center + f.axes[0] * (f.size * 0.6f);
    Ray r{cam.eye, normalize(onX - cam.eye)};
    CHECK(g.hitTest(f, r) == 0);
    auto start = g.begin(f, r, {0, 0, 0}, {0, 0, 0}, {1, 1, 1});
    REQUIRE(start.has_value());
    Vec3 target = f.center + f.axes[0] * (f.size * 0.6f + 2.f);
    auto res = g.drag(*start, Ray{cam.eye, normalize(target - cam.eye)}, false);
    CHECK(res.worldPosition.x == doctest::Approx(2.f).epsilon(1e-3));
    CHECK(res.worldPosition.y == doctest::Approx(0.f).epsilon(1e-3));
    g.snap = 0.5f;
    auto snapped = g.drag(*start, Ray{cam.eye, normalize(f.center + f.axes[0] * (f.size * 0.6f + 1.3f) - cam.eye)}, true);
    CHECK(snapped.worldPosition.x == doctest::Approx(1.5f).epsilon(1e-3));
    // Empty space hits nothing.
    CHECK(g.hitTest(f, Ray{cam.eye, normalize(Vec3{5, 5, 0} - cam.eye)}) == -1);
}

TEST_CASE("tools: scene_overview reports the open scene file and scene_save writes back to it") {
    namespace fs = std::filesystem;
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-save-" + std::to_string(std::rand()))).string();
    fs::create_directories(cfg.projectDir);
    Engine e(cfg);
    (void)e.newScene("Fresh", true);
    CHECK(call(e, "scene_overview", R"({"max_entities":0})").get("path").asString().empty());
    call(e, "scene_save", "{}", false);  // never saved: the editor asks for a name instead
    call(e, "scene_save", R"({"path":"scenes/cave.sky.json"})");
    call(e, "scene_new", R"({"name":"Other"})");
    call(e, "scene_save", R"({"path":"scenes/other.sky.json"})");
    call(e, "scene_load", R"({"path":"scenes/cave.sky.json"})");
    CHECK(call(e, "scene_overview", R"({"max_entities":0})").get("path").asString() == "scenes/cave.sky.json");
    call(e, "entity_create", R"({"name":"Stalagmite"})");
    call(e, "scene_save", "{}");  // File > Save: back into the scene that is open, not another file
    CHECK(e.callTool("scene_overview", Json::object(), "agent:test").content.front().text.find("scenes/cave.sky.json") !=
          std::string::npos);
    std::ifstream cave(fs::path(cfg.projectDir) / "scenes/cave.sky.json"), other(fs::path(cfg.projectDir) / "scenes/other.sky.json");
    std::string caveText((std::istreambuf_iterator<char>(cave)), {}), otherText((std::istreambuf_iterator<char>(other)), {});
    CHECK(caveText.find("Stalagmite") != std::string::npos);
    CHECK(otherText.find("Stalagmite") == std::string::npos);
    fs::remove_all(cfg.projectDir);
}
