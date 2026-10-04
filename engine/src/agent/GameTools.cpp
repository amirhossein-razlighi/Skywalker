// Game tools: ship the project as a macOS app (game_build), try it in the standalone player (game_run) and
// edit its shipping settings (game_settings). See docs/SHIPPING.md.
//
// Building writes outside the project and running starts a process, so both are "open world" tools: MCP clients
// and the in-editor crew ask the human first.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/game/Packager.h"

extern char** environ;

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

/// The player processes started by game_run (killed when the engine goes away).
class PlayerRuns {
public:
    struct Run {
        pid_t pid = 0;
        std::string log;
        std::vector<std::string> argv;
        std::chrono::steady_clock::time_point started;
        bool exited = false;
        int status = 0;
    };

    ~PlayerRuns() {
        for (auto& r : runs_) {
            if (!reap(r)) {
                ::kill(r.pid, SIGKILL);
                ::waitpid(r.pid, nullptr, 0);
            }
        }
    }

    Run& add(Run r) {
        runs_.push_back(std::move(r));
        return runs_.back();
    }
    Run* find(pid_t pid) {
        for (auto& r : runs_) {
            if (r.pid == pid) return &r;
        }
        return nullptr;
    }
    Run* last() { return runs_.empty() ? nullptr : &runs_.back(); }

    /// Non-blocking: true once the process has exited.
    static bool reap(Run& r) {
        if (r.exited) return true;
        int st = 0;
        pid_t w = ::waitpid(r.pid, &st, WNOHANG);
        if (w == r.pid || (w < 0 && errno == ECHILD)) {
            r.exited = true;
            r.status = st;
        }
        return r.exited;
    }

    /// Waits up to `seconds` for the process to exit.
    static bool waitFor(Run& r, double seconds) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!reap(r)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

private:
    std::vector<Run> runs_;
};

std::string tail(const std::string& path, size_t maxBytes = 3000) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = static_cast<size_t>(f.tellg());
    size_t n = std::min(size, maxBytes);
    f.seekg(static_cast<std::streamoff>(size - n));
    std::string text(n, '\0');
    f.read(text.data(), static_cast<std::streamsize>(n));
    return text;
}

Json runJson(PlayerRuns::Run& r) {
    bool done = PlayerRuns::reap(r);
    Json j = Json::object({{"pid", static_cast<int64_t>(r.pid)},
                           {"running", !done},
                           {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - r.started).count()},
                           {"log_file", r.log}});
    if (done) {
        if (WIFEXITED(r.status)) j["exit_code"] = WEXITSTATUS(r.status);
        else if (WIFSIGNALED(r.status)) j["signal"] = WTERMSIG(r.status);
    }
    j["log_tail"] = tail(r.log);
    return j;
}

std::string absPath(const std::string& p) {
    std::error_code ec;
    return fs::absolute(p, ec).lexically_normal().string();
}

}  // namespace

void addGameTools(Engine& engine, ToolRegistry& reg) {
    auto runs = std::make_shared<PlayerRuns>();

    {
        ToolDef def{
            "game_settings", "Game settings (game.json)",
            "Read or change the project's shipping settings in game.json: startScene, window {width,height,fullscreen,resizable,vsync}, "
            "quality (low|medium|high|ultra), renderScale, quitOnEscape, pauseOnFocusLoss, icon (a PNG, 1024x1024 recommended), "
            "bundleId (reverse-DNS), version, copyright, plus include/exclude globs for packaging, and the description fields "
            "(id, title, genre, mood, pitch), and mounts: shared folders outside the project addressed by a top-level name, e.g. "
            "{\"mounts\": {\"kit\": \"../_kit\"}} makes kit/characters/guard.prefab.json resolve into ../_kit (assets, prefabs, materials "
            "and animations load from it; game_build copies what the game uses). `get` returns the effective settings, the resolved start scene and validation "
            "problems. `set` merges the given fields (null removes one) and validates them. Example: {\"operation\":\"set\","
            "\"settings\":{\"title\":\"Sky Dash\",\"startScene\":\"scenes/main.sky.json\",\"window\":{\"width\":1280,\"height\":720}}}",
            "files",
            object({{"operation", enumeration({"get", "set"}, "get (default) or set")},
                    {"settings", Json::object({{"type", "object"}, {"description", "Fields to merge into game.json (set only)"}})}}),
            true, false,
            [&engine](const Json& a, ToolContext&) {
                const std::string dir = engine.config().projectDir;
                if (a.get("operation").asString("get") == "set") {
                    if (!a.get("settings").isObject()) return ToolResult::error(Error::make("invalid_arguments", "set needs a \"settings\" object"));
                    // Merge into the file as written (not the expanded defaults), so game.json stays minimal.
                    Json current = Json::object();
                    fs::path file = fs::path(dir) / "game.json";
                    std::error_code ec;
                    if (fs::is_regular_file(file, ec)) {
                        std::ifstream f(file);
                        std::stringstream ss;
                        ss << f.rdbuf();
                        auto doc = Json::parse(ss.str());
                        if (!doc) return ToolResult::error(Error::make("invalid_game_json", "game.json: " + doc.error().message));
                        current = *doc;
                    }
                    current.mergePatch(a.get("settings"));
                    auto parsed = game::GameSettings::fromJson(current);
                    if (!parsed) return ToolResult::error(parsed.error());
                    fs::create_directories(dir, ec);
                    std::ofstream out(file);
                    out << current.dump(2) << "\n";
                    if (!out) return ToolResult::error(Error::make("io_error", "cannot write " + file.string()));
                    out.close();
                    if (a.get("settings").contains("mounts")) {
                        engine.reloadMounts();
                        engine.refreshAssets();
                    }
                }
                auto settings = game::GameSettings::load(dir);
                if (!settings) return ToolResult::error(settings.error());
                Json j = Json::object({{"settings", settings->toJson()}, {"from_file", settings->fromFile}});
                if (auto scene = game::resolveStartScene(dir, *settings)) j["start_scene"] = *scene;
                else j["start_scene_problem"] = scene.error().message;
                j["bundle_id"] = settings->effectiveBundleId();
                if (!engine.assets().mounts().empty()) {
                    Json m = Json::object();
                    for (const auto& mt : engine.assets().mounts()) m[mt.name] = mt.root;
                    j["mounted"] = m;
                }
                return ToolResult::json(j, settings->fromFile ? "game.json is valid" : "no game.json yet: showing the defaults");
            }};
        def.openWorld = false;
        reg.add(std::move(def));
    }

    {
        ToolDef def{
            "game_build", "Build the game as a macOS app",
            "Package the project as a standalone macOS app (Name.app): the player runtime, the scenes, the assets they reference "
            "(computed from scenes, prefabs, materials and Wander scripts; all_assets ships everything), scripts, the project's native "
            "module (compiled now), Info.plist, an icon made from the game.json icon, and an ad-hoc code signature. Settings come from "
            "game.json (see game_settings). Returns the app path, size, file count by type, and warnings (missing assets, no icon). "
            "dry_run reports what would ship without writing anything. The app runs on this Mac; distributing it needs a Developer ID "
            "signature and notarization (docs/SHIPPING.md). Example: {\"out\":\"/Users/me/Builds\",\"release\":true}",
            "files",
            object({{"out", string("Folder to write <Name>.app into (outside the project is best; ~/Desktop, ~/Builds)")},
                    {"name", string("App name (default: game.json title)")},
                    {"icon", string("PNG path (absolute or project-relative); overrides game.json icon")},
                    {"version", string("Game version like 1.2.0; overrides game.json")},
                    {"bundle_id", string("Reverse-DNS bundle identifier; overrides game.json")},
                    {"scene", string("Start scene override (project-relative)")},
                    {"release", boolean("Strip the player's symbols for a smaller app (default false)")},
                    {"all_assets", boolean("Ship every asset in the project, not only referenced ones (use when scripts build paths at run time)")},
                    {"sign", boolean("Ad-hoc code sign (default true)")},
                    {"build_native", boolean("Compile and ship native/*.cpp (default true)")},
                    {"dry_run", boolean("Only list what would ship")}},
                   {}),
            true, false,
            [&engine](const Json& a, ToolContext&) {
                const std::string project = engine.config().projectDir;
                if (a.get("dry_run").asBool()) {
                    auto settings = game::GameSettings::load(project);
                    if (!settings) return ToolResult::error(settings.error());
                    game::CollectOptions co;
                    co.allAssets = a.get("all_assets").asBool();
                    co.startScene = a.get("scene").asString();
                    auto files = game::collectGameFiles(project, *settings, co);
                    if (!files) return ToolResult::error(files.error());
                    Json j = files->toJson();
                    Json list = Json::array();
                    for (const auto& f : files->files) {
                        if (list.size() < 300) list.push(f);
                    }
                    j["list"] = list;
                    return ToolResult::json(j, std::to_string(files->files.size()) + " files would ship (" +
                                                   std::to_string(files->bytes / 1024) + " KB)");
                }
                if (a.get("out").asString().empty()) {
                    return ToolResult::error(Error::make("invalid_arguments", "\"out\" is required", "e.g. {\"out\":\"~/Builds\"}"));
                }
                game::PackageOptions o;
                o.projectDir = project;
                o.outDir = engine.resolvePath(a.get("out").asString());
                o.name = a.get("name").asString();
                o.icon = a.get("icon").asString();
                o.version = a.get("version").asString();
                o.bundleId = a.get("bundle_id").asString();
                o.scene = a.get("scene").asString();
                o.release = a.get("release").asBool();
                o.allAssets = a.get("all_assets").asBool();
                o.sign = a.get("sign").asBool(true);
                o.buildNative = a.get("build_native").asBool(true);
                o.host = &engine;
                auto report = game::buildGame(o);
                if (!report) return ToolResult::error(report.error());
                std::string summary = "built " + report->app + " (" + std::to_string(report->appBytes / 1024) + " KB, " +
                                      std::to_string(report->files) + " game files, " + report->signature + " signature)";
                if (!report->warnings.empty()) summary += "; " + std::to_string(report->warnings.size()) + " warning(s)";
                return ToolResult::json(report->toJson(), summary);
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    {
        ToolDef def{
            "game_run", "Run the game in the standalone player",
            "Start the standalone player on the current project as a separate process to try the game like a player would "
            "(real window, real input, audio). It runs the saved scene file (save first); without `scene` it uses the open scene or "
            "game.json's start scene. Returns the pid and a log file. `status` shows whether it is still running and its recent "
            "output; `stop` ends it. With `capture` the player renders one frame to a PNG after `frames` frames and exits "
            "(a screenshot of the real player output). `seconds` closes the game by itself. `app` runs a built .app instead. "
            "Example: {\"action\":\"start\",\"scene\":\"scenes/level2.sky.json\",\"width\":1280} or "
            "{\"capture\":\"/tmp/shot.png\",\"frames\":120}",
            "files",
            object({{"action", enumeration({"start", "stop", "status"}, "start (default), stop, or status")},
                    {"scene", string("Scene to start in (project-relative); default: the open scene")},
                    {"app", string("Run this built .app instead of the project")},
                    {"pid", integer("stop/status: which player (default: the latest)")},
                    {"fullscreen", boolean("Start fullscreen")},
                    {"width", integer("Window width in points")},
                    {"height", integer("Window height in points")},
                    {"quality", enumeration({"low", "medium", "high", "ultra"}, "Quality preset override")},
                    {"seconds", number("Quit after this many seconds")},
                    {"capture", string("Write one frame to this PNG and quit")},
                    {"frames", integer("With capture: frames to run before the capture (default 90)")},
                    {"wait", boolean("Wait for the player to exit and return its output (default true with capture/seconds)")}}),
            true, false,
            [&engine, runs](const Json& a, ToolContext&) {
                const std::string action = a.get("action").asString("start");
                if (action == "status" || action == "stop") {
                    PlayerRuns::Run* run = a.get("pid").isNumber() ? runs->find(static_cast<pid_t>(a.get("pid").asInt())) : runs->last();
                    if (!run) return ToolResult::error(Error::make("not_found", "no player was started by game_run", "start one with action \"start\""));
                    if (action == "stop" && !PlayerRuns::reap(*run)) {
                        ::kill(run->pid, SIGTERM);  // the player quits cleanly on SIGTERM
                        if (!PlayerRuns::waitFor(*run, 3.0)) {
                            ::kill(run->pid, SIGKILL);
                            PlayerRuns::waitFor(*run, 2.0);
                        }
                    }
                    Json j = runJson(*run);
                    return ToolResult::json(j, j.get("running").asBool() ? "player " + std::to_string(run->pid) + " is running" : "player has exited");
                }
                if (action != "start") return ToolResult::error(Error::make("invalid_arguments", "action must be start, stop or status"));

                std::vector<std::string> argv;
                std::string project = absPath(engine.config().projectDir);
                if (!a.get("app").asString().empty()) {
                    auto loc = game::locateGame(engine.resolvePath(a.get("app").asString()));
                    if (!loc) return ToolResult::error(loc.error());
                    if (!loc->bundled) return ToolResult::error(Error::make("invalid_arguments", "app must be a built .app"));
                    std::string exe = loc->manifest.get("executable").asString();
                    argv = {(fs::path(loc->appPath) / "Contents" / "MacOS" / exe).string()};
                } else {
                    std::string player = game::findPlayerBinary();
                    if (player.empty()) {
                        return ToolResult::error(Error::make("no_player", "the player executable (skywalker-player) was not found",
                                                             "build it (cmake --build build/release) or set SKYWALKER_PLAYER"));
                    }
                    argv = {player, project};
                    std::string scene = a.get("scene").asString();
                    if (scene.empty()) scene = engine.scenePath();
                    if (!scene.empty()) {
                        // The player wants a project-relative scene.
                        std::error_code ec;
                        fs::path rel = fs::relative(engine.resolvePath(scene), project, ec);
                        argv.insert(argv.end(), {"--scene", ec || rel.empty() ? scene : rel.generic_string()});
                    }
                }
                if (a.get("fullscreen").asBool()) argv.push_back("--fullscreen");
                if (a.get("width").isNumber()) argv.insert(argv.end(), {"--width", std::to_string(a.get("width").asInt())});
                if (a.get("height").isNumber()) argv.insert(argv.end(), {"--height", std::to_string(a.get("height").asInt())});
                if (!a.get("quality").asString().empty()) argv.insert(argv.end(), {"--quality", a.get("quality").asString()});
                if (a.get("seconds").isNumber()) argv.insert(argv.end(), {"--quit-after", std::to_string(a.get("seconds").asNumber())});
                const std::string capture = a.get("capture").asString();
                if (!capture.empty()) {
                    argv.insert(argv.end(), {"--capture-frame", absPath(engine.resolvePath(capture))});
                    if (a.get("frames").isNumber()) argv.insert(argv.end(), {"--frames", std::to_string(a.get("frames").asInt())});
                }

                fs::path logDir = fs::path(project) / ".skywalker" / "logs";
                std::error_code ec;
                fs::create_directories(logDir, ec);
                PlayerRuns::Run run;
                run.argv = argv;
                run.log = (logDir / ("player-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                    std::chrono::system_clock::now().time_since_epoch()).count()) + ".log")).string();
                posix_spawn_file_actions_t fa;
                posix_spawn_file_actions_init(&fa);
                posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
                posix_spawn_file_actions_addopen(&fa, 1, run.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                posix_spawn_file_actions_adddup2(&fa, 1, 2);
                std::vector<char*> cargv;
                for (auto& s : argv) cargv.push_back(s.data());
                cargv.push_back(nullptr);
                pid_t pid = 0;
                int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, cargv.data(), environ);
                posix_spawn_file_actions_destroy(&fa);
                if (rc != 0) {
                    return ToolResult::error(Error::make("spawn_failed", "cannot start " + argv[0] + ": " + std::strerror(rc)));
                }
                run.pid = pid;
                run.started = std::chrono::steady_clock::now();
                PlayerRuns::Run& stored = runs->add(std::move(run));
                bool wait = a.get("wait").asBool(!capture.empty() || a.get("seconds").isNumber());
                if (wait) {
                    double limit = a.get("seconds").isNumber() ? a.get("seconds").asNumber() + 30 : 90;
                    if (!PlayerRuns::waitFor(stored, limit)) {
                        ::kill(stored.pid, SIGTERM);
                        PlayerRuns::waitFor(stored, 3.0);
                    }
                }
                Json j = runJson(stored);
                if (!capture.empty()) j["capture"] = absPath(engine.resolvePath(capture));
                bool failed = stored.exited && (!WIFEXITED(stored.status) || WEXITSTATUS(stored.status) != 0);
                ToolResult r = ToolResult::json(j, stored.exited ? (failed ? "the player exited with an error" : "the player has exited")
                                                                 : "started the player, pid " + std::to_string(stored.pid));
                r.isError = failed;
                return r;
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }
}

}  // namespace sky::tools
