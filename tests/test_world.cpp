#include <doctest/doctest.h>

#include <filesystem>

#include "skywalker/engine/Engine.h"
#include "skywalker/world/Foliage.h"
#include "skywalker/world/Terrain.h"

using namespace sky;
namespace fs = std::filesystem;

TEST_CASE("world: terrain generation is deterministic, eroded and round-trips") {
    world::TerrainData a(129, 256.f), b(129, 256.f);
    auto p = world::terrainPreset("island_beach");
    REQUIRE(p);
    world::generate(a, *p);
    world::generate(b, *p);
    CHECK(a.heights() == b.heights());
    CHECK(a.minHeight() < p->seaLevel);  // sea floor
    CHECK(a.maxHeight() > p->seaLevel + 2.f);  // land
    // A beach: plenty of gentle ground just above sea level.
    int beach = 0;
    for (int z = 0; z < 129; ++z) {
        for (int x = 0; x < 129; ++x) {
            float wx = (x / 128.f - 0.5f) * 256.f, wz = (z / 128.f - 0.5f) * 256.f;
            if (a.h(x, z) > 0.f && a.h(x, z) < 2.f && a.slopeDegAt(wx, wz) < 8.f) ++beach;
        }
    }
    CHECK(beach > 100);

    auto bytes = a.serialize();
    auto c = world::TerrainData::deserialize(bytes);
    REQUIRE(c);
    CHECK(c->heights() == a.heights());
    CHECK(c->weights() == a.weights());
    CHECK(!world::TerrainData::deserialize(std::vector<uint8_t>(10, 0)));

    float h = 0;
    CHECK(a.heightAt(0, 0, h));
    CHECK(!a.heightAt(1000, 0, h));
    float t = a.raycast({0, 200, 0}, {0, -1, 0}, 1000);
    CHECK(t == doctest::Approx(200 - h).epsilon(0.01));
}

TEST_CASE("world: sculpt, paint and auto-paint rules") {
    world::TerrainData t(65, 64.f);
    world::sculpt(t, {0, 0}, 10, 5, world::SculptMode::Raise);
    float h = 0;
    t.heightAt(0, 0, h);
    CHECK(h == doctest::Approx(5).epsilon(0.05));
    world::sculpt(t, {0, 0}, 20, 1, world::SculptMode::Flatten, 2.f, 0.f);
    t.heightAt(0, 0, h);
    CHECK(h == doctest::Approx(2).epsilon(0.05));
    world::paint(t, {0, 0}, 5, 2, 1.f, 0.f);
    CHECK(t.weightsAt(32, 32)[2] > 200);
    Json layers = Json::parse(R"([{"name":"low"},{"name":"high","heightMin":1.0,"noise":0,"sharpness":1}])").value();
    world::autoPaint(t, layers, 1);
    CHECK(t.weightsAt(32, 32)[1] > 200);  // the raised plateau is "high"
    CHECK(t.weightsAt(0, 0)[0] > 200);    // the flat corner stays "low"
}

TEST_CASE("world: foliage chunks are deterministic and respect rules") {
    auto layers = world::foliageLayersFromJson(Json::parse(R"([{"preset":"meadow_grass","density":10,"slopeMax":20}])").value());
    REQUIRE(layers.size() == 1);
    world::SurfaceFn flat = [](float x, float z, world::SurfaceSample& s) {
        s.y = 1.f;
        s.normal = x > 8 ? normalize(Vec3{1, 1, 0}) : Vec3{0, 1, 0};  // steep for x > 8
        return true;
    };
    auto a = world::scatterChunk(layers[0], 0, 7, 0, 0, 16.f, flat, 0.4f);
    auto b = world::scatterChunk(layers[0], 0, 7, 0, 0, 16.f, flat, 0.4f);
    REQUIRE(a.size() == b.size());
    CHECK(a.size() > 400);
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i].row0[3] == b[i].row0[3]);
    for (const auto& inst : a) CHECK(inst.row0[3] <= 8.5f);  // nothing on the 45-degree half
    world::FoliageCache cache;
    int budget = 1000;
    auto chunks = cache.visibleChunks(layers[0], 0, 7, {0, 0, 0}, {-50, -50}, {50, 50}, flat, 0.4f, budget);
    CHECK(!chunks.empty());
    CHECK(cache.chunkCount() >= chunks.size());  // empty (all-steep) chunks are cached but not drawn
}

TEST_CASE("world: terrain and foliage tools, frame items and raycasts") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-world-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    Engine e(cfg);
    (void)e.newScene("World", false);
    ToolResult r = e.callTool("terrain_create", Json::parse(R"({"preset":"island_beach","size":256,"resolution":129,"water":true})").value(), "agent:test");
    INFO(r.content.front().text);
    REQUIRE(!r.isError);
    EntityId terrain = static_cast<EntityId>(r.structured.get("entity").asInt());
    CHECK(fs::exists(e.resolvePath(e.scene().get<Terrain>(terrain)->data)));
    CHECK(fs::exists(e.resolvePath("textures/terrain/sand.png")));
    r = e.callTool("foliage_add", Json::parse(R"({"entity":"Terrain","layers":[{"preset":"dune_grass"},{"preset":"shells"}]})").value(), "agent:test");
    REQUIRE(!r.isError);
    r = e.callTool("foliage_add", Json::parse(R"({"entity":"Terrain","layers":[{"preset":"grasss"}]})").value(), "agent:test");
    CHECK(r.isError);

    r = e.callTool("terrain_query", Json::parse(R"({"points":[[0,0],[5000,0]]})").value(), "agent:test");
    REQUIRE(!r.isError);
    CHECK(r.structured.get("points")[size_t{1}].get("terrain").asBool(true) == false);
    float before = static_cast<float>(r.structured.get("points")[size_t{0}].get("height").asFloat());
    r = e.callTool("terrain_sculpt", Json::parse(R"({"entity":"Terrain","strokes":[{"x":0,"z":0,"radius":20,"strength":4,"mode":"raise"}]})").value(), "agent:test");
    REQUIRE(!r.isError);
    float y = 0;
    REQUIRE(e.world().terrainHeight(e.scene(), 0, 0, y));
    CHECK(y > before + 3.f);
    r = e.callTool("terrain_undo", Json::parse(R"({"entity":"Terrain"})").value(), "agent:test");
    REQUIRE(!r.isError);
    REQUIRE(e.world().terrainHeight(e.scene(), 0, 0, y));
    CHECK(y == doctest::Approx(before).epsilon(0.05));

    auto hit = e.raycast(Ray{{0, 300, 0}, {0, -1, 0}});
    REQUIRE(hit);
    CHECK(hit->entity == terrain);

    CaptureOptions o;
    o.width = 64;
    o.height = 36;
    o.hasCustomView = true;
    o.customView.eye = {0, 30, 60};
    o.customView.target = {0, 0, 0};
    auto cap = e.capture(o);
    REQUIRE(cap);
    REQUIRE(cap->frame.terrains.size() == 1);
    CHECK(cap->frame.terrains[0].layers.size() >= 4);
    CHECK(!cap->frame.instances.empty());
}

TEST_CASE("world: vegetation meshes stand on the ground with sane sizes") {
    for (const char* name : {"grass", "grass_tall", "fern", "flowers", "pebbles", "shell", "rock"}) {
        INFO(name);
        auto m = mesh::primitive(name);
        REQUIRE(m.ok());
        CHECK(m->indices.size() % 3 == 0);
        for (uint32_t i : m->indices) REQUIRE(i < m->vertexCount());
        CHECK(m->bounds.min.y > -0.05f);
        CHECK(m->bounds.max.y > 0.005f);
        CHECK(m->bounds.max.y < 1.5f);
        CHECK(m->hasVertexColors);
    }
}

#include "skywalker/render/ColorGrading.h"

TEST_CASE("grading: looks bake valid LUTs and .cube files parse") {
    for (const auto& look : grading::looks()) {
        auto lut = grading::lookLut(look, 9);
        REQUIRE(lut);
        CHECK(lut->rgb.size() == 9u * 9 * 9 * 3);
        for (float v : lut->rgb) CHECK((v >= 0.f && v <= 1.f));
    }
    auto none = grading::lookLut("none", 5);
    REQUIRE(none);
    CHECK(none->rgb[3] == doctest::Approx(0.25f));  // identity: second red step
    CHECK(!grading::lookLut("tealorange"));
    auto cube = grading::parseCube("TITLE \"t\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    REQUIRE(cube);
    CHECK(cube->size == 2);
    CHECK(cube->rgb[3] == doctest::Approx(1.f));
    CHECK(!grading::parseCube("LUT_3D_SIZE 2\n0 0 0\n"));
}

#include "skywalker/render/LightClusters.h"

TEST_CASE("render: light clusters list exactly the lights that can reach them") {
    FrameData f;
    f.width = 1600;
    f.height = 900;
    f.camera.eye = {0, 2, 10};
    f.camera.target = {0, 2, 0};
    f.view = f.camera.view();
    f.projection = f.camera.projection(16.f / 9.f);
    LightItem sun;
    sun.kind = LightItem::Kind::Directional;
    f.lights.push_back(sun);
    for (int i = 0; i < 200; ++i) {
        LightItem l;
        l.position = {static_cast<float>(i % 20) - 10.f, 1.f, -static_cast<float>(i / 20) * 3.f};
        l.range = 2.f;
        f.lights.push_back(l);
    }
    LightItem behind;
    behind.position = {0, 2, 30};  // behind the camera: in no cluster
    behind.range = 3.f;
    f.lights.push_back(behind);
    LightGrid g = buildLightGrid(f);
    CHECK(g.directionalCount == 1);
    CHECK(g.cells.size() == g.clusterCount() * 2);
    size_t refs = 0;
    for (size_t c = 0; c < g.clusterCount(); ++c) {
        for (uint32_t k = 0; k < g.cells[c * 2 + 1]; ++k) {
            uint32_t li = g.indices[g.cells[c * 2] + k];
            CHECK(li != 0);    // directional lights are not in clusters
            CHECK(li != 201);  // the light behind the camera is culled
            ++refs;
        }
    }
    CHECK(refs > 200);
    // The pixel under a light's center lists that light.
    const LightItem& probe = f.lights[105];
    Vec4 h = f.projection * f.view * Vec4(probe.position, 1.f);
    float px = (h.x / h.w * 0.5f + 0.5f) * f.width, py = (0.5f - h.y / h.w * 0.5f) * f.height;
    float z = dot(probe.position - f.camera.eye, normalize(f.camera.target - f.camera.eye));
    int c = g.clusterAt(px, py, f.width, f.height, z);
    bool found = false;
    for (uint32_t k = 0; k < g.cells[static_cast<size_t>(c) * 2 + 1]; ++k) found = found || g.indices[g.cells[static_cast<size_t>(c) * 2] + k] == 105;
    CHECK(found);
}

TEST_CASE("world: terrains are solid for physics") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-world-phys-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    Engine e(cfg);
    (void)e.newScene("World", false);
    ToolResult r = e.callTool("terrain_create", Json::parse(R"({"preset":"rolling_hills","size":128,"resolution":129})").value(), "agent:test");
    REQUIRE(!r.isError);
    REQUIRE(e.scene().get<Collider>(static_cast<EntityId>(r.structured.get("entity").asInt())));
    float ground = 0;
    REQUIRE(e.world().terrainHeight(e.scene(), 5, 5, ground));
    r = e.callTool("entity_create", Json::parse(R"({"name":"Ball","position":[5,80,5],"components":{"mesh":{"mesh":"sphere"}}})").value(), "agent:test");
    REQUIRE(!r.isError);
    r = e.callTool("physics_add", Json::parse(R"({"entities":["Ball"],"preset":"prop"})").value(), "agent:test");
    INFO(r.content.front().text);
    REQUIRE(!r.isError);
    e.play();
    e.step(60 * 6);
    float y = e.scene().get<Transform>(e.scene().find("Ball"))->position.y;
    e.stop();
    CHECK(y > ground - 0.5f);  // landed on the hills, not through them
    CHECK(y < ground + 3.f);
}
