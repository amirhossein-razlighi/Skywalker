// Movie render queue: frame timing, camera paths, transform interpolation, PNG sequences and
// resume, determinism on the CPU renderer, cancellation, the tool surface (docs/MOVIE_RENDER.md).

#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "skywalker/anim/AnimMath.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/engine/Movie.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct MovieProject {
    fs::path dir;
    std::unique_ptr<Engine> engine;
    MovieProject() {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("sky-movie-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(dir);
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.string();
        cfg.audio = audio::AudioMode::Null;
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Movie", true);  // Ground, Cube, Main Camera
        Scene& s = engine->scene();
        // Something the simulation moves (and rotates), so frames differ over time.
        REQUIRE(s.setBehaviors(s.find("Cube"), Json::array({Json::object(
                                                    {{"source", "on tick\n move self by (2 * dt, 0, 0)\n rotate self by (0, 90 * dt, 0)\nend"}})})));
    }
    ~MovieProject() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    ToolResult call(const char* tool, const Json& args) { return engine->callTool(tool, args, "agent:test"); }
    std::string path(const std::string& rel) const { return (dir / rel).string(); }
};

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

const char* kOrbit = R"({"shots": [{"shot": "orbit", "duration": 0.5, "target": "Cube", "radius": 6, "height": 2, "from": 0, "to": 120},
                                   {"shot": "static", "duration": 0.25, "position": [0, 2, 9], "target": [0, 0, 0]}]})";

}  // namespace

TEST_CASE("movie timing: sample split, shutter sub-frame times, cuts") {
    using namespace movie;
    CHECK(splitSamples(8, 0.0, 0).temporal == 1);
    CHECK(splitSamples(8, 0.0, 0).spatial == 8);
    CHECK(splitSamples(8, 0.5, 0).temporal == 8);
    CHECK(splitSamples(8, 0.5, 0).spatial == 1);
    CHECK(splitSamples(16, 0.5, 4).temporal == 4);
    CHECK(splitSamples(16, 0.5, 4).spatial == 4);
    CHECK(splitSamples(1, 0.5, 0).temporal == 2);  // a shutter needs at least two instants

    Timing t;
    t.fps = 24;
    t.firstFrame = 24;  // starts at 1 s
    t.frames = 10;
    CHECK(t.frameTime(0) == doctest::Approx(1.0));
    CHECK(t.frameTime(3) == doctest::Approx(27.0 / 24.0));
    // No shutter: one instant per frame, exactly the frame time.
    auto none = t.sampleTimes(2);
    REQUIRE(none.size() == 1);
    CHECK(none[0] == doctest::Approx(26.0 / 24.0));
    // 180-degree shutter, 4 sub-frames, centered: stratified over [t - 1/96, t + 1/96].
    t.shutter = 0.5;
    t.temporalSamples = 4;
    auto c = t.sampleTimes(0);
    REQUIRE(c.size() == 4);
    const double len = 0.5 / 24.0;
    for (int i = 0; i < 4; ++i) CHECK(c[static_cast<size_t>(i)] == doctest::Approx(1.0 - len / 2 + (i + 0.5) / 4 * len));
    t.timing = ShutterTiming::Open;
    CHECK(t.sampleTimes(0).front() == doctest::Approx(1.0 + len / 8));
    CHECK(t.sampleTimes(0).back() == doctest::Approx(1.0 + len * 7 / 8));
    t.timing = ShutterTiming::Close;
    CHECK(t.sampleTimes(0).back() == doctest::Approx(1.0 - len / 8));
    // Never before time 0, never across a cut.
    Timing z;
    z.fps = 24;
    z.frames = 2;
    z.shutter = 1.0;
    z.temporalSamples = 4;
    for (double s : z.sampleTimes(0)) CHECK(s >= 0.0);
    std::vector<double> cuts{1.0 + 1.0 / 96.0};
    auto [lo, hi] = shotSpan(cuts, 1.0);
    CHECK(lo == 0.0);
    CHECK(hi == doctest::Approx(1.0 + 1.0 / 96.0));
    t.timing = ShutterTiming::Center;
    t.shutter = 1.0;
    for (double s : t.sampleTimes(0, lo, hi)) CHECK(s < hi);
    CHECK(shotIndex(cuts, 1.0) == 0);
    CHECK(shotIndex(cuts, 1.5) == 1);
    auto [lo2, hi2] = shotSpan(cuts, 1.5);
    CHECK(lo2 == doctest::Approx(cuts[0]));
    CHECK(std::isinf(hi2));
}

TEST_CASE("movie: transforms interpolate between ticks, teleports snap") {
    Transform a, b;
    a.position = {0, 0, 0};
    b.position = {1, 2, 0};
    a.rotation = {0, 0, 0};
    b.rotation = {0, 90, 0};
    b.scale = {3, 1, 1};
    Transform h = movie::interpolateTransform(a, b, 0.5f);
    CHECK(h.position.x == doctest::Approx(0.5f));
    CHECK(h.position.y == doctest::Approx(1.f));
    CHECK(h.scale.x == doctest::Approx(2.f));
    CHECK(h.rotation.y == doctest::Approx(45.f).epsilon(1e-3));
    Transform end = movie::interpolateTransform(a, b, 1.f);
    CHECK(end.rotation.y == doctest::Approx(90.f));
    // Slerp takes the short way round (350 -> 10 degrees passes 0, not 180).
    a.rotation = {0, 350, 0};
    b.rotation = {0, 10, 0};
    Mat4 m = Mat4::rotateEulerDeg(movie::interpolateTransform(a, b, 0.5f).rotation);
    Vec3 fwd = m.transformDir({0, 0, -1});
    CHECK(fwd.z == doctest::Approx(-1.f).epsilon(1e-3));
    // 100 m in a tick is a teleport: no smear.
    b.position = {100, 0, 0};
    CHECK(movie::interpolateTransform(a, b, 0.3f).position.x == doctest::Approx(100.f));
}

TEST_CASE("movie camera paths: keyframes, shots, cuts, validation") {
    using movie::CameraPath;
    auto keys = CameraPath::fromJson(Json::parse(R"({"keys": [
        {"t": 0, "eye": [0, 2, 10], "target": [0, 0, 0], "fov": 40},
        {"t": 2, "eye": [10, 2, 0], "target": [0, 1, 0], "fov": 60, "roll": 10},
        {"t": 4, "eye": [0, 2, -10], "target": [0, 0, 0]}], "aperture": 2.8, "tilt_shift": 0.6})")
                                         .value());
    REQUIRE_MESSAGE(keys.ok(), (keys.ok() ? "" : keys.error().message));
    CHECK(keys->length() == doctest::Approx(4.f));
    CHECK(keys->cuts().empty());
    ViewCamera base;
    ViewCamera v = keys->evaluate(2.0, nullptr, base);
    CHECK(v.eye.x == doctest::Approx(10.f));
    CHECK(v.target.y == doctest::Approx(1.f));
    CHECK(v.fovDeg == doctest::Approx(60.f));
    CHECK(v.aperture == doctest::Approx(2.8f));
    CHECK(v.tiltShift == doctest::Approx(0.6f));
    CHECK(v.up.y < 0.999f);  // rolled
    ViewCamera mid = keys->evaluate(1.0, nullptr, base);
    CHECK(length(mid.eye - Vec3{0, 2, 10}) > 1.f);  // splined between keys
    CHECK(length(mid.eye - Vec3{10, 2, 0}) > 1.f);

    auto shots = CameraPath::fromJson(Json::parse(kOrbit).value());
    REQUIRE_MESSAGE(shots.ok(), (shots.ok() ? "" : shots.error().message));
    CHECK(shots->length() == doctest::Approx(0.75f));
    REQUIRE(shots->cuts().size() == 1);  // shots play back to back; each start is a cut
    CHECK(shots->cuts()[0] == doctest::Approx(0.5));
    anim::PointLookup at = [](const std::string&) { return std::optional<Vec3>(Vec3{0, 0, 0}); };
    ViewCamera o = shots->evaluate(0.0, at, base);
    CHECK(length(Vec3{o.eye.x, 0, o.eye.z}) == doctest::Approx(6.f));
    CHECK(o.eye.y == doctest::Approx(2.f));
    CHECK(dot(normalize(o.target - o.eye), normalize(Vec3{0, 0, 0} - o.eye)) == doctest::Approx(1.f).epsilon(1e-3));
    ViewCamera st = shots->evaluate(0.6, at, base);
    CHECK(st.eye.z == doctest::Approx(9.f));

    auto fly = CameraPath::fromJson(Json::parse(R"({"shots": [{"shot": "flyover", "duration": 2, "target": [0, 0, 0], "distance": 20, "height": 10}]})").value());
    REQUIRE(fly.ok());
    CHECK(fly->evaluate(0.0, nullptr, base).eye.z == doctest::Approx(20.f));
    CHECK(fly->evaluate(2.0, nullptr, base).eye.z == doctest::Approx(-20.f));
    CHECK(fly->evaluate(1.0, nullptr, base).eye.y == doctest::Approx(10.f));

    auto bad = CameraPath::fromJson(Json::parse(R"({"keys": [{"t": 0, "eye": [0, 0, 0], "traget": [1, 0, 0]}]})").value());
    REQUIRE(!bad.ok());
    CHECK(bad.error().hint.find("target") != std::string::npos);
    auto badShot = CameraPath::fromJson(Json::parse(R"({"shots": [{"shot": "orbitt", "duration": 1}]})").value());
    REQUIRE(!badShot.ok());
    CHECK(badShot.error().code == "invalid_camera");
    CHECK(badShot.error().hint.find("orbit") != std::string::npos);
    CHECK(!CameraPath::fromJson(Json::parse(R"({"keys": [], "shots": []})").value()).ok());
}

TEST_CASE("movie PNG sequences: patterns, atomic writes, resume point") {
    fs::path dir = fs::temp_directory_path() / ("sky-pngseq-" + std::to_string(::getpid()));
    using movie::PngSequence;
    CHECK(PngSequence((dir / "shot/frame_####.png").string()).pathFor(7) == (dir / "shot/frame_0007.png").string());
    CHECK(PngSequence((dir / "shot/f_%06d.png").string()).pathFor(12) == (dir / "shot/f_000012.png").string());
    CHECK(PngSequence((dir / "shot.png").string()).pathFor(3) == (dir / "shot_0003.png").string());
    CHECK(PngSequence((dir / "shot").string()).pathFor(3) == (dir / "shot/frame_0003.png").string());
    CHECK(PngSequence((dir / "shot").string()).pattern() == (dir / "shot/frame_####.png").string());

    PngSequence seq((dir / "a/frame_####.png").string());
    movie::FrameBuffer fb(4, 2);
    Image img(4, 2);
    for (auto& p : img.pixels) p = 200;
    fb.accumulate(img, 0.5f);
    fb.accumulate(img, 0.5f);
    CHECK(fb.toImage().pixels[0] == 200);
    CHECK(seq.firstMissing(10, 14) == 10);
    REQUIRE(seq.write(10, fb));
    REQUIRE(seq.write(11, fb));
    REQUIRE(seq.write(13, fb));
    CHECK(fs::exists(seq.pathFor(11)));
    CHECK(!fs::exists(seq.pathFor(11) + ".part"));
    CHECK(seq.firstMissing(10, 14) == 12);
    REQUIRE(seq.write(12, fb));
    REQUIRE(seq.write(14, fb));
    CHECK(seq.firstMissing(10, 14) == 15);  // complete
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("movie_render: deterministic frames on the CPU renderer, scene restored") {
    MovieProject p;
    Scene& s = p.engine->scene();
    const Json before = s.toJson();
    auto render = [&](const std::string& out) {
        ToolResult r = p.call("movie_render", Json::object({{"camera", Json::parse(kOrbit).value()},
                                                            {"simulate", true},
                                                            {"width", 96},
                                                            {"height", 54},
                                                            {"fps", 24},
                                                            {"samples", 4},
                                                            {"shutter", 0.5},
                                                            {"output", out}}));
        INFO((r.content.empty() ? "" : r.content.front().text));
        REQUIRE(!r.isError);
        return r.structured;
    };
    Json a = render("renders/a");
    CHECK(a.get("state").asString() == "done");
    CHECK(a.get("frames").asInt() == 18);  // 0.75 s at 24 fps
    CHECK(a.get("samples").get("temporal").asInt() == 4);
    CHECK(std::string(toString(p.engine->playState())) == "editing");
    CHECK(s.toJson() == before);  // the play session was undone
    Json b = render("renders/b");
    CHECK(b.get("frame").asInt() == 18);
    int differing = 0;
    for (int f = 0; f < 18; ++f) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%04d.png", f);
        std::string fa = readFile(p.path("renders/a/") + name), fb = readFile(p.path("renders/b/") + name);
        REQUIRE(!fa.empty());
        CHECK(fa == fb);
        if (f > 0 && fa != readFile(p.path("renders/a/frame_0000.png"))) ++differing;
    }
    CHECK(differing > 10);  // the camera and the cube move
}

TEST_CASE("movie_render: resume re-renders exactly the missing frames") {
    MovieProject p;
    Json args = Json::object({{"duration", 0.5}, {"simulate", true}, {"width", 64}, {"height", 36}, {"fps", 12}, {"samples", 2},
                              {"shutter", 0.5}, {"output", "out/f_####.png"}});
    ToolResult full = p.call("movie_render", args);
    REQUIRE(!full.isError);
    CHECK(full.structured.get("frames").asInt() == 6);
    std::vector<std::string> original;
    for (int f = 0; f < 6; ++f) original.push_back(readFile(p.path("out/f_000" + std::to_string(f) + ".png")));
    for (int f = 3; f < 6; ++f) fs::remove(p.path("out/f_000" + std::to_string(f) + ".png"));
    args["resume"] = true;
    ToolResult resumed = p.call("movie_render", args);
    REQUIRE(!resumed.isError);
    CHECK(resumed.structured.get("resumed_at").asInt() == 3);
    CHECK(resumed.structured.get("frame").asInt() == 3);
    for (int f = 0; f < 6; ++f) CHECK(readFile(p.path("out/f_000" + std::to_string(f) + ".png")) == original[static_cast<size_t>(f)]);
    // Nothing left: done at once.
    ToolResult again = p.call("movie_render", args);
    REQUIRE(!again.isError);
    CHECK(again.structured.get("frame").asInt() == 0);
}

TEST_CASE("movie_render: sequences set the length; sub-frames see exact sequence values") {
    MovieProject p;
    REQUIRE(!p.call("sequence_create", Json::parse(R"({"path": "cinematics/intro.sequence.json", "name": "Intro", "duration": 1})").value()).isError);
    REQUIRE(!p.call("sequence_camera_shot", Json::parse(R"({"sequence": "Intro", "camera": "Cam A", "shot": "orbit", "target": "Cube",
        "start": 0, "duration": 0.5, "radius": 5})").value()).isError);
    REQUIRE(!p.call("sequence_camera_shot", Json::parse(R"({"sequence": "Intro", "camera": "Cam A", "shot": "static", "target": "Cube",
        "start": 0.5, "duration": 0.5, "position": [0, 3, 8]})").value()).isError);
    std::vector<Json> progress;
    movie::Options o;
    {
        auto parsed = movie::parseOptions(*p.engine, Json::parse(R"({"sequence": "Intro", "width": 64, "height": 36, "fps": 24,
            "samples": 2, "shutter": 0.5, "output": "seq"})").value());
        REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
        o = *parsed;
    }
    auto r = p.engine->renderMovie(o, [&](const Json& j) { progress.push_back(j); });
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->get("frames").asInt() == 24);
    CHECK(r->get("camera").asString() == "Cam A");
    REQUIRE(progress.size() == 24);  // 23 progress reports + the final one
    CHECK(progress.front().get("type").asString() == "movie_progress");
    CHECK(progress.back().get("type").asString() == "movie_finished");
    CHECK(fs::exists(p.path("seq/frame_0023.png")));
    CHECK(p.engine->movieStatus().get("state").asString() == "done");
}

TEST_CASE("movie_render: cancellation keeps finished frames and restores the scene") {
    MovieProject p;
    const Json before = p.engine->scene().toJson();
    auto parsed = movie::parseOptions(*p.engine, Json::parse(R"({"duration": 2, "simulate": true, "width": 32, "height": 18,
        "fps": 24, "samples": 1, "output": "c"})").value());
    REQUIRE(parsed.ok());
    int frames = 0;
    auto r = p.engine->renderMovie(*parsed, [&](const Json& j) {
        if (j.get("type").asString() == "movie_progress" && ++frames == 3) p.engine->cancelMovie();
    });
    REQUIRE(r.ok());
    CHECK(r->get("state").asString() == "cancelled");
    CHECK(r->get("frame").asInt() == 3);
    CHECK(fs::exists(p.path("c/frame_0002.png")));
    CHECK(!fs::exists(p.path("c/frame_0003.png")));
    CHECK(std::string(toString(p.engine->playState())) == "editing");
    CHECK(p.engine->scene().toJson() == before);
}

TEST_CASE("movie_render: background renders advance with update(); errors are helpful") {
    MovieProject p;
    ToolResult start = p.call("movie_render", Json::parse(R"({"duration": 0.25, "width": 32, "height": 18, "samples": 1, "output": "bg",
        "background": true})").value());
    REQUIRE(!start.isError);
    CHECK(p.engine->movieRendering());
    CHECK(p.call("movie_render", Json::parse(R"({"duration": 1, "output": "x"})").value()).isError);  // busy
    for (int i = 0; i < 100 && p.engine->movieRendering(); ++i) p.engine->update(1.0 / 60.0);
    CHECK(!p.engine->movieRendering());
    CHECK(p.call("movie_render", Json::parse(R"({"action": "status"})").value()).structured.get("state").asString() == "done");
    CHECK(fs::exists(p.path("bg/frame_0005.png")));
    CHECK(std::string(toString(p.engine->playState())) == "editing");

    auto err = [&](const char* json) {
        ToolResult r = p.call("movie_render", Json::parse(json).value());
        CHECK(r.isError);
        return r.content.empty() ? std::string() : r.content.front().text;
    };
    CHECK(err(R"({"output": "x"})").find("range") != std::string::npos);  // nothing says how long
    CHECK(err(R"({"duration": 1, "camera": {"shots": [{"shot": "orbit", "duration": 1, "target": "Cubee"}]}})").find("Cube") != std::string::npos);
    CHECK(err(R"({"duration": 1, "width": 5000})").find("3840") != std::string::npos);
    CHECK(err(R"({"duration": 1, "output": "x.mov", "codec": "prores", "resume": true})").find("PNG") != std::string::npos);
    p.engine->play();
    CHECK(err(R"({"duration": 1, "output": "y"})").find("stop") != std::string::npos);
    p.engine->stop();
}

TEST_CASE("movie video writer encodes when AVFoundation is available") {
    if (!movie::videoEncodingAvailable()) {
        MESSAGE("no video encoder in this build: PNG sequences only");
        CHECK(!movie::openVideoWriter({"/tmp/x.mp4", movie::Codec::H264, 64, 64, 24, 0}).ok());
        return;
    }
    fs::path dir = fs::temp_directory_path() / ("sky-video-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    for (movie::Codec c : {movie::Codec::H264, movie::Codec::Hevc, movie::Codec::ProRes422HQ}) {
        std::string path = (dir / (std::string("v_") + movie::codecName(c) + (c == movie::Codec::ProRes422HQ ? ".mov" : ".mp4"))).string();
        auto w = movie::openVideoWriter({path, c, 64, 32, 24, 0});
        REQUIRE_MESSAGE(w.ok(), (w.ok() ? "" : w.error().message));
        movie::FrameBuffer fb(64, 32);
        for (int f = 0; f < 3; ++f) {
            for (size_t i = 0; i < fb.rgba.size(); ++i) fb.rgba[i] = static_cast<float>((i + static_cast<size_t>(f) * 7) % 97) / 96.f;
            REQUIRE((*w)->append(fb));
        }
        CHECK((*w)->framesWritten() == 3);
        REQUIRE((*w)->finish());
        CHECK(fs::file_size(path) > 100);
    }
    CHECK(!movie::openVideoWriter({(dir / "odd.mp4").string(), movie::Codec::H264, 63, 32, 24, 0}).ok());
    CHECK(!movie::openVideoWriter({(dir / "p.mp4").string(), movie::Codec::ProRes422HQ, 64, 32, 24, 0}).ok());
    std::error_code ec;
    fs::remove_all(dir, ec);
}
