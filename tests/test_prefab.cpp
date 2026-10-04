// Entity links (rename-proof references, %Name lookups) and linked prefabs (overrides, propagation, apply,
// revert, unpack, relink). docs/PREFABS.md describes the behavior these tests pin down.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/scene/PrefabLink.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    std::unique_ptr<Engine> engine;
    explicit Project(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-prefab-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.string();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Test", false);
    }
    ~Project() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Scene& scene() { return engine->scene(); }
    Json call(const char* tool, const std::string& args, bool expectOk = true) {
        auto parsed = Json::parse(args);
        REQUIRE(parsed.ok());
        ToolResult r = engine->callTool(tool, parsed.value(), "agent:test");
        INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
        CHECK(r.isError == !expectOk);
        return r.structured;
    }
    std::string text(const char* tool, const std::string& args) {
        ToolResult r = engine->callTool(tool, Json::parse(args).value(), "agent:test");
        return r.content.empty() ? "" : r.content.front().text;
    }
    EntityId id(const std::string& name) { return scene().find(name); }
    Json readJson(const std::string& rel) {
        std::ifstream f(dir / rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return Json::parse(ss.str()).value();
    }
    void writeJson(const std::string& rel, const Json& j) {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel) << j.dump(2);
    }
    /// A little lamp post: Post (root, behavior) > Lamp (light, mesh, unique) + Base.
    void makeLampPrefab() {
        call("entity_create", R"({"name":"Post","mesh":"cylinder","color":"#333333","position":[5,0,5]})");
        call("entity_create", R"({"name":"Lamp","parent":"Post","mesh":"sphere","color":"#ffeeaa","position":[0,2,0],
                                  "components":{"light":{"kind":"point","intensity":3}}})");
        call("entity_create", R"({"name":"Base","parent":"Post","mesh":"cube","position":[0,-0.5,0]})");
        call("entity_update", R"({"entity":"Lamp","unique":true})");
        call("prefab_create", R"({"entity":"Post","path":"prefabs/lamp.prefab.json"})");
    }
};

EntityId childNamed(Scene& s, EntityId parent, const std::string& name) {
    for (EntityId c : s.children(parent)) {
        if (s.record(c)->name == name) return c;
    }
    return kNoEntity;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// WS1: entity links
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("links: entity fields accept names, ids and objects and write {id, name}") {
    Scene s;
    EntityId door = s.create("Door");
    EntityId hinge = s.create("Hinge");
    REQUIRE(s.patchComponent(hinge, "joint", Json::object({{"target", "Door"}})).ok());
    const Joint* j = s.get<Joint>(hinge);
    CHECK(j->target.id == door);  // bound to the id: rename-proof
    CHECK(s.entityToJson(hinge).get("components").get("joint").get("target") ==
          Json::object({{"id", door}, {"name", "Door"}}));
    for (const char* form : {R"("#1")", "1", R"({"id": 1})", R"({"$entity": 1})"}) {
        REQUIRE(s.patchComponent(hinge, "joint", Json::object({{"target", Json::parse(form).value()}})).ok());
        CHECK(s.resolve(s.get<Joint>(hinge)->target, hinge) == door);
    }
    REQUIRE(s.patchComponent(hinge, "joint", Json::object({{"target", Json()}})).ok());
    CHECK(s.get<Joint>(hinge)->target.empty());
    CHECK(s.entityToJson(hinge).get("components").get("joint").get("target").isNull());
    CHECK_FALSE(s.patchComponent(hinge, "joint", Json::object({{"target", Json::array({1, 2})}})).ok());

    // Lists: arrays, or the legacy comma-separated names.
    EntityId hand = s.create("Hand");
    EntityId hair = s.create("Hair");
    REQUIRE(s.patchComponent(hair, "groom", Json::object({{"colliders", "Door, Hand"}})).ok());
    const auto& cols = s.get<Groom>(hair)->colliders;
    REQUIRE(cols.size() == 2);
    CHECK(cols[0].id == door);
    CHECK(cols[1].id == hand);

    Json schema = reflect::schema(Joint::type());
    CHECK(schema.get("properties").get("target").get("x-sky-entity").asBool());
}

TEST_CASE("links: renames keep joints, camera follow and look-at working; the file shows the new name") {
    Scene s;
    EntityId door = s.create("Door");
    EntityId hinge = s.create("Hinge");
    EntityId cam = s.create("Cam");
    EntityId hero = s.create("Hero");
    REQUIRE(s.patchComponent(hinge, "joint", Json::object({{"target", "Door"}})).ok());
    REQUIRE(s.patchComponent(cam, "camera2d", Json::object({{"follow", "Door"}})).ok());
    REQUIRE(s.patchComponent(hero, "animator", Json::object({{"lookAt", "Door"}})).ok());
    REQUIRE(s.rename(door, "Gate").ok());
    CHECK(s.resolve(s.get<Joint>(hinge)->target, hinge) == door);
    CHECK(s.resolve(s.get<Camera2D>(cam)->follow, cam) == door);
    CHECK(s.resolve(s.get<Animator>(hero)->lookAt, hero) == door);
    CHECK(s.entityToJson(hinge).get("components").get("joint").get("target").get("name").asString() == "Gate");
    CHECK(s.linksTo(door).size() == 3);
    CHECK(s.linksFrom(hinge).size() == 1);
}

TEST_CASE("links: legacy scenes with names load and bind; hand-edited dangling ids fall back to the name") {
    Scene s;
    Json doc = Json::parse(R"({"format":"skywalker.scene","entities":[
        {"id":7,"name":"Hinge","components":{"joint":{"target":"Door"}}},
        {"id":9,"name":"Door"},
        {"id":11,"name":"Cam","components":{"camera2d":{"follow":{"id":999,"name":"Door"}}}}]})").value();
    REQUIRE(s.loadJson(doc).ok());
    CHECK(s.get<Joint>(7)->target.id == 9);
    CHECK(s.get<Camera2D>(11)->follow.id == 9);  // rebound by name at load
}

TEST_CASE("links: duplicating and pasting a rig keeps it wired to itself") {
    Project p("dup");
    p.call("entity_create", R"({"name":"Arm","mesh":"cube"})");
    p.call("entity_create", R"({"name":"Hand","mesh":"sphere"})");
    p.call("entity_update", R"({"entity":"Arm","components":{"joint":{"target":"Hand"}}})");
    EntityId arm = p.id("Arm"), hand = p.id("Hand");
    Json r = p.call("entity_duplicate", R"({"entities":["Arm","Hand"],"offset":[3,0,0]})");
    REQUIRE(r.get("created").size() == 2);
    auto arm2 = static_cast<EntityId>(r.get("created")[0].asInt());
    auto hand2 = static_cast<EntityId>(r.get("created")[1].asInt());
    CHECK(p.scene().get<Joint>(arm2)->target.id == hand2);  // not the original Hand
    CHECK(p.scene().get<Joint>(arm)->target.id == hand);

    // Copy one alone: its link keeps pointing at the original target.
    Json single = p.call("entity_duplicate", R"({"entity":"Arm"})");
    auto arm3 = static_cast<EntityId>(single.get("created")[0].asInt());
    CHECK(p.scene().get<Joint>(arm3)->target.id == hand);

    // Paste: same rules.
    Json clip = p.call("entity_copy", R"({"entities":["Arm","Hand"]})");
    Json pasted = p.call("entity_paste", Json::object({{"data", clip}, {"offset", Json::array({0, 0, 4})}}).dump());
    REQUIRE(pasted.get("created").size() == 2);
    auto armP = static_cast<EntityId>(pasted.get("created")[0].asInt());
    auto handP = static_cast<EntityId>(pasted.get("created")[1].asInt());
    CHECK(p.scene().get<Joint>(armP)->target.id == handP);
    CHECK(p.scene().get<Transform>(armP)->position.z == doctest::Approx(4));
}

TEST_CASE("links: entity_refs shows incoming/outgoing links and finds dangling ones; rename reports followers") {
    Project p("refs");
    p.call("entity_create", R"({"name":"Door","mesh":"cube"})");
    p.call("entity_create", R"({"name":"Hinge","mesh":"cube","components":{"joint":{"target":"Door"}}})");
    Json refs = p.call("entity_refs", R"({"entity":"Door"})");
    CHECK(refs.get("incoming").size() == 1);
    CHECK(refs.get("incoming")[0].get("status").asString() == "ok");
    std::string renamed = p.text("entity_update", R"({"entity":"Door","name":"Gate"})");
    CHECK(renamed.find("1 link(s) follow it") != std::string::npos);
    Json clean = p.call("entity_refs", "{}");
    CHECK(clean.get("dangling").size() == 0);
    p.call("entity_delete", R"({"entity":"Gate"})");
    Json broken = p.call("entity_refs", "{}");
    REQUIRE(broken.get("dangling").size() == 1);
    CHECK(broken.get("dangling")[0].get("component").asString() == "joint");
    CHECK(broken.get("dangling")[0].get("fix").asString().find("entity_update") != std::string::npos);
}

TEST_CASE("links: Wander reads entity fields as entities and assigns entities or names") {
    Project p("wander");
    p.call("entity_create", R"({"name":"Target","mesh":"cube"})");
    p.call("entity_create", R"({"name":"Other","mesh":"cube"})");
    p.call("entity_create", R"({"name":"Cam","mesh":"cube","components":{"camera2d":{"follow":"Target"}}})");
    p.call("behavior_set", R"({"entity":"Cam","name":"swap","source":"on start\n  let t = self.camera2d.follow\n  add_tag(t, \"was_followed\")\n  self.camera2d.follow = find(\"Other\")\nend"})");
    p.call("sim_control", R"({"action":"step","ticks":2})");
    Scene& s = p.scene();
    const auto& tags = s.record(p.id("Target"))->tags;
    CHECK(std::find(tags.begin(), tags.end(), "was_followed") != tags.end());
    CHECK(s.get<Camera2D>(p.id("Cam"))->follow.id == p.id("Other"));
    p.call("sim_control", R"({"action":"stop"})");
}

// ---------------------------------------------------------------------------------------------------------------
// WS2a: linked prefabs
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("prefab: instances are linked, save as overrides and load back identically") {
    Project p("roundtrip");
    p.makeLampPrefab();
    Scene& s = p.scene();
    EntityId post = p.id("Post");
    CHECK(s.record(post)->prefab.linked());  // prefab_create links the source entity
    Json a = p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","position":[-4,0,0],"name":"Post A"})");
    Json b = p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","position":[4,0,0],"name":"Post B"})");
    auto ra = static_cast<EntityId>(a.get("entity").asInt());
    auto rb = static_cast<EntityId>(b.get("entity").asInt());
    EntityId lampA = childNamed(s, ra, "Lamp");
    REQUIRE(lampA);
    p.call("entity_update", Json::object({{"entity", lampA}, {"components", Json::parse(R"({"light":{"intensity":9}})").value()}}).dump());

    Json ov = p.call("prefab_overrides", Json::object({{"entity", ra}}).dump());
    REQUIRE(ov.get("overrides").size() == 1);
    CHECK(ov.get("overrides")[0].get("property").asString() == "light.intensity");
    CHECK(ov.get("overrides")[0].get("prefab_value").asNumber() == doctest::Approx(3));
    CHECK(ov.get("placement").contains("transform.position"));
    CHECK(p.call("prefab_overrides", Json::object({{"entity", rb}}).dump()).get("overrides").size() == 0);

    Json before = s.toJson();
    p.call("scene_save", R"({"path":"scenes/test.sky.json"})");
    Json file = p.readJson("scenes/test.sky.json");
    size_t records = file.get("entities").size();
    CHECK(records == 3);  // three instance roots; their members are implicit
    bool sawOverride = false;
    for (const auto& e : file.get("entities").elements()) {
        for (const auto& o : e.get("prefab").get("overrides").elements()) sawOverride |= o.get("property").asString() == "light.intensity";
    }
    CHECK(sawOverride);

    p.call("scene_load", R"({"path":"scenes/test.sky.json"})");
    CHECK(s.toJson() == before);  // ids, values, links and membership all come back
    CHECK(s.get<Light>(lampA)->intensity == doctest::Approx(9));
}

TEST_CASE("prefab: process modes travel through overrides, save/load, copy/paste and duplicate") {
    // The process mode is the `process` component (ecs/ProcessComponent.h); there is no second, record-level copy.
    Project p("process");
    p.makeLampPrefab();
    Scene& s = p.scene();
    Json a = p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","position":[-4,0,0],"name":"Post A"})");
    auto ra = static_cast<EntityId>(a.get("entity").asInt());
    EntityId lampA = childNamed(s, ra, "Lamp");
    REQUIRE(lampA);
    p.call("entity_update", Json::object({{"entity", lampA}, {"components", Json::parse(R"({"process":{"mode":"always","priority":2}})").value()}}).dump());
    REQUIRE(s.get<Process>(lampA));
    CHECK(s.get<Process>(lampA)->mode == "always");
    bool sawProcess = false;
    Json ovp = p.call("prefab_overrides", Json::object({{"entity", ra}}).dump());
    for (const auto& o : ovp.get("overrides").elements()) {
        sawProcess |= o.get("property").asString().rfind("process", 0) == 0;
    }
    CHECK(sawProcess);

    p.call("entity_create", R"({"name":"Menu","components":{"process":{"mode":"when_paused","clock":"real"}}})");
    EntityId menu = p.id("Menu");
    Json before = s.toJson();
    p.call("scene_save", R"({"path":"scenes/process.sky.json"})");
    p.call("scene_load", R"({"path":"scenes/process.sky.json"})");
    CHECK(s.toJson() == before);
    REQUIRE(s.get<Process>(lampA));
    CHECK(s.get<Process>(lampA)->mode == "always");
    CHECK(s.get<Process>(lampA)->priority == 2);
    REQUIRE(s.get<Process>(menu));
    CHECK(s.get<Process>(menu)->mode == "when_paused");
    CHECK(s.get<Process>(menu)->clock == "real");

    Json dup = p.call("entity_duplicate", R"({"entity":"Menu"})");
    auto menu2 = static_cast<EntityId>(dup.get("created")[0].asInt());
    REQUIRE(s.get<Process>(menu2));
    CHECK(s.get<Process>(menu2)->mode == "when_paused");
    Json clip = p.call("entity_copy", R"({"entities":["Menu"]})");
    Json pasted = p.call("entity_paste", Json::object({{"data", clip}}).dump());
    auto menu3 = static_cast<EntityId>(pasted.get("created")[0].asInt());
    REQUIRE(s.get<Process>(menu3));
    CHECK(s.get<Process>(menu3)->clock == "real");
}

TEST_CASE("prefab: editing the source updates every instance and keeps their overrides") {
    Project p("propagate");
    p.makeLampPrefab();
    Scene& s = p.scene();
    Json a = p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","name":"Post A"})");
    auto ra = static_cast<EntityId>(a.get("entity").asInt());
    EntityId lampA = childNamed(s, ra, "Lamp");
    p.call("entity_update", Json::object({{"entity", lampA}, {"components", Json::parse(R"({"light":{"intensity":9}})").value()}}).dump());

    // Change the prefab on disk: lamp color, and rename the node (overrides follow the pid, not the name).
    Json doc = p.readJson("prefabs/lamp.prefab.json");
    CHECK(doc.get("version").asInt() == 2);
    Json root = doc.get("root");
    Json kids = root.get("children");
    Json lamp = kids[0];
    lamp["name"] = "Bulb";
    lamp["components"]["mesh"]["color"] = "#ff0000";
    Json newKids = Json::array({lamp, kids[1]});
    root["children"] = newKids;
    doc["root"] = root;
    p.writeJson("prefabs/lamp.prefab.json", doc);
    fs::last_write_time(p.dir / "prefabs/lamp.prefab.json", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    p.engine->refreshAssets();

    CHECK(s.exists(lampA));                                // same entity id
    CHECK(s.record(lampA)->name == "Bulb");                // renamed by the prefab
    CHECK(reflect::toHexColor(s.get<MeshRenderer>(lampA)->color) == "#ff0000");
    CHECK(s.get<Light>(lampA)->intensity == doctest::Approx(9));  // the override survived
    EntityId lamp0 = childNamed(s, p.id("Post"), "Bulb");
    REQUIRE(lamp0);
    CHECK(s.get<Light>(lamp0)->intensity == doctest::Approx(3));
}

TEST_CASE("prefab: revert, apply (with added and removed children), undo and unpack") {
    Project p("apply");
    p.makeLampPrefab();
    Scene& s = p.scene();
    Json a = p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","name":"Post A","position":[3,0,0]})");
    auto ra = static_cast<EntityId>(a.get("entity").asInt());
    EntityId lampA = childNamed(s, ra, "Lamp");
    EntityId baseA = childNamed(s, ra, "Base");
    EntityId post = p.id("Post");

    // Revert one property.
    p.call("entity_update", Json::object({{"entity", lampA}, {"components", Json::parse(R"({"light":{"intensity":9}})").value()}}).dump());
    p.call("prefab_revert", Json::object({{"entity", lampA}, {"property", "light.intensity"}}).dump());
    CHECK(s.get<Light>(lampA)->intensity == doctest::Approx(3));
    p.call("prefab_revert", Json::object({{"entity", lampA}, {"property", "light.intensty"}}).dump(), false);  // typo -> hint

    // Override, add a child, remove a child, then apply everything.
    p.call("entity_update", Json::object({{"entity", lampA}, {"components", Json::parse(R"({"light":{"intensity":7}})").value()}}).dump());
    p.call("entity_create", Json::object({{"name", "Sign"}, {"parent", ra}, {"mesh", "quad"}}).dump());
    p.call("entity_delete", Json::object({{"entity", baseA}}).dump());
    Json ov = p.call("prefab_overrides", Json::object({{"entity", ra}}).dump());
    CHECK(ov.get("overrides").size() == 1);
    CHECK(ov.get("added").size() == 1);
    CHECK(ov.get("removed").size() == 1);

    Json applied = p.call("prefab_apply", Json::object({{"entity", ra}}).dump());
    CHECK(applied.get("applied").asInt() == 3);
    CHECK(applied.get("instances_updated").asInt() == 1);  // the original Post
    CHECK(p.call("prefab_overrides", Json::object({{"entity", ra}}).dump()).get("override_count").asInt() == 0);
    CHECK(s.get<Light>(childNamed(s, post, "Lamp"))->intensity == doctest::Approx(7));
    CHECK(childNamed(s, post, "Sign") != kNoEntity);
    CHECK(childNamed(s, post, "Base") == kNoEntity);
    Json doc = p.readJson("prefabs/lamp.prefab.json");
    CHECK(doc.get("root").get("children").size() == 2);  // Lamp + Sign

    // Undo restores the scene (the file keeps the new version).
    p.call("history", R"({"action":"undo"})");
    CHECK(childNamed(s, post, "Base") != kNoEntity);
    CHECK(childNamed(s, post, "Sign") == kNoEntity);

    // Unpack: plain entities, saved in full.
    p.call("prefab_unpack", Json::object({{"entity", ra}}).dump());
    CHECK_FALSE(s.record(ra)->prefab.linked());
    CHECK_FALSE(s.record(lampA)->prefab.linked());
}

TEST_CASE("prefab: %Name finds the unique part inside each instance; spawned prefabs are linked") {
    Project p("unique");
    p.makeLampPrefab();
    p.call("behavior_set", R"({"entity":"Post","name":"light_up","source":"on start\n  add_tag(find(\"%Lamp\"), \"lit\")\nend"})");
    p.call("prefab_apply", R"({"entity":"Post"})");
    p.call("prefab_instantiate", R"({"prefab":"prefabs/lamp.prefab.json","name":"Post B","position":[6,0,0]})");
    p.call("sim_control", R"({"action":"step","ticks":2})");
    Scene& s = p.scene();
    int lit = 0;
    for (EntityId e : s.entities()) {
        const auto& tags = s.record(e)->tags;
        if (s.record(e)->name == "Lamp") lit += std::find(tags.begin(), tags.end(), "lit") != tags.end() ? 1 : 0;
    }
    CHECK(lit == 2);  // each instance tagged its own lamp
    p.call("sim_control", R"({"action":"stop"})");

    p.call("behavior_set", R"({"entity":"Post","name":"spawner","source":"on start\n  spawn(\"prefab:prefabs/lamp.prefab.json\", (0, 0, 9), \"Spawned\")\nend"})");
    p.call("sim_control", R"({"action":"step","ticks":2})");
    EntityId spawned = p.id("Spawned");
    REQUIRE(spawned);
    CHECK(s.record(spawned)->prefab.linked());
    CHECK(s.findUnique("%Lamp", spawned) == childNamed(s, spawned, "Lamp"));
    p.call("sim_control", R"({"action":"stop"})");
}

TEST_CASE("prefab: relink flattened copies, and keep instances of missing prefabs as placeholders") {
    Project p("relink");
    p.makeLampPrefab();
    Scene& s = p.scene();
    p.call("prefab_unpack", R"({"entity":"Post"})");
    p.call("entity_duplicate", R"({"entity":"Post","offset":[2,0,0],"count":2})");
    p.call("entity_create", R"({"name":"Odd Post","mesh":"cube"})");
    Json dry = p.call("prefab_relink", R"({"prefab":"prefabs/lamp.prefab.json","dry_run":true})");
    CHECK(dry.get("linked").size() == 3);
    CHECK_FALSE(s.record(p.id("Post"))->prefab.linked());
    Json done = p.call("prefab_relink", R"({"prefab":"prefabs/lamp.prefab.json"})");
    CHECK(done.get("linked").size() == 3);
    CHECK(s.record(p.id("Post"))->prefab.linked());

    // A scene whose prefab file disappears still loads; the placeholder keeps its saved block.
    p.call("entity_update", Json::object({{"entity", childNamed(s, p.id("Post"), "Lamp")},
                                          {"components", Json::parse(R"({"light":{"intensity":5}})").value()}}).dump());
    p.call("scene_save", R"({"path":"scenes/a.sky.json"})");
    fs::rename(p.dir / "prefabs/lamp.prefab.json", p.dir / "prefabs/lamp.bak");
    p.engine->refreshAssets();
    Json loaded = p.call("scene_load", R"({"path":"scenes/a.sky.json"})");
    CHECK(loaded.get("warnings").size() == 3);
    Json health = p.call("entity_refs", "{}");
    CHECK(health.get("broken_prefabs").size() == 3);
    p.call("scene_save", R"({"path":"scenes/b.sky.json"})");
    fs::rename(p.dir / "prefabs/lamp.bak", p.dir / "prefabs/lamp.prefab.json");
    p.engine->refreshAssets();
    p.call("scene_load", R"({"path":"scenes/b.sky.json"})");
    CHECK(s.loadWarnings().empty());
    CHECK(s.get<Light>(childNamed(s, p.id("Post"), "Lamp"))->intensity == doctest::Approx(5));
}

TEST_CASE("prefab: version 1 files get stable depth-first pids and name links become internal") {
    Json doc = Json::parse(R"({"format":"skywalker.prefab","root":{"name":"Rig","children":[
        {"name":"A","components":{"joint":{"target":"B"}}},{"name":"B"}]}})").value();
    auto t = buildPrefabTemplate(doc, "rig.prefab.json");
    REQUIRE(t.ok());
    CHECK((*t)->order == std::vector<uint32_t>{1, 2, 3});
    CHECK((*t)->scene->get<Joint>(2)->target.id == 3);
    Scene s;
    auto r1 = prefab::instantiate(s, *t, kNoEntity, "Rig 1");
    auto r2 = prefab::instantiate(s, *t, kNoEntity, "Rig 2");
    REQUIRE((r1.ok() && r2.ok()));
    EntityId a2 = s.children(*r2)[0], b2 = s.children(*r2)[1];
    CHECK(s.get<Joint>(a2)->target.id == b2);  // each instance links inside itself
    auto d = prefab::diff(s, *r2);
    REQUIRE(d.ok());
    CHECK(d->count(false) == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Every example scene loads, links resolve, and saving + reloading is lossless.
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("examples: every scene loads with resolved links and survives a save/load round trip") {
    fs::path root = fs::path(SKY_SOURCE_DIR) / "examples";
    int scenes = 0, instances = 0;
    // Projects can be nested (examples/render_tests/<name>): any directory with a scenes/ folder counts.
    for (const auto& ex : fs::recursive_directory_iterator(root)) {
        if (!ex.is_directory() || ex.path().filename() == "scenes" || !fs::is_directory(ex.path() / "scenes")) continue;
        for (const auto& f : fs::directory_iterator(ex.path() / "scenes")) {
            std::string name = f.path().filename().string();
            if (name.size() < 9 || name.substr(name.size() - 9) != ".sky.json") continue;  // skips "x.sky 2.json" copies
            INFO(f.path().string());
            EngineConfig cfg;
            cfg.renderer = RendererBackend::Null;
            cfg.projectDir = ex.path().string();
            Engine e(cfg);
            REQUIRE(e.loadScene("scenes/" + name).ok());
            CHECK(e.scene().loadWarnings().empty());
            for (EntityId id : e.scene().entities()) {
                for (const auto& l : e.scene().linksFrom(id)) {
                    INFO(formatEntityRef(id) << " " << l.component << "." << l.field);
                    CHECK(l.target != kNoEntity);
                }
            }
            instances += static_cast<int>(prefab::instances(e.scene()).size());
            Json expanded = e.scene().toJson();
            Scene again;
            again.setPrefabResolver(e.scene().prefabResolver());
            REQUIRE(again.loadJson(e.scene().toFileJson()).ok());
            CHECK(again.toJson() == expanded);
            ++scenes;
        }
    }
    CHECK(scenes >= 27);
    MESSAGE("loaded " << scenes << " example scenes with " << instances << " linked prefab instances");
}
