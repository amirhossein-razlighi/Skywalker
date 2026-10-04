// W3/W4: frame profiler math, debug view table and tools, shader cache bookkeeping.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "skywalker/core/Profiler.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/render/DebugViews.h"
#include "skywalker/render/ShaderCache.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Test", true);
    return e;
}

ToolResult call(Engine& e, const char* tool, const char* args) { return e.callTool(tool, Json::parse(args).value(), "agent:test"); }

/// Sets an environment variable for the scope.
struct EnvGuard {
    std::string name, old;
    bool had = false;
    EnvGuard(const char* n, const char* v) : name(n) {
        if (const char* o = std::getenv(n)) old = o, had = true;
        if (v) ::setenv(n, v, 1);
        else ::unsetenv(n);
    }
    ~EnvGuard() {
        if (had) ::setenv(name.c_str(), old.c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Rolling statistics and the pass timeline
// ---------------------------------------------------------------------------

TEST_CASE("profiler: rolling stat keeps the last 60 samples") {
    prof::RollingStat s;
    CHECK(s.average() == 0.0);
    CHECK(s.count() == 0);
    for (int i = 1; i <= 4; ++i) s.add(i);
    CHECK(s.average() == doctest::Approx(2.5));
    CHECK(s.min() == 1.0);
    CHECK(s.max() == 4.0);
    CHECK(s.last() == 4.0);
    // 100 more samples of 10: the window forgets the first ones.
    for (int i = 0; i < 100; ++i) s.add(10.0);
    CHECK(s.count() == prof::RollingStat::kWindow);
    CHECK(s.total() == 104);
    CHECK(s.average() == doctest::Approx(10.0));
    CHECK(s.min() == 10.0);
    s.add(std::numeric_limits<double>::quiet_NaN());  // ignored
    CHECK(s.total() == 104);
    s.reset();
    CHECK(s.count() == 0);
}

TEST_CASE("profiler: passes with the same label sum per frame, in first-seen order") {
    std::vector<prof::PassSample> frame = {
        {"Shadow cascades", "shadows", 0.0, 0.5, 0.4},
        {"Main", "main", 0.5, 3.5, 0.6},
        {"Bloom down", "post", 3.5, 3.6},
        {"Bloom down", "post", 3.6, 3.8},
        {"Broken", "post", 4.0, 3.0},  // end < start: skipped
        {"Bloom up", "post", 3.8, 4.0},
    };
    auto totals = prof::aggregatePasses(frame);
    REQUIRE(totals.size() == 4);
    CHECK(totals[0].label == "Shadow cascades");
    CHECK(totals[0].vertexMs == doctest::Approx(0.4));
    CHECK(totals[1].ms == doctest::Approx(3.0));
    CHECK(totals[2].label == "Bloom down");
    CHECK(totals[2].ms == doctest::Approx(0.3));
    CHECK(totals[2].count == 2);
    CHECK(totals[2].vertexMs < 0);  // compute/blit-like samples carry no vertex time
    CHECK(totals[3].label == "Bloom up");
    CHECK(prof::passSpanMs(frame) == doctest::Approx(4.0));
    CHECK(prof::passSpanMs({}) == 0.0);
}

TEST_CASE("profiler: timeline averages frames, weighs one-off passes and groups") {
    prof::PassTimeline t;
    CHECK(t.toJson().get("passes").size() == 0);
    t.addFrame({{"Env sky", "environment", 0, 6}, {"Main", "main", 6, 9}});  // first frame bakes the sky
    for (int i = 0; i < 9; ++i) t.addFrame({{"Main", "main", 0, 2}, {"Composite", "post", 2, 2.5}});
    Json j = t.toJson();
    CHECK(j.get("frames").asInt() == 10);
    CHECK(j.get("window").asInt() == 10);
    const Json& passes = j.get("passes");
    REQUIRE(passes.size() == 3);
    // Latest frame's order first; the one-off bake after.
    CHECK(passes[0].get("pass").asString() == "Main");
    CHECK(passes[1].get("pass").asString() == "Composite");
    CHECK(passes[2].get("pass").asString() == "Env sky");
    CHECK(passes[0].get("avgMs").asNumber() == doctest::Approx((3.0 + 9 * 2.0) / 10.0));
    CHECK(passes[0].get("maxMs").asNumber() == doctest::Approx(3.0));
    CHECK(passes[0].get("ms").asNumber() == doctest::Approx(2.0));
    CHECK(passes[2].get("seen").asInt() == 1);
    // The sky bake ran in 1 of 10 frames: 6 ms counts as 0.6 ms per frame.
    CHECK(j.get("groups").get("environment").asNumber() == doctest::Approx(0.6));
    CHECK(j.get("groups").get("main").asNumber() == doctest::Approx(2.1));
    CHECK(j.get("sumMs").asNumber() == doctest::Approx(0.6 + 2.1 + 0.5 * 0.9));
    // Passes that stop running drop out after a window of frames.
    for (int i = 0; i < 70; ++i) t.addFrame({{"Main", "main", 0, 1}});
    CHECK(t.toJson().get("passes").size() == 1);
    t.reset();
    CHECK(t.frames() == 0);
}

TEST_CASE("profiler: CPU scopes record named blocks") {
    prof::CpuProfiler::instance().reset();
    {
        SKY_PROFILE_SCOPE("test.scope");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    { SKY_PROFILE_SCOPE("test.scope"); }
    Json j = prof::CpuProfiler::instance().toJson();
    REQUIRE(j.size() == 1);
    CHECK(j[0].get("scope").asString() == "test.scope");
    CHECK(j[0].get("calls").asInt() == 2);
    CHECK(j[0].get("maxMs").asNumber() >= 1.5);
    prof::CpuProfiler::instance().reset();
}

// ---------------------------------------------------------------------------
// Debug views
// ---------------------------------------------------------------------------

TEST_CASE("debug views: ids, names and the shading-override set are stable") {
    const auto& views = debugViews();
    REQUIRE(!views.empty());
    REQUIRE(views.size() <= static_cast<size_t>(debugview::kCount));  // ids may leave gaps (reserved ids)
    CHECK(views.back().id + 1 == debugview::kCount);
    for (size_t i = 0; i < views.size(); ++i) {
        INFO(views[i].name);
        if (i > 0) CHECK(views[i].id > views[i - 1].id);  // ordered by id (gaps allowed): the shaders switch on these numbers
        if (views[i].id <= debugview::kShadowAtlas) CHECK(views[i].id == static_cast<int>(i));
        CHECK(std::string(views[i].description).size() > 10);
        CHECK(debugViewFromName(views[i].name).value() == views[i].id);
        CHECK(std::string(debugViewName(views[i].id)) == views[i].name);
    }
    CHECK(debugViewFromName("").value() == debugview::kFinal);
    CHECK(std::string(debugViewName(999)) == "final");
    // Ids the shaders (Debug.metal) and the movie presets rely on.
    CHECK(debugViewFromName("sketch").value() == 9);
    CHECK(debugViewFromName("impostors").value() == 10);
    CHECK(debugViewFromName("wireframe").value() == 11);
    CHECK(debugViewFromName("overdraw").value() == 12);
    CHECK(debugViewFromName("lighting_only").value() == 14);
    CHECK(debugViewFromName("texel_density").value() == 21);
    CHECK(debugViewOverridesSurfaces(debugview::kLod));
    CHECK(debugViewOverridesSurfaces(debugview::kWireframe));
    CHECK(debugViewOverridesSurfaces(debugview::kLightingOnly));
    CHECK_FALSE(debugViewOverridesSurfaces(debugview::kAlbedo));
    CHECK_FALSE(debugViewOverridesSurfaces(debugview::kFinal));
    CHECK(debugViewHelp().find("light_complexity:") != std::string::npos);
    // Character views: overlays drawn by the engine, the material-model view shades surfaces.
    CHECK(debugViewFromName("skeleton").value() == debugview::kSkeleton);
    CHECK(debugViewIsOverlay(debugview::kGroomRoots));
    CHECK_FALSE(debugViewIsOverlay(debugview::kSssMask));
    CHECK(debugViewOverridesSurfaces(debugview::kSssMask));
    CHECK(std::string(debugViewName(-1)) == "final");  // ids without a view
    // Workstream ids that sit next to each other: probes 24, vehicles 25, characters 26..29.
    CHECK(debugViewFromName("reflection_probes").value() == 24);
    CHECK(debugViewFromName("vehicles").value() == 25);
    CHECK(debugViewFromName("sss_mask").value() == 29);
    CHECK_FALSE(debugViewIsOverlay(debugview::kReflectionProbes));
}

TEST_CASE("debug views: unknown names fail with a did-you-mean hint") {
    auto r = debugViewFromName("wirefame");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "invalid_debug_view");
    CHECK(r.error().hint.find("wireframe") != std::string::npos);
    auto far = debugViewFromName("xyzzy-nothing");
    REQUIRE_FALSE(far.ok());
    CHECK(far.error().hint.find("valid views") != std::string::npos);
}

TEST_CASE("debug views: tools take every view, reject typos, and drive the live viewport") {
    auto e = makeEngine();
    // Schema enum lists every view.
    const ToolDef* capture = e->tools().find("viewport_capture");
    REQUIRE(capture);
    const Json& en = capture->inputSchema.get("properties").get("debug_view").get("enum");
    CHECK(en.size() == debugViews().size());
    // Captures with new views work on the null renderer (CPU fallback ignores the view).
    for (const char* v : {"wireframe", "overdraw", "lod", "texel_density", "skeleton", "groom_roots", "sss_mask"}) {
        std::string args = std::string(R"({"width":64,"height":36,"samples":1,"include_image":false,"debug_view":")") + v + "\"}";
        ToolResult r = call(*e, "viewport_capture", args.c_str());
        CHECK_FALSE(r.isError);
    }
    ToolResult bad = call(*e, "viewport_capture", R"({"width":64,"height":36,"debug_view":"overdrw"})");
    CHECK(bad.isError);
    CHECK(bad.content.front().text.find("overdraw") != std::string::npos);

    ToolResult set = call(*e, "viewport_debug_view", R"({"view":"light_complexity"})");
    REQUIRE_FALSE(set.isError);
    CHECK(set.structured.get("view").asString() == "light_complexity");
    CHECK(set.structured.get("legend").asString().find("lights") != std::string::npos);
    CHECK(e->viewportDebugView() == debugview::kLightComplexity);
    ToolResult list = call(*e, "viewport_debug_view", R"({"list":true})");
    CHECK(list.structured.get("views").size() == debugViews().size());
    CHECK(call(*e, "viewport_debug_view", R"({"view":"lodd"})").isError);
    call(*e, "viewport_debug_view", R"({"view":"final"})");
    CHECK(e->viewportDebugView() == 0);
}

TEST_CASE("perf_stats: passes=true adds the profile with CPU scopes and a warning off-GPU") {
    auto e = makeEngine();
    ToolResult r = call(*e, "perf_stats", R"({"frames":2,"width":64,"height":64,"passes":true})");
    REQUIRE_FALSE(r.isError);
    const Json& p = r.structured.get("profile");
    REQUIRE(p.isObject());
    CHECK(p.get("supported").asBool(true) == false);  // the null renderer has no GPU timeline
    bool built = false;
    for (const auto& s : p.get("cpu").elements()) built = built || s.get("scope").asString() == "frame.build";
    CHECK(built);
    REQUIRE(r.structured.get("warnings").size() >= 1);
    CHECK(r.structured.get("warnings")[0].asString().find("unavailable") != std::string::npos);
    // Without passes the profile stays out of the result.
    ToolResult plain = call(*e, "perf_stats", "{}");
    CHECK_FALSE(plain.structured.contains("profile"));
}

TEST_CASE("engine_info: reports the shader library section") {
    auto e = makeEngine();
    ToolResult r = call(*e, "engine_info", "{}");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("shaders").isObject());
}

// ---------------------------------------------------------------------------
// Shader cache bookkeeping
// ---------------------------------------------------------------------------

TEST_CASE("shader cache: keys are stable and change with every input") {
    using namespace shadercache;
    CHECK(fnv1a64("") == 14695981039346656037ULL);
    CHECK(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);  // reference FNV-1a 64 value
    CHECK(hex64(0x1234abcdULL) == "000000001234abcd");
    const std::string k = cacheKey("src", "0.1.0", "Apple M1", "macOS 15");
    CHECK(k.size() == 16);
    CHECK(k == cacheKey("src", "0.1.0", "Apple M1", "macOS 15"));
    CHECK(k != cacheKey("src2", "0.1.0", "Apple M1", "macOS 15"));
    CHECK(k != cacheKey("src", "0.2.0", "Apple M1", "macOS 15"));
    CHECK(k != cacheKey("src", "0.1.0", "Apple M2", "macOS 15"));
    CHECK(k != cacheKey("src", "0.1.0", "Apple M1", "macOS 26"));
    // Field boundaries matter.
    CHECK(cacheKey("ab", "c", "", "") != cacheKey("a", "bc", "", ""));
}

TEST_CASE("shader cache: directory, archive path and pruning") {
    using namespace shadercache;
    fs::path dir = fs::temp_directory_path() / "sky_shadercache_test";
    fs::remove_all(dir);
    {
        EnvGuard g("SKY_SHADER_CACHE_DIR", dir.string().c_str());
        CHECK(cacheDir() == dir.string());
    }
    {
        EnvGuard g("SKY_SHADER_CACHE_DIR", nullptr);
        CHECK(cacheDir().find("kywalker") != std::string::npos);
    }
    CHECK(archivePath(dir.string(), "abc") == (dir / "pipelines-abc.binarchive").string());
    CHECK(pruneArchives(dir.string(), "abc") == 0);  // no directory yet
    fs::create_directories(dir);
    auto touch = [&](const std::string& name, int ageSeconds) {
        fs::path p = dir / name;
        std::ofstream(p) << "x";
        fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
    };
    touch("pipelines-current.binarchive", 1000);
    for (int i = 0; i < 5; ++i) touch("pipelines-old" + std::to_string(i) + ".binarchive", 10 + i * 10);
    touch("notes.txt", 5000);
    CHECK(pruneArchives(dir.string(), "current", 3) == 2);
    CHECK(fs::exists(dir / "pipelines-current.binarchive"));  // the current key always stays
    CHECK(fs::exists(dir / "pipelines-old0.binarchive"));      // newest others stay
    CHECK(fs::exists(dir / "pipelines-old2.binarchive"));
    CHECK_FALSE(fs::exists(dir / "pipelines-old4.binarchive"));
    CHECK(fs::exists(dir / "notes.txt"));  // other files are left alone
    fs::remove_all(dir);
}

TEST_CASE("shader cache: environment switches") {
    using namespace shadercache;
    {
        EnvGuard a("SKY_SHADER_CACHE", nullptr), b("MTL_SHADER_VALIDATION", nullptr), c("MTL_DEBUG_LAYER", nullptr);
        CHECK(archiveEnabled());
        CHECK(archiveDisabledReason().empty());
    }
    {
        EnvGuard a("SKY_SHADER_CACHE", "0");
        CHECK_FALSE(archiveEnabled());
        CHECK(archiveDisabledReason().find("SKY_SHADER_CACHE") != std::string::npos);
    }
    {
        EnvGuard a("SKY_SHADER_CACHE", nullptr), b("MTL_SHADER_VALIDATION", "1");
        CHECK_FALSE(archiveEnabled());
        CHECK(archiveDisabledReason().find("validation") != std::string::npos);
    }
    {
        EnvGuard a("SKY_SHADER_SOURCE", "1");
        CHECK(forceSourceCompile());
    }
    {
        EnvGuard a("SKY_SHADER_SOURCE", nullptr);
        CHECK_FALSE(forceSourceCompile());
    }
}
