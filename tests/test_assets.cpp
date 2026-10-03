#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/render/Gltf.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

/// A fresh, empty project directory for one test case.
struct TempProject {
    fs::path dir;
    explicit TempProject(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-test-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
};

std::unique_ptr<Engine> makeEngine(const TempProject& p) {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = p.dir.string();
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Test", true);
    return e;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

/// A one-triangle GLB with a red base color: positions (3 x vec3) + uint16 indices.
std::vector<uint8_t> makeTriangleGlb() {
    std::vector<uint8_t> bin;
    auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    const float pos[9] = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    const uint16_t idx[3] = {0, 1, 2};
    put(pos, sizeof(pos));
    put(idx, sizeof(idx));
    while (bin.size() % 4) bin.push_back(0);
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0,"translation":[5,0,0]}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
        "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],"metallicFactor":0.25,"roughnessFactor":0.5}}],
        "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},
                     {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    while (json.size() % 4) json.push_back(' ');
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    u32(static_cast<uint32_t>(json.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json.begin(), json.end());
    u32(static_cast<uint32_t>(bin.size()));
    out.insert(out.end(), {'B', 'I', 'N', 0});
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

}  // namespace

TEST_CASE("assets: database assigns stable GUIDs that survive moves") {
    TempProject p("db");
    p.write("meshes/rock.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    p.write("textures/grass.png", "not really a png");
    p.write("build/ignored.obj", "v 0 0 0\n");
    AssetDatabase db(p.dir.string());
    db.refresh();
    CHECK(db.size() == 2);
    const AssetRecord* rock = db.find("meshes/rock.obj");
    REQUIRE(rock);
    CHECK((rock->type == AssetType::Mesh));
    std::string guid = rock->guid;
    CHECK_FALSE(fs::exists(p.dir / "meshes/rock.obj.meta"));  // scanning never writes files
    REQUIRE(db.registerFile("meshes/rock.obj"));
    CHECK(fs::exists(p.dir / "meshes/rock.obj.meta"));
    CHECK(db.find("meshes/rock.obj")->guid == guid);  // registering keeps the id

    REQUIRE(db.updateMeta("asset:meshes/rock.obj", Json::parse(R"({"tags":["nature","prop"],"description":"mossy boulder"})").value()));
    CHECK(db.query({AssetType::Unknown, "nature", "", 50}).size() == 1);
    CHECK(db.query({AssetType::Unknown, "", "boulder", 50}).size() == 1);
    CHECK(db.query({AssetType::Texture, "", "", 50}).size() == 1);

    REQUIRE(db.move("meshes/rock.obj", "props/boulder.obj"));
    CHECK_FALSE(db.find("meshes/rock.obj"));
    const AssetRecord* moved = db.find("guid:" + guid);
    REQUIRE(moved);
    CHECK(moved->path == "props/boulder.obj");
    CHECK(fs::exists(p.dir / "props/boulder.obj.meta"));

    // A fresh database reads the same GUID and metadata back from the .meta sidecar.
    AssetDatabase again(p.dir.string());
    again.refresh();
    const AssetRecord* r = again.find("props/boulder.obj");
    REQUIRE(r);
    CHECK(r->guid == guid);
    CHECK(r->description == "mossy boulder");
}

TEST_CASE("assets: glTF binary parsing applies node transforms and material") {
    auto glb = makeTriangleGlb();
    auto g = parseGltf(glb, ".", /*normalize=*/false);
    REQUIRE_MESSAGE(g, (g ? "" : g.error().message));
    CHECK(g->mesh.indices.size() == 3);
    CHECK(g->mesh.bounds.min.x == doctest::Approx(5));  // node translation applied
    CHECK(g->mesh.bounds.max.x == doctest::Approx(7));
    CHECK(g->baseColor.x == doctest::Approx(1));
    CHECK(g->baseColor.y == doctest::Approx(0));
    CHECK(g->metallic == doctest::Approx(0.25));

    std::vector<uint8_t> garbage = {'g', 'l', 'T', 'F', 2, 0, 0, 0};
    CHECK_FALSE(parseGltf(garbage, ".", false));
}

TEST_CASE("assets: importing a GLB creates a mesh asset and a material") {
    TempProject p("glb");
    auto glb = makeTriangleGlb();
    fs::create_directories(p.dir / "models");
    std::ofstream(p.dir / "models/tri.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    auto e = makeEngine(p);
    Json r = call(*e, "asset_import", R"({"path":"models/tri.glb","create_entity":"Tri","position":[1,0,1],"tags":["test"]})");
    CHECK(r.get("mesh").asString() == "asset:models/tri.glb");
    std::string mat = r.get("material").asString();
    CHECK_FALSE(mat.empty());
    const ResolvedMaterial* m = e->resolveMaterial(mat);
    REQUIRE(m);
    CHECK(m->color.x == doctest::Approx(1));
    EntityId tri = e->scene().find("Tri");
    REQUIRE(tri);
    CHECK(e->scene().get<MeshRenderer>(tri)->material == mat);
    CHECK(e->scene().get<Transform>(tri)->position == Vec3{1, 0, 1});
    Json info = call(*e, "asset_info", R"({"asset":"models/tri.glb"})");
    CHECK(info.get("usedBy").size() == 1);
    CHECK(info.get("tags").size() == 1);
}

TEST_CASE("assets: materials are created, assigned, updated and validated") {
    TempProject p("mat");
    auto e = makeEngine(p);
    call(*e, "material_create", R"({"path":"materials/glow.mat.json","color":"#ff0000","emissive":"#ffaa00","roughness":0.3})");
    call(*e, "material_create", R"({"path":"materials/glow.mat.json"})", false);  // exists
    call(*e, "material_create", R"({"path":"materials/bad.json"})", false);       // wrong extension
    call(*e, "material_create", R"({"path":"materials/x.mat.json","roughness":"shiny"})", false);
    call(*e, "material_assign", R"({"entities":["Cube"],"material":"materials/glow.mat.json"})");
    CHECK(e->scene().get<MeshRenderer>(e->scene().find("Cube"))->material == "materials/glow.mat.json");
    call(*e, "material_assign", R"({"entities":["Cube"],"material":"materials/nope.mat.json"})", false);

    call(*e, "material_update", R"({"path":"materials/glow.mat.json","roughness":0.9})");
    const ResolvedMaterial* m = e->resolveMaterial("materials/glow.mat.json");
    REQUIRE(m);
    CHECK(m->roughness == doctest::Approx(0.9));

    // The frame uses the material instead of the inline color.
    CaptureOptions o;
    FrameData f = e->frame(o);
    bool found = false;
    for (const auto& d : f.draws) {
        if (d.entity == e->scene().find("Cube")) {
            found = true;
            CHECK(d.roughness == doctest::Approx(0.9));
        }
    }
    CHECK(found);
}

TEST_CASE("assets: prefabs round-trip and spawn from tools and Wander") {
    TempProject p("prefab");
    auto e = makeEngine(p);
    Json house = call(*e, "entity_create", R"({"name":"House","mesh":"cube","position":[3,0.5,3],"tags":["building"]})");
    auto houseId = static_cast<EntityId>(house.get("id").asInt());
    call(*e, "entity_create", R"({"name":"Roof","mesh":"cone","parent":"House","position":[0,1,0]})");
    call(*e, "behavior_set", R"({"entity":"House","name":"spin","source":"on tick\n  rotate self by (0, 10 * dt, 0)\nend"})");
    call(*e, "prefab_create", R"({"entity":"House","path":"prefabs/house.prefab.json","description":"a small house"})");
    CHECK(fs::exists(p.dir / "prefabs/house.prefab.json"));

    size_t before = e->scene().size();
    Json inst = call(*e, "prefab_instantiate", R"({"prefab":"prefabs/house.prefab.json","position":[-4,0,0],"name":"House B"})");
    CHECK(e->scene().size() == before + 2);
    auto b = static_cast<EntityId>(inst.get("entity").asInt());
    REQUIRE(e->scene().exists(b));
    CHECK(b != houseId);
    CHECK(e->scene().record(b)->name == "House B");
    CHECK(e->scene().get<Transform>(b)->position.x == doctest::Approx(-4));
    CHECK(e->scene().children(b).size() == 1);
    CHECK(e->scene().get<Behavior>(b));

    call(*e, "history", R"({"action":"undo"})");
    CHECK(e->scene().size() == before);

    // Wander: spawn("prefab:...") during play.
    call(*e, "behavior_set", R"({"entity":"Cube","name":"builder","source":"on start\n  let h = spawn(\"prefab:prefabs/house.prefab.json\", (0, 0, 6))\nend"})");
    call(*e, "sim_control", R"({"action":"step","ticks":2})");
    size_t houses = 0;
    for (EntityId id : e->scene().entities()) houses += e->scene().record(id)->name == "House" ? 1 : 0;
    CHECK(houses == 2);
    call(*e, "sim_control", R"({"action":"stop"})");
}

TEST_CASE("world: raycast, place_on_surface and deterministic scatter") {
    TempProject p("world");
    auto e = makeEngine(p);
    Json hit = call(*e, "raycast", R"({"origin":[5,10,5],"direction":[0,-1,0]})");
    CHECK(hit.get("hit").asBool());
    CHECK(hit.get("name").asString() == "Ground");
    CHECK(hit.get("point")[1].asFloat() == doctest::Approx(0).epsilon(0.001));
    Json top = call(*e, "raycast", R"({"origin":[0,10,0]})");
    CHECK(top.get("name").asString() == "Cube");
    CHECK(top.get("point")[1].asFloat() == doctest::Approx(1));

    call(*e, "entity_create", R"({"name":"Ball","mesh":"sphere","position":[4,7,4]})");
    call(*e, "place_on_surface", R"({"entities":["Ball"]})");
    EntityId ball = e->scene().find("Ball");
    CHECK(e->scene().get<Transform>(ball)->position.y == doctest::Approx(0.5).epsilon(0.01));

    // Dropping onto the cube's top face.
    call(*e, "transform", R"({"entity":"Ball","position":[0,6,0]})");
    call(*e, "place_on_surface", R"({"entities":["Ball"]})");
    CHECK(e->scene().get<Transform>(ball)->position.y == doctest::Approx(1.5).epsilon(0.01));

    auto positions = [&](EntityId group) {
        std::vector<Vec3> out;
        for (EntityId c : e->scene().children(group)) out.push_back(e->scene().get<Transform>(c)->position);
        return out;
    };
    Json s1 = call(*e, "scatter", R"({"source":"Ball","count":25,"size":[30,30],"min_distance":2,"seed":7,"scale":[0.5,1.5]})");
    CHECK(s1.get("count").asInt() == 25);
    auto g1 = static_cast<EntityId>(s1.get("group").asInt());
    auto p1 = positions(g1);
    REQUIRE(p1.size() == 25);
    for (size_t i = 0; i < p1.size(); ++i) {
        for (size_t j = i + 1; j < p1.size(); ++j) {
            float dx = p1[i].x - p1[j].x, dz = p1[i].z - p1[j].z;
            CHECK(dx * dx + dz * dz >= 4.f - 1e-3f);
        }
    }
    // Each copy rests on the ground (or the cube) given its random scale.
    for (EntityId c : e->scene().children(g1)) {
        const Transform* t = e->scene().get<Transform>(c);
        CHECK(t->position.y - 0.5f * t->scale.y >= -0.01f);
    }
    call(*e, "history", R"({"action":"undo"})");
    CHECK_FALSE(e->scene().exists(g1));

    Json s2 = call(*e, "scatter", R"({"source":"Ball","count":25,"size":[30,30],"min_distance":2,"seed":7,"scale":[0.5,1.5]})");
    auto p2 = positions(static_cast<EntityId>(s2.get("group").asInt()));
    REQUIRE(p2.size() == p1.size());
    for (size_t i = 0; i < p1.size(); ++i) CHECK(p1[i] == p2[i]);
}

TEST_CASE("world: sim_trace samples properties and restores the scene") {
    TempProject p("trace");
    auto e = makeEngine(p);
    call(*e, "behavior_set", R"({"entity":"Cube","name":"rise","source":"var score = 0\non tick\n  move self by (0, dt, 0)\n  score = score + 1\nend"})");
    Json t = call(*e, "sim_trace", R"({"entities":["Cube"],"properties":["transform.position","vars.score"],"ticks":60,"every":30})");
    const Json& samples = t.get("samples");
    REQUIRE(samples.size() == 3);
    EntityId cube = e->scene().find("Cube");
    std::string key = std::to_string(cube);
    float y0 = samples[0].get(key).get("transform.position")[1].asFloat();
    float y2 = samples[2].get(key).get("transform.position")[1].asFloat();
    CHECK(y2 - y0 == doctest::Approx(1).epsilon(0.05));
    CHECK(samples[2].get(key).get("vars.score").asInt() == 60);
    CHECK((e->playState() == PlayState::Editing));
    CHECK(e->scene().get<Transform>(cube)->position.y == doctest::Approx(0.5));
}

TEST_CASE("assets: moving an asset rewrites scene references and keeps its GUID") {
    TempProject p("move");
    auto e = makeEngine(p);
    call(*e, "material_create", R"({"path":"materials/a.mat.json","color":"#00ff00"})");
    call(*e, "material_assign", R"({"entities":["Cube","Ground"],"material":"materials/a.mat.json"})");
    std::string guid = e->assets().find("materials/a.mat.json")->guid;
    call(*e, "asset_move", R"({"asset":"materials/a.mat.json","to":"materials/lib/green.mat.json"})");
    CHECK(e->scene().get<MeshRenderer>(e->scene().find("Cube"))->material == "materials/lib/green.mat.json");
    CHECK(e->scene().get<MeshRenderer>(e->scene().find("Ground"))->material == "materials/lib/green.mat.json");
    REQUIRE(e->assets().find("guid:" + guid));
    CHECK(e->assets().find("guid:" + guid)->path == "materials/lib/green.mat.json");
    CHECK(e->resolveMaterial("materials/lib/green.mat.json"));
    call(*e, "asset_move", R"({"asset":"materials/lib/green.mat.json","to":"materials/green.png"})", false);  // type change
    Json list = call(*e, "asset_list", R"({"type":"material"})");
    CHECK(list.get("assets").size() == 1);
}
