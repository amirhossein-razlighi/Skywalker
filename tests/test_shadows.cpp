// Local (point / spot) light shadows: projection math mirrored by the shaders, atlas allocation,
// static caching, budgets and the agent tools. GPU-free (the Metal output is checked by
// tools/render_checks/local_shadows.sh).

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <set>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render/ShadowAtlas.h"

using namespace sky;
using namespace sky::shadows;

namespace {

Vec3 randomDir(std::mt19937& rng) {
    std::normal_distribution<float> n(0.f, 1.f);
    Vec3 v{n(rng), n(rng), n(rng)};
    return normalize(v);
}

/// uv / depth of a world point through a view-projection matrix (Metal NDC, y up, depth 0..1).
Projected throughMatrix(const Mat4& vp, Vec3 p) {
    Vec4 c = vp * Vec4(p, 1.f);
    Projected r;
    r.uv = {c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f};
    r.depth = c.z / c.w;
    r.inside = c.w > 0.f && std::fabs(c.x) <= c.w && std::fabs(c.y) <= c.w;
    return r;
}

LightItem pointLight(Vec3 pos, float range, uint64_t id) {
    LightItem l;
    l.kind = LightItem::Kind::Point;
    l.position = pos;
    l.range = range;
    l.intensity = 5.f;
    l.id = id;
    l.shadows = true;
    return l;
}

FrameData baseFrame() {
    FrameData f;
    f.width = 640;
    f.height = 360;
    f.camera.eye = {0, 2, 10};
    f.camera.target = {0, 1, 0};
    f.view = f.camera.view();
    f.projection = f.camera.projection(640.f / 360.f);
    return f;
}

DrawItem box(EntityId e, Vec3 center, Vec3 size) {
    DrawItem d;
    d.entity = e;
    d.mesh = "cube";
    d.model = Mat4::trs(center, {0, 0, 0}, size);
    d.worldBounds = Aabb{center - size * 0.5f, center + size * 0.5f};
    return d;
}

}  // namespace

TEST_CASE("shadows: perspective views match their matrices (spot and cube faces)") {
    std::mt19937 rng(7);
    const Vec3 pos{1.f, 3.f, -2.f};
    const float near = shadowNear(12.f), far = 12.f;
    CHECK(near == doctest::Approx(0.048f));
    // Our view-projection agrees with the engine's own lookAt + perspective (same depth convention).
    {
        Vec3 dir = normalize(Vec3{0.3f, -0.8f, 0.2f});
        Basis b = lightBasis(dir);
        Mat4 ref = Mat4::perspective(2.f * std::atan(0.7f), 1.f, near, far) * Mat4::lookAt(pos, pos + dir, {0, 1, 0});
        Mat4 ours = perspectiveViewProj(pos, b, 0.7f, near, far);
        for (int i = 0; i < 16; ++i) CHECK(ours.m[i] == doctest::Approx(ref.m[i]).epsilon(1e-4));
    }
    for (int i = 0; i < 300; ++i) {
        Vec3 dir = randomDir(rng);
        Vec3 p = pos + randomDir(rng) * (0.5f + 10.f * static_cast<float>(i % 17) / 17.f);
        Projected a = projectSpot(p, pos, dir, 0.9f, near, far);
        Projected m = throughMatrix(perspectiveViewProj(pos, lightBasis(dir), 0.9f, near, far), p);
        if (dot(p - pos, dir) <= near) continue;
        CHECK(a.inside == m.inside);
        CHECK(a.uv.x == doctest::Approx(m.uv.x).epsilon(1e-3));
        CHECK(a.uv.y == doctest::Approx(m.uv.y).epsilon(1e-3));
        CHECK(a.depth == doctest::Approx(m.depth).epsilon(1e-4));

        Projected c = projectCube(p, pos, 1.02f, near, far);
        Projected cm = throughMatrix(perspectiveViewProj(pos, cubeFaceBasis(c.face), 1.02f, near, far), p);
        CHECK(c.inside);  // the six faces cover every direction
        CHECK(cm.inside);
        CHECK(c.uv.x == doctest::Approx(cm.uv.x).epsilon(1e-3));
        CHECK(c.uv.y == doctest::Approx(cm.uv.y).epsilon(1e-3));
        CHECK(c.depth == doctest::Approx(cm.depth).epsilon(1e-4));
    }
    // Cube faces: orthonormal right-handed bases looking down their axis.
    for (int face = 0; face < 6; ++face) {
        Basis b = cubeFaceBasis(face);
        CHECK(length(b.s) == doctest::Approx(1.f));
        CHECK(dot(b.s, b.u) == doctest::Approx(0.f));
        CHECK(dot(cross(b.s, b.u), b.f) == doctest::Approx(-1.f));  // view convention: s x u points back
        CHECK(cubeFaceOf(b.f) == face);
    }
    // Straight-down spot (the usual lamp): a stable basis.
    Basis down = lightBasis({0, -1, 0});
    CHECK(down.s == Vec3{-1, 0, 0});
    CHECK(down.u == Vec3{0, 0, 1});
}

TEST_CASE("shadows: dual paraboloid caster encoding round-trips with receiver lookups") {
    std::mt19937 rng(11);
    const Vec3 pos{0.f, 2.f, 0.f};
    const Vec3 dir{0.f, 0.f, -1.f};
    const float near = 0.05f, far = 15.f, k = 1.1f;
    int seen[2] = {0, 0};
    for (int i = 0; i < 400; ++i) {
        Vec3 p = pos + randomDir(rng) * (0.3f + 14.f * static_cast<float>(i % 23) / 23.f);
        Projected r = projectParaboloid(p, pos, dir, k, near, far);
        CHECK(r.inside);  // two hemispheres cover the sphere
        ++seen[r.face];
        Vec4 clip = shadowClip(paraboloidMatrix(pos, dir, r.face, near, far, k), p);
        CHECK(clip.w == 1.f);
        CHECK(clip.x * 0.5f + 0.5f == doctest::Approx(r.uv.x).epsilon(1e-4));
        CHECK(0.5f - clip.y * 0.5f == doctest::Approx(r.uv.y).epsilon(1e-4));
        CHECK(clip.z == doctest::Approx(r.depth).epsilon(1e-4));
        // The other hemisphere clips it away (or projects it outside its half).
        Vec4 other = shadowClip(paraboloidMatrix(pos, dir, 1 - r.face, near, far, k), p);
        CHECK((other.z < 0.f || other.x * other.x + other.y * other.y > 0.9f * 0.9f));  // at most the seam overlap
    }
    CHECK(seen[0] > 100);
    CHECK(seen[1] > 100);
    // Ordinary matrices pass through unchanged.
    Mat4 m = Mat4::translate({1, 2, 3});
    CHECK(shadowClip(m, {0, 0, 0}) == Vec4{1, 2, 3, 1});
}

TEST_CASE("shadows: face rectangles tile the slot without overlap") {
    for (int size : {256, 512, 1024, 2048}) {
        std::set<std::pair<int, int>> covered;
        int area = 0;
        for (int face = 0; face < 6; ++face) {
            Rect r = faceRect(Projection::Cube, face, size);
            CHECK(r.x >= 0);
            CHECK(r.y >= 0);
            CHECK(r.x + r.w <= size);
            CHECK(r.y + r.h <= size);
            area += r.w * r.h;
            for (int other = 0; other < face; ++other) {
                Rect o = faceRect(Projection::Cube, other, size);
                bool overlap = r.x < o.x + o.w && o.x < r.x + r.w && r.y < o.y + o.h && o.y < r.y + r.h;
                CHECK_FALSE(overlap);
            }
        }
        CHECK(area >= size * size * 99 / 100);  // nearly the whole slot is used
        Rect a = faceRect(Projection::DualParaboloid, 0, size), b = faceRect(Projection::DualParaboloid, 1, size);
        CHECK(a.x + a.w == b.x);
        CHECK(b.x + b.w == size);
        CHECK(faceRect(Projection::Spot, 0, size).w == size);
    }
    CHECK(faceCount(Projection::Cube) == 6);
    CHECK(faceCount(Projection::DualParaboloid) == 2);
    CHECK(faceCount(Projection::Spot) == 1);
}

TEST_CASE("shadows: atlas allocation (tiers, no overlap, keeping slots, priority eviction, LRU)") {
    ShadowAtlas atlas(AtlasConfig{4096, {1, 2, 4, 8}});
    CHECK(atlas.quadrantSize() == 2048);
    CHECK(atlas.slotSize(0) == 2048);
    CHECK(atlas.slotSize(3) == 256);
    CHECK(atlas.slotCount(3) == 64);
    CHECK(atlas.quadrantFor(5000) == 0);
    CHECK(atlas.quadrantFor(1500) == 1);
    CHECK(atlas.quadrantFor(600) == 2);
    CHECK(atlas.quadrantFor(10) == 3);

    // Many requests: distinct, in-bounds slots.
    std::vector<ShadowAtlas::Request> reqs;
    for (uint64_t k = 1; k <= 40; ++k) reqs.push_back({k, static_cast<int>(200 + k * 40), static_cast<float>(k)});
    auto slots = atlas.allocate(reqs, 1);
    std::set<std::pair<int, int>> used;
    for (size_t i = 0; i < slots.size(); ++i) {
        REQUIRE(slots[i].valid());
        CHECK(used.insert({slots[i].quadrant, slots[i].index}).second);
        CHECK(slots[i].x + slots[i].size <= 2048);
        CHECK(slots[i].y + slots[i].size <= 2048);
        CHECK(atlas.owner(slots[i]) == reqs[i].key);
    }
    // The same requests next frame keep their slots (static cache).
    auto again = atlas.allocate(reqs, 2);
    for (size_t i = 0; i < slots.size(); ++i) CHECK(again[i] == slots[i]);
    // Content validity follows the hash.
    atlas.markRendered(slots[0], 42);
    CHECK(atlas.contentValid(slots[0], 42));
    CHECK_FALSE(atlas.contentValid(slots[0], 43));
    atlas.invalidateAll();
    CHECK_FALSE(atlas.contentValid(slots[0], 42));

    // Priority eviction: one big slot, two lights want it; the important one wins.
    ShadowAtlas small(AtlasConfig{1024, {1, 1, 1, 1}});
    auto s1 = small.allocate({{1, 512, 1.f}, {2, 512, 1.f}, {3, 512, 1.f}, {4, 512, 1.f}}, 1);
    for (const auto& s : s1) CHECK(s.valid());
    auto s2 = small.allocate({{1, 512, 1.f}, {2, 512, 1.f}, {3, 512, 1.f}, {4, 512, 1.f}, {5, 512, 9.f}}, 2);
    CHECK(s2[4].valid());  // the newcomer is the most important
    int lost = 0;
    for (int i = 0; i < 4; ++i) lost += s2[static_cast<size_t>(i)].valid() ? 0 : 1;
    CHECK(lost == 1);
    // LRU: a light that is not requested keeps its slot cached until space is needed.
    ShadowAtlas lru(AtlasConfig{1024, {1, 1, 1, 1}});
    (void)lru.allocate({{10, 512, 1.f}, {11, 512, 1.f}, {12, 512, 1.f}, {13, 512, 1.f}}, 1);
    AtlasSlot cached = lru.slotOf(10);
    (void)lru.allocate({{11, 512, 1.f}, {12, 512, 1.f}, {13, 512, 1.f}}, 2);
    CHECK(lru.slotOf(10) == cached);  // still cached
    auto taken = lru.allocate({{11, 512, 1.f}, {12, 512, 1.f}, {13, 512, 1.f}, {14, 512, 0.1f}}, 3);
    CHECK(taken[3] == cached);  // the stale light's slot goes to the newcomer
    CHECK_FALSE(lru.slotOf(10).valid());
    // Tier changes beyond a factor of two move the light; small changes don't.
    ShadowAtlas tiers;
    auto t1 = tiers.allocate({{7, 600, 1.f}}, 1);
    CHECK(t1[0].size == 512);
    auto t2 = tiers.allocate({{7, 900, 1.f}}, 2);
    CHECK(t2[0] == t1[0]);
    auto t3 = tiers.allocate({{7, 2000, 1.f}}, 3);
    CHECK(t3[0].size == 1024);
    CHECK(tiers.used(2) == 0);  // the old slot was released
}

TEST_CASE("shadows: planner picks lights, caches static shadows and re-renders when casters move") {
    FrameData f = baseFrame();
    f.lights.push_back(pointLight({0, 2.5f, 0}, 8.f, lightId(1, 0)));
    LightItem spot = pointLight({3, 3, 0}, 9.f, lightId(2, 0));
    spot.kind = LightItem::Kind::Spot;
    spot.direction = {0, -1, 0};
    spot.cosCone = std::cos(radians(35.f));
    f.lights.push_back(spot);
    LightItem off = pointLight({-3, 2, 0}, 6.f, lightId(3, 0));
    off.shadows = false;
    f.lights.push_back(off);
    f.draws.push_back(box(10, {0, 0.5f, 0}, {1, 1, 1}));
    f.draws.push_back(box(11, {0, -0.05f, 0}, {20, 0.1f, 20}));
    f.draws.push_back(box(12, {40, 1, 0}, {1, 1, 1}));  // far outside every light

    LocalShadowPlanner planner;
    ShadowSettings s;
    ShadowPlan p = planner.plan(f, s);
    CHECK(p.requested == 2);
    CHECK(p.shadowed == 2);
    CHECK(p.lights[0].projection == Projection::Cube);
    CHECK(p.lights[1].projection == Projection::Spot);
    CHECK(p.lights[2].projection == Projection::None);
    CHECK(p.lights[2].reason == "disabled");
    CHECK(p.faces.size() == 7);  // 6 cube faces + 1 spot view
    for (const ShadowFace& face : p.faces) {
        CHECK(face.viewport.w > 0);
        CHECK(face.viewport.x + face.viewport.w <= 2048);
        CHECK(face.viewport.y + face.viewport.h <= 2048);
    }
    // GPU parameters: slot uv, projection + 4 * quadrant, strength / bias / tanHalf.
    auto g = gpuShadowParams(p.lights[0], p.settings.atlas);
    CHECK(static_cast<int>(g[0].w) % 4 == static_cast<int>(Projection::Cube));
    CHECK(static_cast<int>(g[0].w) / 4 == p.lights[0].slot.quadrant);
    CHECK(g[0].z == doctest::Approx(static_cast<float>(p.lights[0].slot.size) / 2048.f));
    CHECK(g[1].x == doctest::Approx(1.f));
    CHECK(g[1].w > 1.f);
    CHECK(gpuShadowParams(p.lights[2], p.settings.atlas)[0].w == 0.f);

    // Nothing moved: both shadows come from the cache.
    p = planner.plan(f, s);
    CHECK(p.faces.empty());
    CHECK(p.cachedLights == 2);
    // A caster far away moves: still cached.
    f.draws[2].model = Mat4::translate({41, 1, 0});
    f.draws[2].worldBounds = Aabb{{40.5f, 0.5f, -0.5f}, {41.5f, 1.5f, 0.5f}};
    p = planner.plan(f, s);
    CHECK(p.faces.empty());
    // A caster inside the point light's range moves: only that light re-renders.
    f.draws[0].model = Mat4::trs({0.2f, 0.5f, 0}, {0, 0, 0}, {1, 1, 1});
    f.draws[0].worldBounds = Aabb{{-0.3f, 0, -0.5f}, {0.7f, 1, 0.5f}};
    p = planner.plan(f, s);
    CHECK(p.lights[0].updated);
    CHECK(p.lights[1].updated);  // the spot reaches the box too (range 9)
    // Moving the light itself re-renders it.
    p = planner.plan(f, s);
    CHECK(p.faces.empty());
    f.lights[0].position.x += 0.1f;
    p = planner.plan(f, s);
    CHECK(p.lights[0].updated);
    CHECK_FALSE(p.lights[1].updated);
    // invalidate (shadow_atlas_info) forces everything.
    planner.invalidate();
    p = planner.plan(f, s);
    CHECK(p.faces.size() == 7);
    // Dual paraboloid: 2 views.
    f.lights[0].shadowMode = 1;
    p = planner.plan(f, s);
    CHECK(p.lights[0].projection == Projection::DualParaboloid);
    int dp = 0;
    for (const auto& face : p.faces) dp += face.light == 0 ? 1 : 0;
    CHECK(dp == 2);
    // The big floor box makes paraboloid shadows approximate: agents are told to use a cube.
    bool warned = false;
    for (const auto& w : p.warnings) warned = warned || w.find("dual_paraboloid") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("shadows: budgets degrade gracefully (light count, face updates, stills)") {
    FrameData f = baseFrame();
    f.camera.eye = {0, 30, 30};
    f.camera.target = {0, 0, 0};
    f.view = f.camera.view();
    for (int i = 0; i < 30; ++i) {
        f.lights.push_back(pointLight({static_cast<float>(i % 6) * 3.f - 7.5f, 2.f, static_cast<float>(i / 6) * 3.f - 6.f}, 4.f,
                                      lightId(static_cast<EntityId>(100 + i), 0)));
    }
    f.lights[5].intensity = 50.f;  // the most important one
    LocalShadowPlanner planner;
    ShadowSettings s;
    s.maxLights = 8;
    s.maxFaceUpdates = 12;
    ShadowPlan p = planner.plan(f, s);
    CHECK(p.candidates == 30);
    CHECK(p.overBudget == 22);
    CHECK(p.faces.size() <= 12);
    CHECK(p.deferredFaces > 0);
    CHECK_FALSE(p.warnings.empty());
    CHECK(p.lights[5].projection != Projection::None);  // kept and rendered first
    int pending = 0;
    for (const auto& ls : p.lights) pending += ls.reason == "pending" ? 1 : 0;
    CHECK(pending == 6);  // 8 kept, 2 rendered (12 faces), 6 wait without a shadow for now
    // Over the next frames the rest fills in, then everything is cached.
    for (int i = 0; i < 4; ++i) p = planner.plan(f, s);
    CHECK(p.shadowed == 8);
    CHECK(p.faces.empty());
    // Stills render every face at once.
    LocalShadowPlanner still;
    s.unlimitedUpdates = true;
    p = still.plan(f, s);
    CHECK(p.faces.size() == 48);
    CHECK(p.shadowed == 8);
    // Hard caps stay bounded whatever the environment says.
    Environment env;
    env.localShadowLights = 1000;
    env.localShadowUpdates = 100000;
    ShadowSettings capped = settingsFor(env, 0, false);
    CHECK(capped.maxLights == kMaxShadowedLights);
    CHECK(capped.maxFaceUpdates <= kMaxShadowedLights * 6);
    CHECK(settingsFor(env, 2, false).maxLights < capped.maxLights);  // fast tier halves
    env.localShadowLights = 0;
    p = LocalShadowPlanner{}.plan(f, settingsFor(env, 0, true));
    CHECK(p.shadowed == 0);
    CHECK(p.faces.empty());
}

TEST_CASE("shadows: out of view, max distance and lamp fixtures") {
    FrameData f = baseFrame();
    f.lights.push_back(pointLight({0, 2, -200}, 5.f, lightId(1, 0)));  // behind the far plane? no: far away
    f.lights.push_back(pointLight({0, 2, 60}, 5.f, lightId(2, 0)));    // behind the camera
    LightItem limited = pointLight({0, 2, -30}, 5.f, lightId(3, 0));
    limited.shadowMaxDistance = 20.f;
    f.lights.push_back(limited);
    LocalShadowPlanner planner;
    ShadowPlan p = planner.plan(f, ShadowSettings{});
    CHECK(p.lights[1].reason == "out_of_view");
    CHECK(p.lights[2].reason == "beyond_max_distance");

    // A bulb mesh around the light does not swallow it; a wall next to it does cast.
    DrawItem bulb = box(5, {0, 2, 0}, {0.1f, 0.1f, 0.1f});
    DrawItem wall = box(6, {1, 2, 0}, {0.2f, 4, 4});
    CHECK_FALSE(castsLocalShadow(bulb, {0, 2, 0}, 8.f));
    CHECK(castsLocalShadow(wall, {0, 2, 0}, 8.f));
    DrawItem head = box(8, {0.35f, 2.15f, 0}, {0.5f, 0.3f, 0.4f});  // a lamp head just beside the light
    CHECK_FALSE(castsLocalShadow(head, {0, 2, 0}, 8.f));
    DrawItem crate = box(9, {1.5f, 0.5f, 0}, {0.6f, 0.6f, 0.6f});  // small, but away from the light
    CHECK(castsLocalShadow(crate, {0, 2, 0}, 8.f));
    DrawItem room = box(7, {0, 1.5f, 0}, {6, 3, 6});  // one big mesh around the light: still casts
    CHECK(castsLocalShadow(room, {0, 2, 0}, 8.f));
    DrawItem glass = wall;
    glass.surface.color.w = 0.3f;
    CHECK_FALSE(castsLocalShadow(glass, {0, 2, 0}, 8.f));
}

TEST_CASE("shadows: buildFrame carries the light component's shadow settings") {
    Scene s;
    EntityId e = s.create("Lamp");
    REQUIRE(s.patchComponent(e, "light", Json::object({{"kind", "point"}, {"shadowMode", "dual_paraboloid"}, {"shadowBias", 0.05},
                                                       {"shadowResolution", 512}, {"shadowMaxDistance", 40}}))
                .ok());
    EntityId plain = s.create("Plain");
    REQUIRE(s.patchComponent(plain, "light", Json::object({{"kind", "spot"}})).ok());
    EntityId off = s.create("Off");
    REQUIRE(s.patchComponent(off, "light", Json::object({{"castShadows", false}})).ok());
    ViewCamera cam;
    FrameData f = buildFrame(s, cam, 320, 180, {});
    REQUIRE(f.lights.size() == 3);
    int checked = 0;
    for (const LightItem& l : f.lights) {
        if (l.id == lightId(e, 0)) {
            CHECK(l.shadows);
            CHECK(l.shadowMode == 1);
            CHECK(l.shadowBias == doctest::Approx(0.05f));
            CHECK(l.shadowResolution == 512);
            CHECK(l.shadowMaxDistance == doctest::Approx(40.f));
            ++checked;
        } else if (l.id == lightId(plain, 0)) {
            CHECK(l.shadows);  // lights cast shadows by default
            CHECK(l.shadowMode == 0);
            ++checked;
        } else if (l.id == lightId(off, 0)) {
            CHECK_FALSE(l.shadows);
            ++checked;
        }
    }
    CHECK(checked == 3);
}

namespace {

std::unique_ptr<Engine> shadowEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;  // the shadow plan runs on the CPU there too
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Shadows", true);
    return e;
}

ToolResult callTool(Engine& e, const char* tool, const char* args) {
    return e.callTool(tool, Json::parse(args).value(), "agent:test");
}

}  // namespace

TEST_CASE("shadows: shadow_atlas_info and light_shadows tools") {
    auto e = shadowEngine();
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Wall","position":[0,1,-2],"components":{"mesh":{"mesh":"cube"}}})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create", R"({"name":"Lamp","position":[0,2,0],"components":{"light":{"kind":"point","intensity":5}}})").isError);
    REQUIRE_FALSE(callTool(*e, "entity_create",
                           R"({"name":"Spot","position":[2,3,0],"rotation":[-90,0,0],"components":{"light":{"kind":"spot","intensity":5}}})")
                      .isError);
    const char* view = R"({"view":{"eye":[0,3,8],"target":[0,1,0]}})";

    ToolResult r = callTool(*e, "shadow_atlas_info", view);
    const std::string summary = r.content.empty() ? std::string() : r.content.front().text;
    INFO(summary);
    REQUIRE_FALSE(r.isError);
    const Json& j = r.structured;
    CHECK(j.get("shadowed").asInt() == 2);
    CHECK(j.get("atlasSize").asInt() == 4096);
    CHECK(j.get("facesRendered").asInt() == 7);
    REQUIRE(j.get("lights").size() == 2);
    bool sawLamp = false;
    for (const Json& l : j.get("lights").elements()) {
        if (l.get("name").asString() == "Lamp") {
            sawLamp = true;
            CHECK(l.get("projection").asString() == "cube");
        }
    }
    CHECK(sawLamp);
    // Second frame: everything comes from the static cache.
    r = callTool(*e, "shadow_atlas_info", view);
    CHECK(r.structured.get("facesRendered").asInt() == 0);
    CHECK(r.structured.get("cachedLights").asInt() == 2);
    // invalidate re-renders everything.
    r = callTool(*e, "shadow_atlas_info", R"({"invalidate":true,"view":{"eye":[0,3,8],"target":[0,1,0]}})");
    CHECK(r.structured.get("facesRendered").asInt() == 7);
    // One light: its casters.
    r = callTool(*e, "shadow_atlas_info", R"({"entity":"Lamp","view":{"eye":[0,3,8],"target":[0,1,0]}})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("lights").size() == 1);
    CHECK(r.structured.get("casters").size() >= 1);
    // Misuse: a non-light entity, a bad view.
    CHECK(callTool(*e, "shadow_atlas_info", R"({"entity":"Wall"})").isError);
    CHECK(callTool(*e, "shadow_atlas_info", R"({"view":"sideways"})").isError);

    // Bulk edit with undo.
    r = callTool(*e, "light_shadows", R"({"lights":"all","kind":"point","enabled":false})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("changed").size() == 1);
    CHECK(callTool(*e, "shadow_atlas_info", view).structured.get("shadowed").asInt() == 1);
    REQUIRE_FALSE(callTool(*e, "history", R"({"action":"undo"})").isError);
    CHECK(callTool(*e, "shadow_atlas_info", view).structured.get("shadowed").asInt() == 2);
    REQUIRE_FALSE(callTool(*e, "light_shadows", R"({"lights":["Spot"],"resolution":256,"bias":0.05})").isError);
    const Light* spot = e->scene().get<Light>(e->scene().find("Spot"));
    REQUIRE(spot);
    CHECK(spot->shadowResolution == 256);
    CHECK(spot->shadowBias == doctest::Approx(0.05f));
    CHECK(callTool(*e, "light_shadows", R"({"lights":"Wall","enabled":true})").isError);   // not a light
    CHECK(callTool(*e, "light_shadows", R"({"lights":"Spot"})").isError);                 // nothing to change
    CHECK(callTool(*e, "light_shadows", R"({"lights":"Lamp","mode":"sphere"})").isError);  // bad enum value

    // The atlas debug view is a valid capture mode.
    CHECK_FALSE(callTool(*e, "viewport_capture", R"({"width":64,"height":36,"samples":1,"debug_view":"shadow_atlas"})").isError);
}
