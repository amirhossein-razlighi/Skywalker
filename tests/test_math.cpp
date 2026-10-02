#include <doctest/doctest.h>

#include "skywalker/math/Math.h"

using namespace sky;

static bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return length(a - b) < eps; }

TEST_CASE("math: trs and inverse") {
    Mat4 m = Mat4::trs({1, 2, 3}, {10, 45, -20}, {2, 2, 2});
    Mat4 id = m * m.inverse();
    for (int i = 0; i < 16; ++i) CHECK(id.m[i] == doctest::Approx(Mat4{}.m[i]).epsilon(1e-4));
    CHECK(near(m.translation(), {1, 2, 3}));
}

TEST_CASE("math: yaw rotates -Z forward toward -X for +90 degrees") {
    Vec3 fwd = Mat4::rotateEulerDeg({0, 90, 0}).transformDir({0, 0, -1});
    CHECK(near(fwd, {-1, 0, 0}));
}

TEST_CASE("math: lookAt + perspective projects target to screen center with depth in [0,1]") {
    Mat4 view = Mat4::lookAt({0, 2, 5}, {0, 0, 0}, {0, 1, 0});
    Mat4 proj = Mat4::perspective(radians(60), 16.f / 9.f, 0.1f, 100.f);
    Vec3 ndc = (proj * view).transformPoint({0, 0, 0});
    CHECK(ndc.x == doctest::Approx(0).epsilon(1e-5));
    CHECK(ndc.y == doctest::Approx(0).epsilon(1e-5));
    CHECK(ndc.z > 0.f);
    CHECK(ndc.z < 1.f);
    Vec3 nearPt = (proj * view).transformPoint(Vec3{0, 2, 5} + normalize(Vec3{0, -2, -5}) * 0.1f);
    CHECK(nearPt.z == doctest::Approx(0).epsilon(1e-3));
}

TEST_CASE("math: ray vs aabb") {
    Aabb box{{-1, -1, -1}, {1, 1, 1}};
    CHECK(intersect({{0, 0, 5}, {0, 0, -1}}, box) == doctest::Approx(4));
    CHECK(intersect({{0, 3, 5}, {0, 0, -1}}, box) < 0);
    CHECK(intersect({{0, 0, 0}, {1, 0, 0}}, box) == doctest::Approx(1));
}
