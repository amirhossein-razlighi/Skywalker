#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>

#include "skywalker/engine/Engine.h"
#include "skywalker/fx/Ocean.h"
#include "skywalker/fx/Particles.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeFxEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-fx-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("FX", false);
    return e;
}

Json callTool(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

float heightStdDev(fx::Ocean& o) {
    double sum = 0, sum2 = 0;
    int n = 0;
    for (float x = 0; x < 200; x += 3.7f) {
        for (float z = 0; z < 200; z += 3.7f) {
            float h = o.height(x, z);
            sum += h;
            sum2 += static_cast<double>(h) * h;
            ++n;
        }
    }
    double mean = sum / n;
    return static_cast<float>(std::sqrt(std::max(0.0, sum2 / n - mean * mean)));
}

}  // namespace

TEST_CASE("fx: radix-2 FFT round trip") {
    const int n = 16;
    float re[n], im[n], r0[n], i0[n];
    for (int i = 0; i < n; ++i) {
        re[i] = r0[i] = std::sin(i * 0.7f) + 0.3f * i;
        im[i] = i0[i] = std::cos(i * 1.3f);
    }
    fx::fft1d(re, im, n, false);
    fx::fft1d(re, im, n, true);
    for (int i = 0; i < n; ++i) {
        CHECK(re[i] / n == doctest::Approx(r0[i]).epsilon(1e-4));
        CHECK(im[i] / n == doctest::Approx(i0[i]).epsilon(1e-4));
    }
}

TEST_CASE("fx: FFT ocean has wind-driven, deterministic, evolving waves") {
    Water breezy;
    breezy.windSpeed = 8;
    fx::Ocean a, b;
    a.configure(breezy);
    b.configure(breezy);
    a.evaluate(3.0);
    b.evaluate(3.0);
    float sd = heightStdDev(a);
    // Significant wave height (4 sigma) of a breezy sea: roughly half a meter to a few meters.
    CHECK(sd * 4 > 0.4f);
    CHECK(sd * 4 < 5.0f);
    CHECK(a.height(12.3f, -40.f) == doctest::Approx(b.height(12.3f, -40.f)));  // replayable
    float h3 = a.height(5, 5);
    a.evaluate(4.5);
    CHECK(a.height(5, 5) != doctest::Approx(h3));  // the sea moves
    Vec3 n = a.normal(5, 5);
    CHECK(n.y > 0.5f);

    Water calm = breezy;
    calm.windSpeed = 1.5f;
    fx::Ocean c;
    c.configure(calm);
    c.evaluate(3.0);
    CHECK(heightStdDev(c) < sd * 0.3f);  // gentle wind, small waves

    auto cas = a.cascades();
    REQUIRE(cas);
    CHECK(cas->resolution == fx::Ocean::kN);
    CHECK(cas->displacement[0].size() == static_cast<size_t>(fx::Ocean::kN * fx::Ocean::kN * 4));
}

TEST_CASE("fx: particle emitters spawn, age, collide and replay exactly") {
    Scene scene;
    EntityId e = scene.create("Rain");
    scene.get<Transform>(e)->position = {0, 5, 0};
    ParticleEmitter& em = scene.add<ParticleEmitter>(e);
    em.look = "rain";
    em.rate = 200;
    em.lifetime = 3;
    em.lifetimeJitter = 0;
    em.shape = "box";
    em.shapeSize = {4, 0, 4};
    em.direction = {0, -1, 0};
    em.speed = 6;
    em.gravity = 9.8f;
    em.collide = true;
    em.floorHeight = 0;
    em.splash = 2;
    em.prewarm = false;

    fx::ParticleSystem a, b;
    for (int i = 0; i < 30; ++i) {
        a.update(scene, 1.f / 60.f);
        b.update(scene, 1.f / 60.f);
    }
    size_t n = a.liveCount(e);
    CHECK(n > 80);
    CHECK(n == b.liveCount(e));
    for (int i = 0; i < 60; ++i) a.update(scene, 1.f / 60.f);  // drops hit the floor and splash

    ViewCamera cam;
    cam.eye = {0, 2, 12};
    cam.target = {0, 1, 0};
    std::vector<ParticleInstance> out;
    std::vector<LightItem> lights;
    a.gather(scene, cam, out, lights);
    REQUIRE(!out.empty());
    bool splashes = false;
    for (const auto& p : out) {
        splashes = splashes || p.look == static_cast<float>(ParticleLook::Splash);
        CHECK(p.position[1] > -0.5f);  // nothing falls through the floor
    }
    CHECK(splashes);
    auto dist2 = [&](const ParticleInstance& p) {
        Vec3 d{p.position[0] - cam.eye.x, p.position[1] - cam.eye.y, p.position[2] - cam.eye.z};
        return dot(d, d);
    };
    for (size_t i = 1; i < out.size(); ++i) CHECK(dist2(out[i - 1]) >= dist2(out[i]) - 1e-3f);  // back to front

    a.burst(e, 50);
    size_t before = a.liveCount(e);
    a.update(scene, 1.f / 60.f);
    CHECK(a.liveCount(e) >= before + 40);

    scene.destroy(e);
    a.update(scene, 1.f / 60.f);
    CHECK(a.emitterCount() == 0);
}

TEST_CASE("fx: fires light the scene with a flickering light") {
    Scene scene;
    EntityId e = scene.create("Fire");
    Json patch = fx::particlePreset("fire");
    REQUIRE(scene.patchComponent(e, "particles", patch));
    CHECK(scene.get<ParticleEmitter>(e)->look == "flame");
    fx::ParticleSystem ps;
    ps.update(scene, 1.f / 60.f);  // prewarmed: already burning
    ViewCamera cam;
    std::vector<ParticleInstance> out;
    std::vector<LightItem> lights;
    ps.gather(scene, cam, out, lights);
    CHECK(out.size() > 5);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].intensity > 1.f);
}

TEST_CASE("fx: fuel-less fluid presets (steam, smoke) do not glow like flames") {
    // The volume shader turns heat into blackbody emission; steam and smoke inject heat only
    // for buoyancy, so their presets must switch the flame emission off.
    for (const char* name : {"steam_vent", "volume_smoke"}) {
        Json p = fx::fluidPreset(name);
        INFO(name);
        REQUIRE(p.size() > 0);
        CHECK(p.get("fuel").asNumber() == 0.0);
        CHECK(p.get("flameIntensity").asNumber() == 0.0);
    }
    CHECK(fx::fluidPreset("volume_fire").get("flameIntensity").asNumber() > 0.0);
}

TEST_CASE("fx: tools, Wander and water queries") {
    auto e = makeFxEngine();
    Json fire = callTool(*e, "fx_create", R"({"effect":"campfire","position":[2,0,0]})");
    REQUIRE(fire.get("children").size() == 2);  // volumetric fire + embers
    CHECK(e->scene().get<FluidVolume>(e->scene().find("Fire")));
    callTool(*e, "fx_create", R"({"effect":"lava"})", false);
    Json sea = callTool(*e, "fx_create", R"({"effect":"calm_sea","name":"Sea","overrides":{"windSpeed":6}})");
    EntityId seaId = static_cast<EntityId>(sea.get("entity").asInt());
    REQUIRE(e->scene().get<Water>(seaId));
    CHECK(e->scene().get<Water>(seaId)->windSpeed == doctest::Approx(6));

    Json q = callTool(*e, "water_query", R"({"points":[[0,0],[10,5]]})");
    REQUIRE(q.get("points").size() == 2);
    CHECK(std::fabs(q.get("points")[size_t{0}].get("height").asFloat()) < 3.f);

    // A buoy follows the waves; a cannon fires a burst.
    callTool(*e, "entity_create", R"({"name":"Buoy","mesh":"sphere"})");
    callTool(*e, "behavior_set", R"({"entity":"Buoy","name":"Float","source":"on tick\n  self.position = (3, water_height(3, 4), 4)\nend"})");
    callTool(*e, "fx_create", R"({"effect":"sparks","name":"Muzzle","overrides":{"rate":0}})");
    callTool(*e, "behavior_set", R"({"entity":"Muzzle","name":"Fire","source":"on start\n  burst(25)\nend"})");
    callTool(*e, "sim_control", R"({"action":"play"})");
    callTool(*e, "sim_control", R"({"action":"step","ticks":5})");
    float h = 0;
    REQUIRE(e->waterHeight(3, 4, h));
    EntityId buoy = e->scene().find("Buoy");
    CHECK(e->scene().get<Transform>(buoy)->position.y == doctest::Approx(h).epsilon(0.05));
    CHECK(e->particles().liveCount(e->scene().find("Muzzle")) >= 20);
    CHECK(e->particles().liveCount(e->scene().find("Embers")) > 0);
    callTool(*e, "fx_burst", R"({"entity":"Muzzle","count":10})");

    // The frame carries the simulated effects to the renderer.
    CaptureOptions o;
    o.width = 64;
    o.height = 36;
    auto cap = e->capture(o);
    REQUIRE(cap);
    CHECK(!cap->frame.particles.empty());
    REQUIRE(cap->frame.water.size() == 1);
    REQUIRE(cap->frame.volumes.size() == 1);
    CHECK(cap->frame.volumes[0].params.fuel > 0.f);
    bool fireLight = false;
    for (const auto& l : cap->frame.lights) fireLight = fireLight || l.range == doctest::Approx(10.f);
    CHECK(fireLight);  // the fluid fire lights the scene
    CHECK(cap->frame.water[0].ocean->resolution == fx::Ocean::kN);

    Json schema = callTool(*e, "component_schema", "{}");
    CHECK(schema.contains("particles"));
    CHECK(schema.contains("water"));
}

TEST_CASE("fx: fx_create position is world space under a parent") {
    auto e = makeFxEngine();
    callTool(*e, "entity_create", R"({"name":"Car","position":[5,0,40],"rotation":[0,90,0],"scale":[2,2,2]})");
    Json smoke = callTool(*e, "fx_create", R"({"effect":"smoke","parent":"Car","position":[3,1,38]})");
    EntityId id = static_cast<EntityId>(smoke.get("entity").asInt());
    REQUIRE(id != kNoEntity);
    CHECK(e->scene().record(id)->parent == e->scene().find("Car"));
    Vec3 w = e->scene().worldMatrix(id).translation();
    CHECK(w.x == doctest::Approx(3).epsilon(1e-4));
    CHECK(w.y == doctest::Approx(1).epsilon(1e-4));
    CHECK(w.z == doctest::Approx(38).epsilon(1e-4));
}

TEST_CASE("fx: ocean evaluation cost (manual)" * doctest::skip()) {
    Water w;
    fx::Ocean o;
    o.configure(w);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) o.evaluate(i * 0.033);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    MESSAGE("evaluate: " << ms << " ms");
}
