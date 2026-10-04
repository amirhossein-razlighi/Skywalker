// Hair (grooms) and GPU particles: CPU-side generation, file formats, packing, presets,
// tools and render smoke tests (Null renderer always; Metal when a GPU is present).

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/fx/GpuParticles.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/fx/Particles.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeEngine(RendererBackend backend = RendererBackend::Null) {
    EngineConfig cfg;
    cfg.renderer = backend;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-hair-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Hair", false);
    return e;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

Groom smallGroom() {
    Groom g;
    g.strands = 3000;
    g.guides = 60;
    g.segments = 8;
    g.length = 0.2f;
    g.clumps = 40;
    g.clumpStrength = 0.6f;
    g.curlRadius = 0.01f;
    g.curlFrequency = 12.f;
    g.frizz = 0.002f;
    g.maskDirection = {0, 1, 0};
    g.maskAngle = 70.f;
    return g;
}

bool sameData(const GroomData& a, const GroomData& b) {
    if (a.points != b.points || a.children.size() != b.children.size() || a.guideRest.size() != b.guideRest.size()) return false;
    for (size_t i = 0; i < a.guideRest.size(); ++i) {
        if (!(a.guideRest[i] == b.guideRest[i])) return false;
    }
    for (size_t i = 0; i < a.offsets.size(); ++i) {
        if (!(a.offsets[i] == b.offsets[i])) return false;
    }
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (!(a.children[i].root == b.children[i].root) || a.children[i].guide[0] != b.children[i].guide[0]) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("hair: groom generation is deterministic, counted and masked") {
    MeshData sphere = mesh::sphere();
    Groom g = smallGroom();
    auto a = fx::generateGroom(g, &sphere);
    auto b = fx::generateGroom(g, &sphere);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->points == 9u);
    CHECK(a->strandCount() == 3000u);
    CHECK(a->guideCount() == 60u);
    CHECK(a->offsets.size() == 3000u * 9u);
    CHECK(sameData(*a, *b));  // bit-identical, even though generation is multi-threaded

    // Roots respect the cap mask (with its soft edge).
    float maxAngle = 0;
    for (const auto& c : a->children) {
        float ang = degrees(std::acos(std::clamp(normalize(c.root).y, -1.f, 1.f)));
        maxAngle = std::max(maxAngle, ang);
        float wsum = c.weight[0] + c.weight[1] + c.weight[2];
        CHECK(wsum == doctest::Approx(1.f).epsilon(1e-4));
        CHECK(c.guide[0] < 60u);
        CHECK(c.lengthScale <= 1.f);
    }
    // The mask reads the interpolated surface normal; on a coarse sphere it differs from the root's position
    // direction by up to a degree.
    CHECK(maxAngle < 70.f + g.maskSoftness + 1.f);

    // A different seed gives a different head of the same style.
    Groom g2 = g;
    g2.seed = 7;
    auto c = fx::generateGroom(g2, &sphere);
    REQUIRE(c);
    CHECK(!sameData(*a, *c));

    // Strands are about `length` long and stay outside the scalp.
    fx::StrandSet rest = fx::restStrands(*a);
    REQUIRE(rest.points.size() == 3000u * 9u);
    double total = 0;
    for (size_t s = 0; s < 3000; ++s) {
        float len = 0;
        for (size_t k = 1; k < 9; ++k) len += distance(rest.points[s * 9 + k], rest.points[s * 9 + k - 1]);
        total += len;
        CHECK(length(rest.points[s * 9 + 8]) > 0.45f);
    }
    double avg = total / 3000.0;
    CHECK(avg > 0.12);  // curls add length beyond the base curve; length variation removes some
    CHECK(avg < 0.45);

    // Nothing to grow on, or a mask that excludes everything: clear errors.
    CHECK(!fx::generateGroom(g, nullptr));
    Groom none = g;
    none.maskAngle = 0.f;
    none.maskSoftness = 0.f;
    auto e = fx::generateGroom(none, &sphere);
    REQUIRE(!e);
    CHECK(e.error().code == "empty_mask");
}

TEST_CASE("hair: GPU reconstruction reference reproduces strands under rigid motion") {
    MeshData sphere = mesh::sphere();
    auto d = fx::generateGroom(smallGroom(), &sphere);
    REQUIRE(d);
    std::vector<Vec3> rest;
    fx::reconstructStrands(*d, d->guideRest, Mat4{}, rest);
    // Rotate + translate the guides like the GPU does with the model matrix: children follow.
    Mat4 m = Mat4::trs({1, 2, 3}, {20, 35, -10}, {1, 1, 1});
    std::vector<Vec3> guides(d->guideRest.size());
    for (size_t i = 0; i < guides.size(); ++i) guides[i] = m.transformPoint(d->guideRest[i]);
    std::vector<Vec3> moved;
    fx::reconstructStrands(*d, guides, m, moved);
    REQUIRE(moved.size() == rest.size());
    float maxErr = 0;
    for (size_t i = 0; i < rest.size(); ++i) maxErr = std::max(maxErr, distance(moved[i], m.transformPoint(rest[i])));
    CHECK(maxErr < 1e-3f);
}

TEST_CASE("hair: .hair, .groom.json and .skygroom round trips") {
    fx::StrandSet s;
    s.counts = {3, 4};
    s.points = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0.5f}, {1, 0, 0}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}};
    s.widths = {0.08f, 0.05f};
    auto bytes = fx::writeHairFile(s);
    CHECK(std::memcmp(bytes.data(), "HAIR", 4) == 0);
    auto back = fx::parseHairFile(bytes);
    REQUIRE(back);
    CHECK(back->counts == s.counts);
    REQUIRE(back->points.size() == 7u);
    CHECK(back->points[2] == s.points[2]);
    REQUIRE(back->widths.size() == 2u);
    CHECK(back->widths[1] == doctest::Approx(0.05f));
    // Truncated / bad files fail cleanly.
    CHECK(!fx::parseHairFile(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 140)));
    CHECK(!fx::parseHairFile(std::vector<uint8_t>(10, 0)));

    auto json = fx::parseGroomJson(fx::writeGroomJson(s));
    REQUIRE(json);
    CHECK(json->counts == s.counts);
    CHECK(!fx::parseGroomJson(Json::parse(R"({"format":"other"})").value()));

    auto bin = fx::parseSkyGroom(fx::writeSkyGroom(s));
    REQUIRE(bin);
    CHECK(bin->strandCount() == 2u);
    CHECK(bin->counts[0] == 4u);  // resampled to a uniform point count

    // A groom built from file strands keeps every strand (subset when capped) and their shapes.
    fx::StrandSet many;
    for (int i = 0; i < 400; ++i) {
        float a = static_cast<float>(i) * 0.31f;
        many.counts.push_back(5);
        for (int k = 0; k < 5; ++k) many.points.push_back({std::cos(a) * 0.1f, 0.1f - k * 0.05f, std::sin(a) * 0.1f + k * 0.01f});
    }
    Groom g;
    g.segments = 4;
    g.strands = 300;
    auto d = fx::groomFromStrands(g, many, nullptr);
    REQUIRE(d);
    CHECK(d->strandCount() == 300u);
    fx::StrandSet rest = fx::restStrands(*d);
    // Every rebuilt strand matches one of the source strands (resampling 5 -> 5 points is exact).
    for (size_t sIdx = 0; sIdx < 20; ++sIdx) {
        Vec3 root = rest.points[sIdx * 5];
        float best = 1e9f;
        for (size_t j = 0; j < 400; ++j) best = std::min(best, distance(root, many.points[j * 5]));
        CHECK(best < 1e-4f);
    }

    // Files on disk, with unit scale and Z-up conversion.
    auto dir = fs::temp_directory_path() / ("skywalker-groom-" + AssetDatabase::newGuid().substr(0, 8));
    fs::create_directories(dir);
    std::ofstream((dir / "s.hair").string(), std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                                     static_cast<std::streamsize>(bytes.size()));
    auto loaded = fx::loadStrands((dir / "s.hair").string(), 0.01f, true);
    REQUIRE(loaded);
    CHECK(loaded->points[2].y == doctest::Approx(0.005f));   // z (0.5) -> y, scaled
    CHECK(loaded->points[2].z == doctest::Approx(-0.02f));  // -y (2) -> z, scaled
    fs::remove_all(dir);
}

TEST_CASE("hair: melanin absorption and color are physically ordered") {
    Vec3 blond = fx::hairColorFromAbsorption(fx::hairAbsorption(0.2f, 0.1f));
    Vec3 brown = fx::hairColorFromAbsorption(fx::hairAbsorption(0.7f, 0.1f));
    Vec3 black = fx::hairColorFromAbsorption(fx::hairAbsorption(0.98f, 0.1f));
    Vec3 ginger = fx::hairColorFromAbsorption(fx::hairAbsorption(0.5f, 0.9f));
    Vec3 mid = fx::hairColorFromAbsorption(fx::hairAbsorption(0.5f, 0.1f));
    CHECK(blond.x > brown.x);
    CHECK(brown.x > black.x);
    CHECK(blond.x > blond.y);
    CHECK(blond.y > blond.z);  // warm
    CHECK(ginger.x / std::max(ginger.y, 1e-6f) > mid.x / std::max(mid.y, 1e-6f));  // redder
    Vec3 none = fx::hairAbsorption(0.f, 0.f);
    CHECK(none.x == doctest::Approx(0.f));
}

TEST_CASE("vfx: curve and gradient strings bake into tables") {
    auto grad = fx::parseGradient("#ffffff@0 #ff0000@0.5 #00000000@1");
    REQUIRE(grad);
    CHECK(grad->size() == 3u);
    CHECK((*grad)[1].second.y == doctest::Approx(0.f));
    auto even = fx::parseGradient("#000 #fff");  // no positions: spread evenly
    REQUIRE(even);
    CHECK((*even)[1].first == doctest::Approx(1.f));
    CHECK(!fx::parseGradient("#ff@0"));
    CHECK(!fx::parseCurve("1@x"));
    auto curve = fx::parseCurve("0@0 1@0.5 0@1");
    REQUIRE(curve);

    ParticleEmitter em;
    em.sizeStart = 1.f;
    em.sizeEnd = 1.f;
    em.sizeCurve = "0@0 1@0.5 0@1";
    em.colorGradient = "#ffffff@0 #000000@1";
    em.opacityCurve = "1@0 0@1";
    std::string warning;
    GpuCurves c = fx::bakeCurves(em, &warning);
    CHECK(warning.empty());
    CHECK(c.size[0] == doctest::Approx(0.f));
    CHECK(c.size[16] == doctest::Approx(1.f).epsilon(0.08));
    CHECK(c.color[0].x == doctest::Approx(1.f));
    CHECK(c.color[31].w == doctest::Approx(0.f));
    em.colorGradient = "nonsense";
    fx::bakeCurves(em, &warning);
    CHECK(!warning.empty());
}

TEST_CASE("vfx: FGA vector fields and mesh surface samplers") {
    auto f = fx::parseFga("2,1,1,\n-1,-1,-1,\n1,1,1,\n1,0,0,\n0,2,0,\n");
    REQUIRE(f);
    CHECK(f->nx == 2);
    CHECK(f->data.size() == 8u);
    CHECK(f->data[5] == doctest::Approx(2.f));
    CHECK(!fx::parseFga("2,2,2,\n0,0,0,\n1,1,1,\n1,0,0,"));  // too few vectors
    CHECK(!fx::parseFga("a,b,c"));

    MeshSurface s = fx::buildMeshSurface(mesh::cube());
    CHECK(s.triangles() == 12u);
    CHECK(s.area == doctest::Approx(6.f));
    CHECK(s.cdf.back() == doctest::Approx(1.f));
    for (size_t i = 1; i < s.cdf.size(); ++i) CHECK(s.cdf[i] >= s.cdf[i - 1]);

    for (const char* key : {"fx:leaf", "fx:shard", "fx:pebble"}) {
        MeshData m;
        REQUIRE(fx::builtinParticleMesh(key, m));
        CHECK(m.indices.size() % 3 == 0);
        CHECK(m.vertexCount() > 3);
    }
    MeshData m;
    CHECK(!fx::builtinParticleMesh("fx:unknown", m));
}

TEST_CASE("vfx: GPU emitter parameter packing") {
    static_assert(sizeof(fx::GpuEmitterParams) == 1392);
    GpuEmitterItem item;
    item.world = Mat4::translate({1, 2, 3}) * Mat4::rotateY(radians(90.f));
    item.params.direction = {0, 0, 1};
    item.params.spread = 60.f;
    item.params.shape = "box";
    item.params.shapeSize = {2, 4, 6};
    item.params.facing = "ribbon";
    item.params.look = "spark";
    item.params.intensity = 5.f;
    item.params.field = "vortex";
    item.params.fieldCenter = {0, 1, 0};
    item.params.subEmitOn = "both";
    item.wind = {2, 0, 0};
    item.params.wind = 0.5f;
    item.colliders.push_back({FxCollider::Kind::Sphere, {0, 1, 0}, {0, 1, 0}, 0.75f});
    item.subEmitter = 42;
    item.curves = fx::bakeCurves(item.params);
    fx::GpuEmitterParams p = fx::packEmitter(item);
    CHECK(p.spawn[0] == doctest::Approx(static_cast<float>(fx::GpuShape::Box)));
    CHECK(p.spawn[3] == doctest::Approx(0.5f));  // cos(60 deg)
    CHECK(p.shapeSize[1] == doctest::Approx(2.f));  // half size
    CHECK(p.direction[0] == doctest::Approx(1.f).epsilon(1e-4));  // local +Z rotated 90 deg about Y -> +X
    CHECK(p.look[0] == doctest::Approx(static_cast<float>(ParticleLook::Spark)));
    CHECK(p.look[1] == doctest::Approx(static_cast<float>(fx::GpuFacing::Ribbon)));
    CHECK(p.look[2] == doctest::Approx(5.f));  // emissive looks carry intensity
    CHECK(p.wind[0] == doctest::Approx(1.f));
    CHECK(p.field[0] == doctest::Approx(1.f));
    CHECK(p.fieldCenter[1] == doctest::Approx(3.f));  // world space
    CHECK(p.collision[2] == doctest::Approx(1.f));
    CHECK(p.collision[3] == doctest::Approx(3.f));
    CHECK(p.colliders[0][7] == doctest::Approx(0.75f));
    CHECK(p.sub[2] == doctest::Approx(1.f));
    CHECK(p.world[12] == doctest::Approx(1.f));
    // A lit look is not scaled by intensity.
    item.params.look = "smoke";
    CHECK(fx::packEmitter(item).look[2] == doctest::Approx(1.f));
}

TEST_CASE("vfx & hair: presets are valid components") {
    Scene scene;
    for (const auto& name : fx::particlePresets()) {
        EntityId e = scene.create(name);
        INFO(name);
        Json patch = fx::particlePreset(name);
        CHECK(patch.size() > 3);
        CHECK(scene.patchComponent(e, "particles", patch));
        const ParticleEmitter* em = scene.get<ParticleEmitter>(e);
        if (em->simulation == "gpu") {
            std::string warning;
            fx::bakeCurves(*em, &warning);
            CHECK(warning.empty());
        }
    }
    for (const auto& name : fx::compositeEffects()) {
        INFO(name);
        CHECK(fx::compositeEffect(name).size() >= 2);
    }
    MeshData sphere = mesh::sphere();
    for (const auto& name : Groom::presets()) {
        EntityId e = scene.create(name);
        INFO(name);
        REQUIRE(scene.patchComponent(e, "groom", fx::groomPreset(name)));
        Groom g = *scene.get<Groom>(e);
        g.strands = 500;  // keep the test fast
        g.maskRadius = {0, 0, 0};  // face presets (beard, eyebrows) target a character's head region
        auto d = fx::generateGroom(g, &sphere);
        CHECK(d);
    }
    CHECK(fx::groomPreset("nope").size() == 0);
}

TEST_CASE("hair & vfx: tools and frames (null renderer)") {
    auto e = makeEngine();
    call(*e, "entity_create", R"({"name":"Head","mesh":"sphere","position":[0,1.6,0],"scale":[0.18,0.22,0.2]})");
    Json g = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_wavy","overrides":{"strands":2000,"melanin":0.2}})");
    CHECK(g.get("strands").asInt() == 2000);
    CHECK(g.get("guides").asInt() > 10);
    CHECK(g.get("gpuMemoryMB").asNumber() > 0.0);
    call(*e, "groom_create", R"({"entity":"Head","preset":"hair_nope"})", false);
    call(*e, "entity_create", R"({"name":"Empty"})");
    call(*e, "groom_create", R"({"entity":"Empty"})", false);  // no mesh
    Json u = call(*e, "groom_update", R"({"entity":"Head","fields":{"segments":6,"curlRadius":0.01,"curlFrequency":10}})");
    CHECK(u.get("pointsPerStrand").asInt() == 7);
    call(*e, "groom_update", R"({"entity":"Head","fields":{"melanine":0.3}})", false);  // typo -> did-you-mean error
    Json info = call(*e, "groom_info", "{}");
    CHECK(info.get("grooms").size() == 1);
    Json ex = call(*e, "groom_export", R"({"entity":"Head","path":"head.hair"})");
    CHECK(ex.get("strands").asInt() == 2000);
    auto loaded = fx::loadStrands(e->resolvePath("head.hair"));
    REQUIRE(loaded);
    CHECK(loaded->strandCount() == 2000u);

    // Grooms are generated in meters: the entity's scale does not scale the hair.
    const GroomData* data = e->grooms()
                                .groomFor(e->scene(), e->scene().find("Head"), [&](const std::string& k) { return e->cpuMesh(k); },
                                          [&](const std::string& p) { return e->resolvePath(p); })
                                .get();
    REQUIRE(data);
    CHECK(length(data->bounds.extents()) < 1.f);

    Json fx = call(*e, "fx_create", R"({"effect":"sparks_shower","position":[2,0,0]})");
    CHECK(fx.get("children").size() == 2);
    call(*e, "fx_create", R"({"effect":"falling_leaves","position":[0,6,0]})");
    call(*e, "fx_burst", R"({"entity":"Sparks","count":100})");
    CaptureOptions o;
    o.width = 64;
    o.height = 36;
    auto cap = e->capture(o);
    REQUIRE(cap);
    REQUIRE(cap->frame.grooms.size() == 1);
    CHECK(cap->frame.grooms[0].colliders.size() >= 1);  // the scalp proxy
    REQUIRE(cap->frame.gpuEmitters.size() == 3);
    const GpuEmitterItem* sparks = nullptr;
    const GpuEmitterItem* embers = nullptr;
    for (const auto& it : cap->frame.gpuEmitters) {
        if (it.entity == e->scene().find("Sparks")) sparks = &it;
        if (it.entity == e->scene().find("Embers")) embers = &it;
        if (it.params.facing == "mesh") CHECK(it.particleMesh == "fx:leaf");
    }
    REQUIRE(sparks);
    REQUIRE(embers);
    CHECK(sparks->subEmitter == embers->entity);
    CHECK(embers->isSubEmitter);
    CHECK(sparks->burstSerial == 100u);
    CHECK(e->particles().liveCount(sparks->entity) == 0u);  // GPU emitters are not simulated on the CPU
    Json stats = call(*e, "fx_stats", "{}");
    CHECK(stats.isObject());
}

TEST_CASE("hair & vfx: Metal smoke render (skipped without a GPU)") {
    auto e = makeEngine(RendererBackend::Auto);
    if (e->renderer().info().backend != "metal") return;
    call(*e, "entity_create", R"({"name":"Floor","mesh":"plane","scale":[10,1,10]})");
    call(*e, "entity_create", R"({"name":"Head","mesh":"sphere","position":[0,1.6,0],"scale":[0.18,0.22,0.2]})");
    call(*e, "groom_create", R"({"entity":"Head","preset":"hair_curly","overrides":{"strands":5000}})");
    call(*e, "fx_create", R"({"effect":"sparks_shower","position":[1,0,0]})");
    call(*e, "fx_create", R"({"effect":"smoke_column_gpu","position":[-1,0,0]})");
    call(*e, "fx_create", R"({"effect":"falling_leaves","position":[0,4,0]})");
    CaptureOptions o;
    o.width = 160;
    o.height = 90;
    o.samples = 1;
    o.hasCustomView = true;
    o.customView.eye = {0, 1.7f, 1.2f};
    o.customView.target = {0, 1.6f, 0};
    for (int i = 0; i < 4; ++i) {
        call(*e, "sim_control", R"({"action":"step","ticks":3})");
        auto cap = e->capture(o);
        REQUIRE(cap);
    }
    o.samples = 4;
    auto cap = e->capture(o);
    REQUIRE(cap);
    // The hair is drawn: the center of the frame (the head of hair) is not the plain sky.
    const uint8_t* c = cap->image.at(80, 40);
    CHECK((c[0] + c[1] + c[2]) > 0);
    Json stats = e->renderer().stats();
    CHECK(stats.get("hair").asBool());
    CHECK(stats.get("gpuParticles").asBool());
    REQUIRE(stats.get("grooms").size() == 1);
    CHECK(stats.get("grooms")[size_t{0}].get("strands").asInt() == 5000);
    bool anyAlive = false;
    for (const auto& em : stats.get("emitters").elements()) anyAlive = anyAlive || em.get("alive").asInt() > 0;
    CHECK(anyAlive);
}

TEST_CASE("hair: grooms stack on one mesh (scalp, beard, brows), each on its own entity") {
    auto e = makeEngine();
    call(*e, "entity_create", R"({"name":"Head","mesh":"sphere","position":[0,1.6,0],"scale":[0.18,0.22,0.2]})");
    Json a = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_short","overrides":{"strands":1500}})");
    Json b = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_wavy","overrides":{"strands":1200}})");
    Json c = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_wavy","overrides":{"strands":800}})");
    Scene& s = e->scene();
    const EntityId head = s.find("Head");
    // The first groom is on the mesh entity, the next ones on children growing on the same mesh.
    CHECK(a.get("entity").asInt() == static_cast<int64_t>(head));
    const EntityId wavy = static_cast<EntityId>(b.get("entity").asInt());
    const EntityId wavy2 = static_cast<EntityId>(c.get("entity").asInt());
    CHECK(wavy != head);
    CHECK(wavy2 != wavy);
    REQUIRE(s.record(wavy));
    REQUIRE(s.record(wavy2));
    CHECK(s.record(wavy)->parent == head);
    CHECK(s.record(wavy)->name == "Hair Wavy");
    CHECK(s.record(wavy2)->name == "Hair Wavy 2");
    CHECK(fx::groomMeshEntity(s, wavy) == head);
    CHECK(b.get("strands").asInt() == 1200);
    // All three render; groom_info on one groom entity reports that groom.
    CaptureOptions o;
    o.width = 64;
    o.height = 36;
    auto cap = e->capture(o);
    REQUIRE(cap);
    CHECK(cap->frame.grooms.size() == 3);
    Json info = call(*e, "groom_info", R"({"entity":"Head"})");
    CHECK(info.get("strands").asInt() == 1500);  // the head's own groom
    Json second = call(*e, "groom_info", R"({"entity":"Hair Wavy 2"})");
    CHECK(second.get("strands").asInt() == 800);
    // replace swaps the head's own groom instead of adding one; a custom name names the new entity.
    Json r = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_curly","replace":true,"overrides":{"strands":900}})");
    CHECK(r.get("entity").asInt() == static_cast<int64_t>(head));
    CHECK(r.get("strands").asInt() == 900);
    Json n = call(*e, "groom_create", R"({"entity":"Head","preset":"hair_short","name":"Stubble","overrides":{"strands":700}})");
    REQUIRE(s.record(static_cast<EntityId>(n.get("entity").asInt())));
    CHECK(s.record(static_cast<EntityId>(n.get("entity").asInt()))->name == "Stubble");
    // A parent without a groom (a character root) lists every groom below it.
    call(*e, "entity_create", R"({"name":"Rig"})");
    REQUIRE(s.setParent(head, s.find("Rig")));
    Json all = call(*e, "groom_info", R"({"entity":"Rig"})");
    CHECK(all.get("grooms").size() == 4);
}

TEST_CASE("hair: density-preserving LOD shares one budget fairly between many grooms") {
    auto groomAt = [](Vec3 p, uint32_t strands) {
        auto d = std::make_shared<GroomData>();
        d->children.resize(strands);
        d->points = 10;
        d->bounds = {{-0.12f, -0.12f, -0.12f}, {0.12f, 0.12f, 0.12f}};
        GroomItem it;
        it.data = d;
        it.params.widthRoot = 0.07f;
        it.params.length = 0.1f;
        it.model = Mat4::translate(p);
        return it;
    };
    fx::StrandLodView view;
    view.pixelAt1m = 2.f * std::tan(radians(30.f) * 0.5f) / 720.f;  // 30 degrees, 720 p
    // One head: everything up close, fewer (never below the floor) far away; stills draw all.
    for (float dist : {0.5f, 2.5f, 10.f, 40.f}) {
        std::vector<GroomItem> one{groomAt({0, 0, -dist}, 90000)};
        const uint32_t n = fx::groomStrandBudget(one, view)[0];
        CHECK(n >= fx::kMinStrandsPerGroom);
        CHECK(n <= 90000u);
        if (dist >= 10.f) CHECK(n < 30000u);
    }
    std::vector<GroomItem> nearHead{groomAt({0, 0, -0.5f}, 90000)}, farHead{groomAt({0, 0, -10.f}, 90000)};
    CHECK(fx::groomStrandBudget(nearHead, view)[0] > fx::groomStrandBudget(farHead, view)[0]);
    fx::StrandLodView still = view;
    still.realtime = false;
    CHECK(fx::groomStrandBudget(farHead, still)[0] == 90000u);
    // A crowd of 40 heads between 0.6 and 20 m: within the shared budget, every groom keeps its
    // floor, nearer heads get at least as many strands as farther ones.
    std::vector<GroomItem> crowd;
    for (int i = 0; i < 40; ++i) crowd.push_back(groomAt({0, 0, -(0.6f + 0.5f * static_cast<float>(i))}, 90000));
    bool limited = false;
    std::vector<uint32_t> n = fx::groomStrandBudget(crowd, view, &limited);
    uint64_t sum = 0;
    for (size_t i = 0; i < n.size(); ++i) {
        sum += n[i];
        CHECK(n[i] >= fx::kMinStrandsPerGroom);
        if (i) CHECK(n[i] <= n[i - 1]);
    }
    CHECK(sum <= fx::kRealtimeStrandBudget + n.size());
    // Stills: every strand up to the still budget (40 x 90k is over it: scaled, an equal share).
    std::vector<uint32_t> st = fx::groomStrandBudget(crowd, still, &limited);
    CHECK(limited);
    uint64_t ssum = 0;
    for (uint32_t x : st) ssum += x;
    CHECK(ssum <= fx::kStillStrandBudget + st.size());
    CHECK(st[0] == st[39]);
}
