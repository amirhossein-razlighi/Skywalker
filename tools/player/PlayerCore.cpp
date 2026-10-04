#include "PlayerCore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "skywalker/core/Strings.h"
#include "skywalker/game/SaveGame.h"
#include "skywalker/locale/Localization.h"
#include "skywalker/ui/World2D.h"
#include "skywalker/native/NativeModules.h"
#include "skywalker/render/Image.h"

namespace sky::player {

namespace fs = std::filesystem;

const char* usageText() {
    return "skywalker-player — runs a Skywalker game\n\n"
           "usage: skywalker-player [PROJECT_DIR | Game.app] [options]\n"
           "  (inside a built Game.app it runs that game; no arguments needed)\n\n"
           "  --scene FILE            start scene (project-relative), default game.json startScene\n"
           "  --windowed | --fullscreen\n"
           "  --width N --height N    window content size in points\n"
           "  --no-vsync              allow tearing for lowest latency\n"
           "  --quality low|medium|high|ultra   --render-scale 0.33..1\n"
           "  --quit-on-escape        Escape closes the game\n"
           "  --no-audio              silent\n"
           "  --check [TICKS]         headless validation: load, play TICKS ticks (default 120), render a test frame, print JSON\n"
           "  --capture-frame PNG     render one frame of the real player pipeline to PNG, then quit  (--frames N, default 90)\n"
           "  --display-hz N          automated runs (capture, quit-after): frames per second, 30..240 (default 60;\n"
           "                          120 shows render interpolation between the 60 Hz ticks)\n"
           "  --quit-after SECONDS    close after this long\n"
           "  --agent-socket PATH     serve MCP on a Unix socket so agents can inspect the running game (development)\n"
           "  --verbose               engine log\n"
           "  --version, --help\n";
}

namespace {

Error bad(const std::string& message) { return Error::make("invalid_arguments", message, "see skywalker-player --help"); }

}  // namespace

Result<Options> parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* flag) -> Result<std::string> {
            if (i + 1 >= argc) return bad(std::string(flag) + " needs a value");
            return std::string(argv[++i]);
        };
        auto number = [&](const char* flag) -> Result<double> {
            auto v = value(flag);
            if (!v) return v.error();
            double d = 0;
            if (!str::parseDouble(*v, d)) return bad(std::string(flag) + " needs a number, got '" + *v + "'");
            return d;
        };
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--version") o.version = true;
        else if (a == "--windowed") o.fullscreen = false;
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--no-vsync") o.vsync = false;
        else if (a == "--vsync") o.vsync = true;
        else if (a == "--quit-on-escape") o.quitOnEscape = true;
        else if (a == "--no-audio") o.noAudio = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--check") {
            o.check = true;
            if (i + 1 < argc && std::atoi(argv[i + 1]) > 0) o.checkTicks = std::atoi(argv[++i]);
        } else if (a == "--scene") {
            auto v = value("--scene");
            if (!v) return v.error();
            o.scene = *v;
        } else if (a == "--quality") {
            auto v = value("--quality");
            if (!v) return v.error();
            o.quality = str::lower(*v);
            const auto& presets = game::GameSettings::qualityPresets();
            if (std::find(presets.begin(), presets.end(), o.quality) == presets.end()) return bad("--quality must be low, medium, high or ultra");
        } else if (a == "--render-scale") {
            auto v = number("--render-scale");
            if (!v) return v.error();
            if (*v < 0.33 || *v > 1.0) return bad("--render-scale must be between 0.33 and 1");
            o.renderScale = static_cast<float>(*v);
        } else if (a == "--width" || a == "--height" || a == "--frames") {
            auto v = number(a.c_str());
            if (!v) return v.error();
            if (*v < 1 || *v > 16384) return bad(a + " must be between 1 and 16384");
            if (a == "--width") o.width = static_cast<int>(*v);
            else if (a == "--height") o.height = static_cast<int>(*v);
            else o.frames = static_cast<int>(*v);
        } else if (a == "--display-hz") {
            auto v = number("--display-hz");
            if (!v) return v.error();
            if (*v < 30 || *v > 240) return bad("--display-hz must be between 30 and 240");
            o.displayHz = *v;
        } else if (a == "--quit-after") {
            auto v = number("--quit-after");
            if (!v) return v.error();
            o.quitAfter = *v;
        } else if (a == "--capture-frame") {
            auto v = value("--capture-frame");
            if (!v) return v.error();
            o.capture = *v;
        } else if (a == "--agent-socket") {
            auto v = value("--agent-socket");
            if (!v) return v.error();
            o.agentSocket = *v;
        } else if (a.rfind("-psn_", 0) == 0) {
            // Launch Services passes a process serial number to double-clicked apps on older systems.
        } else if (a.rfind("-", 0) == 0) {
            return bad("unknown option " + a);
        } else if (o.target.empty()) {
            o.target = a;
        } else {
            return bad("more than one project given: '" + o.target + "' and '" + a + "'");
        }
    }
    return o;
}

Result<std::unique_ptr<Session>> openSession(const Options& options, const std::string& executable, audio::AudioMode audio) {
    auto session = std::make_unique<Session>();
    if (!options.target.empty()) {
        auto loc = game::locateGame(options.target);
        if (!loc) return loc.error();
        session->location = *loc;
    } else {
        session->location = game::locateBundledGame(executable);
        if (session->location.projectDir.empty()) {
            return Error::make("no_game", "no game to run",
                               "pass a project folder or a built .app: skywalker-player path/to/project");
        }
    }
    const std::string& dir = session->location.projectDir;

    auto settings = game::GameSettings::load(dir);
    if (!settings) return settings.error();
    session->settings = *settings;
    game::GameSettings& s = session->settings;
    if (options.fullscreen) s.window.fullscreen = *options.fullscreen;
    if (options.vsync) s.window.vsync = *options.vsync;
    if (options.width) s.window.width = *options.width;
    if (options.height) s.window.height = *options.height;
    if (!options.quality.empty()) s.quality = options.quality;
    if (options.renderScale > 0.f) s.renderScale = options.renderScale;
    if (options.quitOnEscape) s.quitOnEscape = true;

    auto scene = game::resolveStartScene(dir, s, options.scene);
    if (!scene) return scene.error();
    session->scene = *scene;

    EngineConfig cfg;
    cfg.projectDir = dir;
    cfg.audio = options.noAudio ? audio::AudioMode::Off : audio;
    session->engine = std::make_unique<Engine>(cfg);
    Engine& engine = *session->engine;

    // A built app ships its native module precompiled: the player never invokes a compiler.
    const std::string nativeLib = session->location.manifest.get("native").asString();
    if (session->location.bundled && !nativeLib.empty()) {
        Status n = engine.native().usePrebuilt((fs::path(session->location.appPath) / "Contents" / nativeLib).string());
        if (!n) return Error::make(n.error().code, "native module: " + n.error().message, n.error().hint);
    }
    if (Status loaded = engine.loadScene(session->scene); !loaded) {
        return Error::make(loaded.error().code, "cannot load the scene " + session->scene + ": " + loaded.error().message, loaded.error().hint);
    }
    s.applyQuality(engine.scene().environment());
    // Save games: a shipped app keeps them in the user's data folder; a project run from its folder keeps
    // them in <project>/.skywalker/saves like the editor (docs/SAVE_GAMES.md).
    if (session->location.bundled) engine.saves().setDirectory(game::userSaveDir(s.id.empty() ? game::slugify(s.displayName()) : s.id));
    // Localization: the player's language (game.json localization.useSystemLocale decides whether it wins).
    engine.world2d().localization().setSystemLocale(loc::systemLocale());
    return session;
}

int runCheck(const Options& options, const std::string& executable) {
    Json report = Json::object();
    Json problems = Json::array();
    auto finish = [&](bool ok) {
        report["ok"] = ok;
        report["problems"] = problems;
        std::printf("%s\n", report.dump(2).c_str());
        return ok ? 0 : 1;
    };

    auto session = openSession(options, executable, audio::AudioMode::Null);
    if (!session) {
        problems.push(session.error().message + (session.error().hint.empty() ? "" : " (" + session.error().hint + ")"));
        return finish(false);
    }
    Session& s = **session;
    Engine& engine = *s.engine;
    report["name"] = s.settings.displayName();
    report["bundled"] = s.location.bundled;
    report["start_scene"] = s.scene;
    report["entities"] = engine.scene().size();

    if (s.location.bundled) {
        game::BundleCheck bundle = game::verifyBundle(s.location);
        report["bundle"] = bundle.toJson();
        for (const auto& p : bundle.problems) problems.push("bundle: " + p);
    }

    engine.play();
    engine.step(options.checkTicks);
    report["ticks"] = options.checkTicks;
    report["sim_seconds"] = engine.runtime().time();
    Json logs = Json::array();
    for (const auto& m : engine.recentMessages(500)) {
        const std::string kind = m.get("kind").asString();
        if (kind == "log" && logs.size() < 20) logs.push(m.get("text"));
        if (kind == "runtime_error" || kind == "compile_error") {
            problems.push(kind + ": " + m.get("text").asString());
        }
    }
    report["logs"] = logs;
    report["audio"] = audio::toString(engine.audio().mode());

    // One small frame through the renderer proves the GPU path and shaders work on this machine.
    CaptureOptions co;
    co.width = 160;
    co.height = 90;
    co.samples = 1;
    co.useSceneCamera = true;
    co.editorOverlays = false;
    auto frame = engine.capture(co);
    Json rendering = Json::object({{"backend", engine.renderer().info().backend}, {"device", engine.renderer().info().device}});
    if (!frame) {
        rendering["ok"] = false;
        problems.push("render: " + frame.error().message);
    } else {
        // A frame of a single flat color means nothing drew (broken scene, camera inside a wall, failed shaders).
        const Image& img = frame->image;
        uint32_t lo = 255, hi = 0;
        for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
            uint32_t luma = (img.pixels[i] * 54u + img.pixels[i + 1] * 183u + img.pixels[i + 2] * 19u) >> 8;
            lo = std::min(lo, luma);
            hi = std::max(hi, luma);
        }
        rendering["ok"] = true;
        rendering["contrast"] = hi - lo;
        rendering["visible_entities"] = frame->visible.size();
    }
    report["render"] = rendering;
    engine.stop();
    return finish(problems.size() == 0);
}

}  // namespace sky::player
