// Character tech: humanoid bone maps, pose-space retargeting, root yaw, directional blend spaces
// (and, further down, foot / hand IK and skinned groom roots).

#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>

#include "skywalker/anim/CharacterIk.h"
#include "skywalker/anim/Controller.h"
#include "skywalker/anim/HumanoidMap.h"
#include "skywalker/anim/Retarget.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/fx/GroomBinding.h"
#include "skywalker/scene/Scene.h"

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

/// A humanoid with side-suffixed names (thigh_l) in an A-pose (arms 45 degrees down), shorter (hips
/// 0.8 m), with a root bone and bone frames that point each bone's local +Y along the bone (other axes than the source).
Skeleton suffixedAPose() {
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

TEST_CASE("humanoid map: mixamo, suffixed and generic names, inferred links, sides by position") {
    Skeleton mx = mixamoTPose();
    HumanoidMap m = detectHumanoid(mx);
    CHECK(m.complete());
    CHECK(m.convention == "mixamo");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::LeftUpperArm])].name == "mixamorig:LeftArm");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::RightLowerLeg])].name == "mixamorig:RightLeg");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::LeftToes])].name == "mixamorig:LeftToeBase");
    CHECK(mx.bones[static_cast<size_t>(m[HumanBone::UpperChest])].name == "mixamorig:Spine2");
    CHECK(m.confidence > 0.95f);

    Skeleton sfx = suffixedAPose();
    HumanoidMap u = detectHumanoid(sfx);
    CHECK(u.complete());
    CHECK(u.convention == "suffixed");
    CHECK(sfx.bones[static_cast<size_t>(u[HumanBone::Hips])].name == "pelvis");
    CHECK(sfx.bones[static_cast<size_t>(u[HumanBone::LeftUpperArm])].name == "upperarm_l");  // not the twist bone
    CHECK(sfx.bones[static_cast<size_t>(u[HumanBone::LeftShoulder])].name == "clavicle_l");
    CHECK(sfx.bones[static_cast<size_t>(u[HumanBone::RightToes])].name == "ball_r");

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

    // A rig whose feet are IK controls under the root and whose "Hips" is a spine segment above the legs' parent.
    Skeleton knight;
    int bone = addBone(knight, "Bone", -1, {0, 0, 0});
    addBone(knight, "Foot.L", bone, {0.1f, 0.05f, 0});
    int body = addBone(knight, "Body", bone, {0, 0.9f, 0});
    int khips = addBone(knight, "Hips", body, {0, 0.05f, 0});
    int abdomen = addBone(knight, "Abdomen", khips, {0, 0.15f, 0});
    int torso = addBone(knight, "Torso", abdomen, {0, 0.2f, 0});
    int kneck = addBone(knight, "Neck", torso, {0, 0.2f, 0});
    addBone(knight, "Head", kneck, {0, 0.1f, 0});
    for (int side = 0; side < 2; ++side) {
        std::string S = side ? ".R" : ".L";
        float x = side ? -1.f : 1.f;
        int sh = addBone(knight, "Shoulder" + S, torso, {0.08f * x, 0.15f, 0});
        int ua = addBone(knight, "UpperArm" + S, sh, {0.1f * x, 0, 0});
        int la = addBone(knight, "LowerArm" + S, ua, {0.25f * x, 0, 0});
        addBone(knight, "Palm" + S, la, {0.22f * x, 0, 0});
        int ul = addBone(knight, "UpperLeg" + S, body, {0.1f * x, -0.05f, 0});
        addBone(knight, "LowerLeg" + S, ul, {0, -0.42f, 0});
        addBone(knight, "PoleTarget" + S, bone, {0.1f * x, 0.5f, 0.4f});
    }
    addBone(knight, "Foot.R", bone, {-0.1f, 0.05f, 0});
    HumanoidMap k = detectHumanoid(knight);
    CHECK(knight.bones[static_cast<size_t>(k[HumanBone::Hips])].name == "Body");
    CHECK(k[HumanBone::LeftFoot] == -1);  // Foot.L is an IK target, not part of the leg
    CHECK(knight.bones[static_cast<size_t>(k[HumanBone::LeftHand])].name == "Palm.L");
    CHECK_FALSE(k.complete());
    CHECK(k.retargetable());
    auto kSetup = prepareRetarget(mx, -1, knight, 0);
    REQUIRE(kSetup);
    // Its IK-control feet follow the retargeted legs: a bent knee carries Foot.L along with the shin.
    REQUIRE(k.footControls[0] == knight.find("Foot.L"));
    Pose bent = restPose(mx);
    const int knee = mx.find("mixamorig:LeftLeg");
    bent[static_cast<size_t>(knee)].r = Quat::axisAngle({1, 0, 0}, radians(70.f)) * bent[static_cast<size_t>(knee)].r;
    Pose kp;
    retargetPose(*kSetup, mx, bent, knight, kp);
    std::vector<Mat4> kRest, kNow;
    computeGlobals(knight, restPose(knight), kRest);
    computeGlobals(knight, kp, kNow);
    const int kLower = k[HumanBone::LeftLowerLeg], kFoot = k.footControls[0];
    const float restGap = distance(kRest[static_cast<size_t>(kLower)].translation(), kRest[static_cast<size_t>(kFoot)].translation());
    CHECK(distance(kNow[static_cast<size_t>(kLower)].translation(), kNow[static_cast<size_t>(kFoot)].translation()) ==
          doctest::Approx(restGap).epsilon(1e-3));
    CHECK(distance(kNow[static_cast<size_t>(kFoot)].translation(), kRest[static_cast<size_t>(kFoot)].translation()) > 0.1f);

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

TEST_CASE("retarget: T-pose mixamo onto an A-pose suffixed-name rig with other bone axes and proportions") {
    Skeleton src = mixamoTPose(), dst = suffixedAPose();
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

// ---------------------------------------------------------------------------------------------
// Foot and hand IK
// ---------------------------------------------------------------------------------------------

namespace {

GroundProbe ground(float y, Vec3 normal = {0, 1, 0}, float x = 0.f, float z = 0.f) {
    GroundProbe g;
    g.hit = true;
    g.point = {x, y, z};
    g.normal = normalize(normal);
    return g;
}

std::array<FootInput, 2> standing(float leftGround, float rightGround) {
    std::array<FootInput, 2> in;
    for (int i = 0; i < 2; ++i) {
        float x = i ? -0.1f : 0.1f;
        in[static_cast<size_t>(i)].ankle = {x, 0.08f, 0};
        in[static_cast<size_t>(i)].toe = {x, 0.02f, 0.12f};
        in[static_cast<size_t>(i)].heel = ground(i ? rightGround : leftGround, {0, 1, 0}, x);
        in[static_cast<size_t>(i)].toeProbe = ground(i ? rightGround : leftGround, {0, 1, 0}, x, 0.12f);
    }
    return in;
}

}  // namespace

TEST_CASE("foot IK: ground offsets, pelvis drop, slopes, reach and toe clearance") {
    FootIkSettings s;
    s.footHeight = 0.08f;
    s.toeHeight = 0.02f;
    SUBCASE("raised ground lifts both feet, the pelvis stays") {
        FootIkState st;
        FootIkResult r = solveFeet(s, standing(0.2f, 0.2f), {0, 0, 0}, {0, 1, 0}, 0.f, st);
        CHECK(r.ankle[0].y == doctest::Approx(0.28f));
        CHECK(r.ankle[1].y == doctest::Approx(0.28f));
        CHECK(r.pelvisOffset == doctest::Approx(0.f));
        CHECK(r.weight[0] == doctest::Approx(1.f));
    }
    SUBCASE("a lower foot drops the pelvis by its offset") {
        FootIkState st;
        FootIkResult r = solveFeet(s, standing(-0.15f, 0.f), {0, 0, 0}, {0, 1, 0}, 0.f, st);
        CHECK(r.ankle[0].y == doctest::Approx(0.08f - 0.15f));
        CHECK(r.ankle[1].y == doctest::Approx(0.08f));
        CHECK(r.pelvisOffset == doctest::Approx(-0.15f));
    }
    SUBCASE("feet tilt onto the slope, clamped at maxSlope") {
        FootIkState st;
        auto in = standing(0.f, 0.f);
        Vec3 n20 = Quat::axisAngle({1, 0, 0}, radians(20.f)).rotate({0, 1, 0});
        Vec3 n60 = Quat::axisAngle({1, 0, 0}, radians(60.f)).rotate({0, 1, 0});
        in[0].heel.normal = n20;
        in[1].heel.normal = n60;
        FootIkResult r = solveFeet(s, in, {0, 0, 0}, {0, 1, 0}, 0.f, st);
        CHECK(degrees(r.tilt[0].angle()) == doctest::Approx(20.f).epsilon(0.01));
        CHECK(degrees(r.tilt[1].angle()) == doctest::Approx(40.f).epsilon(0.01));
    }
    SUBCASE("ground out of reach fades the IK out (jumps, ledges)") {
        FootIkState st;
        FootIkResult r = solveFeet(s, standing(-1.f, -1.f), {0, 0, 0}, {0, 1, 0}, 0.f, st);
        CHECK(r.weight[0] == doctest::Approx(0.f));
        CHECK(r.pelvisOffset == doctest::Approx(0.f));
    }
    SUBCASE("the toe never sinks into a higher stair step") {
        FootIkState st;
        auto in = standing(0.f, 0.f);
        in[0].toeProbe = ground(0.18f, {0, 1, 0}, 0.1f, 0.12f);
        FootIkResult r = solveFeet(s, in, {0, 0, 0}, {0, 1, 0}, 0.f, st);
        // The toe (0.02 above the sole) must end at least toeHeight above the 0.18 m step.
        CHECK(r.ankle[0].y == doctest::Approx(0.08f + 0.18f).epsilon(0.001));
        CHECK(r.ankle[1].y == doctest::Approx(0.08f));
    }
}

TEST_CASE("foot IK: with leg lengths the pelvis drops only as far as a leg cannot reach") {
    FootIkSettings s;
    s.footHeight = 0.08f;
    FootIkState st;
    auto in = standing(-0.15f, 0.f);
    for (int i = 0; i < 2; ++i) {
        in[static_cast<size_t>(i)].hip = in[static_cast<size_t>(i)].ankle + Vec3{0, 0.8f, 0};
        in[static_cast<size_t>(i)].legLength = 0.85f;  // the animated leg is bent: 5 cm of reach to spare
    }
    FootIkResult r = solveFeet(s, in, {0, 0, 0}, {0, 1, 0}, 0.f, st);
    CHECK(r.ankle[0].y == doctest::Approx(0.08f - 0.15f));
    // The left leg reaches 0.8 + 0.15 = 0.95 m but has 0.85 * 0.995: the pelvis drops the difference only.
    CHECK(r.pelvisOffset == doctest::Approx(-(0.95f - 0.85f * 0.995f)).epsilon(0.01));
    // A swing foot high in the air over a low step needs no drop at all.
    FootIkState st2;
    auto swing = standing(-0.15f, 0.f);
    swing[0].ankle.y = 0.3f;
    swing[0].hip = swing[0].ankle + Vec3{0, 0.55f, 0};
    swing[0].legLength = 0.85f;
    swing[1].hip = swing[1].ankle + Vec3{0, 0.8f, 0};
    swing[1].legLength = 0.85f;
    FootIkResult r2 = solveFeet(s, swing, {0, 0, 0}, {0, 1, 0}, 0.f, st2);
    CHECK(r2.pelvisOffset == doctest::Approx(0.f));
}

TEST_CASE("foot IK: planted feet lock while sliding slowly and re-plant with a step") {
    FootIkSettings s;
    s.footHeight = 0.08f;
    s.lockSpeed = 0.35f;
    s.lockDistance = 0.22f;
    FootIkState st;
    const float dt = 1.f / 60.f;
    float x = 0.f;
    FootIkResult r;
    auto step = [&] {
        auto in = standing(0.f, 0.f);
        in[0].ankle.x = 0.1f + x;  // the animation slides the left foot slowly (0.1 m/s)
        in[0].toe.x = 0.1f + x;
        r = solveFeet(s, in, {0, 0, 0}, {0, 1, 0}, dt, st);
        x += 0.1f * dt;
    };
    for (int i = 0; i < 60; ++i) step();
    CHECK(st.feet[0].contact);
    CHECK(st.feet[0].locked);
    CHECK(r.ankle[0].x < 0.11f);  // stays where it planted while the animation slid 0.1 m
    for (int i = 0; i < 120; ++i) step();  // 0.3 m away: a re-plant happened
    CHECK(st.feet[0].locked);
    CHECK(r.ankle[0].x > 0.25f);
    CHECK(r.ankle[0].x < 0.1f + x + 1e-3f);
    // Fast motion (a swing foot) never locks.
    FootIkState fast;
    for (int i = 0; i < 30; ++i) {
        auto in = standing(0.f, 0.f);
        in[1].ankle.x = -0.1f + 1.2f * dt * static_cast<float>(i);
        solveFeet(s, in, {0, 0, 0}, {0, 1, 0}, dt, fast);
    }
    CHECK_FALSE(fast.feet[1].locked);
}

TEST_CASE("foot and hand IK on a skeleton: pelvis, ankles on target, hands reach, bones keep lengths") {
    Skeleton sk = mixamoTPose();
    HumanoidMap map = detectHumanoid(sk);
    REQUIRE(map.complete());
    float ankleH = 0.f, toeH = 0.f;
    restFootHeights(sk, map, ankleH, toeH);
    CHECK(ankleH > 0.05f);
    Pose pose = restPose(sk);
    std::vector<Mat4> g;
    computeGlobals(sk, pose, g);
    const int lf = map[HumanBone::LeftFoot], rf = map[HumanBone::RightFoot];
    Vec3 la = pos(g, lf), ra = pos(g, rf);
    FootIkResult r;
    r.ankle = {la + Vec3{0, -0.05f, 0.1f}, ra + Vec3{0, 0.05f, 0}};
    r.tilt = {Quat::axisAngle({1, 0, 0}, radians(-10.f)), Quat{}};
    r.weight = {1.f, 1.f};
    r.pelvisOffset = -0.1f;
    float shin0 = distance(pos(g, map[HumanBone::LeftLowerLeg]), la);
    applyFeet(sk, map, r, Mat4{}, pose, g);
    CHECK(pos(g, map[HumanBone::Hips]).y == doctest::Approx(0.9f));
    CHECK(distance(pos(g, lf), r.ankle[0]) < 1e-3f);
    CHECK(distance(pos(g, rf), r.ankle[1]) < 1e-3f);
    CHECK(distance(pos(g, map[HumanBone::LeftLowerLeg]), pos(g, lf)) == doctest::Approx(shin0).epsilon(1e-4));
    CHECK(pos(g, map[HumanBone::LeftLowerLeg]).z > la.z + 0.01f);  // a straight leg bends its knee forward
    // A grip point in front of the chest (e.g. on a staff held by the other hand) within the arm's reach.
    Mat4 grip = Mat4::translate(pos(g, map[HumanBone::LeftUpperArm]) + Vec3{-0.1f, -0.25f, 0.3f});
    REQUIRE(applyHand(sk, map, true, grip, Mat4{}, 1.f, false, pose, g));
    CHECK(distance(pos(g, map[HumanBone::LeftHand]), grip.translation()) < 1e-3f);
    CHECK_FALSE(applyHand(sk, HumanoidMap{}, true, grip, Mat4{}, 1.f, false, pose, g));
    // Beyond the arm's reach the clavicle swings toward the grip: closer than the straight arm alone.
    const Vec3 sh = pos(g, map[HumanBone::LeftUpperArm]);
    const float armLen = distance(sh, pos(g, map[HumanBone::LeftLowerArm])) + distance(pos(g, map[HumanBone::LeftLowerArm]), pos(g, map[HumanBone::LeftHand]));
    Mat4 far = Mat4::translate(sh + normalize(Vec3{0.6f, -0.5f, 0.6f}) * (armLen + 0.15f));
    REQUIRE(applyHand(sk, map, true, far, Mat4{}, 1.f, false, pose, g));
    CHECK(distance(pos(g, map[HumanBone::LeftHand]), far.translation()) < 0.15f - 0.02f);
}

// ---------------------------------------------------------------------------------------------
// Skinned grooms: barycentric root binding, rebinding, roots on the posed skin, masks
// ---------------------------------------------------------------------------------------------

namespace {

/// A vertical 0.4 x 2 m grid strip skinned to two bones (Root below y = 1, Upper above), facing +Z.
MeshData skinnedStrip() {
    MeshData m;
    const int rows = 20, cols = 4;
    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= cols; ++c) {
            float x = -0.2f + 0.4f * static_cast<float>(c) / cols, y = 2.f * static_cast<float>(r) / rows;
            m.addVertex({x, y, 0}, {0, 0, 1}, {static_cast<float>(c) / cols, static_cast<float>(r) / rows});
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            uint32_t a = static_cast<uint32_t>(r * (cols + 1) + c), b = a + 1, d = a + cols + 1, e = d + 1;
            m.indices.insert(m.indices.end(), {a, b, e, a, e, d});
        }
    }
    m.bounds = {{-0.2f, 0, 0}, {0.2f, 2, 0}};
    SkinStream& s = m.skin;
    s.jointNames = {"Root", "Upper"};
    s.inverseBind = {Mat4{}, Mat4::translate({0, -1, 0})};
    s.restGlobal = {Mat4{}, Mat4::translate({0, 1, 0})};
    s.slotBounds = {m.bounds, m.bounds};
    for (size_t v = 0; v < m.vertexCount(); ++v) {
        const float* p = &m.vertices[v * MeshData::kFloatsPerVertex];
        s.bind.insert(s.bind.end(), {p[0], p[1], p[2], p[3], p[4], p[5]});
        bool upper = p[1] > 1.f + 1e-4f;
        s.joints.insert(s.joints.end(), {static_cast<uint16_t>(upper ? 1 : 0), 0, 0, 0});
        s.weights.insert(s.weights.end(), {1.f, 0.f, 0.f, 0.f});
    }
    return m;
}

Skeleton stripSkeleton() {
    Skeleton sk;
    sk.bones.push_back({"Root", -1, {}});
    sk.bones.push_back({"Upper", 0, {{0, 1, 0}, {}, {1, 1, 1}}});
    return sk;
}

}  // namespace

TEST_CASE("groom binding: barycentric roots, rebinding imported strands, roots on the posed skin") {
    MeshData mesh = skinnedStrip();
    Groom g;
    g.strands = 600;
    g.guides = 40;
    g.segments = 4;
    g.length = 0.05f;
    g.maskAngle = 180.f;
    auto d = fx::generateGroom(g, &mesh);
    REQUIRE(d);
    REQUIRE(d->bound());
    CHECK(d->meshVertices == mesh.vertexCount());
    // Every root is its triangle's barycentric point.
    for (size_t i = 0; i < d->children.size(); i += 37) {
        fx::RootFrame f = fx::evalRoot(mesh.vertices.data(), mesh.vertexCount(), d->childBind[i], {1, 1, 1});
        CHECK(distance(f.position, d->children[i].root) < 1e-5f);
    }
    // Rest frames: the normal (frame y axis) is the surface normal.
    Vec3 n = fx::quatRotate(d->guideFrames[0], {0, 1, 0});
    CHECK(n.z == doctest::Approx(1.f).epsilon(1e-4));

    // Rebinding points slightly off the surface finds the same spot.
    std::vector<Vec3> pts;
    for (size_t i = 0; i < d->children.size(); i += 50) pts.push_back(d->children[i].root + Vec3{0, 0, 0.003f});
    float err = 0.f;
    auto binds = fx::bindToMesh(mesh, pts, &err);
    CHECK(err == doctest::Approx(0.003f).epsilon(0.01));
    for (size_t i = 0; i < pts.size(); ++i) {
        fx::RootFrame f = fx::evalRoot(mesh.vertices.data(), mesh.vertexCount(), binds[i], {1, 1, 1});
        CHECK(distance(f.position, pts[i] - Vec3{0, 0, 0.003f}) < 1e-4f);
    }

    // Pose the upper bone (bend 90 degrees about Z): the CPU-skinned guide roots lie on the posed surface
    // and their frames turned with it.
    Skeleton sk = stripSkeleton();
    Pose pose = restPose(sk);
    pose[1].r = Quat::axisAngle({0, 0, 1}, radians(90.f));
    std::vector<Mat4> globals, palette;
    computeGlobals(sk, pose, globals);
    skinPalette(mesh.skin, mapSkin(mesh.skin, sk), globals, palette);
    MeshData posed;
    skinMesh(mesh, palette, posed);
    auto roots = fx::skinnedGuideRoots(*d, mesh, palette, {1, 1, 1});
    REQUIRE(roots.size() == d->guideCount());
    int upper = 0;
    for (size_t gi = 0; gi < roots.size(); ++gi) {
        fx::RootFrame ref = fx::evalRoot(posed.vertices.data(), posed.vertexCount(), d->guideBind[gi], {1, 1, 1});
        CHECK(distance(roots[gi].position, ref.position) < 1e-4f);
        Vec4 r = fx::rootRotation(roots[gi].rotation, d->guideFrames[gi]);
        if (d->guideRest[gi * d->points].y > 1.05f) {
            ++upper;
            CHECK(degrees(2.f * std::acos(std::clamp(std::fabs(r.w), 0.f, 1.f))) == doctest::Approx(90.f).epsilon(0.01));
            CHECK(roots[gi].position.x < 0.f);  // bent over to -X
        }
    }
    CHECK(upper > 5);
}

TEST_CASE("groom masks: bone and mirrored region masks only grow where asked") {
    MeshData mesh = skinnedStrip();
    fx::GroomSystem sys;
    Scene scene;
    EntityId e = scene.create("Strip");
    scene.add<MeshRenderer>(e).mesh = "asset:strip.glb";
    Groom g;
    g.strands = 800;
    g.segments = 3;
    g.length = 0.02f;
    g.maskAngle = 180.f;
    g.maskBone = "Upper";
    scene.add<Groom>(e) = g;
    auto meshes = [&](const std::string&) -> const MeshData* { return &mesh; };
    auto paths = [](const std::string& p) { return p; };
    auto d = sys.groomFor(scene, e, meshes, paths);
    REQUIRE(d);
    for (const auto& c : d->children) CHECK(c.root.y > 0.9f);  // only the Upper bone's half
    // Region (mirrored): two small discs at x = +-0.15, y = 1.5.
    g.maskBone = "";
    g.maskCenter = {0.15f, 1.5f, 0};
    g.maskRadius = {0.04f, 0.08f, 0.2f};  // smaller than the 0.1 m vertex spacing: evaluated per root
    g.maskMirror = true;
    *scene.get<Groom>(e) = g;
    d = sys.groomFor(scene, e, meshes, paths);
    REQUIRE(d);
    int left = 0, right = 0;
    for (const auto& c : d->children) {
        CHECK(std::fabs(c.root.y - 1.5f) < 0.08f);
        CHECK(std::fabs(std::fabs(c.root.x) - 0.15f) < 0.04f);
        (c.root.x > 0 ? right : left)++;
    }
    CHECK(left > 100);
    CHECK(right > 100);
    // A bone that does not exist fails with a hint.
    scene.get<Groom>(e)->maskBone = "Uper";
    std::string error;
    CHECK(sys.groomFor(scene, e, meshes, paths, &error) == nullptr);
    CHECK(error.find("Upper") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Tools and Wander: character_inspect, character_ik, animation_retarget, retargetFrom, builtins
// ---------------------------------------------------------------------------------------------

namespace {

struct CharacterProject {
    std::string dir;
    std::unique_ptr<Engine> engine;

    CharacterProject() {
        dir = (std::filesystem::temp_directory_path() / ("sky-characters-" + std::to_string(::getpid()))).string();
        std::filesystem::create_directories(dir + "/anims");
        std::filesystem::create_directories(dir + "/chars");
        // Clips on a T-pose mixamo rig; the character is an A-pose suffixed-name rig.
        Library src;
        src.skeleton = mixamoTPose();
        src.rootBone = 0;
        Clip wave;
        wave.name = "Wave";
        wave.duration = 1.f;
        int arm = src.skeleton.find("mixamorig:LeftArm");
        wave.channels.push_back(rotationKeys(arm, {0, 1}, {Quat{}, Quat::axisAngle({0, 0, 1}, radians(80.f))}));
        src.clips.push_back(wave);
        REQUIRE(saveLibrary(dir + "/anims/pack.anim", src).ok());
        Library dst;
        dst.skeleton = suffixedAPose();
        dst.rootBone = 0;
        Clip idle;
        idle.name = "Idle";
        idle.duration = 1.f;
        dst.clips.push_back(idle);
        REQUIRE(saveLibrary(dir + "/chars/hero.anim", dst).ok());
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir;
        engine = std::make_unique<Engine>(cfg);
        REQUIRE(engine->newScene("Characters", false).ok());
        EntityId hero = engine->scene().create("Hero");
        engine->scene().add<Transform>(hero);
        engine->scene().add<Animator>(hero).library = "chars/hero.anim";
        EntityId grip = engine->scene().create("Grip");
        engine->scene().add<Transform>(grip).position = {0.3f, 1.1f, -0.3f};
    }
    ~CharacterProject() {
        engine.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    ToolResult call(const char* tool, const char* args) { return engine->callTool(tool, Json::parse(args).value(), "agent:test"); }
};

}  // namespace

TEST_CASE("character tools: retarget, inspect, IK setup, retargetFrom and Wander builtins") {
    CharacterProject p;
    Engine& e = *p.engine;

    ToolResult prev = p.call("animation_retarget", R"({"source": "anims/pack.anim", "target": "Hero", "preview": true})");
    INFO(prev.content.front().text);
    REQUIRE_FALSE(prev.isError);
    CHECK(prev.structured.get("source").get("convention").asString() == "mixamo");
    CHECK(prev.structured.get("target").get("convention").asString() == "suffixed");
    ToolResult out = p.call("animation_retarget", R"({"source": "anims/pack.anim", "target": "chars/hero.anim", "clips": ["Wave"],
                                                     "output": "chars/hero_pack.anim"})");
    INFO(out.content.front().text);
    REQUIRE_FALSE(out.isError);
    CHECK(std::filesystem::exists(p.dir + "/chars/hero_pack.anim"));
    CHECK(out.structured.get("clips")[0].get("stretch").asFloat() < 1e-3f);
    ToolResult typo = p.call("animation_retarget", R"({"source": "anims/pack.anim", "target": "Hero", "clips": ["Wvae"]})");
    CHECK(typo.isError);
    CHECK(typo.content.front().text.find("Wave") != std::string::npos);

    ToolResult insp = p.call("character_inspect", R"({"entity": "Hero"})");
    INFO(insp.content.front().text);
    REQUIRE_FALSE(insp.isError);
    CHECK(insp.structured.get("humanoid").get("complete").asBool());
    CHECK(insp.structured.get("keyBones").contains("head"));
    CHECK(insp.structured.get("clips").size() == 1);

    ToolResult ik = p.call("character_ik", R"({"entity": "Hero", "feet": true, "step_height": 0.35, "left_hand": "Grip"})");
    INFO(ik.content.front().text);
    REQUIRE_FALSE(ik.isError);
    const CharacterIk* c = e.scene().get<CharacterIk>(e.scene().find("Hero"));
    REQUIRE(c);
    CHECK(c->stepHeight == doctest::Approx(0.35f));
    CHECK(e.scene().resolve(c->leftHand, e.scene().find("Hero")) == e.scene().find("Grip"));
    CHECK(p.call("character_ik", R"({"entity": "Hero", "left_hand": "Gripp"})").isError);
    CHECK(p.call("character_ik", R"({"entity": "Grip"})").isError);  // no animator

    // retargetFrom: the pack's clip plays on the character (pose space: the bone names differ).
    REQUIRE_FALSE(p.call("entity_update", R"({"entity": "Hero", "components": {"animator": {"retargetFrom": "anims/pack.anim",
                                                    "clip": "Wave"}}})").isError);
    auto d = e.animation().describe(e.scene().find("Hero"));
    REQUIRE(d);
    CHECK_FALSE(d->contains("warning"));
    CHECK(e.animation().retargetMethod(**e.animation().library("anims/pack.anim"), **e.animation().library("chars/hero.anim"),
                                       "auto") == "pose");

    // Wander builtins drive the component while playing.
    REQUIRE_FALSE(p.call("behavior_set", R"({"entity": "Hero", "name": "Ik", "source":
        "on start\n foot_ik(self, false)\n hand_ik(self, \"right\", find(\"Grip\"), 0.5)\n turn_in_place(self, 90)\n look_at(self, find(\"Grip\"))\nend"})")
                     .isError);
    e.play();
    e.step(30);
    c = e.scene().get<CharacterIk>(e.scene().find("Hero"));
    REQUIRE(c);
    CHECK_FALSE(c->feet);
    CHECK(c->rightHandWeight == doctest::Approx(0.5f));
    CHECK(e.scene().get<Animator>(e.scene().find("Hero"))->lookAt.empty() == false);
    // Turning in place reaches the target yaw at turnSpeed (220 deg/s: 90 degrees in under half a second).
    CHECK(e.scene().get<Transform>(e.scene().find("Hero"))->rotation.y == doctest::Approx(90.f).epsilon(0.02));
    e.stop();
}
