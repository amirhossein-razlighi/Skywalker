#include <doctest/doctest.h>

#include <filesystem>

#include "skywalker/render/MeshData.h"
#include "skywalker/render/Renderer.h"

using namespace sky;

TEST_CASE("mesh: primitives are well formed with outward CCW winding") {
    for (const auto& name : MeshRenderer::primitives()) {
        // Vegetation / ground-detail meshes stand on y = 0 at natural sizes and are partly
        // open (blades, fronds): they are checked in test_world.
        if (name == "grass" || name == "grass_tall" || name == "fern" || name == "flowers" || name == "pebbles" ||
            name == "shell" || name == "rock") {
            continue;
        }
        auto m = mesh::primitive(name);
        REQUIRE(m.ok());
        CHECK(m->indices.size() % 3 == 0);
        CHECK(m->vertexCount() > 0);
        for (uint32_t i : m->indices) REQUIRE(i < m->vertexCount());
        // Bounds fit the unit cube.
        CHECK(m->bounds.min.x >= -0.501f);
        CHECK(m->bounds.max.y <= 0.501f);
        // Non-degenerate triangles face along their vertex normals.
        int inward = 0;
        for (size_t t = 0; t < m->indices.size(); t += 3) {
            auto P = [&](uint32_t i) {
                const float* v = &m->vertices[i * MeshData::kFloatsPerVertex];
                return Vec3{v[0], v[1], v[2]};
            };
            auto N = [&](uint32_t i) {
                const float* v = &m->vertices[i * MeshData::kFloatsPerVertex + 3];
                return Vec3{v[0], v[1], v[2]};
            };
            uint32_t a = m->indices[t], b = m->indices[t + 1], c = m->indices[t + 2];
            Vec3 face = cross(P(b) - P(a), P(c) - P(a));
            if (length(face) < 1e-7f) continue;
            if (dot(face, N(a) + N(b) + N(c)) < 0) ++inward;
        }
        INFO(name);
        CHECK(inward == 0);
    }
    CHECK_FALSE(mesh::primitive("dodecahedron").ok());
}

TEST_CASE("mesh: OBJ parsing (quads, negative indices, missing normals, normalization)") {
    const char* obj = R"(
# a unit quad made of one polygon, using negative indices
v 0 0 0
v 2 0 0
v 2 2 0
v 0 2 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f -4/-4 -3/-3 -2/-2 -1/-1
)";
    auto m = mesh::parseObj(obj, true);
    REQUIRE(m.ok());
    CHECK(m->indices.size() == 6);
    CHECK(m->bounds.max.x == doctest::Approx(0.5f));
    CHECK(m->bounds.min.y == doctest::Approx(-0.5f));
    const float* n = &m->vertices[3];
    CHECK(std::fabs(n[2]) == doctest::Approx(1.f));  // generated normal along Z
    CHECK_FALSE(mesh::parseObj("v 0 0 0\nf 1 2 3\n").ok());
    CHECK_FALSE(mesh::parseObj("# nothing\n").ok());
}

TEST_CASE("png: encoder emits a valid signature and IHDR") {
    Image img(4, 3);
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint8_t>(i * 7);
    auto png = encodePng(img);
    REQUIRE(png.size() > 33);
    CHECK(png[0] == 0x89);
    CHECK(png[1] == 'P');
    CHECK(png[12] == 'I');
    CHECK(png[15] == 'R');
    CHECK(png[19] == 4);  // width (big endian, low byte)
    CHECK(png[23] == 3);  // height
}

TEST_CASE("png: writing into a folder that does not exist yet creates it") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "sky_png_test" / "nested" / "lookdev";
    fs::remove_all(fs::temp_directory_path() / "sky_png_test");
    Image img(2, 2);
    REQUIRE(writePng(img, (dir / "shot.png").string()).ok());
    CHECK(fs::exists(dir / "shot.png"));
    CHECK(fs::file_size(dir / "shot.png") > 33);
    fs::remove_all(fs::temp_directory_path() / "sky_png_test");
}

TEST_CASE("frame: visible entities, picking and scene camera") {
    Scene s;
    EntityId near = s.create("Near");
    (void)s.patchComponent(near, "mesh", Json::object({{"mesh", "cube"}}));
    EntityId far = s.create("Far");
    (void)s.patchComponent(far, "mesh", Json::object({{"mesh", "sphere"}}));
    s.get<Transform>(far)->position = {0, 0, -10};
    EntityId behind = s.create("Behind");
    (void)s.patchComponent(behind, "mesh", Json::object());
    s.get<Transform>(behind)->position = {0, 0, 20};

    ViewCamera cam;
    cam.eye = {0, 0, 5};
    cam.target = {0, 0, 0};
    FrameData f = buildFrame(s, cam, 800, 600, {});
    auto vis = visibleEntities(s, f);
    REQUIRE(vis.size() == 2);
    CHECK(vis[0].id == near);  // sorted by depth
    CHECK(vis[1].id == far);
    CHECK(vis[0].x > 300);
    CHECK(vis[0].x + vis[0].w < 500);

    CHECK(pick(s, f, 400, 300) == near);
    CHECK(pick(s, f, 5, 5) == kNoEntity);
    s.get<Transform>(near)->position = {5, 0, 0};
    f = buildFrame(s, cam, 800, 600, {});
    CHECK(pick(s, f, 400, 300) == far);

    ViewCamera sc;
    CHECK_FALSE(sceneCamera(s, sc));
    EntityId camE = s.create("Cam");
    (void)s.patchComponent(camE, "camera", Json::object({{"fov", 40}, {"tiltShift", 0.5}}));
    s.get<Transform>(camE)->position = {1, 2, 3};
    REQUIRE(sceneCamera(s, sc));
    CHECK(sc.eye == Vec3{1, 2, 3});
    CHECK(sc.fovDeg == doctest::Approx(40));
    CHECK(sc.tiltShift == doctest::Approx(0.5f));  // the miniature lens reaches the post stack
}

TEST_CASE("frame: hidden, disabled and child-of-disabled entities are not drawn") {
    Scene s;
    EntityId a = s.create("A");
    (void)s.patchComponent(a, "mesh", Json::object({{"visible", false}}));
    EntityId p = s.create("P");
    EntityId c = s.create("C", p);
    (void)s.patchComponent(c, "mesh", Json::object());
    (void)s.setEnabled(p, false);
    FrameData f = buildFrame(s, ViewCamera{}, 64, 64, {});
    CHECK(f.draws.empty());
}

TEST_CASE("orbit camera: lookAt round trips and frame fits a box") {
    OrbitCamera c;
    c.lookAt({3, 4, 5}, {0, 1, 0});
    ViewCamera v = c.toView();
    CHECK(length(v.eye - Vec3{3, 4, 5}) < 1e-3f);
    c.frame({{-1, -1, -1}, {1, 1, 1}});
    CHECK(c.target == Vec3{0, 0, 0});
    CHECK(c.distance > 1.7f);
}

TEST_CASE("renderer: null backend produces an image of the requested size") {
    auto r = createNullRenderer();
    Scene s;
    EntityId e = s.create("Box");
    (void)s.patchComponent(e, "mesh", Json::object({{"color", "#ff0000"}}));
    ViewCamera cam;
    cam.eye = {0, 0, 3};
    FrameData f = buildFrame(s, cam, 64, 48, {});
    REQUIRE(r->render(f).ok());
    auto img = r->readback();
    REQUIRE(img.ok());
    CHECK(img->width == 64);
    CHECK(img->height == 48);
    const uint8_t* center = img->at(32, 24);
    CHECK(center[0] > center[2]);  // red box in the middle
}

TEST_CASE("render: custom views clip for close-ups and kilometer vistas alike") {
    ViewCamera v;  // e.g. inherited from an orbit camera close to a small scene
    v.nearPlane = 2.5f;
    v.farPlane = 1000.f;
    v.lookFrom({150, 320, 700}, {-120, 60, -150});
    CHECK(v.farPlane >= 40000.f);  // far ranges several kilometers away stay visible
    CHECK(v.nearPlane <= 0.25f);
    v.lookFrom({0, 1, 0.3f}, {0, 1, 0});
    CHECK(v.nearPlane < 0.03f);  // macro close-up
    CHECK(v.eye.z == doctest::Approx(0.3f));
}
