// `skywalker serve`: a headless engine that serves the agent socket, exactly like the editor does.
//
//   skywalker serve [--project DIR] [--scene FILE] [--socket PATH] [--lifeline] [--quiet]
//
// Every client attaches the same way it attaches to the editor (`skywalker mcp --attach PATH`,
// the Python agent layer, `skywalker call ... --attach --socket PATH`), so several agents share one
// live engine: their edits are attributed, they can follow the event stream (events_poll) and
// serve tools to each other (tool_host_*). The main loop pumps agent jobs and advances play mode
// in real time. Prints one JSON line when ready: {"event":"ready","socket":...,"pid":...}.
// --lifeline exits when stdin closes (the parent process died); SIGINT/SIGTERM exit cleanly.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "skywalker/core/Log.h"
#include "skywalker/engine/Engine.h"

using namespace sky;

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

std::string option(const std::vector<std::string>& raw, const std::string& flag, const std::string& fallback = "") {
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] == flag) return raw[i + 1];
    }
    return fallback;
}

bool hasFlag(const std::vector<std::string>& raw, const std::string& flag) {
    for (const auto& r : raw) {
        if (r == flag) return true;
    }
    return false;
}

/// A short default socket path per project (Unix socket paths are limited to ~104 bytes).
std::string defaultServeSocket(const std::string& project) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string abs = fs::weakly_canonical(fs::absolute(project, ec), ec).string();
    size_t h = std::hash<std::string>{}(abs);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "serve-%08zx.sock", h & 0xffffffffu);
    return (fs::path(Engine::defaultSocketPath()).parent_path() / buf).string();
}

/// True when stdin reached EOF (the parent closed its end).
bool stdinClosed() {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (::poll(&p, 1, 0) <= 0) return false;
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return true;
    if (p.revents & POLLIN) {
        char buf[256];
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        return n == 0;
    }
    return false;
}

}  // namespace

int runServe(const std::vector<std::string>& raw) {
    if (hasFlag(raw, "--help") || hasFlag(raw, "-h")) {
        std::printf(
            "usage: skywalker serve [--project DIR] [--scene FILE] [--socket PATH] [--lifeline] [--quiet]\n"
            "  A headless engine serving the agent socket (like the editor). Attach with\n"
            "  `skywalker mcp --attach PATH`, the Python agent layer (sky-agents), or `call --attach --socket PATH`.\n");
        return 0;
    }
    log::setMinLevel(hasFlag(raw, "--quiet") ? LogLevel::Error : LogLevel::Warn);
    EngineConfig cfg;
    cfg.projectDir = option(raw, "--project", ".");
    cfg.audio = audio::AudioMode::Null;
    Engine engine(cfg);
    std::string scene = option(raw, "--scene");
    if (scene.empty()) {
        std::error_code ec;
        // Scene paths resolve against the project (like `skywalker call --scene`).
        if (std::filesystem::exists(std::filesystem::path(cfg.projectDir) / "scenes" / "main.sky.json", ec)) {
            scene = "scenes/main.sky.json";
        }
    }
    if (!scene.empty()) {
        if (Status s = engine.loadScene(scene); !s) {
            std::fprintf(stderr, "skywalker serve: %s\n", s.error().message.c_str());
            return 1;
        }
    } else {
        (void)engine.newScene("Untitled", true);
    }
    std::string socket = option(raw, "--socket", defaultServeSocket(cfg.projectDir));
    if (Status s = engine.startAgentServer(socket); !s) {
        std::fprintf(stderr, "skywalker serve: %s\n", s.error().message.c_str());
        if (!s.error().hint.empty()) std::fprintf(stderr, "  hint: %s\n", s.error().hint.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    bool lifeline = hasFlag(raw, "--lifeline");
    Json ready = Json::object({{"event", "ready"},
                               {"socket", socket},
                               {"pid", static_cast<int64_t>(::getpid())},
                               {"project", cfg.projectDir},
                               {"scene", engine.scenePath()},
                               {"version", SKY_VERSION_STRING}});
    std::printf("%s\n", ready.dump().c_str());
    std::fflush(stdout);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    while (!gStop) {
        auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        engine.update(std::min(dt, 0.25));  // pumps agent jobs; advances play mode in real time
        (void)engine.drainEvents();       // agents follow the event log (events_poll) instead
        if (lifeline && stdinClosed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(engine.playState() == PlayState::Playing ? 4 : 2));
    }
    engine.stopAgentServer();
    std::fprintf(stderr, "skywalker serve: stopped\n");
    return 0;
}
