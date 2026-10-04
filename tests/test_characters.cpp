// Character tech: humanoid bone maps, pose-space retargeting, root yaw, directional blend spaces
// (and, further down, foot / hand IK and skinned groom roots).

#include <doctest/doctest.h>

#include <cmath>

#include "skywalker/anim/Controller.h"
#include "skywalker/anim/HumanoidMap.h"
#include "skywalker/anim/Retarget.h"

using namespace sky;
using namespace sky::anim;

namespace {

int addBone(Skeleton& sk, const std::string& name, int parent, Vec3 t, Quat r = {}) {
    sk.bones.push_back({name, parent, {t, r, {1, 1, 1}}});
    return static_cast<int>(sk.bones.size() - 1);
}

/// A mixamo-named humanoid in a T-pose, identity bone frames, hips 1 m high (faces +Z, left = +X).
Skeleton mixamoTPose() {
    Skeleton sk;
    int hips = addBone(sk, "mixamorig:Hips", -1, {0, 1.0f, 0});
    int s0 = addBone(sk, "mixamorig:Spine", hips, {0, 0.1f, 0});
    int s1 = addBone(sk, "mixamorig:Spine1", s0, {0, 0.12f, 0});
    int s2 = addBone(sk, "mixamorig:Spine2", s1, {0, 0.12f, 0});
    int neck = addBone(sk, "mixamorig:Neck", s2, {0, 0.16f, 0});
    addBone(sk, "mixamorig:Head", neck, {0, 0.1f, 0});
    for (int side = 0; side < 2; ++side) {
        const char* S = side ? "Right" : "Left";
        float x = side ? -1.f : 1.f;
        int sh = addBone(sk, std::string("mixamorig:") + S + "Shoulder", s2, {0.06f * x, 0.12f, 0});
        int arm = addBone(sk, std::string("mixamorig:") + S + "Arm", sh, {0.12f * x, 0, 0});
        int fore = addBone(sk, std::string("mixamorig:") + S + "ForeArm", arm, {0.27f * x, 0, 0});
        int hand = addBone(sk, std::string("mixamorig:") + S + "Hand", fore, {0.25f * x, 0, 0});
        addBone(sk, std::string("mixamorig:") + S + "HandIndex1", hand, {0.08f * x, 0, 0});
        int up = addBone(sk, std::string("mixamorig:") + S + "UpLeg", hips, {0.1f * x, -0.05f, 0});
        int leg = addBone(sk, std::string("mixamorig:") + S + "Leg", up, {0, -0.45f, 0});
        int foot = addBone(sk, std::string("mixamorig:") + S + "Foot", leg, {0, -0.42f, 0});
        addBone(sk, std::string("mixamorig:") + S + "ToeBase", foot, {0, -0.05f, 0.12f});
    }
    return sk;
}

/// A UE-named humanoid in an A-pose (arms 45 degrees down), shorter (hips 0.8 m), with a root bone
/// and bone frames that point each bone's local +Y along the bone (other axes than the source).
Skeleton ueAPose() {
    Skeleton sk;
    int root = addBone(sk, "root", -1, {0, 0, 0});
    // pelvis rotated -90 about X: its local +Y points to world +Z... the children compensate.
    const Quat tilt = Quat::axisAngle({1, 0, 0}, radians(-90.f));
    int pelvis = addBone(sk, "pelvis", root, {0, 0.8f, 0}, tilt);
    // Children of pelvis live in the tilted frame: world up (0,1,0) is local (0,0,-1)... use tilt^-1.
    auto local = [&](Vec3 world) { return tilt.conjugate().rotate(world); };
    int s1 = addBone(sk, "spine_01", pelvis, local({0, 0.08f, 0}), tilt.conjugate());
    int s2 = addBone(sk, "spine_02", s1, {0, 0.1f, 0});
    int s3 = addBone(sk, "spine_03", s2, {0, 0.1f, 0});
    int neck = addBone(sk, "neck_01", s3, {0, 0.13f, 0});
    addBone(sk, "head", neck, {0, 0.08f, 0});
    for (int side = 0; side < 2; ++side) {
        const char* S = side ? "_r" : "_l";
        float x = side ? -1.f : 1.f;
        int cl = addBone(sk, std::string("clavicle") + S, s3, {0.05f * x, 0.1f, 0});
        // A-pose: the upper arm hangs 45 degrees down; its frame rotates so local +Y runs down the arm.
        Quat armDown = Quat::axisAngle({0, 0, 1}, radians(side ? 45.f : -45.f));
        Vec3 armDir = armDown.rotate({x, 0, 0});
        int up = addBone(sk, std::string("upperarm") + S, cl, {0.1f * x, 0, 0}, Quat::fromTo({0, 1, 0}, armDir));
        int lo = addBone(sk, std::string("lowerarm") + S, up, {0, 0.22f, 0});
        int hand = addBone(sk, std::string("hand") + S, lo, {0, 0.2f, 0});
        addBone(sk, std::string("upperarm_twist_01") + S, up, {0, 0.1f, 0});
        addBone(sk, std::string("index_01") + S, hand, {0, 0.06f, 0});
        int th = addBone(sk, std::string("thigh") + S, pelvis, local({0.09f * x, -0.04f, 0}), tilt.conjugate());
        int calf = addBone(sk, std::string("calf") + S, th, {0, -0.36f, 0});
        int foot = addBone(sk, std::string("foot") + S, calf, {0, -0.34f, 0});
        addBone(sk, std::string("ball") + S, foot, {0, -0.04f, 0.1f});
    }
    return sk;
}

Vec3 pos(const std::vector<Mat4>& g, int b) { return g[static_cast<size_t>(b)].translation(); }

Vec3 dirBetween(const Skeleton& sk, const Pose& pose, int a, int b) {
    std::vector<Mat4> g;
    computeGlobals(sk, pose, g);
    return normalize(pos(g, b) - pos(g, a));
}

Channel rotationKeys(int bone, std::vector<float> times, std::vector<Quat> qs) {
    Channel c;
    c.bone = bone;
    c.path = Path::Rotation;
    c.times = std::move(times);
    for (const Quat& q : qs) c.values.insert(c.values.end(), {q.x, q.y, q.z, q.w});
    return c;
}

}  // namespace

TEST_CASE("humanoid map: mixamo, UE and generic names, inferred links, sides by position") {
    Skeleton mx = mixamoTPose();
    HumanoidMap m = detectHumanoid(mx);
    CHECK(m.complete());
    CHECK(m.convention == "mixamo");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::LeftUpperArm])].name == "mixamorig:LeftArm");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::RightLowerLeg])].name == "mixamorig:RightLeg");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::LeftToes])].name == "mixamorig:LeftToeBase");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::UpperChest])].name == "mixamorig:Spine2");
    CHECK(m.confidence > 0.95f);

    Skeleton ue = ueAPose();
    HumanoidMap u = detectHumanoid(ue);
    CHECK(u.complete());
    CHECK(u.convention == "ue");
    CHECK(ue.bones[static_cast<size_t>(u[HumanBone::Hips])].name == "pelvis");
    CHECK(ue.bones[static_cast<size_t>(u[HumanBone::LeftUpperArm])].name == "upperarm_l");  // not the twist bone
    CHECK(ue.bones[static_cast<size_t>(u[HumanBone::LeftShoulder])].name == "clavicle_l");
    CHECK(ue.bones[static_cast<size_t>(u[HumanBone::RightToes])].name == "ball_r");

    // Blender-style names; a chain with "Leg" as the upper leg and "Shin" below it.
    Skeleton gen;
    int hips = addBone(gen, "Hips", -1, {0, 1, 0});
    int sp = addBone(gen, "Spine", hips, {0, 0.2f, 0});
    int ch = addBone(gen, "Chest", sp, {0, 0.2f, 0});
    int nk = addBone(gen, "Neck", ch, {0, 0.2f, 0});
    addBone(gen, "Head", nk, {0, 0.1f, 0});
    for (int side = 0; side < 2; ++side) {
        std::string S = side ? ".R" : ".L";
        float x = side ? -1.f : 1.f;
        int a = addBone(gen, "upper_arm" + S, ch, {0.15f * x, 0.15f, 0});
        int f = addBone(gen, "forearm" + S, a, {0.25f * x, 0, 0});
        addBone(gen, "hand" + S, f, {0.25f * x, 0, 0});
        int l = addBone(gen, "Leg" + S, hips, {0.1f * x, 0, 0});
        int s = addBone(gen, "Shin" + S, l, {0, -0.45f, 0});
        addBone(gen, "Foot" + S, s, {0, -0.45f, 0});
    }
    HumanoidMap g = detectHumanoid(gen);
    CHECK(g.complete());
    CHECK(gen.bones[static_cast<size_t>(g[HumanBone::LeftUpperLeg])].name == "Leg.L");
    CHECK(gen.bones[static_cast<size_t>(g[HumanBone::LeftLowerLeg])].name == "Shin.L");
    CHECK(gen.bones[static_cast<size_t>(g[HumanBone::Chest])].name == "Chest");

    // Sideless names: the bone at +X is the left one.
    Skeleton noSide;
    int h2 = addBone(noSide, "Hips", -1, {0, 1, 0});
    addBone(noSide, "Thigh", h2, {-0.1f, 0, 0});
    addBone(noSide, "Thigh", h2, {0.1f, 0, 0});
    HumanoidMap ns = detectHumanoid(noSide);
    REQUIRE(ns[HumanBone::LeftUpperLeg] >= 0);
    CHECK(ns[HumanBone::LeftUpperLeg] == 2);
    CHECK(ns[HumanBone::RightUpperLeg] == 1);
    CHECK_FALSE(ns.complete());

    // Overrides: a bad slot or bone fails with a hint.
    HumanoidMap o = detectHumanoid(mx);
    Status bad = applyHumanoidOverrides(o, mx, Json::parse(R"({"lefthand": "mixamorig:LeftHand", "leftHnad": "x"})").value());
    CHECK_FALSE(bad);
    CHECK(bad.error().hint.find("leftHand") != std::string::npos);
    Status badBone = applyHumanoidOverrides(o, mx, Json::parse(R"({"head": "mixamorig:Haed"})").value());
    CHECK_FALSE(badBone);
    CHECK(badBone.error().hint.find("mixamorig:Head") != std::string::npos);
    REQUIRE(applyHumanoidOverrides(o, mx, Json::parse(R"({"upperChest": null})").value()));
    CHECK(o[HumanBone::UpperChest] == -1);
    CHECK(o.convention == "custom");
}

TEST_CASE("retarget: T-pose mixamo onto an A-pose UE rig with other bone axes and proportions") {
    Skeleton src = mixamoTPose(), dst = ueAPose();
    auto setup = prepareRetarget(src, -1, dst, 0);
    REQUIRE(setup);
    const RetargetSetup& s = *setup;
    CHECK(s.targetRoot == 0);
    CHECK(s.align.angle() < 1e-3f);  // both rigs: Y up, facing +Z
    CHECK(s.scale == doctest::Approx((0.36f + 0.34f) / (0.45f + 0.42f)).epsilon(0.01));
    bool restWarning = false;
    for (const auto& w : s.warnings) restWarning = restWarning || w.find("rest poses differ") != std::string::npos;
    CHECK(restWarning);

    const int la = s.target[HumanBone::LeftUpperArm], lf = s.target[HumanBone::LeftLowerArm];
    const int sla = s.source[HumanBone::LeftUpperArm], slf = s.source[HumanBone::LeftLowerArm];

    SUBCASE("the source rest pose puts the A-pose arms into a T-pose") {
        Pose tp;
        retargetPose(s, src, restPose(src), dst, tp);
        Vec3 d = dirBetween(dst, tp, la, lf);
        CHECK(d.x == doctest::Approx(1.f).epsilon(0.001));
        CHECK(std::fabs(d.y) < 1e-3f);
        // The hips stay at the target's own height.
        std::vector<Mat4> g;
        computeGlobals(dst, tp, g);
        CHECK(pos(g, s.target[HumanBone::Hips]).y == doctest::Approx(0.8f));
    }
    SUBCASE("an animated pose lands on the same directions, hips move by the scaled motion") {
        Clip c;
        c.name = "RaiseAndBend";
        c.duration = 1.f;
        // Left arm swings up 70 degrees (about +Z), the forearm bends 60 degrees about -Y, hips drop 0.1 m and step 0.3 m forward.
        c.channels.push_back(rotationKeys(sla, {0, 1}, {Quat{}, Quat::axisAngle({0, 0, 1}, radians(70.f))}));
        c.channels.push_back(rotationKeys(slf, {0, 1}, {Quat{}, Quat::axisAngle({0, 1, 0}, radians(-60.f))}));
        Channel hipsT;
        hipsT.bone = s.source[HumanBone::Hips];
        hipsT.path = Path::Translation;
        hipsT.times = {0, 1};
        hipsT.values = {0, 1, 0, 0, 0.9f, 0.3f};
        c.channels.push_back(hipsT);

        Pose sp = restPose(src);
        sampleClip(c, 1.f, sp);
        Pose tp;
        retargetPose(s, src, sp, dst, tp);
        Vec3 want = dirBetween(src, sp, sla, slf), got = dirBetween(dst, tp, la, lf);
        CHECK(dot(want, got) > 0.9999f);
        Vec3 wantFore = dirBetween(src, sp, slf, s.source[HumanBone::LeftHand]);
        Vec3 gotFore = dirBetween(dst, tp, lf, s.target[HumanBone::LeftHand]);
        CHECK(dot(wantFore, gotFore) > 0.9999f);
        std::vector<Mat4> g;
        computeGlobals(dst, tp, g);
        Vec3 hips = pos(g, s.target[HumanBone::Hips]);
        CHECK(hips.y == doctest::Approx(0.8f - 0.1f * s.scale).epsilon(0.001));
        CHECK(hips.z == doctest::Approx(0.3f * s.scale).epsilon(0.001));

        Clip out = retargetClipPose(s, src, c, dst);
        CHECK(out.duration == doctest::Approx(1.f));
        CHECK(retargetStretch(dst, out) < 1e-4f);  // rotations only: bones keep their lengths
        Pose sampled = restPose(dst);
        sampleClip(out, 1.f, sampled);
        CHECK(dot(dirBetween(dst, sampled, la, lf), want) > 0.9999f);
    }
    SUBCASE("non-humanoids are refused with the missing slots") {
        Skeleton tail;
        addBone(tail, "Tail1", -1, {0, 0, 0});
        auto r = prepareRetarget(src, -1, tail, -1);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "not_humanoid");
        CHECK(r.error().message.find("hips") != std::string::npos);
    }
}

TEST_CASE("blend spaces: directional (polar) weights keep speed and direction") {
    // idle, forward, back, left, right (velocities: x = strafe right, y = forward; m/s)
    std::vector<Vec2> v = {{0, 0}, {0, 1.4f}, {0, -1.2f}, {-1.3f, 0}, {1.3f, 0}};
    auto sum = [](const std::vector<float>& w) {
        float s = 0;
        for (float x : w) s += x;
        return s;
    };
    auto w = blendWeightsDirectional(v, {0, 1.4f});
    CHECK(w[1] == doctest::Approx(1.f));
    w = blendWeightsDirectional(v, {0, 0.7f});
    CHECK(w[0] == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(w[1] == doctest::Approx(0.5f).epsilon(0.02));
    // A diagonal at full speed: forward and right only, no idle (a cartesian blend pulls toward idle).
    Vec2 diag{0.99f, 0.99f};
    w = blendWeightsDirectional(v, diag);
    CHECK(sum(w) == doctest::Approx(1.f));
    CHECK(w[0] < 0.02f);
    CHECK(w[2] < 1e-4f);
    CHECK(w[3] < 1e-4f);
    CHECK(w[1] == doctest::Approx(w[4]).epsilon(0.1));
    auto wc = blendWeights2D(v, diag);
    CHECK(sum(wc) == doctest::Approx(1.f));
    CHECK(wc[0] > w[0]);
    // With diagonal clips, the diagonal plays alone.
    std::vector<Vec2> v8 = v;
    v8.push_back({0.99f, 0.99f});
    w = blendWeightsDirectional(v8, diag);
    CHECK(w[5] == doctest::Approx(1.f).epsilon(0.001));
    // Outside every motion: the nearest one.
    w = blendWeightsDirectional({{0, 1}}, {5, 5});
    CHECK(w[0] == doctest::Approx(1.f));
}

TEST_CASE("blend spaces: blend2d mode parses, round-trips and drives weights") {
    auto def = ControllerDef::fromJson(Json::parse(R"({"format": "skywalker.animctl", "version": 1,
        "parameters": {"vx": "float", "vy": "float"},
        "layers": [{"name": "Base", "default": "Move", "states": {"Move": {"blend2d": {"x": "vx", "y": "vy", "mode": "directional",
            "motions": [{"clip": "Idle", "pos": [0, 0]}, {"clip": "Walk", "pos": [0, 1.4]}]}}}}]})").value());
    REQUIRE(def);
    CHECK(def->layers[0].states[0].mode2d == Blend2DMode::Directional);
    CHECK(def->toJson().get("layers")[0].get("states").get("Move").get("blend2d").get("mode").asString() == "directional");
    auto bad = ControllerDef::fromJson(Json::parse(R"({"format": "skywalker.animctl", "version": 1, "parameters": {"s": "float"},
        "layers": [{"name": "Base", "default": "M", "states": {"M": {"blend": {"parameter": "s", "mode": "directional",
            "motions": [{"clip": "Idle", "at": 0}]}}}}]})").value());
    CHECK_FALSE(bad);
}

TEST_CASE("root motion: yaw is extracted from turning clips, the body keeps facing forward") {
    auto lib = std::make_shared<Library>();
    lib->skeleton.bones.push_back({"Hips", -1, {{0, 1, 0}, {}, {1, 1, 1}}});
    lib->skeleton.bones.push_back({"Spine", 0, {{0, 0.3f, 0}, {}, {1, 1, 1}}});
    lib->rootBone = 0;
    Clip turn;
    turn.name = "TurnWalk";
    turn.duration = 1.f;
    // The hips walk 1 m along +Z (the clip's own frame) while turning 90 degrees left (+Y).
    Channel t;
    t.bone = 0;
    t.path = Path::Translation;
    for (int i = 0; i <= 10; ++i) {
        t.times.push_back(static_cast<float>(i) / 10.f);
        t.values.insert(t.values.end(), {0, 1, static_cast<float>(i) / 10.f});
    }
    turn.channels.push_back(t);
    std::vector<float> times;
    std::vector<Quat> qs;
    for (int i = 0; i <= 10; ++i) {
        times.push_back(static_cast<float>(i) / 10.f);
        qs.push_back(Quat::axisAngle({0, 1, 0}, radians(9.f * static_cast<float>(i))));
    }
    turn.channels.push_back(rotationKeys(0, times, qs));
    lib->clips.push_back(turn);
    auto ctl = std::make_shared<ControllerDef>(simpleController({"TurnWalk"}, "TurnWalk", true));
    AnimatorRuntime rt;
    REQUIRE(rt.init(lib, ctl, [&](const std::string& n) { return std::shared_ptr<const Clip>(lib, lib->clip(n)); }));
    rt.setRootMotion(true, {0, 1, 0}, true);
    CHECK(rt.rootYaw());
    // Integrate like the AnimationSystem: each step's delta is in the frame faced at its start.
    Vec3 world{0, 0, 0};
    float heading = 0.f;
    auto run = [&](float seconds) {
        int steps = static_cast<int>(std::lround(seconds * 60.f));
        for (int i = 0; i < steps; ++i) {
            Vec3 d;
            float yaw = 0.f;
            rt.update(1.f / 60.f, nullptr, &d, &yaw);
            world += Quat::axisAngle({0, 1, 0}, heading).rotate(d);
            heading += yaw;
        }
    };
    run(0.5f);
    Pose p;
    rt.evaluate(p);
    // Halfway through the turn the hips' rotation has been taken out of the pose.
    CHECK(degrees(p[0].r.angle()) < 0.5f);
    run(0.5f);
    CHECK(degrees(heading) == doctest::Approx(90.f).epsilon(0.01));
    CHECK(world.z == doctest::Approx(1.f).epsilon(0.01));
    CHECK(std::fabs(world.x) < 0.01f);
    // The next cycle starts turned: its forward metre goes along +X.
    run(1.f);
    CHECK(degrees(heading) == doctest::Approx(180.f).epsilon(0.01));
    CHECK(world.x == doctest::Approx(1.f).epsilon(0.02));
    CHECK(world.z == doctest::Approx(1.f).epsilon(0.02));

    // Translation-only root motion keeps the rotation in the pose and reports no yaw.
    AnimatorRuntime plain;
    REQUIRE(plain.init(lib, ctl, [&](const std::string& n) { return std::shared_ptr<const Clip>(lib, lib->clip(n)); }));
    plain.setRootMotion(true, {0, 1, 0}, false);
    Vec3 d;
    float yaw = 1.f;
    for (int i = 0; i < 30; ++i) plain.update(1.f / 60.f, nullptr, &d, &yaw);
    CHECK(yaw == 0.f);
    plain.evaluate(p);
    CHECK(degrees(p[0].r.angle()) == doctest::Approx(45.f).epsilon(0.02));
}
