#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/Impostor.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

Vec3 randomDirection(std::mt19937& rng) {
    std::normal_distribution<float> n(0.f, 1.f);
    for (;;) {
        Vec3 v{n(rng), n(rng), n(rng)};
        if (length(v) > 1e-3f) return normalize(v);
    }
}

ImpostorModel sampleModel() {
    ImpostorModel m;
    auto parts = std::make_shared<std::vector<InstancePart>>();
    InstancePart p;
    p.mesh = "asset:trees/oak.glb#0";
    p.surface.texture = "trees/oak_leaves.png";
    p.surface.alphaCutoff = 0.4f;
    parts->push_back(p);
    m.parts = parts;
    m.source = "{asset:trees/oak.glb#0|trees/oak_leaves.png|a0.4}";
    m.stamp = "|1234@99";
    m.frames = 12;
    m.resolution = 2048;
    m.label = "oaks";
    return m;
}

}  // namespace

TEST_CASE("impostor: octahedral encodings round-trip") {
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        Vec3 d = randomDirection(rng);
        Vec2 e = impostor::octEncode(d);
        CHECK(std::fabs(e.x) <= 1.0001f);
        CHECK(std::fabs(e.y) <= 1.0001f);
        Vec3 back = impostor::octDecode(e);
        CHECK(dot(back, d) > 0.99999f);
        if (d.y >= 0.f) {
            Vec2 h = impostor::hemiOctEncode(d);
            CHECK(std::fabs(h.x) <= 1.0001f);
            CHECK(std::fabs(h.y) <= 1.0001f);
            CHECK(dot(impostor::hemiOctDecode(h), d) > 0.99999f);
        } else {
            // Below the horizon: clamped onto it.
            Vec3 h = impostor::hemiOctDecode(impostor::hemiOctEncode(d));
            CHECK(h.y == doctest::Approx(0.f).epsilon(1e-5));
        }
    }
    // The whole square decodes to the upper hemisphere.
    for (float x = -1.f; x <= 1.f; x += 0.125f) {
        for (float y = -1.f; y <= 1.f; y += 0.125f) CHECK(impostor::hemiOctDecode({x, y}).y >= -1e-6f);
    }
    CHECK(dot(impostor::hemiOctDecode({0, 0}), Vec3{0, 1, 0}) > 0.9999f);
}

TEST_CASE("impostor: frame directions, bases and blend weights") {
    for (bool hemi : {true, false}) {
        const int frames = 12;
        // (The border of a full octahedral grid folds onto itself: check its interior frames.)
        const int lo = hemi ? 0 : 1, hi = hemi ? frames : frames - 1;
        for (int y = lo; y < hi; ++y) {
            for (int x = lo; x < hi; ++x) {
                Vec3 d = impostor::frameDirection(x, y, frames, hemi);
                CHECK(length(d) == doctest::Approx(1.f).epsilon(1e-4));
                Vec2 g = impostor::gridCoord(d, frames, hemi);
                CHECK(g.x == doctest::Approx(static_cast<float>(x)).epsilon(1e-3));
                CHECK(g.y == doctest::Approx(static_cast<float>(y)).epsilon(1e-3));
                // Looking exactly along a frame: that frame alone.
                impostor::Blend b = impostor::blendFrames(d, frames, hemi);
                float self = 0.f;
                for (int k = 0; k < 3; ++k) self += (b.x[k] == x && b.y[k] == y) ? b.w[k] : 0.f;
                CHECK(self == doctest::Approx(1.f).epsilon(1e-3));
                impostor::Basis basis = impostor::frameBasis(d);
                CHECK(std::fabs(dot(basis.right, basis.up)) < 1e-4f);
                CHECK(std::fabs(dot(basis.right, basis.forward)) < 1e-4f);
                CHECK(length(basis.right) == doctest::Approx(1.f).epsilon(1e-4));
                CHECK(dot(cross(basis.right, basis.up), basis.forward) > 0.999f);  // right-handed
            }
        }
    }
    // Side views keep "up" up (upright vegetation looks upright in every side frame).
    impostor::Basis side = impostor::frameBasis({0, 0, 1});
    CHECK(side.up.y > 0.999f);
    CHECK(impostor::frameBasis({0, 1, 0}).up.z < -0.999f);

    // Weights: non-negative, sum to one, neighbours of the grid triangle, continuous.
    std::mt19937 rng(3);
    for (int i = 0; i < 500; ++i) {
        Vec3 d = randomDirection(rng);
        d.y = std::fabs(d.y);
        impostor::Blend b = impostor::blendFrames(d, 12, true);
        float sum = 0.f;
        for (int k = 0; k < 3; ++k) {
            CHECK(b.w[k] >= 0.f);
            CHECK(b.x[k] >= 0);
            CHECK(b.x[k] < 12);
            CHECK(b.y[k] >= 0);
            CHECK(b.y[k] < 12);
            sum += b.w[k];
        }
        CHECK(sum == doctest::Approx(1.f).epsilon(1e-4));
        // A tiny change of view changes the blended weights only a little (no popping).
        Vec3 d2 = normalize(d + Vec3{1e-3f, 0.f, 1e-3f});
        impostor::Blend b2 = impostor::blendFrames(d2, 12, true);
        auto weightOf = [](const impostor::Blend& bl, int x, int y) {
            float w = 0.f;
            for (int k = 0; k < 3; ++k) w += (bl.x[k] == x && bl.y[k] == y) ? bl.w[k] : 0.f;
            return w;
        };
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(weightOf(b, b.x[k], b.y[k]) - weightOf(b2, b.x[k], b.y[k])) < 0.05f);
    }
}

TEST_CASE("impostor: transition distance from on-screen size, tiers and overrides") {
    CHECK(impostor::tileSize(2048, 12) == 160);
    CHECK(impostor::tileSize(1024, 12) == 80);
    CHECK(impostor::tileSize(100, 32) == 16);
    for (int res : {256, 512, 1000, 2048, 4096}) {
        for (int f : {4, 8, 12, 16, 32}) CHECK(impostor::tileSize(res, f) % 16 == 0);
    }
    CHECK(impostor::autoResolution(20.f) == 2048);
    CHECK(impostor::autoResolution(4.f) == 1024);
    CHECK(impostor::autoResolution(1.f) == 512);

    impostor::TransitionParams p;
    p.modelRadius = 6.f;
    p.atlasResolution = 2048;
    p.frames = 12;
    p.screenHeight = 1080;
    p.fovDeg = 55.f;
    p.cullDistance = 1500.f;
    const float full = impostor::transitionDistance(p);
    CHECK(full > 20.f);
    CHECK(full < 400.f);
    // Bigger models, sharper screens and narrower lenses switch later; bigger atlases sooner.
    impostor::TransitionParams q = p;
    q.modelRadius = 12.f;
    CHECK(impostor::transitionDistance(q) > full);
    q = p;
    q.screenHeight = 2160;
    CHECK(impostor::transitionDistance(q) == doctest::Approx(full * 2.f).epsilon(1e-3));
    q = p;
    q.fovDeg = 25.f;
    CHECK(impostor::transitionDistance(q) > full);
    q = p;
    q.atlasResolution = 1024;
    CHECK(impostor::transitionDistance(q) > full);
    // Lower viewport tiers move the transition closer.
    q = p;
    q.quality = 1;
    CHECK(impostor::transitionDistance(q) == doctest::Approx(full * 0.75f).epsilon(1e-3));
    q.quality = 2;
    CHECK(impostor::transitionDistance(q) == doctest::Approx(full * 0.5f).epsilon(1e-3));
    // Overrides, disabling, never right in front of the camera, no point beyond the cull range.
    q = p;
    q.overrideDistance = 80.f;
    CHECK(impostor::transitionDistance(q) == doctest::Approx(80.f));
    q.overrideDistance = -1.f;
    CHECK(impostor::transitionDistance(q) == 0.f);
    q = p;
    q.overrideDistance = 1.f;
    CHECK(impostor::transitionDistance(q) >= 6.f * 2.f * 1.5f);
    q = p;
    q.cullDistance = full;
    CHECK(impostor::transitionDistance(q) == 0.f);
    CHECK(impostor::crossfadeWidth(100.f) > 0.f);
    CHECK(impostor::crossfadeWidth(100.f) < 100.f);
}

TEST_CASE("impostor: cache keys are stable and track every input") {
    ImpostorModel a = sampleModel(), b = sampleModel();
    CHECK(impostor::cacheKey(a) == impostor::cacheKey(b));
    CHECK(impostor::cacheKey(a).size() == 32);
    // Labels and cache paths are not inputs.
    b.label = "other";
    b.cachePath = "/elsewhere/x.skyimp";
    CHECK(impostor::cacheKey(a) == impostor::cacheKey(b));
    auto differs = [&](auto change) {
        ImpostorModel m = sampleModel();
        change(m);
        return impostor::cacheKey(m) != impostor::cacheKey(a);
    };
    CHECK(differs([](ImpostorModel& m) { m.source += "|c0.5"; }));
    CHECK(differs([](ImpostorModel& m) { m.stamp = "|1234@100"; }));
    CHECK(differs([](ImpostorModel& m) { m.frames = 16; }));
    CHECK(differs([](ImpostorModel& m) { m.resolution = 1024; }));
    CHECK(differs([](ImpostorModel& m) { m.hemi = false; }));
    // Resolutions that give the same frame tiles share a cache.
    CHECK(!differs([](ImpostorModel& m) { m.resolution = 2050; }));
    CHECK(impostor::hash64("abc") == impostor::hash64("abc"));
    CHECK(impostor::hash64("abc") != impostor::hash64("abd"));
}

TEST_CASE("impostor: atlas post-process, coverage-preserving mips and the cache file") {
    impostor::Atlas a;
    a.frames = 4;
    a.size = 4 * 16;
    const size_t n = static_cast<size_t>(a.size) * a.size;
    a.albedo.assign(n * 4, 0);
    a.normal.assign(n * 4, 0);
    auto set = [&](int x, int y, uint8_t cov, uint8_t r, uint8_t nx) {
        uint8_t* p = &a.albedo[(static_cast<size_t>(y) * a.size + x) * 4];
        p[0] = r, p[1] = r, p[2] = r, p[3] = cov;
        uint8_t* q = &a.normal[(static_cast<size_t>(y) * a.size + x) * 4];
        q[0] = nx, q[1] = nx, q[2] = nx, q[3] = 0;
    };
    // Frame (0,0): a solid disc; frame (1,0): a sparse checker of leaves; the rest empty.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if ((x - 8) * (x - 8) + (y - 8) * (y - 8) < 25) set(x, y, 255, 200, 180);
            if (((x * 73856093u) ^ (y * 19349663u)) % 5u < 2u) set(16 + x, y, 255, 120, 100);  // ~40% leaves
        }
    }
    set(3, 8, 128, 100, 90);  // a half-covered (premultiplied) edge texel
    impostor::finalize(a);
    auto at = [&](const std::vector<uint8_t>& v, int x, int y, int c) { return v[(static_cast<size_t>(y) * a.size + x) * 4 + c]; };
    CHECK(at(a.albedo, 3, 8, 3) == 128);           // coverage kept
    CHECK(at(a.albedo, 3, 8, 0) == 138);           // color un-premultiplied in linear space (sRGB 100 at half coverage)
    CHECK(at(a.normal, 3, 8, 0) == doctest::Approx(180).epsilon(0.02));
    CHECK(at(a.albedo, 0, 0, 0) == 200);           // empty corner of frame (0,0) dilated from the disc
    CHECK(at(a.albedo, 0, 0, 3) == 0);             // ... but still transparent
    CHECK(at(a.normal, 0, 0, 0) == 180);
    CHECK(at(a.albedo, 40, 40, 0) == 0);           // empty frames get neutral values, no bleeding
    CHECK(at(a.normal, 40, 40, 0) == 128);

    std::vector<impostor::Atlas> mips = impostor::buildMips(a);
    REQUIRE(mips.size() == static_cast<size_t>(impostor::kMipLevels));
    CHECK(mips[1].size == a.size / 2);
    CHECK(mips.back().tile() == 1);
    for (int level = 1; level < 4; ++level) {
        // The alpha-tested area stays (close to) what level 0 covers: forests keep their density.
        CHECK(impostor::frameCoverage(mips[static_cast<size_t>(level)], 1, 0) ==
              doctest::Approx(impostor::frameCoverage(a, 1, 0)).epsilon(0.2));
        CHECK(impostor::frameCoverage(mips[static_cast<size_t>(level)], 0, 0) ==
              doctest::Approx(impostor::frameCoverage(a, 0, 0)).epsilon(0.2));
        CHECK(impostor::frameCoverage(mips[static_cast<size_t>(level)], 2, 2) == 0.f);
    }

    const std::string path = (fs::temp_directory_path() / ("skywalker-imp-" + AssetDatabase::newGuid().substr(0, 8)) / "a.skyimp").string();
    Json meta = Json::object({{"key", "k1"}, {"radius", 3.5}});
    REQUIRE(impostor::save(path, a, meta));
    auto loaded = impostor::load(path);
    REQUIRE(loaded);
    CHECK(loaded->atlas.size == a.size);
    CHECK(loaded->atlas.frames == a.frames);
    CHECK(loaded->atlas.albedo == a.albedo);
    CHECK(loaded->atlas.normal == a.normal);
    CHECK(loaded->meta.get("key").asString() == "k1");
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "not an impostor";
    }
    CHECK(!impostor::load(path));
    CHECK(impostor::load(path + ".missing").error().code == "not_found");
    Image preview = impostor::preview(a, 32);
    CHECK(preview.width == 32);
    fs::remove_all(fs::path(path).parent_path());
}

TEST_CASE("impostor: foliage layers with heavy meshes build impostor frame data") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-impostor-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    {  // a "tree": a UV sphere of ~1150 triangles
        std::ofstream obj(fs::path(cfg.projectDir) / "tree.obj");
        const int rings = 24, segs = 24;
        for (int r = 0; r <= rings; ++r) {
            for (int s = 0; s <= segs; ++s) {
                float th = kPi * r / rings, ph = 2.f * kPi * s / segs;
                obj << "v " << std::sin(th) * std::cos(ph) << " " << 1.f + std::cos(th) << " " << std::sin(th) * std::sin(ph) << "\n";
            }
        }
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < segs; ++s) {
                int a = r * (segs + 1) + s + 1, b = a + segs + 1;
                obj << "f " << a << " " << b << " " << a + 1 << "\nf " << a + 1 << " " << b << " " << b + 1 << "\n";
            }
        }
    }
    Engine e(cfg);
    (void)e.newScene("Forest", false);
    ToolResult r = e.callTool("terrain_create", Json::parse(R"({"preset":"flat","size":256,"resolution":129})").value(), "agent:test");
    REQUIRE(!r.isError);
    r = e.callTool("foliage_add", Json::parse(R"({"entity":"Terrain","name":"Forest","layers":[
        {"mesh":"asset:tree.obj","density":0.02,"cullDistance":300,"impostorDistance":40,"name":"trees"},
        {"mesh":"asset:tree.obj","density":0.02,"cullDistance":300,"impostors":false,"seed":4,"name":"meshes only"},
        {"preset":"meadow_grass","density":2,"cullDistance":30}]})").value(), "agent:test");
    INFO(r.content.front().text);
    REQUIRE(!r.isError);

    CaptureOptions o;
    o.width = 320;
    o.height = 180;
    o.samples = 2;  // generate every chunk in range
    o.hasCustomView = true;
    o.customView.eye = {0, 20, -120};
    o.customView.target = {0, 0, 0};
    FrameData f = e.frame(o);
    REQUIRE(f.impostors.size() == 1);
    const ImpostorModel& m = f.impostors[0];
    CHECK(m.key == impostor::cacheKey(m));
    CHECK(m.cachePath.find(".skywalker/cache/impostors/" + m.key + ".skyimp") != std::string::npos);
    REQUIRE(m.parts);
    CHECK(m.parts->size() == 1);
    CHECK(m.bounds.max.y > m.bounds.min.y);
    CHECK(m.frames == 12);
    size_t withImpostor = 0, meshOnly = 0;
    for (const InstanceBatch& b : f.instances) {
        REQUIRE(b.parts);
        REQUIRE(!b.parts->empty());
        CHECK(b.modelBounds.max.y > b.modelBounds.min.y);
        if (b.impostor >= 0) {
            CHECK(b.impostor == 0);
            CHECK(b.impostorDistance == doctest::Approx(40.f));
            CHECK(b.parts.get() == m.parts.get());  // chunks share the model's parts
            ++withImpostor;
        } else {
            CHECK(b.impostorDistance == 0.f);
            ++meshOnly;
        }
    }
    CHECK(withImpostor > 0);
    CHECK(meshOnly > 0);  // the "impostors": false layer, and the primitive grass

    // The same inputs give the same key in a fresh frame; the fast tier switches sooner.
    o.quality = 2;
    FrameData fast = e.frame(o);
    REQUIRE(fast.impostors.size() == 1);
    CHECK(fast.impostors[0].key == m.key);
    for (const InstanceBatch& b : fast.instances) {
        if (b.impostor >= 0) CHECK(b.impostorDistance == doctest::Approx(20.f));
    }

    // Clay renders bake their own impostors.
    o.quality = 0;
    o.clay = true;
    FrameData clay = e.frame(o);
    REQUIRE(clay.impostors.size() == 1);
    CHECK(clay.impostors[0].key != m.key);

    // The bake tool reports the layers; the null renderer cannot bake.
    r = e.callTool("impostor_bake", Json::parse(R"({"entity":"Forest"})").value(), "agent:test");
    CHECK(r.isError);
    r = e.callTool("impostor_bake", Json::parse(R"({"entity":"Forest","layer":"tres"})").value(), "agent:test");
    CHECK(r.isError);
    auto layers = e.world().impostorModels(e.scene(), static_cast<EntityId>(e.scene().find("Forest")));
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].name == "trees");
    CHECK(layers[0].transitionDistance == doctest::Approx(40.f));
    fs::remove_all(cfg.projectDir);
}
