#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "anim_fixtures.h"
#include "skywalker/anim/Animation.h"
#include "skywalker/core/Random.h"
#include "skywalker/render/Gltf.h"

using namespace sky;
using namespace sky::anim;
namespace fs = std::filesystem;

namespace {

bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return distance(a, b) <= eps; }

Vec3 vpos(const MeshData& m, size_t i) {
    const float* v = &m.vertices[i * MeshData::kFloatsPerVertex];
    return {v[0], v[1], v[2]};
}

GltfImport strip(const skytest::StripOptions& o = {}) {
    auto g = parseGltf(skytest::bytesOfString(skytest::makeStripGltf(o)), ".", false);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
    return std::move(g.value());
}

/// Index of the vertex closest to a rest-pose point.
size_t vertexAt(const MeshData& m, Vec3 p) {
    size_t best = 0;
    for (size_t i = 0; i < m.vertexCount(); ++i) {
        if (distance(vpos(m, i), p) < distance(vpos(m, best), p)) best = i;
    }
    return best;
}

}  // namespace

TEST_CASE("anim math: Euler round trips match Mat4::rotateEulerDeg") {
    Random rng(7);
    for (int i = 0; i < 200; ++i) {
        Vec3 e{rng.nextFloat() * 160.f - 80.f, rng.nextFloat() * 340.f - 170.f, rng.nextFloat() * 340.f - 170.f};
        Quat q = Quat::fromEulerDeg(e);
        Mat4 a = Mat4::rotateEulerDeg(e), b = q.matrix();
        for (int k = 0; k < 16; ++k) CHECK(a.m[k] == doctest::Approx(b.m[k]).epsilon(1e-4));
        Vec3 back = eulerDegFromQuat(q);
        Mat4 c = Mat4::rotateEulerDeg(back);
        for (int k = 0; k < 16; ++k) CHECK(a.m[k] == doctest::Approx(c.m[k]).epsilon(1e-3));
    }
}

TEST_CASE("anim math: slerp, fromTo and TRS decomposition") {
    Quat a, b = Quat::axisAngle({0, 0, 1}, kPi / 2);
    Quat mid = slerp(a, b, 0.5f);
    CHECK(near(mid.rotate({1, 0, 0}), {std::cos(kPi / 4), std::sin(kPi / 4), 0}));
    // Shortest path: q and -q are the same rotation.
    Quat negB{-b.x, -b.y, -b.z, -b.w};
    CHECK(near(slerp(a, negB, 0.5f).rotate({1, 0, 0}), mid.rotate({1, 0, 0})));
    CHECK(near(Quat::fromTo({0, 0, 1}, {1, 0, 0}).rotate({0, 0, 1}), {1, 0, 0}));
    CHECK(near(Quat::fromTo({0, 1, 0}, {0, -1, 0}).rotate({0, 1, 0}), {0, -1, 0}));

    Trs t{{1, 2, 3}, Quat::fromEulerDeg({10, 20, 30}), {2, 3, 4}};
    Trs d = Trs::fromMatrix(t.matrix());
    CHECK(near(d.t, t.t));
    CHECK(near(d.s, t.s));
    CHECK(std::fabs(dot(d.r, t.r)) == doctest::Approx(1.f).epsilon(1e-5));
}

TEST_CASE("gltf skins: skeleton, slots, influences and clips are parsed") {
    GltfImport g = strip();
    REQUIRE(g.animation);
    CHECK(g.skinned);
    const Library& lib = *g.animation;
    REQUIRE(lib.skeleton.bones.size() == 4);  // Armature, Root, Upper, Body
    CHECK(lib.skeleton.bones[0].name == "Armature");
    CHECK(lib.skeleton.bones[2].name == "Upper");
    CHECK(lib.skeleton.bones[2].parent == 1);
    CHECK(near(lib.skeleton.bones[2].rest.t, {0, 1, 0}));
    CHECK(lib.rootBone == 1);
    CHECK(lib.clipNames() == std::vector<std::string>{"Bend", "Steps", "Smooth", "Walk", "Wave"});
    CHECK(lib.clip("walk") != nullptr);  // case-insensitive lookup
    CHECK(lib.clip("Bend")->duration == doctest::Approx(1.f));

    const MeshData& m = g.mesh;
    REQUIRE(m.skinned());
    CHECK(m.skin.jointNames == std::vector<std::string>{"Root", "Upper"});
    CHECK(m.skin.joints.size() == m.vertexCount() * 4);
    CHECK(m.skin.bind.size() == m.vertexCount() * 6);
    // The shared middle row is split 50/50.
    size_t mid = vertexAt(m, {0.1f, 1.f, 0.f});
    CHECK(m.skin.weights[mid * 4] == doctest::Approx(0.5f));
    CHECK(m.skin.weights[mid * 4 + 1] == doctest::Approx(0.5f));
    // Rest pose = bind pose here, so static vertices equal the authored positions.
    CHECK(near(vpos(m, vertexAt(m, {0.1f, 2.f, 0.f})), {0.1f, 2.f, 0.f}));
    // Unsigned-byte joints parse the same.
    GltfImport g8 = strip({.u8Joints = true});
    CHECK(g8.mesh.skin.joints == m.skin.joints);
}

TEST_CASE("anim sampling: linear (slerp), step and cubic spline") {
    GltfImport g = strip();
    const Library& lib = *g.animation;
    Pose pose = restPose(lib.skeleton);
    sampleClip(*lib.clip("Bend"), 0.5f, pose);
    CHECK(near(pose[2].r.rotate({1, 0, 0}), {std::cos(kPi / 4), std::sin(kPi / 4), 0}));
    sampleClip(*lib.clip("Bend"), 5.f, pose);  // clamps past the end
    CHECK(near(pose[2].r.rotate({1, 0, 0}), {0, 1, 0}));

    Pose p2 = restPose(lib.skeleton);
    sampleClip(*lib.clip("Steps"), 0.99f, p2);
    CHECK(near(p2[1].t, {0, 0, 0}));
    sampleClip(*lib.clip("Steps"), 1.f, p2);
    CHECK(near(p2[1].t, {1, 0, 0}));

    Pose p3 = restPose(lib.skeleton);
    sampleClip(*lib.clip("Smooth"), 0.5f, p3);
    CHECK(near(p3[1].t, {0, 1, 0}));  // Hermite with zero tangents: h01(0.5) = 0.5
    sampleClip(*lib.clip("Smooth"), 0.25f, p3);
    CHECK(p3[1].t.y == doctest::Approx(2.f * (-2 * 0.015625f + 3 * 0.0625f)));
    CHECK(sampleTranslation(*lib.clip("Walk"), 1, 0.5f, {}).z == doctest::Approx(0.75f));
}

TEST_CASE("cpu skinning: a two-bone strip bends correctly") {
    GltfImport g = strip();
    const Library& lib = *g.animation;
    const MeshData& m = g.mesh;
    Pose pose = restPose(lib.skeleton);
    sampleClip(*lib.clip("Bend"), 1.f, pose);  // Upper rotated 90 degrees about Z
    std::vector<Mat4> globals, palette;
    computeGlobals(lib.skeleton, pose, globals);
    std::vector<int> map = mapSkin(m.skin, lib.skeleton);
    CHECK(map == std::vector<int>{1, 2});
    skinPalette(m.skin, map, globals, palette);
    MeshData posed;
    skinMesh(m, palette, posed);
    // Top vertex: (0.1, 1) above the joint rotates to (-1, 0.1) around (0, 1).
    CHECK(near(vpos(posed, vertexAt(m, {0.1f, 2.f, 0.f})), {-1.f, 1.1f, 0.f}));
    // Bottom vertex follows Root only: unchanged.
    CHECK(near(vpos(posed, vertexAt(m, {0.1f, 0.f, 0.f})), {0.1f, 0.f, 0.f}));
    // Shared vertex: halfway between both joints' results.
    CHECK(near(vpos(posed, vertexAt(m, {0.1f, 1.f, 0.f})), {0.05f, 1.05f, 0.f}));
    // Normals rotate with the bone (+Z stays +Z for a rotation about Z).
    const float* n = &posed.vertices[vertexAt(m, {0.1f, 2.f, 0.f}) * MeshData::kFloatsPerVertex + 3];
    CHECK(near({n[0], n[1], n[2]}, {0, 0, 1}));
    // Posed bounds are conservative.
    Aabb b = posedBounds(m.skin, palette);
    for (size_t i = 0; i < posed.vertexCount(); ++i) {
        Vec3 p = vpos(posed, i);
        CHECK(p.x >= b.min.x - 1e-4f);
        CHECK(p.x <= b.max.x + 1e-4f);
        CHECK(p.y >= b.min.y - 1e-4f);
        CHECK(p.y <= b.max.y + 1e-4f);
    }
}

TEST_CASE("cpu skinning: rest palette reproduces the static mesh after import transforms") {
    fs::path dir = fs::temp_directory_path() / ("sky-anim-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::ofstream(dir / "strip.gltf") << skytest::makeStripGltf({.twoMaterials = true});
    for (int variant = 0; variant < 4; ++variant) {
        mesh::LoadOptions lo;
        lo.zUp = variant & 1;
        lo.turnAround = true;
        lo.normalize = variant & 2;
        lo.part = variant == 3 ? 1 : kGltfAllParts;
        auto m = mesh::loadMeshFile((dir / "strip.gltf").string(), lo);
        REQUIRE(m.ok());
        REQUIRE(m->skinned());
        std::vector<Mat4> palette;
        restPalette(m->skin, palette);
        MeshData posed;
        skinMesh(*m, palette, posed);
        for (size_t i = 0; i < m->vertexCount(); ++i) CHECK(near(vpos(posed, i), vpos(*m, i), 1e-3f));
    }
    fs::remove_all(dir);
}

TEST_CASE("anim library: .anim files round-trip; clips retarget by bone name") {
    GltfImport g = strip();
    std::vector<uint8_t> bytes = encodeLibrary(*g.animation);
    auto back = decodeLibrary(bytes);
    REQUIRE(back.ok());
    CHECK(back->skeleton.names() == g.animation->skeleton.names());
    CHECK(back->rootBone == g.animation->rootBone);
    REQUIRE(back->clips.size() == g.animation->clips.size());
    for (size_t i = 0; i < back->clips.size(); ++i) {
        const Clip& a = g.animation->clips[i];
        const Clip& b = back->clips[i];
        CHECK(a.name == b.name);
        REQUIRE(a.channels.size() == b.channels.size());
        for (size_t c = 0; c < a.channels.size(); ++c) {
            CHECK(a.channels[c].times == b.channels[c].times);
            CHECK(a.channels[c].values == b.channels[c].values);
            CHECK(a.channels[c].interp == b.channels[c].interp);
        }
    }
    bytes[3] = 'X';
    CHECK(!decodeLibrary(bytes).ok());
    bytes.resize(20);
    CHECK(!decodeLibrary(bytes).ok());

    // A rig with namespaced names ("mixamorig:Upper") and a different bone order.
    Skeleton other;
    other.bones.push_back({"mixamorig:Root", -1, {}});
    other.bones.push_back({"Extra", 0, {}});
    other.bones.push_back({"mixamorig:Upper", 0, {{0, 2, 0}, {}, {1, 1, 1}}});
    CHECK(other.find("Upper") == 2);
    CHECK(other.find("ROOT") == 0);
    CHECK(other.find("missing") == -1);
    Clip walk;
    CHECK(retarget(*g.animation->clip("Walk"), g.animation->skeleton, other, walk) == 2);
    CHECK(walk.hasChannel(0, Path::Translation));  // the hips keep their translation
    CHECK(walk.hasChannel(2, Path::Rotation));
}

TEST_CASE("gltf skins: animation-only files and node-animated rigid parts") {
    auto g = parseGltf(skytest::bytesOfString(skytest::makeStripGltf({.withMesh = false})), ".", false);
    REQUIRE(g.ok());
    CHECK(g->mesh.indices.empty());
    REQUIRE(g->animation);
    CHECK(g->animation->clips.size() == 5);

    // A static (unskinned) mesh under an animated node follows it rigidly.
    std::string doc = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"name":"Spinner","children":[1]},{"name":"Blade","mesh":0,"translation":[0,1,0]}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
      "animations":[{"name":"Spin","samplers":[{"input":2,"output":3}],"channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}]}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                   {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
                   {"bufferView":2,"componentType":5126,"count":2,"type":"SCALAR"},
                   {"bufferView":3,"componentType":5126,"count":2,"type":"VEC4"}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},
                     {"buffer":0,"byteOffset":44,"byteLength":8},{"buffer":0,"byteOffset":52,"byteLength":32}],
      "buffers":[{"byteLength":84,"uri":"URI"}]})";
    std::vector<uint8_t> bin(84, 0);
    float pos[9] = {1, 0, 0, 0, 0, 1, 0, 0, 0};
    uint16_t idx[3] = {0, 1, 2};
    float times[2] = {0, 1};
    const float s = std::sin(kPi / 4), c = std::cos(kPi / 4);
    float rot[8] = {0, 0, 0, 1, 0, s, 0, c};  // 0 -> 90 degrees about Y
    std::memcpy(bin.data(), pos, 36);
    std::memcpy(bin.data() + 36, idx, 6);
    std::memcpy(bin.data() + 44, times, 8);
    std::memcpy(bin.data() + 52, rot, 32);
    doc.replace(doc.find("URI"), 3, "data:application/octet-stream;base64," + str::base64Encode(bin.data(), bin.size()));
    auto r = parseGltf(skytest::bytesOfString(doc), ".", false);
    REQUIRE(r.ok());
    REQUIRE(r->mesh.skinned());
    CHECK(r->mesh.skin.jointNames == std::vector<std::string>{"Blade"});
    CHECK(near(vpos(r->mesh, 0), {1, 1, 0}));  // static = rest world position
    Pose pose = restPose(r->animation->skeleton);
    sampleClip(*r->animation->clip("Spin"), 1.f, pose);
    std::vector<Mat4> globals, palette;
    computeGlobals(r->animation->skeleton, pose, globals);
    skinPalette(r->mesh.skin, mapSkin(r->mesh.skin, r->animation->skeleton), globals, palette);
    MeshData posed;
    skinMesh(r->mesh, palette, posed);
    CHECK(near(vpos(posed, 0), {0, 1, -1}));  // (1,0,0) turned 90 degrees about +Y
}
