#include <doctest/doctest.h>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "anim_fixtures.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    std::unique_ptr<Engine> engine;

    explicit Project(skytest::StripOptions strip = {}) {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("sky-anim-engine-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(dir / "chars");
        std::ofstream(dir / "chars/strip.gltf") << skytest::makeStripGltf(strip);
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.string();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Anim", false);
    }
    ~Project() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Json call(const char* tool, const Json& args, bool expectOk = true) {
        ToolResult r = engine->callTool(tool, args, "agent:test");
        INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
        CHECK(r.isError == !expectOk);
        return r.structured;
    }
    void write(const std::string& rel, const std::string& text) {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel) << text;
    }
    /// Imports the strip and places it; returns the character root.
    EntityId character(const char* name = "Hero") {
        Json r = call("asset_import", Json::object({{"path", "chars/strip.gltf"}, {"create_entity", name}}));
        return static_cast<EntityId>(r.get("entity").asInt());
    }
    Scene& scene() { return engine->scene(); }
    void patch(EntityId e, const char* comp, const char* json) { REQUIRE(scene().patchComponent(e, comp, Json::parse(json).value()).ok()); }
};

Vec3 worldPos(Scene& s, EntityId e) { return s.worldMatrix(e).translation(); }

}  // namespace

TEST_CASE("anim import: rigged glTF -> mesh + .anim library + prefab with an animator") {
    Project p;
    Json r = p.call("asset_import", Json::object({{"path", "chars/strip.gltf"}}));
    CHECK(r.get("animation").asString() == "chars/strip.anim");
    CHECK(r.get("rigged").asBool());
    CHECK(r.get("clips").size() == 5);
    CHECK(r.get("prefab").asString() == "chars/strip.prefab.json");
    CHECK(fs::exists(p.dir / "chars/strip.anim"));
    const AssetRecord* rec = p.engine->assets().find("chars/strip.gltf");
    REQUIRE(rec);
    CHECK(rec->importSettings.get("turnAround").asBool());
    CHECK(rec->importSettings.get("animation").asString() == "chars/strip.anim");
    CHECK_FALSE(rec->importSettings.get("normalize").asBool(true));  // characters keep their size
    // Real size kept: the strip is 2 m tall.
    CHECK(r.get("size")[size_t{1}].asFloat() == doctest::Approx(2.f));

    EntityId hero = p.character();
    REQUIRE(hero);
    const Animator* a = p.scene().get<Animator>(hero);
    REQUIRE(a);
    CHECK(a->library == "chars/strip.anim");
    auto lib = p.engine->animation().libraryOf(hero);
    REQUIRE(lib.ok());
    CHECK((*lib)->clips.size() == 5);
}

TEST_CASE("anim import: multi-material rigged models get a prefab whose parts share one animator") {
    Project p({.twoMaterials = true});
    Json r = p.call("asset_import", Json::object({{"path", "chars/strip.gltf"}, {"create_entity", "Hero"}}));
    REQUIRE(r.get("parts").size() == 2);
    EntityId hero = static_cast<EntityId>(r.get("entity").asInt());
    REQUIRE(p.scene().get<Animator>(hero));
    auto kids = p.scene().children(hero);
    REQUIRE(kids.size() == 2);
    p.patch(hero, "animator", R"({"clip": "Bend", "time": 1})");
    CaptureOptions o;
    o.width = 64;
    o.height = 64;
    FrameData f = p.engine->frame(o);
    CHECK(f.skins.size() == 2);
    REQUIRE(f.skins.size() == 2);
    const auto& a = *f.skins[0].palette;
    const auto& b = *f.skins[1].palette;
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Mat4)) == 0);  // one skeleton pose for both parts
}

TEST_CASE("anim edit preview: posed draws, bounds and raycasts without touching the scene") {
    Project p;
    EntityId hero = p.character();
    p.patch(hero, "animator", R"({"clip": "Bend", "time": 0})");
    CaptureOptions o;
    o.width = 64;
    o.height = 64;
    Json before = p.scene().toJson();
    FrameData f = p.engine->frame(o);
    REQUIRE(f.skins.size() == 1);
    REQUIRE(f.draws.size() == 1);
    CHECK(f.draws[0].skin == 0);
    CHECK(f.draws[0].mesh.find("@skin") != std::string::npos);
    CHECK(f.draws[0].worldBounds.max.x < 0.2f);  // upright

    // A ray beside the strip misses the rest pose...
    Ray ray{{0.6f, 1.05f, 5.f}, {0, 0, -1}};
    CHECK_FALSE(p.engine->raycast(ray).has_value());
    // ...and hits the bent pose (Upper turned 90 degrees; the character faces -Z so +X in glTF is -X here).
    p.patch(hero, "animator", R"({"time": 1})");
    f = p.engine->frame(o);
    CHECK(f.draws[0].worldBounds.max.x > 0.9f);
    auto hit = p.engine->raycast(ray);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == hero);
    CHECK(hit->point.z == doctest::Approx(0.f).epsilon(1e-3));

    // Rest preview = the bind pose (static mesh, no skinning work).
    p.patch(hero, "animator", R"({"preview": "rest"})");
    f = p.engine->frame(o);
    CHECK(f.skins.empty());
    // Previews never changed the scene itself.
    p.patch(hero, "animator", R"({"preview": "pose", "time": 0})");
    CHECK(p.scene().toJson() == before);
}

TEST_CASE("anim play: root motion moves the entity; stop resets; replays are identical") {
    Project p;
    EntityId hero = p.character();
    p.patch(hero, "animator", R"({"clip": "Walk", "rootMotion": true})");
    auto session = [&](int ticks) {
        p.engine->play();
        p.engine->step(ticks);
        Vec3 pos = worldPos(p.scene(), hero);
        Mat4 bone = p.engine->animation().boneWorld(hero, "Upper").value();
        p.engine->stop();
        return std::make_pair(pos, bone);
    };
    auto [pos1, bone1] = session(60);
    // Walk moves 1.5 m per second along glTF +Z; the character is turned to face -Z.
    CHECK(pos1.z == doctest::Approx(-1.5f).epsilon(0.02));
    CHECK(std::fabs(pos1.x) < 1e-3f);
    CHECK(worldPos(p.scene(), hero) == Vec3{0, 0, 0});  // stop restored the scene
    auto [pos2, bone2] = session(60);
    CHECK(std::memcmp(&pos1, &pos2, sizeof(Vec3)) == 0);
    CHECK(std::memcmp(bone1.m, bone2.m, sizeof(bone1.m)) == 0);
}

TEST_CASE("anim attachments: an entity follows a bone (preview per frame, live while playing)") {
    Project p;
    EntityId hero = p.character();
    p.patch(hero, "animator", R"({"clip": "Bend", "time": 1})");
    EntityId hat = p.scene().create("Hat", hero);
    p.patch(hat, "attach", R"({"bone": "upper", "offset": [0, 0.5, 0]})");  // fuzzy bone name
    Vec3 restPos = worldPos(p.scene(), hat);
    CaptureOptions o;
    o.width = 32;
    o.height = 32;
    (void)p.engine->frame(o);
    CHECK(worldPos(p.scene(), hat) == restPos);  // edit preview did not persist

    p.patch(hero, "animator", R"({"clip": "Bend", "loop": false})");
    p.engine->play();
    p.engine->step(70);  // Bend has reached 90 degrees
    Mat4 bone = p.engine->animation().boneWorld(hero, "Upper").value();
    Vec3 expected = bone.transformPoint({0, 0.5f, 0});
    Vec3 got = worldPos(p.scene(), hat);
    CHECK(distance(got, expected) < 1e-3f);
    // Upper sits at y = 1; bent 90 degrees about Z, its +Y offset points sideways.
    CHECK(got.y == doctest::Approx(1.f).epsilon(1e-3));
    CHECK(std::fabs(got.x) == doctest::Approx(0.5f).epsilon(1e-3));
    p.engine->stop();
}

TEST_CASE("anim wander: set_param / trigger / play_animation / anim_state and on anim events") {
    Project p;
    p.write("chars/strip.animctl.json", R"({
      "parameters": {"speed": "float", "jump": "trigger"},
      "states": {
        "Idle": {"clip": "Bend", "speed": 0.0001},
        "Walk": {"clip": "Walk", "events": [{"time": 0.5, "name": "footstep"}]},
        "Jump": {"clip": "Steps", "loop": false}
      },
      "transitions": [
        {"from": "Idle", "to": "Walk", "when": "speed > 0.5", "duration": 0.1},
        {"from": "any", "to": "Jump", "when": "jump", "duration": 0}
      ]})");
    EntityId hero = p.character();
    p.patch(hero, "animator", R"({"controller": "chars/strip.animctl.json"})");
    REQUIRE(p.scene().setBehaviors(hero, Json::parse(R"([{"name": "Brain", "source": "behavior Brain\n  var steps = 0\n  var seen = \"\"\n  on start\n    set_param(self, \"speed\", 2)\n  end\n  on tick\n    seen = anim_state(self)\n    if frame == 150 then\n      trigger(self, \"jump\")\n    end\n  end\n  on anim \"footstep\"\n    steps = steps + 1\n  end\nend\n"}])").value()).ok());
    p.engine->play();
    p.engine->step(10);
    CHECK(p.engine->animation().stateName(hero) == "Walk");
    p.engine->step(140);  // ~2.5 s of walking: footsteps at 0.5, 1.5 (+ the tick delay)
    CHECK(p.scene().record(hero)->vars.get("steps").asInt() >= 2);
    CHECK(p.scene().record(hero)->vars.get("seen").asString() == "Walk");
    p.engine->step(3);
    CHECK(p.engine->animation().stateName(hero) == "Jump");
    for (const auto& m : p.engine->recentMessages()) INFO(m.dump());
    p.engine->stop();

    // Errors carry did-you-mean hints into Wander's runtime errors.
    REQUIRE(p.scene().setBehaviors(hero, Json::parse(R"([{"name": "Bad", "source": "behavior Bad\n  on start\n    set_param(self, \"sped\", 1)\n  end\nend\n"}])").value()).ok());
    p.engine->play();
    p.engine->step(2);
    bool found = false;
    for (const auto& m : p.engine->recentMessages()) found = found || m.dump().find("speed") != std::string::npos;
    CHECK(found);
    p.engine->stop();
}

TEST_CASE("anim ik: a hand reaches an effector (tool, edit preview, play), bones keep their length") {
    Project p;
    p.write("chars/arm.gltf", skytest::makeArmGltf());
    Json r = p.call("asset_import", Json::object({{"path", "chars/arm.gltf"}, {"create_entity", "Arm"}}));
    EntityId arm = static_cast<EntityId>(r.get("entity").asInt());
    REQUIRE(arm);
    anim::AnimationSystem& as = p.engine->animation();
    // The arm points along -X once turned to face -Z; the shoulder is at (0, 1.5, 0).
    CHECK(distance(as.boneWorld(arm, "Hand").value().translation(), Vec3{-1.f, 1.5f, 0.f}) < 1e-3f);
    p.call("bone_ik", Json::object({{"character", "Arm"}, {"bone", "Hand"}, {"position", Json::array({-0.6, 1.9, 0.2})}}));
    EntityId effector = p.scene().find("Hand IK");
    REQUIRE(effector);
    CHECK(p.scene().get<IkTarget>(effector));
    auto check = [&](Vec3 want) {
        Vec3 s = as.boneWorld(arm, "Shoulder").value().translation();
        Vec3 e = as.boneWorld(arm, "Elbow").value().translation();
        Vec3 h = as.boneWorld(arm, "Hand").value().translation();
        CHECK(distance(h, want) < 2e-3f);
        CHECK(distance(s, e) == doctest::Approx(0.5f).epsilon(1e-3));
        CHECK(distance(e, h) == doctest::Approx(0.5f).epsilon(1e-3));
    };
    check({-0.6f, 1.9f, 0.2f});
    // Moving the effector moves the reach (editor preview re-poses).
    p.patch(effector, "transform", R"({"position": [-0.3, 1.6, -0.5]})");  // the arm root is at the origin
    check({-0.3f, 1.6f, -0.5f});
    // Playing: solved every tick; weight 0 leaves the animation alone.
    p.engine->play();
    p.engine->step(2);
    check({-0.3f, 1.6f, -0.5f});
    p.patch(effector, "ik", R"({"weight": 0})");
    p.engine->step(1);
    CHECK(distance(as.boneWorld(arm, "Hand").value().translation(), Vec3{-1.f, 1.5f, 0.f}) < 1e-3f);
    p.engine->stop();
    p.call("bone_ik", Json::object({{"character", "Arm"}, {"bone", "Shoulder"}}), false);  // no parent chain
}

TEST_CASE("anim look-at: the head turns toward a target, within the angle limit") {
    Project p({.humanNames = true});
    EntityId hero = p.character();
    EntityId target = p.scene().create("Target");
    p.patch(target, "transform", R"({"position": [5, 1.5, 0]})");  // to the character's side
    p.patch(hero, "animator", R"({"clip": "Bend", "time": 0})");
    anim::AnimationSystem& as = p.engine->animation();
    Mat4 rest = as.boneWorld(hero, "Head").value();
    auto facing = [&](const Mat4& head) {
        // The character faces -Z; the head's turn relative to its rest pose turns that direction.
        anim::Quat delta = anim::rotationOf(head) * anim::rotationOf(rest).conjugate();
        return delta.rotate({0, 0, -1});
    };
    p.patch(hero, "animator", R"({"lookAt": "Target", "lookAtLimit": 120})");
    Mat4 head = as.boneWorld(hero, "Head").value();
    Vec3 want = normalize(Vec3{5, 1.5f, 0} - head.translation());
    CHECK(dot(facing(head), want) > 0.98f);
    p.patch(hero, "animator", R"({"lookAtLimit": 45})");
    head = as.boneWorld(hero, "Head").value();
    float angle = degrees(std::acos(std::clamp(dot(facing(head), Vec3{0, 0, -1}), -1.f, 1.f)));
    CHECK(angle == doctest::Approx(45.f).epsilon(0.05));
    p.patch(hero, "animator", R"({"lookAtWeight": 0})");
    head = as.boneWorld(hero, "Head").value();
    CHECK(dot(facing(head), Vec3{0, 0, -1}) > 0.999f);
}
