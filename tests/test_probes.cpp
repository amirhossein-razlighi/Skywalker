// Reflection probes: the math the shaders mirror (cube faces, influence, blending, box projection),
// the planner (atlas slots, budgets, eviction, update modes), serialization, warnings, the agent tools
// and the Wander builtin. GPU-free; the Metal output is checked visually with
// examples/render_tests/reflection_probes (docs/RENDERING.md "Reflection probes").

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <random>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/DebugViews.h"
#include "skywalker/render/LightClusters.h"
#include "skywalker/render/ReflectionProbes.h"
#include "skywalker/render/Renderer.h"

using namespace sky;
using namespace sky::probes;

namespace {

ProbeItem boxProbe(EntityId e, Vec3 center, Vec3 size, float blend = 1.f, int priority = 0) {
    ReflectionProbe c;
    c.size = size;
    c.blendDistance = blend;
    c.priority = priority;
    return makeItem(e, Mat4::translate(center), c);
}

FrameData frameWith(std::vector<ProbeItem> probes, Vec3 eye = {0, 1.6f, 12}, Vec3 target = {0, 1, 0}) {
    FrameData f;
    f.width = 320;
    f.height = 180;
    f.camera.eye = eye;
    f.camera.target = target;
    f.view = f.camera.view();
    f.projection = f.camera.projection(320.f / 180.f);
    f.probes = std::move(probes);
    return f;
}

DrawItem cubeDraw(EntityId e, Vec3 pos, Vec3 scale) {
    DrawItem d;
    d.entity = e;
    d.mesh = "cube";
    d.model = Mat4::translate(pos) * Mat4::scale(scale);
    d.worldBounds = Aabb{Vec3(-0.5f), Vec3(0.5f)}.transformed(d.model);
    return d;
}

Vec3 randomDir(std::mt19937& rng) {
    std::normal_distribution<float> n(0.f, 1.f);
    return normalize(Vec3{n(rng), n(rng), n(rng)});
}

}  // namespace

TEST_CASE("probes: capture faces match the GPU cube map convention") {
    std::mt19937 rng(7);
    const Vec3 eye{1.f, 2.f, -3.f};
    for (int i = 0; i < 400; ++i) {
        const Vec3 d = randomDir(rng);
        int inside = 0;
        for (int face = 0; face < 6; ++face) {
            const Vec4 c = faceViewProj(eye, face, 0.05f, 50.f) * Vec4(eye + d * 5.f, 1.f);
            if (c.w <= 0.f || std::fabs(c.x) > c.w || std::fabs(c.y) > c.w) continue;
            ++inside;
            // NDC -> texture uv (y down): the texel there must look along d.
            const Vec2 uv{c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f};
            const Vec3 back = cubeDir(face, uv);
            CHECK(dot(back, d) == doctest::Approx(1.f).epsilon(1e-3));
            const float depth = c.z / c.w;
            CHECK(depth > 0.f);
            CHECK(depth < 1.f);
        }
        CHECK(inside >= 1);  // the six faces cover every direction (edges belong to two)
    }
    // Face bases are orthonormal and mirrored (cube maps): right x up = forward.
    for (int face = 0; face < 6; ++face) {
        const FaceBasis b = faceBasis(face);
        CHECK(length(cross(b.right, b.up) - b.forward) < 1e-5f);
        CHECK(dot(b.forward, cubeDir(face, {0.5f, 0.5f})) == doctest::Approx(1.f));
    }
}

TEST_CASE("probes: influence fades over the blend distance; interior volumes cover their walls") {
    ProbeItem p = boxProbe(1, {0, 1.5f, 0}, {8, 3, 6}, 1.f);
    CHECK(influence(p, {0, 1.5f, 0}) == doctest::Approx(1.f));
    CHECK(influence(p, {3.5f, 1.5f, 0}) == doctest::Approx(0.5f));  // 0.5 m from the +x face
    CHECK(influence(p, {4.f, 1.5f, 0}) == doctest::Approx(0.f));
    CHECK(influence(p, {4.05f, 1.5f, 0}) == 0.f);
    CHECK(edgeDistance(p, {0, 0.f, 0}) == doctest::Approx(0.f));
    // Rotated volume: the local axes follow the entity.
    ReflectionProbe c;
    c.size = {8, 3, 2};
    c.blendDistance = 0.f;
    const ProbeItem r = makeItem(2, Mat4::translate({10, 0, 0}) * Mat4::rotateY(radians(90.f)), c);
    CHECK(influence(r, {10, 0, 3.5f}) > 0.f);   // 8 m along world z after the turn
    CHECK(influence(r, {13.5f, 0, 0}) == 0.f);  // only 2 m along world x
    // Interior: a wall on (or just outside) a face still gets the probe (renormalized later), not the sky.
    p.interior = true;
    CHECK(influence(p, {4.f, 1.5f, 0}) > 0.f);
    CHECK(influence(p, {4.05f, 1.5f, 0}) > 0.f);
    CHECK(influence(p, {4.2f, 1.5f, 0}) == 0.f);
    // Spheres.
    c = ReflectionProbe{};
    c.shape = "sphere";
    c.radius = 2.f;
    c.blendDistance = 1.f;
    const ProbeItem s = makeItem(3, Mat4::translate({0, 0, 0}) * Mat4::scale({2, 2, 2}), c);  // scale doubles the radius
    CHECK(s.halfExtents.x == doctest::Approx(4.f));
    CHECK(influence(s, {3.5f, 0, 0}) == doctest::Approx(0.5f));
    CHECK(influence(s, {0, 0, 4.5f}) == 0.f);
}

TEST_CASE("probes: blend weights front to back, sky for the rest, interiors renormalized") {
    const ProbeItem big = boxProbe(1, {0, 0, 0}, {20, 20, 20}, 2.f, 0);
    const ProbeItem small = boxProbe(2, {0, 0, 0}, {4, 4, 4}, 1.f, 0);
    const ProbeItem high = boxProbe(3, {0, 0, 0}, {30, 30, 30}, 1.f, 5);
    // Shading order: priority, then the smaller volume.
    CHECK(shadesBefore(high, small));
    CHECK(shadesBefore(small, big));
    CHECK_FALSE(shadesBefore(big, small));
    std::vector<const ProbeItem*> ordered{&small, &big};
    Blend b = blend(ordered, {0, 0, 0});
    REQUIRE(b.count == 1);  // the small probe covers the center fully: nothing behind it is needed
    CHECK(b.weight[0] == doctest::Approx(1.f));
    CHECK(b.sky == doctest::Approx(0.f));
    b = blend(ordered, {1.5f, 0, 0});  // in the small probe's blend band
    REQUIRE(b.count == 2);
    CHECK(b.weight[0] == doctest::Approx(0.5f));
    CHECK(b.weight[1] == doctest::Approx(0.5f));
    CHECK(b.weight[0] + b.weight[1] + b.sky == doctest::Approx(1.f));
    b = blend(ordered, {9.5f, 0, 0});  // only the big one, half faded: the sky fills the rest
    REQUIRE(b.count == 1);
    CHECK(b.probe[0] == 1);
    CHECK(b.weight[0] == doctest::Approx(0.25f));
    CHECK(b.sky == doctest::Approx(0.75f));
    CHECK(blend(ordered, {50, 0, 0}).sky == doctest::Approx(1.f));
    // Interior: no sky, the probe renormalized to full weight even in its blend band.
    ProbeItem room = small;
    room.interior = true;
    std::vector<const ProbeItem*> inside{&room};
    b = blend(inside, {1.8f, 0, 0});
    CHECK(b.interior);
    CHECK(b.sky == 0.f);
    CHECK(b.weight[0] == doctest::Approx(1.f));
    // Never more than kMaxPerPixel probes per pixel, weights never above 1 in total.
    std::vector<ProbeItem> many;
    for (int i = 0; i < 12; ++i) many.push_back(boxProbe(10 + i, {0, 0, 0}, {10.f + i, 10, 10}, 50.f));
    std::vector<const ProbeItem*> manyPtr;
    for (const auto& m : many) manyPtr.push_back(&m);
    b = blend(manyPtr, {0, 0, 0});
    CHECK(b.count <= kMaxPerPixel);
    float sum = b.sky;
    for (int i = 0; i < b.count; ++i) sum += b.weight[static_cast<size_t>(i)];
    CHECK(sum == doctest::Approx(1.f));
}

TEST_CASE("probes: box projection points from the capture to where the reflection leaves the box") {
    ReflectionProbe c;
    c.size = {8, 3, 6};
    c.captureOffset = {1, 0.2f, -0.5f};
    ProbeItem p = makeItem(1, Mat4::translate({0, 1.5f, 0}), c);
    CHECK(length(p.capture - Vec3{1, 1.7f, -0.5f}) < 1e-5f);
    std::mt19937 rng(3);
    for (int i = 0; i < 200; ++i) {
        const Vec3 pos{std::uniform_real_distribution<float>(-3.5f, 3.5f)(rng), std::uniform_real_distribution<float>(0.1f, 2.9f)(rng),
                       std::uniform_real_distribution<float>(-2.5f, 2.5f)(rng)};
        const Vec3 R = randomDir(rng);
        const Vec3 dir = lookupDir(p, pos, R);
        // The lookup ray from the capture point meets the reflection ray on a wall of the box.
        float t = 0.f;
        for (int k = 0; k < 3; ++k) {
            const float half = k == 0 ? 4.f : (k == 1 ? 1.5f : 3.f), o = k == 0 ? pos.x : (k == 1 ? pos.y - 1.5f : pos.z);
            const float d = k == 0 ? R.x : (k == 1 ? R.y : R.z);
            const float tk = std::fabs(d) < 1e-6f ? 1e9f : std::max((half - o) / d, (-half - o) / d);
            t = k == 0 ? tk : std::min(t, tk);
        }
        const Vec3 hit = pos + R * t;
        CHECK(edgeDistance(p, hit) == doctest::Approx(0.f).epsilon(1e-3));
        CHECK(dot(dir, normalize(hit - p.capture)) == doctest::Approx(1.f).epsilon(1e-4));
    }
    // At the capture point (and without box projection) the lookup is the reflection itself.
    const Vec3 R = normalize(Vec3{0.3f, 0.5f, -0.8f});
    CHECK(dot(lookupDir(p, p.capture, R), R) == doctest::Approx(1.f));
    p.boxProjection = false;
    CHECK(dot(lookupDir(p, {3, 0.2f, 2}, R), R) == doctest::Approx(1.f));
    // A separate projection box (a street split into probes projects onto the whole street).
    c.projectionSize = {8, 3, 60};
    c.projectionOffset = {0, 0, -20};
    p = makeItem(1, Mat4::translate({0, 1.5f, 0}), c);
    REQUIRE(p.projectionBox);
    const Vec3 down{0.f, 0.05f, -1.f};
    const Vec3 dir = lookupDir(p, {0, 0.1f, 2.f}, normalize(down));
    // The ray leaves the projection box at its far end (z = -50), not at the influence volume's (z = -3).
    const Vec3 farHit = Vec3{0, 0.1f, 2.f} + normalize(down) * ((2.f + 50.f) / std::fabs(normalize(down).z));
    CHECK(dot(dir, normalize(farHit - p.capture)) == doctest::Approx(1.f).epsilon(1e-4));
}

TEST_CASE("probes: GPU packing") {
    ReflectionProbe c;
    c.size = {4, 2, 6};
    c.interior = true;
    c.ambient = "sky";  // an interior has no sky: becomes probe
    c.projectionSize = {5, 3, 7};
    const ProbeItem p = makeItem(9, Mat4::translate({3, 1, -2}) * Mat4::rotateY(radians(30.f)), c);
    CHECK(p.ambientMode == 0);
    const GpuProbe g = gpuProbe(p, 5, 2, 5);
    Mat4 w2l;
    std::memcpy(w2l.m, g.worldToLocal, sizeof(w2l.m));
    CHECK(length(w2l.transformPoint({3, 1, -2})) < 1e-5f);
    CHECK(g.extents[0] == doctest::Approx(2.f));
    CHECK(g.params[0] == 5.f);
    CHECK(g.params[1] == 2.f);
    CHECK(static_cast<int>(g.params[2]) == (1 | 2));  // box projection + interior
    CHECK(g.projection[3] == 1.f);
    CHECK(g.projectionHalf[2] == doctest::Approx(3.5f));
    CHECK(sanitizeResolution(200) == 256);
    CHECK(sanitizeResolution(10) == 64);
    CHECK(sanitizeResolution(5000) == 512);
}

TEST_CASE("probes: planner budgets, slots, eviction and update modes") {
    Planner planner;
    Settings settings;
    settings.budget = 2;
    settings.facesPerFrame = 6;
    std::vector<ProbeItem> ps{boxProbe(1, {0, 1, 0}, {6, 3, 6}), boxProbe(2, {0, 1, -8}, {6, 3, 6}), boxProbe(3, {0, 1, -60}, {6, 3, 6})};
    FrameData f = frameWith(ps);
    Plan p = planner.plan(f, settings);
    CHECK(p.overBudget == 1);  // the farthest one has no slot
    CHECK(p.probes[2].slot == -1);
    CHECK(p.probes[2].reason == "over_budget");
    REQUIRE_FALSE(p.warnings.empty());
    CHECK(p.warnings.front().find("atlas is full") != std::string::npos);
    // One cube (6 faces) per frame: the nearest first.
    REQUIRE(p.jobs.size() == 1);
    CHECK(p.jobs[0].probe == 0);
    CHECK(p.jobs[0].completes);
    CHECK_FALSE(p.jobs[0].bounce);  // no budget left for the bounce pass: it comes as a later capture
    CHECK(p.facesDeferred == 6);
    CHECK(p.shaded.size() == 1);
    const int slot0 = p.probes[0].slot;
    p = planner.plan(f, settings);
    CHECK(p.jobs.size() == 1);
    CHECK(p.jobs[0].probe == 1);  // the second probe's first capture comes before probe 1's bounce re-capture
    p = planner.plan(f, settings);
    REQUIRE(p.jobs.size() == 1);
    CHECK(p.jobs[0].probe == 0);  // the bounce re-capture
    p = planner.plan(f, settings);
    REQUIRE(p.jobs.size() == 1);
    CHECK(p.jobs[0].probe == 1);
    p = planner.plan(f, settings);
    CHECK(p.jobs.empty());  // `once` probes stay cached
    CHECK(p.shaded.size() == 2);
    CHECK(p.probes[0].slot == slot0);  // slots are kept
    // Shading order: equal priority, smaller first; the cluster masks reference shaded probes.
    LightGrid grid = buildLightGrid(f);
    std::vector<uint32_t> masks = clusterMasks(f, grid, p);
    uint32_t all = 0;
    for (uint32_t m : masks) all |= m;
    CHECK(all == 0b11u);
    const GpuProbeBlock block = gpuBlock(f, p);
    CHECK(block.info[0] == 2.f);
    // The camera turns to the far probe: it takes the least recently used slot.
    FrameData far = frameWith(ps, {0, 1.6f, -50}, {0, 1, -70});
    p = planner.plan(far, settings);
    CHECK(p.probes[2].slot >= 0);
    CHECK(p.overBudget == 1);
    // Moving a probe re-captures it; invalidate re-captures one or all.
    ps[0] = boxProbe(1, {0.5f, 1, 0}, {6, 3, 6});
    f = frameWith(ps);
    p = planner.plan(f, settings);
    bool moved = false;
    for (const auto& j : p.jobs) moved = moved || j.probe == 0;
    CHECK(moved);
}

TEST_CASE("probes: stills capture everything at once, with the bounce pass") {
    Planner planner;
    Settings settings;
    settings.unlimited = true;
    FrameData f = frameWith({boxProbe(1, {0, 1, 0}, {6, 3, 6}), boxProbe(2, {0, 1, -8}, {6, 3, 6})});
    Plan p = planner.plan(f, settings);
    REQUIRE(p.jobs.size() == 2);
    CHECK(p.jobs[0].bounce);
    CHECK(p.facesCaptured == 24);
    CHECK(p.shaded.size() == 2);
    CHECK(planner.plan(f, settings).jobs.empty());
    planner.invalidate(2);
    p = planner.plan(f, settings);
    REQUIRE(p.jobs.size() == 1);
    CHECK(p.probes[static_cast<size_t>(p.jobs[0].probe)].entity == 2);
    CHECK_FALSE(p.jobs[0].bounce);  // it already had a capture: one pass, lit by it
    planner.invalidate();
    CHECK(planner.plan(f, settings).jobs.size() == 2);
    // GPU memory stays bounded: 512 px probes fit 16 slots in the 256 MB atlas cap, whatever the budget.
    std::vector<ProbeItem> big;
    for (int i = 0; i < 20; ++i) {
        ProbeItem it = boxProbe(100 + i, {static_cast<float>(i) * 10.f, 1, 0}, {6, 3, 6});
        it.resolution = 512;
        big.push_back(it);
    }
    Planner capped;
    Settings wide;
    wide.budget = 32;
    wide.unlimited = true;
    p = capped.plan(frameWith(big, {95, 40, 60}, {95, 0, 0}), wide);
    CHECK(p.atlasSlots == 16);
    CHECK(p.atlasResolution == 512);
    CHECK(slotBytes(p.atlasResolution) * p.atlasSlots <= kMaxAtlasMB * 1048576.0);
    CHECK(p.overBudget == 4);
    // Budget 0 turns probes off.
    settings.budget = 0;
    p = planner.plan(f, settings);
    CHECK(p.shaded.empty());
    CHECK(p.probes[0].reason == "disabled");
}

TEST_CASE("probes: on_change and realtime probes") {
    Planner planner;
    Settings settings;
    settings.unlimited = true;
    ProbeItem a = boxProbe(1, {0, 1, 0}, {6, 3, 6});
    a.update = static_cast<int>(Update::OnChange);
    ProbeItem rt = boxProbe(2, {20, 1, 0}, {6, 3, 6});
    rt.update = static_cast<int>(Update::Realtime);
    rt.interval = 3;
    FrameData f = frameWith({a, rt}, {10, 2, 12}, {10, 1, 0});
    f.draws.push_back(cubeDraw(50, {1, 0.5f, 0}, {1, 1, 1}));
    (void)planner.plan(f, settings);
    auto captured = [](const Plan& p, int idx) {
        for (const auto& j : p.jobs) {
            if (j.probe == idx) return true;
        }
        return false;
    };
    Plan p = planner.plan(f, settings);  // frame 2 (captured on frame 1)
    CHECK_FALSE(captured(p, 0));
    CHECK_FALSE(captured(p, 1));
    p = planner.plan(f, settings);  // frame 3
    CHECK_FALSE(captured(p, 1));
    p = planner.plan(f, settings);  // frame 4: every 3 frames
    CHECK_FALSE(captured(p, 0));
    CHECK(captured(p, 1));
    f.draws[0] = cubeDraw(50, {1.5f, 0.5f, 0}, {1, 1, 1});  // something in range moves
    p = planner.plan(f, settings);
    CHECK(captured(p, 0));
    CHECK(contentHash(f, f.probes[0]) != contentHash(frameWith({a}), a));
}

TEST_CASE("probes: setup warnings") {
    FrameData f = frameWith({boxProbe(1, {0, 1.5f, 0}, {8, 3, 6}, 0.5f), boxProbe(2, {0.5f, 1.5f, 0}, {8, 3, 6}, 0.5f),
                             boxProbe(3, {100, 1.5f, 0}, {4, 3, 4}, 0.5f)});
    f.draws.push_back(cubeDraw(40, {0, -0.05f, 0}, {10, 0.1f, 10}));  // floor
    f.draws.push_back(cubeDraw(41, {0.5f, 1.6f, 0}, {1, 1, 1}));     // a crate around probe 2's capture point
    const std::vector<std::string> w = sceneWarnings(f);
    auto has = [&](const std::string& a, const std::string& b) {
        for (const auto& s : w) {
            if (s.find(a) != std::string::npos && s.find(b) != std::string::npos) return true;
        }
        return false;
    };
    CHECK(has("#1 and probe #2", "equal priority"));
    CHECK(has("#3", "no geometry"));
    CHECK(has("#2", "inside the solid mesh #41"));
    CHECK(has("#1", "the floor under it"));  // the floor lies on the volume's bottom face
}

TEST_CASE("probes: the component round-trips through scene JSON and buildFrame") {
    Scene s;
    EntityId e = s.create("Hall Probe");
    REQUIRE(s.patchComponent(e, "reflection_probe",
                             Json::object({{"size", Json::array({8, 3, 6})}, {"interior", true}, {"update", "realtime"}, {"interval", 4},
                                           {"resolution", 128}, {"priority", 2}, {"cullMask", 5}, {"ambient", "color"},
                                           {"ambientColor", "#336699"}, {"projectionSize", Json::array({10, 3, 6})}}))
                .ok());
    CHECK_FALSE(s.patchComponent(e, "reflection_probe", Json::object({{"update", "always"}})).ok());  // unknown enum value
    REQUIRE(s.patchComponent(e, "transform", Json::object({{"position", Json::array({1, 2, 3})}, {"scale", Json::array({2, 1, 1})}})).ok());
    const ReflectionProbe* rp = s.get<ReflectionProbe>(e);
    REQUIRE(rp);
    CHECK(rp->interior);
    CHECK(rp->interval == 4);
    ViewCamera cam;
    FrameData f = buildFrame(s, cam, 64, 36, {});
    REQUIRE(f.probes.size() == 1);
    const ProbeItem& p = f.probes[0];
    CHECK(p.entity == e);
    CHECK(p.halfExtents.x == doctest::Approx(8.f));  // the entity scale doubles x
    CHECK(p.update == 2);
    CHECK(p.resolution == 128);
    CHECK(p.cullMask == 5u);
    CHECK(p.ambientMode == 2);
    CHECK(p.priority == 2);
    CHECK(p.projectionBox);
    CHECK(p.projectionHalf.x == doctest::Approx(10.f));
    // Disabled entities have no probe.
    REQUIRE(s.setEnabled(e, false).ok());
    CHECK(buildFrame(s, cam, 64, 36, {}).probes.empty());
}

namespace {

std::unique_ptr<Engine> probeEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;  // the probe plan runs on the CPU there too
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Probes", true);
    return e;
}

ToolResult callTool(Engine& e, const char* tool, const char* args) {
    return e.callTool(tool, Json::parse(args).value(), "agent:test");
}

}  // namespace

TEST_CASE("probes: probe_add, probe_info, probe_bake tools and the Wander builtin") {
    auto e = probeEngine();
    // A closed room: floor, ceiling and four walls (8 x 3 x 6 m inside).
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Floor","mesh":"cube","position":[0,-0.1,0],"scale":[8,0.2,6]})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Ceiling","mesh":"cube","position":[0,3.1,0],"scale":[8,0.2,6]})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Wall E","mesh":"cube","position":[4.1,1.5,0],"scale":[0.2,3,6]})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Wall W","mesh":"cube","position":[-4.1,1.5,0],"scale":[0.2,3,6]})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Wall N","mesh":"cube","position":[0,1.5,-3.1],"scale":[8,3,0.2]})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Wall S","mesh":"cube","position":[0,1.5,3.1],"scale":[8,3,0.2]})").isError);

    ToolResult r = callTool(*e, "probe_add", R"({"name":"Room Probe","position":[1,1.5,0.5],"interior":true})");
    const std::string summary = r.content.empty() ? std::string() : r.content.front().text;
    INFO(summary);
    REQUIRE_FALSE(r.isError);
    const Json& probe = r.structured.get("probe");
    // Auto size: the room (8 x 3 x 6) plus the blend distance on every side; reflections project onto the room.
    CHECK(probe.get("size")[0].asFloat() == doctest::Approx(10.f).epsilon(0.02));
    CHECK(probe.get("size")[1].asFloat() == doctest::Approx(5.f).epsilon(0.02));
    CHECK(probe.get("projectionSize")[2].asFloat() == doctest::Approx(6.f).epsilon(0.02));
    CHECK(r.structured.get("state").get("ready").asBool());
    const EntityId id = e->scene().find("Room Probe");
    REQUIRE(id);
    const Transform* t = e->scene().get<Transform>(id);
    REQUIRE(t);
    CHECK(t->position.x == doctest::Approx(0.f).epsilon(0.02));  // centered in the room, capture point kept
    CHECK(e->scene().get<ReflectionProbe>(id)->captureOffset.x == doctest::Approx(1.f).epsilon(0.02));

    r = callTool(*e, "probe_info", R"({"view":{"eye":[0,1.6,2],"target":[0,1,-2]}})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("count").asInt() == 1);
    CHECK(r.structured.get("probes").size() == 1);
    CHECK(r.structured.get("probes")[0].get("name").asString() == "Room Probe");
    CHECK(r.structured.get("probes")[0].get("interior").asBool());
    CHECK(r.structured.get("atlas").get("resolution").asInt() == 256);
    CHECK(r.structured.get("probes")[0].get("capturedFaces").asInt() == 0);  // cached

    // Moving the floor makes the `once` probe stale; probe_bake re-captures it.
    REQUIRE_FALSE(callTool(*e, "entity_update", R"({"entity":"Floor","components":{"transform":{"position":[0,-0.2,0]}}})").isError);
    r = callTool(*e, "probe_info", R"({"view":{"eye":[0,1.6,2],"target":[0,1,-2]}})");
    CHECK(r.structured.get("probes")[0].get("stale").asBool(false));
    r = callTool(*e, "probe_bake", R"({"probes":["Room Probe"],"view":{"eye":[0,1.6,2],"target":[0,1,-2]}})");
    REQUIRE_FALSE(r.isError);
    REQUIRE(r.structured.get("probes").size() == 1);
    CHECK(r.structured.get("probes")[0].get("capturedFaces").asInt() >= 6);
    CHECK_FALSE(r.structured.get("probes")[0].get("stale").asBool(false));

    // Misuse.
    CHECK(callTool(*e, "probe_info", R"({"entity":"Floor"})").isError);              // not a probe
    CHECK(callTool(*e, "probe_add", R"({"position":[0,1,0],"size":[0,2,2]})").isError);  // bad size
    CHECK(callTool(*e, "probe_add", R"({"position":[0,1,0],"update":"always"})").isError);  // bad enum
    CHECK(callTool(*e, "probe_add", R"({"size":[2,2,2]})").isError);                     // position required
    CHECK(callTool(*e, "probe_bake", R"({"probes":"Wall E"})").isError);
    // The probe's cubemap needs the GPU backend (a clear error on the null renderer).
    r = callTool(*e, "viewport_capture", R"({"width":64,"height":36,"samples":1,"probe":"Room Probe"})");
    CHECK(r.isError);
    // The debug view exists (id 24, appended after the shadow atlas).
    CHECK(debugViewFromName("reflection_probes").value() == debugview::kReflectionProbes);
    CHECK(debugview::kReflectionProbes == FrameData::kDebugViewReflectionProbes);
    CHECK(debugview::kReflectionProbes == 24);
    CHECK_FALSE(debugViewOverridesSurfaces(debugview::kReflectionProbes));
    CHECK_FALSE(callTool(*e, "viewport_capture", R"({"width":64,"height":36,"samples":1,"debug_view":"reflection_probes"})").isError);

    // Undo removes the probe.
    REQUIRE_FALSE(callTool(*e, "history", R"({"action":"undo"})").isError);  // the floor move
    REQUIRE_FALSE(callTool(*e, "history", R"({"action":"undo"})").isError);  // the probe
    CHECK_FALSE(e->scene().find("Room Probe"));
    REQUIRE_FALSE(callTool(*e, "history", R"({"action":"redo"})").isError);
    REQUIRE(e->scene().find("Room Probe"));

    // Wander: probe_bake() re-captures on the next rendered frame.
    REQUIRE_FALSE(callTool(*e, "behavior_set", R"({"entity":"Room Probe","name":"Rebake","source":"behavior Rebake\n  on start\n    probe_bake(self)\n  end\nend\n"})")
                      .isError);
    REQUIRE_FALSE(callTool(*e, "sim_control", R"({"action":"step","ticks":2})").isError);
    r = callTool(*e, "probe_info", R"({"view":{"eye":[0,1.6,2],"target":[0,1,-2]}})");
    CHECK(r.structured.get("probes")[0].get("capturedFaces").asInt() == 6);
    REQUIRE_FALSE(callTool(*e, "sim_control", R"({"action":"stop"})").isError);
}
