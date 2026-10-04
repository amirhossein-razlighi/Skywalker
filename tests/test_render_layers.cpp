// Velocity buffer bookkeeping (per-object motion vectors) and render layers / light v2.
#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>

#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/render/MotionHistory.h"
#include "skywalker/render/RenderLayers.h"
#include "skywalker/render/Renderer.h"

using namespace sky;

TEST_CASE("motion history: an entity moved by (1,0,0) reports its previous transform") {
    Scene scene;
    EntityId e = scene.create("Mover");
    scene.add<MeshRenderer>(e);
    MotionHistory h;
    auto frameAt = [&](Vec3 p) {
        scene.add<Transform>(e).position = p;
        ViewCamera cam;
        return buildFrame(scene, cam, 64, 64, BuildOptions{});
    };
    FrameData f0 = frameAt({0, 0, 0});
    h.begin(false);
    REQUIRE(f0.draws.size() == 1);
    Mat4 p0 = h.previous(f0.draws[0].entity, f0.draws[0].mesh, f0.draws[0].model);
    h.end();
    CHECK_FALSE(transformChanged(p0, f0.draws[0].model));  // first frame: no history, no motion
    CHECK(h.stats().moving == 0);

    FrameData f1 = frameAt({1, 0, 0});
    h.begin(false);
    Mat4 p1 = h.previous(f1.draws[0].entity, f1.draws[0].mesh, f1.draws[0].model);
    // Shadow passes and accumulated sub-samples ask again: same answer within a frame.
    Mat4 p1b = h.previous(f1.draws[0].entity, f1.draws[0].mesh, f1.draws[0].model);
    h.end();
    Vec3 delta = f1.draws[0].model.translation() - p1.translation();
    CHECK(delta.x == doctest::Approx(1.f));
    CHECK(delta.y == doctest::Approx(0.f));
    CHECK(delta.z == doctest::Approx(0.f));
    CHECK_FALSE(transformChanged(p1, p1b));
    CHECK(h.stats().moving == 1);
    CHECK(h.stats().maxDistance == doctest::Approx(1.f));

    // Standing still: previous == current (static objects have no object motion).
    FrameData f2 = frameAt({1, 0, 0});
    h.begin(false);
    CHECK_FALSE(transformChanged(h.previous(f2.draws[0].entity, f2.draws[0].mesh, f2.draws[0].model), f2.draws[0].model));
    h.end();
}

TEST_CASE("motion history: cuts, teleports, gaps and mesh swaps start without motion") {
    MotionHistory h;
    const EntityId e = 7;
    Mat4 a = Mat4::translate({0, 0, 0}), b = Mat4::translate({2, 0, 0}), far = Mat4::translate({500, 0, 0});
    h.begin(false);
    h.previous(e, "cube", a);
    h.end();
    // A camera cut / history reset: everything starts static.
    h.begin(true);
    CHECK_FALSE(transformChanged(h.previous(e, "cube", b), b));
    h.end();
    // A teleport (beyond teleportDistance) is not motion.
    h.begin(false);
    CHECK_FALSE(transformChanged(h.previous(e, "cube", far), far));
    CHECK(h.stats().teleported == 1);
    h.end();
    // Not drawn for a frame: forgotten.
    h.begin(false);
    h.end();
    h.begin(false);
    CHECK_FALSE(transformChanged(h.previous(e, "cube", a), a));
    h.end();
    // Same entity, another mesh (swapped model, skinned instance key): its own history.
    h.begin(false);
    CHECK_FALSE(transformChanged(h.previous(e, "sphere", b), b));
    CHECK(transformChanged(h.previous(e, "cube", b), b));  // the cube moved a -> b
    h.end();
    // Deterministic: replaying the same sequence gives the same answers.
    MotionHistory h2;
    for (int i = 0; i < 3; ++i) {
        h2.begin(false);
        Mat4 m = Mat4::translate({static_cast<float>(i) * 0.5f, 0, 0});
        Mat4 prev = h2.previous(1, "cube", m);
        h2.end();
        if (i > 0) CHECK(prev.translation().x == doctest::Approx((i - 1) * 0.5f));
    }
}

// --- Render layers and light v2 -----------------------------------------------------------

TEST_CASE("render layers: camera cullMask filters draws, lights carry their mask and v2 fields") {
    Scene scene;
    EntityId world = scene.create("World");
    scene.add<MeshRenderer>(world);  // layer 1 (default)
    EntityId arms = scene.create("Arms");
    scene.add<MeshRenderer>(arms).layers = 1 << 1;  // layer 2
    EntityId cam = scene.create("Cam");
    scene.add<Transform>(cam).position = {0, 0, 5};
    scene.add<Camera>(cam);
    EntityId lamp = scene.create("Lamp");
    Light& l = scene.add<Light>(lamp);
    l.kind = "spot";
    l.cullMask = 1 << 1;
    l.specular = 0.25f;
    l.temperature = 2700.f;
    l.innerAngle = 20.f;
    l.negative = true;
    l.attenuation = "inverse_square";
    l.size = 0.3f;

    ViewCamera all;
    FrameData f = buildFrame(scene, all, 64, 64, BuildOptions{});
    CHECK(f.draws.size() == 2);
    REQUIRE(f.lights.size() == 1);
    const LightItem& li = f.lights[0];
    CHECK(li.mask == 2u);
    CHECK(li.specular == doctest::Approx(0.25f));
    CHECK(li.negative);
    CHECK(li.inverseSquare);
    CHECK(li.size == doctest::Approx(0.3f));
    CHECK(li.cosInner == doctest::Approx(std::cos(radians(20.f))));
    CHECK(li.color.x > li.color.z);  // 2700 K is warm

    // The scene camera only sees layer 1: the arms are not drawn.
    scene.get<Camera>(cam)->cullMask = 1;
    ViewCamera sc;
    REQUIRE(sceneCamera(scene, sc));
    CHECK(sc.cullMask == 1u);
    FrameData f2 = buildFrame(scene, sc, 64, 64, BuildOptions{});
    REQUIRE(f2.draws.size() == 1);
    CHECK(f2.draws[0].entity == world);
    CHECK(f2.draws[0].layers == 1u);

    // A light with an empty mask lights nothing and is not sent at all.
    l.cullMask = 0;
    CHECK(buildFrame(scene, all, 64, 64, BuildOptions{}).lights.empty());
    // Distance fade: past begin + length the light is gone, inside it is full strength.
    l.cullMask = 0xFFFFF;
    l.distanceFade = true;
    l.fadeBegin = 1.f;
    l.fadeLength = 1.f;
    ViewCamera farCam;
    farCam.eye = {0, 0, 50};
    CHECK(buildFrame(scene, farCam, 64, 64, BuildOptions{}).lights.empty());
    ViewCamera nearCam;
    nearCam.eye = {0, 0, 0.5f};
    CHECK(buildFrame(scene, nearCam, 64, 64, BuildOptions{}).lights.size() == 1);
}

TEST_CASE("render layers: masks from names, numbers and lists with did-you-mean errors") {
    auto names = render::LayerNames::fromJson(Json::parse(R"({"1": "world", "2": "player", "20": "editor_only"})").value());
    REQUIRE(names.ok());
    CHECK(names->find("Player") == 2);
    CHECK(*render::parseLayerMask(Json("player"), *names) == 2u);
    CHECK(*render::parseLayerMask(Json(5), *names) == 5u);  // a number is the raw mask
    CHECK(*render::parseLayerMask(Json::parse(R"(["world", 3, "editor_only"])").value(), *names) == (1u | 4u | (1u << 19)));
    CHECK(*render::parseLayerMask(Json("layer4"), *names) == 8u);
    CHECK(*render::parseLayerMask(Json("all"), *names) == render::kAllLayers);
    CHECK(*render::parseLayerMask(Json("none"), *names) == 0u);
    auto bad = render::parseLayerMask(Json("playr"), *names);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "unknown_layer");
    CHECK(bad.error().hint.find("player") != std::string::npos);
    CHECK_FALSE(render::parseLayerMask(Json::parse("[21]").value(), *names).ok());
    CHECK_FALSE(render::parseLayerMask(Json(2000000), *names).ok());
    CHECK(render::describeLayerMask(1u | 4u, *names) == Json::parse(R"(["world", 3])").value());
    // game.json validation
    CHECK_FALSE(render::LayerNames::fromJson(Json::parse(R"({"21": "x"})").value()).ok());
    CHECK_FALSE(render::LayerNames::fromJson(Json::parse(R"({"1": "a", "2": "a"})").value()).ok());
    CHECK_FALSE(render::LayerNames::fromJson(Json::parse(R"({"1": "all"})").value()).ok());
    auto g = game::GameSettings::fromJson(Json::parse(R"({"render": {"layers": {"2": "hero"}}})").value());
    REQUIRE(g.ok());
    CHECK(g->renderLayers.find("hero") == 2);
    CHECK(g->toJson().get("render").get("layers").get("2").asString() == "hero");
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"render": {"layer": {}}})").value()).ok());
}

TEST_CASE("light v2: Kelvin color temperature and distance fade math") {
    Vec3 white = render::kelvinToRgb(6500.f);
    CHECK(white.x > 0.95f);
    CHECK(white.y > 0.95f);
    CHECK(white.z > 0.9f);
    Vec3 warm = render::kelvinToRgb(2700.f);
    CHECK(warm.x == doctest::Approx(1.f));
    CHECK(warm.x > warm.y);
    CHECK(warm.y > warm.z);
    Vec3 cool = render::kelvinToRgb(10000.f);
    CHECK(cool.z == doctest::Approx(1.f));
    CHECK(cool.z > cool.x);
    CHECK(render::lightDistanceFade(5.f, 10.f, 4.f) == doctest::Approx(1.f));
    CHECK(render::lightDistanceFade(12.f, 10.f, 4.f) == doctest::Approx(0.5f));
    CHECK(render::lightDistanceFade(20.f, 10.f, 4.f) == doctest::Approx(0.f));
}

TEST_CASE("render_layers tool: name layers, set masks by name, undo, warnings") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("sky-layers-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.string();
        Engine engine(cfg);
        (void)engine.newScene("Layers", false);
        auto call = [&](const char* tool, const char* json, bool ok = true) {
            ToolResult r = engine.callTool(tool, Json::parse(json).value(), "agent:test");
            INFO(tool, " ", json, " -> ", (r.content.empty() ? "" : r.content.front().text));
            CHECK(r.isError == !ok);
            return r;
        };
        call("entity_create", R"({"name": "Hero", "mesh": "capsule"})");
        call("entity_create", R"({"name": "Rim", "components": {"light": {"kind": "spot"}}})");
        call("entity_create", R"({"name": "Cam", "components": {"camera": {}}})");
        call("render_layers", R"({"action": "name", "layer": 2, "name": "hero"})");
        auto settings = game::GameSettings::load(dir.string());
        REQUIRE(settings.ok());
        CHECK(settings->renderLayers.find("hero") == 2);
        call("render_layers", R"({"action": "set", "entities": ["Hero"], "layers": ["hero"], "mode": "add"})");
        Scene& s = engine.scene();
        CHECK(s.get<MeshRenderer>(s.find("Hero"))->layers == 3);
        call("render_layers", R"({"action": "set", "entities": ["Rim"], "cull_mask": "hero"})");
        CHECK(s.get<Light>(s.find("Rim"))->cullMask == 2);
        // Did-you-mean for a misspelled layer, and nothing changes.
        ToolResult bad = call("render_layers", R"({"action": "set", "entities": ["Rim"], "cull_mask": "heroo"})", false);
        CHECK(bad.content.front().text.find("hero") != std::string::npos);
        CHECK(s.get<Light>(s.find("Rim"))->cullMask == 2);
        // Undo restores the previous mask.
        call("history", R"({"action": "undo"})");
        CHECK(s.get<Light>(s.find("Rim"))->cullMask == 0xFFFFF);
        // The camera stops seeing layer 1: a warning names the meshes nobody draws.
        call("render_layers", R"({"action": "set", "entities": ["Cam"], "cull_mask": ["hero"]})");
        call("render_layers", R"({"action": "set", "entities": ["Hero"], "layers": 1})");
        ToolResult r = call("render_layers", R"({})");
        CHECK(r.structured.get("warnings").size() >= 1);
        // Wander: layer_mask() resolves the project's names at run time.
        call("behavior_set", R"({"entity": "Hero", "name": "Layers", "source": "behavior Layers\n  on start\n    self.mesh.layers = layer_mask(\"hero\", [\"1\"])\n  end\nend\n"})");
        call("sim_control", R"({"action": "step", "ticks": 2})");
        CHECK(s.get<MeshRenderer>(s.find("Hero"))->layers == 3);
        call("sim_control", R"({"action": "stop"})");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
