// scene_audit (render/SceneAudit.h + the tool) and game.json mounts (shared kits outside a project).
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "skywalker/anim/Controller.h"
#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/game/Packager.h"
#include "skywalker/render/Image.h"
#include "skywalker/render/SceneAudit.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

/// A scene, a camera at (0, 1.5, 6) looking at (0, 1, 0), and the frame it sees.
struct AuditFixture {
    Scene scene;
    ViewCamera cam;
    audit::Sources src;
    std::unordered_map<std::string, std::shared_ptr<MeshData>> meshes;

    AuditFixture() {
        cam.lookFrom({0, 1.5f, 6}, {0, 1, 0});
        cam.fovDeg = 50.f;
        src.mesh = [this](const std::string& key) -> const MeshData* {
            auto& slot = meshes[key];
            if (!slot) {
                auto m = mesh::primitive(key);
                if (!m) return nullptr;
                slot = std::make_shared<MeshData>(std::move(*m));
            }
            return slot.get();
        };
        src.resolvePath = [](const std::string& p) { return p; };
    }

    EntityId add(const char* name, const char* meshName, Vec3 pos, Vec3 scale = {1, 1, 1}, EntityId parent = kNoEntity) {
        EntityId e = scene.create(name, parent);
        auto& t = scene.add<Transform>(e);
        t.position = pos;
        t.scale = scale;
        scene.add<MeshRenderer>(e).mesh = meshName;
        return e;
    }

    Json audit(audit::Options o = {}) {
        FrameData f = buildFrame(scene, cam, 1920, 1080, BuildOptions{});
        return audit::auditFrame(scene, f, src, o, "test");
    }
};

bool hasWarning(const Json& r, const std::string& code) {
    for (const auto& w : r.get("warnings").elements()) {
        if (w.get("code").asString() == code) return true;
    }
    return false;
}

float coverageOf(const Json& r, const std::string& name) {
    for (const auto& p : r.get("primitives").elements()) {
        if (p.get("name").asString() == name) return p.get("coverage").asFloat();
    }
    return 0.f;
}

}  // namespace

TEST_CASE("audit: primitives on screen get coverage; strict fails above 0.1%") {
    AuditFixture fx;
    fx.add("Crate", "cube", {0, 1, 0});
    Json r = fx.audit();
    REQUIRE(r.get("primitives").size() == 1);
    const float cov = coverageOf(r, "Crate");
    // A 1 m cube 6 m away with a 50 degree lens: a few percent of the image.
    CHECK(cov > 1.f);
    CHECK(cov < 10.f);
    CHECK(r.get("primitiveCoverage").asFloat() == doctest::Approx(cov));
    CHECK(r.get("pass").asBool() == false);  // above the 2% default too
    CHECK(hasWarning(r, "primitive_coverage"));

    fx.scene.get<Transform>(fx.scene.find("Crate"))->scale = {0.3f, 0.3f, 0.3f};  // small: ~0.2% of the image
    Json small = fx.audit();
    CHECK(small.get("pass").asBool());  // under the 2% default
    audit::Options strict;
    strict.strict = true;
    Json s = fx.audit(strict);
    CHECK(s.get("pass").asBool() == false);  // over 0.1%
    CHECK(s.get("primitiveLimit").asFloat() == doctest::Approx(0.1f));
}

TEST_CASE("audit: occlusion - hidden primitives do not count") {
    AuditFixture fx;
    fx.add("Wall", "cube", {0, 1, 2}, {6, 4, 0.2f});
    fx.add("Hidden Ball", "sphere", {0, 1, 0}, {0.5f, 0.5f, 0.5f});  // behind the wall
    Json r = fx.audit();
    CHECK(coverageOf(r, "Hidden Ball") == 0.f);
    CHECK(coverageOf(r, "Wall") > 10.f);

    // A ground plane under water: the water sheet hides it.
    AuditFixture w;
    w.add("Seabed", "plane", {0, -2, 0}, {200, 1, 200});
    EntityId sea = w.scene.create("Sea");
    w.scene.add<Transform>(sea).position = {0, 0, 0};
    w.scene.add<Water>(sea);
    FrameData f = buildFrame(w.scene, w.cam, 1920, 1080, BuildOptions{});
    WaterItem wi;
    wi.entity = sea;
    wi.level = 0.f;
    f.water.push_back(wi);
    Json rw = audit::auditFrame(w.scene, f, w.src, {}, "water");
    CHECK(coverageOf(rw, "Seabed") == 0.f);
    CHECK(rw.get("stats").get("waterCoverage").asFloat() > 20.f);
}

TEST_CASE("audit: instanced foliage occludes props behind it and its materials are audited") {
    AuditFixture fx;
    fx.add("Hidden Crate", "cube", {0, 1, 0}, {0.6f, 0.6f, 0.6f});
    FrameData f = buildFrame(fx.scene, fx.cam, 1920, 1080, BuildOptions{});
    // A hedge of instanced boxes between the camera and the crate; an untextured flat green.
    EntityId hedge = fx.scene.create("Hedge");
    auto parts = std::make_shared<std::vector<InstancePart>>();
    InstancePart part;
    part.mesh = "cube";
    part.surface.color = {0.2f, 0.5f, 0.2f, 1.f};
    parts->push_back(part);
    auto instances = std::make_shared<std::vector<world::FoliageInstance>>();
    for (int i = -3; i <= 3; ++i) {
        world::FoliageInstance in{};
        // Rows of a 1 x 2.5 x 0.6 m box at (i, 1.25, 2).
        in.row0[0] = 1.f, in.row0[3] = static_cast<float>(i);
        in.row1[1] = 2.5f, in.row1[3] = 1.25f;
        in.row2[2] = 0.6f, in.row2[3] = 2.f;
        instances->push_back(in);
    }
    InstanceBatch batch;
    batch.entity = hedge;
    batch.parts = parts;
    batch.instances = instances;
    batch.cullDistance = 100.f;
    f.instances.push_back(batch);
    Json r = audit::auditFrame(fx.scene, f, fx.src, {}, "foliage");
    CHECK(coverageOf(r, "Hidden Crate") == 0.f);
    CHECK(r.get("stats").get("foliageCoverage").asFloat() > 10.f);
    CHECK(r.get("stats").get("foliageInstancesRasterized").asInt() == 7);
    REQUIRE(r.get("foliage").size() == 1);
    CHECK(r.get("foliage")[0].get("parts")[0].get("issue").asString() == "untextured");
    CHECK(hasWarning(r, "untextured_foliage"));
}

TEST_CASE("audit: primitive characters - body arrangement and animated primitives; props are not characters") {
    AuditFixture fx;
    EntityId visitor = fx.scene.create("Visitor 1");
    fx.scene.add<Transform>(visitor).position = {-1.2f, 0, 0};
    fx.add("Visitor 1 Body", "capsule", {0, 0.6f, 0}, {0.4f, 1.2f, 0.4f}, visitor);
    fx.add("Visitor 1 Head", "sphere", {0, 1.42f, 0}, {0.32f, 0.32f, 0.32f}, visitor);

    EntityId guard = fx.add("Guard", "cube", {1.2f, 0.9f, 0}, {0.5f, 1.8f, 0.4f});
    fx.scene.add<Animator>(guard);

    // A market stall: a small lamp bulb on a wide counter is not a body.
    EntityId stall = fx.scene.create("Stall");
    fx.scene.add<Transform>(stall).position = {0, 0, -2};
    fx.add("Stall Counter", "cube", {0, 0.5f, 0}, {2.5f, 1, 0.8f}, stall);
    fx.add("Stall Bulb", "sphere", {0.8f, 1.1f, 0}, {0.08f, 0.08f, 0.08f}, stall);

    Json r = fx.audit();
    const Json& chars = r.get("primitiveCharacters");
    std::vector<std::string> names;
    for (const auto& c : chars.elements()) names.push_back(c.get("name").asString());
    CHECK(std::find(names.begin(), names.end(), "Visitor 1") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Guard") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Stall") == names.end());
    CHECK(hasWarning(r, "primitive_character"));
    CHECK_FALSE(r.get("pass").asBool());

    // With kit characters available, the hint names the matching one.
    fx.src.kitCharacters = {"kit/characters/guard.prefab.json", "kit/characters/villager_male.prefab.json"};
    Json h = fx.audit();
    bool suggested = false;
    for (const auto& c : h.get("primitiveCharacters").elements()) {
        if (c.get("name").asString() == "Guard") suggested = c.get("suggestion").asString() == "kit/characters/guard.prefab.json";
    }
    CHECK(suggested);
}

TEST_CASE("audit: default and untextured materials, missing textures, default sky") {
    AuditFixture fx;
    EntityId grey = fx.scene.create("Grey");
    fx.scene.add<Transform>(grey).position = {-1, 1, 0};
    fx.scene.add<MeshRenderer>(grey).mesh = "cube";  // default color, no material
    EntityId red = fx.add("Red", "cube", {1, 1, 0});
    fx.scene.get<MeshRenderer>(red)->color = {0.8f, 0.1f, 0.1f, 1.f};
    EntityId broken = fx.add("Broken", "cube", {0, 2.2f, 0}, {0.5f, 0.5f, 0.5f});
    fx.scene.get<MeshRenderer>(broken)->texture = "/nonexistent/tex_albedo.png";
    Json r = fx.audit();
    std::map<std::string, std::string> issues;
    for (const auto& m : r.get("materials").elements()) issues[m.get("name").asString()] = m.get("issue").asString();
    CHECK(issues["Grey"] == "default");
    CHECK(issues["Red"] == "untextured");
    CHECK(issues.count("Broken") == 0);  // it has a texture (that is missing)
    CHECK(hasWarning(r, "default_material"));
    CHECK(hasWarning(r, "untextured_material"));
    CHECK(hasWarning(r, "missing_texture"));
    CHECK(hasWarning(r, "default_sky"));

    audit::Options stylized;
    stylized.stylized = true;
    Json s = fx.audit(stylized);
    for (const auto& w : s.get("warnings").elements()) {
        if (w.get("code").asString() == "untextured_material") CHECK(w.get("severity").asString() == "info");
    }
}

TEST_CASE("audit: image headers") {
    fs::path dir = fs::temp_directory_path() / ("sky-audit-img-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    Image img(37, 21);
    REQUIRE(writePng(img, (dir / "a.png").string()));
    int w = 0, h = 0;
    CHECK(audit::imageSize((dir / "a.png").string(), w, h));
    CHECK(w == 37);
    CHECK(h == 21);
    // A minimal JPEG header: SOI, APP0 (empty payload), SOF0 with 8-bit precision, 10 x 20.
    const unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x02, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x14, 0x00, 0x0A,
                                 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xD9, 0, 0, 0, 0, 0};
    std::ofstream(dir / "b.jpg", std::ios::binary).write(reinterpret_cast<const char*>(jpg), sizeof(jpg));
    CHECK(audit::imageSize((dir / "b.jpg").string(), w, h));
    CHECK(w == 10);
    CHECK(h == 20);
    CHECK_FALSE(audit::imageSize((dir / "missing.png").string(), w, h));
    fs::remove_all(dir);
}

TEST_CASE("scene_audit tool: views, strict, cameras, sequences and errors") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    (void)engine.newScene("Audit", true);  // default scene: a ground plane and a cube
    auto call = [&](const char* json) { return engine.callTool("scene_audit", Json::parse(json).value(), "agent:test"); };

    ToolResult r = call(R"({"camera": {"eye": [0, 2, 5], "target": [0, 0.5, 0], "fov": 45}})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("pass").asBool() == false);
    CHECK(r.structured.get("primitiveCount").asInt() == 2);  // the ground plane and the cube
    CHECK(r.content.front().text.rfind("FAIL", 0) == 0);

    // Looking at the sky only: nothing on screen.
    r = call(R"({"camera": {"eye": [0, 2, 5], "target": [0, 50, 0]}, "strict": true})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("primitiveCount").asInt() == 0);

    r = call(R"({"cameras": [{"eye": [0, 2, 5], "target": [0, 0.5, 0], "label": "front"}, {"eye": [0, 2, 5], "target": [0, 50, 0]}]})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("views").asInt() == 2);
    CHECK(r.structured.get("results").size() == 2);
    CHECK(r.structured.get("pass").asBool() == false);

    r = call(R"({"view": "scene", "max_primitive_coverage": 100})");
    REQUIRE_FALSE(r.isError);

    CHECK(call(R"({"sequence": "cinematics/none.sequence.json"})").isError);
    CHECK(call(R"({"camera": {"target": [0, 0, 0]}})").isError);
    CHECK(call(R"({"view": "sideways"})").isError);
}

TEST_CASE("animator: a rigged model without clips reports its state instead of crashing") {
    auto lib = std::make_shared<anim::Library>();
    lib->skeleton.bones.push_back({"root", -1, {}});
    auto ctl = std::make_shared<anim::ControllerDef>(anim::simpleController({}, "", true));
    anim::AnimatorRuntime rt;
    REQUIRE(rt.init(lib, ctl, nullptr));
    CHECK(rt.stateName() == "");
    rt.update(0.1f);
    anim::Pose pose;
    rt.evaluate(pose);
    Json state = rt.stateJson();
    CHECK(state.get("layers").size() == ctl->layers.size());
}

TEST_CASE("mounts: game.json mounts resolve kit/ paths, scan, import and package") {
    fs::path base = fs::temp_directory_path() / ("sky-mounts-" + std::to_string(::getpid()));
    fs::path project = base / "game", kit = base / "_kit";
    fs::create_directories(project / "scenes");
    fs::create_directories(kit / "materials");
    fs::create_directories(kit / "props");
    std::ofstream(kit / "materials" / "brass.mat.json")
        << R"({"format": "skywalker.material", "version": 1, "color": "#c09040", "metallic": 1, "roughness": 0.3})";
    std::ofstream(kit / "props" / "lamp.prefab.json")
        << R"({"format": "skywalker.prefab", "version": 1, "root": {"name": "Lamp", "components": {"mesh": {"mesh": "cylinder", "material": "kit/materials/brass.mat.json"}}}})";
    std::ofstream(project / "game.json") << R"({"id": "mounted", "title": "Mounted", "mounts": {"kit": "../_kit"}})";

    SUBCASE("settings") {
        auto g = game::GameSettings::load(project.string());
        REQUIRE(g);
        REQUIRE(g->mounts.size() == 1);
        CHECK(g->mounts[0].first == "kit");
        CHECK(g->toJson().get("mounts").get("kit").asString() == "../_kit");
        auto bad = game::GameSettings::fromJson(Json::parse(R"({"mounts": {"my kit": "../x"}})").value());
        CHECK_FALSE(bad);
        std::vector<std::string> warnings;
        auto m = game::GameSettings::readMounts(project.string(), &warnings);
        REQUIRE(m.size() == 1);
        CHECK(fs::path(m[0].second) == (base / "_kit").lexically_normal());
        CHECK(warnings.empty());
    }
    SUBCASE("asset database") {
        AssetDatabase db(project.string());
        db.setMounts({{"kit", kit.string()}});
        db.refresh();
        CHECK(db.find("kit/materials/brass.mat.json") != nullptr);
        CHECK(db.absolute("kit/props/lamp.prefab.json") == (kit / "props" / "lamp.prefab.json").lexically_normal().string());
        CHECK(db.relative((kit / "props" / "lamp.prefab.json").string()) == "kit/props/lamp.prefab.json");
        CHECK(db.relative("scenes/main.sky.json") == "scenes/main.sky.json");
        CHECK(db.mountOf("kit/a.png") != nullptr);
        CHECK(db.mountOf("kitchen/a.png") == nullptr);
        db.setMounts({});
        db.refresh();
        CHECK(db.find("kit/materials/brass.mat.json") == nullptr);
    }
    SUBCASE("engine and packaging") {
        {
            EngineConfig cfg;
            cfg.renderer = RendererBackend::Null;
            cfg.projectDir = project.string();
            Engine engine(cfg);
            (void)engine.newScene("Mounted", false);
            CHECK(engine.resolvePath("kit/props/lamp.prefab.json") == (kit / "props" / "lamp.prefab.json").lexically_normal().string());
            ToolResult r = engine.callTool("prefab_instantiate", Json::parse(R"({"prefab": "kit/props/lamp.prefab.json", "name": "Lamp"})").value(),
                                           "agent:test");
            INFO(r.content.front().text);
            REQUIRE_FALSE(r.isError);
            EntityId lamp = engine.scene().find("Lamp");
            REQUIRE(lamp);
            CHECK(engine.scene().get<MeshRenderer>(lamp)->material == "kit/materials/brass.mat.json");
            REQUIRE(engine.saveScene("scenes/main.sky.json"));
            ToolResult s = engine.callTool("game_settings", Json::parse(R"({"operation": "get"})").value(), "agent:test");
            CHECK(s.structured.get("mounted").contains("kit"));
        }
        auto settings = game::GameSettings::load(project.string());
        REQUIRE(settings);
        auto files = game::collectGameFiles(project.string(), *settings);
        REQUIRE(files);
        CHECK(files->files.count("kit/props/lamp.prefab.json") == 1);  // a linked prefab instance references its prefab
        CHECK(files->mounted.count("kit/props/lamp.prefab.json") == 1);
        CHECK(files->files.count("kit/materials/brass.mat.json") == 1);
        CHECK(files->mounted.count("kit/materials/brass.mat.json") == 1);
        CHECK(files->missing.empty());
    }
    fs::remove_all(base);
}
