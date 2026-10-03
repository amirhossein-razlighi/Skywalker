#include <doctest/doctest.h>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "anim_fixtures.h"
#include "skywalker/anim/Sequence.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
using namespace sky::anim;
namespace fs = std::filesystem;

namespace {

Track propertyTrack(const char* json) {
    auto def = SequenceDef::fromJson(Json::parse(std::string(R"({"tracks": [)") + json + "]}").value());
    REQUIRE_MESSAGE(def.ok(), (def.ok() ? "" : def.error().message));
    return def->tracks.at(0);
}

float num(const Json& j) { return j.asFloat(); }

struct Project {
    fs::path dir;
    std::unique_ptr<Engine> engine;
    Project() {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("sky-seq-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(dir / "chars");
        std::ofstream(dir / "chars/strip.gltf") << skytest::makeStripGltf();
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.string();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Seq", true);  // Ground, Cube, Main Camera
    }
    ~Project() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Json call(const char* tool, const char* args, bool expectOk = true) {
        ToolResult r = engine->callTool(tool, Json::parse(args).value(), "agent:test");
        INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
        CHECK(r.isError == !expectOk);
        return r.isError ? Json::object({{"error", r.content.front().text}}) : r.structured;
    }
    ToolResult raw(const char* tool, const char* args) { return engine->callTool(tool, Json::parse(args).value(), "agent:test"); }
    Scene& scene() { return engine->scene(); }
    EntityId id(const char* name) { return scene().find(name); }
};

}  // namespace

TEST_CASE("sequence keys: linear, step, smooth, ease_in/out, bezier, auto spline and colors") {
    Track t = propertyTrack(R"({"entity": "A", "property": "light.intensity", "keys": [
        {"t": 0, "value": 0}, {"t": 2, "value": 4, "ease": "smooth"}, {"t": 4, "value": 0, "ease": "step"}, {"t": 6, "value": 10}]})");
    CHECK(num(evaluateProperty(t, -1.f)) == doctest::Approx(0.f));
    CHECK(num(evaluateProperty(t, 1.f)) == doctest::Approx(2.f));  // linear segment
    CHECK(num(evaluateProperty(t, 2.5f)) == doctest::Approx(4.f - 4.f * (0.25f * 0.25f * (3.f - 0.5f))));  // smoothstep
    CHECK(num(evaluateProperty(t, 5.9f)) == doctest::Approx(0.f));  // step holds
    CHECK(num(evaluateProperty(t, 6.f)) == doctest::Approx(10.f));
    CHECK(num(evaluateProperty(t, 99.f)) == doctest::Approx(10.f));

    SeqKey k;
    k.ease = "ease_in";
    CHECK(ease(k, 0.5f) == doctest::Approx(0.25f));
    k.ease = "ease_out";
    CHECK(ease(k, 0.5f) == doctest::Approx(0.75f));
    k.ease = "bezier";
    k.bezier[0] = 0.f, k.bezier[1] = 0.f, k.bezier[2] = 1.f, k.bezier[3] = 1.f;  // = linear
    CHECK(ease(k, 0.3f) == doctest::Approx(0.3f).epsilon(1e-3));

    // Vectors and hex colors interpolate per component; auto = Catmull-Rom through the keys.
    Track v = propertyTrack(R"({"entity": "A", "property": "transform.position", "keys": [
        {"t": 0, "value": [0, 0, 0], "ease": "auto"}, {"t": 1, "value": [1, 2, 0], "ease": "auto"}, {"t": 2, "value": [2, 0, 0]}]})");
    Json mid = evaluateProperty(v, 1.f);
    CHECK(mid[size_t{1}].asFloat() == doctest::Approx(2.f));
    Json q = evaluateProperty(v, 0.5f);
    CHECK(q[size_t{0}].asFloat() == doctest::Approx(0.5f));  // symmetric x
    CHECK(q[size_t{1}].asFloat() > 1.f);                     // spline overshoots the straight line (1.0)
    Track c = propertyTrack(R"({"entity": "A", "property": "light.color", "keys": [{"t": 0, "value": "#000000"}, {"t": 1, "value": "#ffffff"}]})");
    Json half = evaluateProperty(c, 0.5f);
    REQUIRE(half.size() == 4);
    CHECK(half[size_t{0}].asFloat() == doctest::Approx(0.5f).epsilon(0.02));

    auto bad = SequenceDef::fromJson(Json::parse(R"({"tracks": [{"type": "property", "entity": "A", "property": "light.intensity",
        "keys": [{"t": 0, "value": 1, "ease": "smoth"}]}]})").value());
    REQUIRE(!bad.ok());
    CHECK(bad.error().hint.find("smooth") != std::string::npos);
    bad = SequenceDef::fromJson(Json::parse(R"({"tracks": [{"type": "shot", "camera": "C", "keys": [{"t": 0, "shot": "orbitt", "duration": 1}]}]})").value());
    REQUIRE(!bad.ok());
    CHECK(bad.error().hint.find("orbit") != std::string::npos);
}

TEST_CASE("sequence shots: orbit, dolly and crane frame their target") {
    auto def = SequenceDef::fromJson(Json::parse(R"({"tracks": [{"type": "shot", "camera": "Cam", "keys": [
        {"t": 0, "shot": "orbit", "duration": 2, "target": [1, 0, 1], "radius": 5, "height": 2, "from": 0, "to": 90, "ease": "linear"},
        {"t": 2, "shot": "dolly", "duration": 2, "target": "Hero", "distance": [8, 2], "angle": 180, "height": 1.5},
        {"t": 4, "shot": "crane", "duration": 1, "target": [0, 0, 0], "height": [0.5, 6], "distance": 4, "fov": [60, 30]}]}]})").value());
    REQUIRE(def.ok());
    const Track& t = def->tracks[0];
    auto lookup = [](const std::string& n) -> std::optional<Vec3> {
        if (n == "Hero") return Vec3{0, 1, 0};
        return std::nullopt;
    };
    auto o0 = evaluateShot(t, 0.f, lookup);
    REQUIRE(o0);
    CHECK(distance(o0->position, Vec3{1, 2, 6}) < 1e-4f);  // angle 0 = +Z of the target
    auto o1 = evaluateShot(t, 1.f, lookup);
    CHECK(distance(o1->position, Vec3{1.f + 5.f * std::sin(radians(45.f)), 2, 1.f + 5.f * std::cos(radians(45.f))}) < 1e-3f);
    // Looking at the target: forward (-Z rotated) points from the camera to it.
    Vec3 fwd = Mat4::rotateEulerDeg(o1->rotation).transformDir({0, 0, -1});
    CHECK(dot(normalize(fwd), normalize(Vec3{1, 0, 1} - o1->position)) > 0.999f);
    auto d0 = evaluateShot(t, 2.f, lookup), d1 = evaluateShot(t, 4.f - 1e-4f, lookup);
    CHECK(distance(d0->position, Vec3{0, 2.5f, -8}) < 1e-3f);
    CHECK(distance(d1->position, Vec3{0, 2.5f, -2}) < 1e-2f);
    auto c1 = evaluateShot(t, 5.f, lookup);
    CHECK(c1->position.y == doctest::Approx(6.f));
    REQUIRE(c1->fov);
    CHECK(*c1->fov == doctest::Approx(30.f));
}

TEST_CASE("sequencer tools: create, key, shots, scrub (no scene change), play, events, reset, determinism") {
    Project p;
    Json c = p.call("sequence_create", R"({"path": "cinematics/intro.sequence.json", "duration": 4, "entity": "Director"})");
    EntityId director = p.id("Director");
    REQUIRE(director);
    CHECK(p.scene().get<SequencePlayer>(director));
    p.call("sequence_create", R"({"path": "cinematics/intro.sequence.json"})", false);  // exists
    p.call("entity_create", R"({"name": "Lamp", "position": [0, 3, 0]})");
    p.call("entity_update", R"({"entity": "Lamp", "components": {"light": {"intensity": 1}}})");
    p.call("sequence_key", R"({"sequence": "Director", "keys": [
        {"entity": "Lamp", "property": "light.intensity", "t": 0, "value": 0},
        {"entity": "Lamp", "property": "light.intensity", "t": 2, "value": 8},
        {"entity": "Cube", "property": "transform.position", "t": 0, "value": [0, 0.5, 0]},
        {"entity": "Cube", "property": "transform.position", "t": 4, "value": [4, 0.5, 0], "ease": "smooth"}],
      "events": [{"t": 1, "event": "boom", "target": "Cube"}]})");
    Json err = p.call("sequence_key", R"({"sequence": "Director", "keys": [{"entity": "Lamp", "property": "light.intensty", "t": 0, "value": 1}]})", false);
    CHECK(err.get("error").asString().find("light.intensity") != std::string::npos);
    p.call("sequence_key", R"({"sequence": "Director", "keys": [{"entity": "Lamp", "property": "lihgt.intensity", "t": 0, "value": 1}]})", false);
    Json shot = p.call("sequence_camera_shot", R"({"sequence": "Director", "camera": "Cam A", "shot": "orbit", "target": "Cube", "duration": 4, "radius": 6})");
    CHECK(shot.get("preview").size() == 3);
    EntityId camA = p.id("Cam A");
    REQUIRE(camA);
    Json got = p.call("sequence_get", R"({"sequence": "cinematics/intro.sequence.json", "time": 1})");
    CHECK(got.get("camera").asString() == "Cam A");
    CHECK(got.get("tracks").size() == 5);  // lamp, cube, event, shot, camera cuts

    // Scrub: renders with everything applied, then the scene is exactly as before.
    Json before = p.scene().toJson();
    ToolResult scrub = p.raw("sequence_scrub", R"({"sequence": "Director", "times": [0, 2, 4], "width": 64, "height": 36})");
    REQUIRE_FALSE(scrub.isError);
    size_t images = 0;
    for (const auto& b : scrub.content) images += b.type == ContentBlock::Type::Image;
    CHECK(images == 3);
    CHECK(scrub.structured.get("shots")[size_t{0}].get("camera").asString() == "Cam A");
    CHECK(p.scene().toJson() == before);
    // A frame rendered at a scrub time sees the keyed values.
    p.engine->animation().setSequenceScrub(director, 2.f);
    FrameData f = p.engine->frame(CaptureOptions{});
    p.engine->animation().clearSequenceScrub(director);
    bool lampAt8 = false;
    for (const auto& l : f.lights) lampAt8 = lampAt8 || std::fabs(l.intensity - 8.f) < 1e-3f;
    CHECK(lampAt8);
    CHECK(p.scene().get<Light>(p.id("Lamp"))->intensity == doctest::Approx(1.f));

    // Play: values follow the keys, the event reaches Wander, cameras cut, stop resets.
    REQUIRE(p.scene().setBehaviors(p.id("Cube"), Json::parse(R"([{"name": "B", "source": "behavior B\n  var booms = 0\n  on event \"boom\"\n    booms = booms + 1\n  end\nend\n"}])").value()).ok());
    auto session = [&]() {
        p.call("sequence_play", R"({"sequence": "Director"})");
        CHECK((p.engine->playState() == PlayState::Playing));
        p.engine->pause();
        p.engine->step(120);  // 2 s
        CHECK(p.scene().get<Light>(p.id("Lamp"))->intensity == doctest::Approx(8.f).epsilon(0.01));
        CHECK(p.scene().record(p.id("Cube"))->vars.get("booms").asInt() == 1);
        CHECK(p.scene().get<Camera>(camA)->primary);
        CHECK_FALSE(p.scene().get<Camera>(p.id("Main Camera"))->primary);
        Vec3 cube = p.scene().get<Transform>(p.id("Cube"))->position;
        Vec3 cam = p.scene().get<Transform>(camA)->position;
        p.engine->stop();
        return std::make_pair(cube, cam);
    };
    auto [cube1, cam1] = session();
    CHECK(cube1.x == doctest::Approx(2.f).epsilon(0.02));  // smooth ease, halfway
    CHECK(p.scene().get<Light>(p.id("Lamp"))->intensity == doctest::Approx(1.f));  // reset on stop
    CHECK(p.scene().get<Camera>(p.id("Main Camera"))->primary);
    auto [cube2, cam2] = session();
    CHECK(std::memcmp(&cube1, &cube2, sizeof(Vec3)) == 0);
    CHECK(std::memcmp(&cam1, &cam2, sizeof(Vec3)) == 0);

    // Persisted preview: the editor shows that time (an undoable sequencer edit).
    p.call("sequence_scrub", R"({"sequence": "Director", "time": 1, "persist": true})");
    CHECK(p.scene().get<SequencePlayer>(director)->preview);

    // Render frames to disk.
    Json frames = p.call("sequence_scrub", R"({"sequence": "Director", "fps": 4, "from": 0, "to": 1, "save_dir": "renders/intro", "width": 32, "height": 18})");
    CHECK(frames.get("frames").asInt() == 5);
    CHECK(fs::exists(p.dir / "renders/intro/frame_0004.png"));
}

TEST_CASE("animation tools: list, setup locomotion, preview, drive, attach") {
    Project p;
    p.call("asset_import", R"({"path": "chars/strip.gltf", "create_entity": "Hero"})");
    Json libs = p.call("animation_list", "{}");
    REQUIRE(libs.get("libraries").size() == 1);
    CHECK(libs.get("libraries")[size_t{0}].get("path").asString() == "chars/strip.anim");
    Json model = p.call("animation_list", R"({"model": "chars/strip.gltf"})");
    CHECK(model.get("clips").size() == 5);
    CHECK(model.get("bones").size() == 4);
    Json walk;
    for (const auto& c : model.get("clips").elements()) {
        if (c.get("name").asString() == "Walk") walk = c;
    }
    CHECK(walk.get("rootSpeed").asFloat() == doctest::Approx(1.5f));

    Json setup = p.call("animator_setup", R"({"entity": "Hero", "root_motion": true})");
    CHECK(setup.get("controller").asString() == "chars/hero.animctl.json");
    CHECK(fs::exists(p.dir / "chars/hero.animctl.json"));
    const Json& motions = setup.get("document").get("layers")[size_t{0}].get("states").get("Locomotion").get("blend").get("motions");
    REQUIRE(motions.size() == 2);
    CHECK(motions[size_t{1}].get("clip").asString() == "Walk");
    CHECK(motions[size_t{1}].get("at").asFloat() == doctest::Approx(1.5f));  // the clip's real speed
    EntityId hero = p.id("Hero");
    CHECK(p.scene().get<Animator>(hero)->controller == "chars/hero.animctl.json");
    p.call("animator_setup", R"({"entity": "Hero", "controller": {"states": {"A": {"clip": "Wlk"}}}})", false);

    ToolResult prev = p.raw("animation_preview", R"({"entity": "Hero", "clip": "Bend", "times": [0, 0.5, 1], "width": 64, "height": 64})");
    REQUIRE_FALSE(prev.isError);
    size_t images = 0;
    for (const auto& b : prev.content) images += b.type == ContentBlock::Type::Image;
    CHECK(images == 3);

    Json err = p.call("animator_set", R"({"entity": "Hero", "params": {"sped": 2}})", false);
    CHECK(err.get("error").asString().find("speed") != std::string::npos);
    p.engine->play();
    Json st = p.call("animator_set", R"({"entity": "Hero", "params": {"speed": 1.5}})");
    CHECK(st.get("state").get("parameters").get("speed").asFloat() == doctest::Approx(1.5f));
    p.engine->step(60);
    Vec3 pos = p.scene().get<Transform>(hero)->position;
    CHECK(pos.z == doctest::Approx(-1.5f).epsilon(0.05));  // root motion at the walk speed
    p.engine->stop();

    p.call("entity_create", R"({"name": "Hat", "mesh": "cube"})");
    Json bad = p.call("bone_attach", R"({"entity": "Hat", "to": "Hero", "bone": "Uper"})", false);
    CHECK(bad.get("error").asString().find("Upper") != std::string::npos);
    Json ok = p.call("bone_attach", R"({"entity": "Hat", "to": "Hero", "bone": "Upper", "offset": [0, 1, 0]})");
    CHECK(ok.get("boneWorld")[size_t{1}].asFloat() == doctest::Approx(1.f));
    CHECK(p.scene().record(p.id("Hat"))->parent == hero);
    CHECK(p.scene().get<BoneAttachment>(p.id("Hat"))->bone == "Upper");
}
