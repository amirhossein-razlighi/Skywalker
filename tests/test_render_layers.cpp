// Velocity buffer bookkeeping (per-object motion vectors) and render layers / light v2.
#include <doctest/doctest.h>

#include "skywalker/render/MotionHistory.h"
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
