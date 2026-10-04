// Runtime scene flow (docs/SCENE_FLOW.md): changing scenes while playing and stopping back to the
// edited scene, carried entities and their running behaviors, transitions, loading scenes with
// progress, additive sub-scenes with ownership, determinism across a change, did-you-mean errors.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/game/SceneFlow.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    explicit Project(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-flow-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
};

std::string scene(const std::string& name, const std::string& entities) {
    return R"({"format": "skywalker.scene", "version": 1, "name": ")" + name + R"(", "seed": 7, "entities": [)" + entities + "]}";
}

std::string behavior(const std::string& source) {
    return R"("behaviors": [{"name": "Logic", "source": )" + Json(source).dump() + "}]";
}

const char* kManager = R"(var ticks = 0
var heard_unloading = ""
var heard_loaded = ""
var progress: list = []
var scenes_seen: list = []
on tick
  ticks += 1
  if loading_progress() < 1 then progress.push(loading_progress()) end
  if not scenes_seen.contains(current_scene()) then scenes_seen.push(current_scene()) end
end
on scene_unloading
  heard_unloading = data.to
end
on scene_loaded
  heard_loaded = data.id
end)";

/// menu (with a carried game manager and player), level1 (its own copy of the manager, a spawn
/// point, scenery), loading (a loading screen), rooms/cellar (a sub-scene), six prefabs for preloading.
void buildGame(const Project& p) {
    p.write("game.json", R"({"id": "flow", "scenes": {"menu": "scenes/menu.sky.json", "level1": "scenes/level1.sky.json",
                             "loading": "scenes/loading.sky.json"}, "sceneFlow": {"preloadPerTick": 2}})");
    for (int i = 1; i <= 6; ++i) {
        p.write("prefabs/p" + std::to_string(i) + ".prefab.json",
                R"({"format": "skywalker.prefab", "version": 2, "name": "p", "root": {"pid": 1, "name": "Prop)" + std::to_string(i) +
                    R"(", "enabled": true, "components": {"transform": {}}}})");
    }
    p.write("scenes/menu.sky.json",
            scene("Menu", R"({"id": 1, "name": "GameManager", "components": {"carry": {"id": "gm"}}, )" + behavior(kManager) + R"(},
                             {"id": 2, "name": "Player", "tags": ["player"], "components": {"carry": {}, "transform": {"position": [0, 1, 0]}}},
                             {"id": 3, "name": "Title", "components": {"transform": {"position": [0, 3, 0]}}},
                             {"id": 4, "name": "MenuLogic", )" + behavior("on key \"enter\"\n  change_scene(\"level1\", {transition: \"fade\", duration: 0.1, spawn_at: \"Spawn\"})\nend\n"
                                                                         "on scene_unloading\n  find(\"GameManager\").menu_heard = data.to\nend") + "}"));
    std::string props;
    for (int i = 1; i <= 6; ++i) {
        props += R"(, {"id": )" + std::to_string(20 + i) + R"(, "name": "Prop)" + std::to_string(i) + R"(", "prefab": {"source": "prefabs/p)" +
                 std::to_string(i) + R"(.prefab.json"}})";
    }
    p.write("scenes/level1.sky.json",
            scene("Level 1", R"({"id": 1, "name": "GameManager", "components": {"carry": {"id": "gm"}}, )" + behavior(kManager) + R"(},
                                {"id": 2, "name": "Spawn", "components": {"transform": {"position": [10, 0, -4], "rotation": [0, 90, 0]}}},
                                {"id": 3, "name": "Wall", "components": {"transform": {"position": [5, 0, 5]}}},
                                {"id": 4, "name": "Roller", )" + behavior("var r = 0\non tick\n  r += random(0, 1)\nend") + "}" + props));
    p.write("scenes/loading.sky.json", scene("Loading", R"({"id": 1, "name": "LoadingBar"})"));
    p.write("rooms/cellar.sky.json", scene("Cellar", R"({"id": 1, "name": "Cellar", "components": {"transform": {"position": [1, 0, 0]}}},
                                                       {"id": 2, "name": "Barrel", "parent": 1},
                                                       {"id": 3, "name": "Torch"})"));
}

std::unique_ptr<Engine> makeEngine(const Project& p, const char* sceneFile = "scenes/menu.sky.json") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Null;
    cfg.projectDir = p.dir.string();
    auto e = std::make_unique<Engine>(cfg);
    REQUIRE(e->loadScene(sceneFile));
    return e;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

/// Ticks until the pending change is done (at most `max`).
int settle(Engine& e, int max = 600) {
    int n = 0;
    while (e.sceneFlow().busy() && n < max) {
        e.step(1);
        ++n;
    }
    return n;
}

const Json& var(Engine& e, EntityId id, const char* name) { return e.scene().record(id)->vars.get(name); }

Json worldState(Engine& e) {
    Json out = Json::array();
    for (EntityId id : e.scene().entities()) {
        Json j = e.scene().entityToJson(id);
        const Transform& t = *e.scene().get<Transform>(id);
        for (float v : {t.position.x, t.position.y, t.position.z}) j["exact"].push(static_cast<double>(v));
        out.push(std::move(j));
    }
    return out;
}

}  // namespace

TEST_CASE("scene flow: changing scenes during play, then stop returns the editor to the scene play started in") {
    Project p("stop");
    buildGame(p);
    auto e = makeEngine(p);
    const Json edited = e->scene().toJson();
    const std::string editedPath = e->scenePath();
    e->play();
    e->step(2);
    CHECK(e->sceneFlow().current() == "menu");
    call(*e, "scene_change", R"({"scene": "level1"})");
    settle(*e);
    CHECK(e->sceneFlow().current() == "level1");
    CHECK(e->scene().find("Wall") != kNoEntity);
    CHECK(e->scene().find("Title") == kNoEntity);
    call(*e, "scene_additive_load", R"({"scene": "rooms/cellar.sky.json"})");
    e->step(3);
    e->stop();
    CHECK(e->scene().toJson() == edited);
    CHECK(e->scenePath() == editedPath);
    CHECK(e->sceneFlow().current().empty());
    CHECK(e->sceneFlow().info().get("additive").size() == 0);
    // Play again: the game starts over in the menu.
    e->play();
    CHECK(e->sceneFlow().current() == "menu");
    e->stop();
}

TEST_CASE("scene flow: carried entities survive with their running behaviors; duplicates give way; spawn_at") {
    Project p("persist");
    buildGame(p);
    auto e = makeEngine(p);
    e->play();
    e->step(5);
    const EntityId gm = e->scene().find("GameManager"), player = e->scene().find("Player"), title = e->scene().find("Title");
    REQUIRE(gm);
    const int64_t ticksBefore = var(*e, gm, "ticks").asInt();
    CHECK(ticksBefore == 5);
    EntityId maxBefore = 0;
    for (EntityId id : e->scene().entities()) maxBefore = std::max(maxBefore, id);

    e->input().pressed.insert("enter");  // MenuLogic: change_scene("level1", {fade 0.1 s, spawn_at: "Spawn"})
    e->step(1);
    const int ticks = settle(*e);
    CHECK(ticks > 6);  // fade out (6 ticks) + preload + fade in
    CHECK(e->sceneFlow().current() == "level1");
    // The running manager and player are the same entities, still ticking.
    CHECK(e->scene().exists(gm));
    CHECK(e->scene().exists(player));
    CHECK_FALSE(e->scene().exists(title));
    CHECK(var(*e, gm, "ticks").asInt() == ticksBefore + 1 + ticks);
    size_t managers = 0;
    for (EntityId id : e->scene().entities()) managers += e->scene().record(id)->name == "GameManager" ? 1 : 0;
    CHECK(managers == 1);  // level1's own copy gave way
    // New scene entities have fresh ids (never reused in a play session).
    const EntityId wall = e->scene().find("Wall");
    CHECK(wall > maxBefore);
    // spawn_at: the player stands on Spawn.
    CHECK(e->scene().get<Transform>(player)->position == Vec3{10, 0, -4});
    CHECK(e->scene().get<Transform>(player)->rotation.y == doctest::Approx(90.f));
    // Events: the manager heard both sides of the change.
    CHECK(var(*e, gm, "heard_unloading").asString() == "level1");
    CHECK(var(*e, gm, "heard_loaded").asString() == "level1");
    CHECK(var(*e, gm, "menu_heard").asString() == "level1");  // the leaving scene heard it before it went
    // scene_flow_info lists the carried entities.
    Json info = call(*e, "scene_flow_info", "{}");
    std::vector<std::string> kept;
    for (const auto& n : info.get("carried").elements()) kept.push_back(n.asString());
    CHECK(std::find(kept.begin(), kept.end(), "GameManager") != kept.end());
    CHECK(std::find(kept.begin(), kept.end(), "Player") != kept.end());
}

TEST_CASE("scene flow: fade transitions are state the renderer reads, and loading scenes report progress") {
    Project p("fade");
    buildGame(p);
    auto e = makeEngine(p);
    e->play();
    e->step(1);
    auto brightness = [&] {
        CaptureOptions o;
        o.width = 32;
        o.height = 18;
        o.samples = 1;
        auto shot = e->capture(o);
        REQUIRE(shot);
        double sum = 0;
        for (size_t i = 0; i < shot->image.pixels.size(); i += 4) sum += shot->image.pixels[i] + shot->image.pixels[i + 1] + shot->image.pixels[i + 2];
        return sum / static_cast<double>(shot->image.pixels.size());
    };
    const double clear = brightness();
    call(*e, "scene_change", R"({"scene": "level1", "transition": {"kind": "fade", "duration": 0.25, "color": "#000000"}, "loading": "loading"})");
    auto alpha = [&] {
        CaptureOptions o;
        o.width = 64;
        o.height = 36;
        return e->frame(o).fade.alpha;
    };
    e->step(1);  // activation: the menu hears on scene_unloading
    CHECK(alpha() == doctest::Approx(0.f));
    e->step(8);
    const float mid = alpha();
    CHECK(mid > 0.2f);
    CHECK(mid < 1.f);
    CHECK(brightness() < clear * 0.9);  // the CPU renderer fades the picture too
    e->step(8);  // fully out: the loading scene comes in
    Json info = call(*e, "scene_flow_info", "{}");
    CHECK(info.get("transition").get("phase").asString() == "loading");
    CHECK(e->sceneFlow().current() == "loading");
    CHECK(e->scene().find("LoadingBar") != kNoEntity);
    settle(*e);
    CHECK(e->sceneFlow().current() == "level1");
    CHECK(alpha() == doctest::Approx(0.f));
    // The manager saw the progress climb in steps of 2 of 6 prefabs, and the loading scene in between.
    const EntityId gm = e->scene().find("GameManager");
    const Json& progress = var(*e, gm, "progress");
    REQUIRE(progress.size() >= 3);
    double last = -1;
    bool steps = false;
    for (const auto& v : progress.elements()) {
        CHECK(v.asNumber() >= last);
        steps = steps || std::abs(v.asNumber() - 1.0 / 3.0) < 1e-6;
        last = v.asNumber();
    }
    CHECK(steps);
    bool sawLoading = false;
    for (const auto& s : var(*e, gm, "scenes_seen").elements()) sawLoading = sawLoading || s.asString() == "loading";
    CHECK(sawLoading);
    CHECK(e->sceneFlow().progress() == doctest::Approx(1.0));
}

TEST_CASE("scene flow: additive sub-scenes own exactly the entities they loaded") {
    Project p("additive");
    buildGame(p);
    auto e = makeEngine(p);
    e->play();
    e->step(1);
    const size_t before = e->scene().size();
    Json a = call(*e, "scene_additive_load", R"({"scene": "rooms/cellar.sky.json", "offset": [40, 0, 0]})");
    CHECK(a.get("handle").asString() == "cellar");
    CHECK(a.get("entities").asInt() == 3);
    CHECK(e->scene().size() == before + 3);
    const EntityId cellar = e->scene().find("Cellar");
    REQUIRE(cellar);
    CHECK(e->scene().get<Transform>(cellar)->position == Vec3{41, 0, 0});
    CHECK(e->scene().get<Transform>(e->scene().find("Torch"))->position == Vec3{40, 0, 0});
    // A second copy gets its own handle.
    Json b = call(*e, "scene_additive_load", R"({"scene": "rooms/cellar.sky.json"})");
    CHECK(b.get("handle").asString() == "cellar#2");
    CHECK(e->scene().size() == before + 6);
    // Something the game puts in the cellar is not the cellar's.
    const EntityId loot = e->scene().create("Loot", cellar);
    Json u = call(*e, "scene_additive_unload", R"({"handle": "cellar"})");
    CHECK(u.get("removed").asInt() == 3);
    REQUIRE(u.get("orphans").size() == 1);
    CHECK(u.get("orphans")[0].asInt() == static_cast<int64_t>(loot));
    CHECK(e->scene().exists(loot));
    CHECK(e->scene().record(loot)->parent == kNoEntity);
    CHECK_FALSE(e->scene().exists(cellar));
    CHECK(e->scene().size() == before + 3 + 1);  // cellar#2 and the loot
    // Unknown handles get a did-you-mean; Wander unloads take a tick to be heard first.
    ToolResult r = e->callTool("scene_additive_unload", Json::parse(R"({"handle": "celar#2"})").value(), "agent:test");
    REQUIRE(r.isError);
    CHECK(r.content.front().text.find("cellar#2") != std::string::npos);
    const EntityId logic = e->scene().find("MenuLogic");
    REQUIRE(e->scene().setBehaviors(logic, Json::array({Json::object({{"name", "L"}, {"source", "on key \"u\"\n  unload_scene(\"cellar#2\")\nend\non key \"a\"\n  load_additive(\"rooms/cellar\", {id: \"attic\"})\nend"}})})));
    e->input().pressed.insert("a");
    e->step(1);
    CHECK(e->sceneFlow().info().get("additive").size() == 2);
    e->input().pressed.insert("u");
    e->step(1);
    CHECK(e->sceneFlow().info().get("additive").size() == 2);  // heard on scene_unloading, goes next tick
    e->step(1);
    CHECK(e->sceneFlow().info().get("additive").size() == 1);
}

TEST_CASE("scene flow: a scene change replays deterministically") {
    Project p("determinism");
    buildGame(p);
    auto run = [&] {
        auto e = makeEngine(p);
        e->play();
        e->step(10);
        e->input().pressed.insert("enter");
        e->step(90);
        Json w = worldState(*e);
        w.push(Json(static_cast<double>(e->runtime().frame())));
        return w;
    };
    const Json a = run();
    const Json b = run();
    CHECK(a == b);
}

TEST_CASE("scene flow: unknown scenes get a did-you-mean; changes need play mode; settings are validated") {
    Project p("errors");
    buildGame(p);
    auto e = makeEngine(p);
    ToolResult editing = e->callTool("scene_change", Json::parse(R"({"scene": "level1"})").value(), "agent:test");
    REQUIRE(editing.isError);
    CHECK(editing.structured.get("error").asString() == "not_playing");
    e->play();
    ToolResult typo = e->callTool("scene_change", Json::parse(R"({"scene": "levl1"})").value(), "agent:test");
    REQUIRE(typo.isError);
    CHECK(typo.structured.get("error").asString() == "scene_not_found");
    CHECK(typo.content.front().text.find("did you mean 'level1'") != std::string::npos);
    ToolResult badOpt = e->callTool("scene_change", Json::parse(R"({"scene": "level1", "transition": "fadee"})").value(), "agent:test");
    REQUIRE(badOpt.isError);
    CHECK(badOpt.content.front().text.find("fade") != std::string::npos);
    // Files without an alias resolve too, with or without folder and extension.
    CHECK(e->sceneFlow().resolve("rooms/cellar").value() == "rooms/cellar.sky.json");
    CHECK(e->sceneFlow().resolve("rooms/cellar.sky.json").value() == "rooms/cellar.sky.json");
    CHECK(e->sceneFlow().resolve("level1").value() == "scenes/level1.sky.json");
    CHECK(e->sceneFlow().resolve("loading.sky.json").value() == "scenes/loading.sky.json");
    CHECK(e->sceneFlow().resolve("scenes/loading.sky.json").value() == "scenes/loading.sky.json");
    // Wander: the handler fails with the same message.
    const EntityId logic = e->scene().find("MenuLogic");
    REQUIRE(e->scene().setBehaviors(logic, Json::array({Json::object({{"name", "L"}, {"source", "on start\n  change_scene(\"nowhere\")\nend"}})})));
    e->step(2);
    bool logged = false;
    for (const auto& m : e->recentMessages(50)) logged = logged || m.get("text").asString().find("no scene 'nowhere'") != std::string::npos;
    CHECK(logged);
    CHECK(e->sceneFlow().current() == "menu");
    // game.json validation.
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"sceneFlow": {"cary": ["A"]}})").value()));
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"sceneFlow": {"persistent": ["A"]}})").value()));  // the old name
    CHECK(game::GameSettings::fromJson(Json::parse(R"({"sceneFlow": {"carry": ["Music"]}})").value()));
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"scenes": {"menu": 3}})").value()));
    CHECK(game::GameSettings::fromJson(Json::parse(R"({"scenes": {"menu": "scenes/menu.sky.json"}, "sceneFlow": {"transition": "fade"}})").value()));
}
