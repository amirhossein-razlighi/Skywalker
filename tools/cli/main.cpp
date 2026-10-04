// `skywalker` — headless command-line front end.
//
//   skywalker mcp [--project DIR] [--scene FILE]     MCP server on stdio (headless engine)
//   skywalker mcp --attach [SOCKET]                  MCP stdio bridge to a running editor
//   skywalker mcp --auto [--project DIR]             bridge to the editor if it runs, else headless
//   skywalker setup <claude|codex|gemini|cursor|all> install MCP config, skills, subagents (SetupCommand.cpp)
//   skywalker render SCENE -o out.png [--width W --height H --annotate --scene-camera]
//   skywalker run SCENE [--ticks N] [-o out.png]     simulate deterministically, print logs
//   skywalker check FILE.wander [--project DIR] [--disassemble] [--format]
//                                                    compile Wander, print diagnostics
//   skywalker call TOOL [JSON] [--scene FILE]        call one tool, print the result
//   skywalker tools [--markdown|--json]              list tools
//   skywalker studio status|agents|board|feedback|loops|run --project DIR ...   (StudioCommand.cpp)
//   skywalker build --project DIR --out DIR [--name N --icon F --release --all-assets]   package a macOS app (BuildCommand.cpp)
//   skywalker movie SCENE [JSON] -o out.mp4 [...]    render a cinematic to video / PNG frames (MovieCommand.cpp)
//   skywalker legal [terms|privacy|license|licensing] [--accept] [--status] [--json]   terms, privacy notice, license
//   skywalker version

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "skywalker/agent/McpServer.h"
#include "skywalker/agent/SocketServer.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/legal/Legal.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/Parser.h"
#include "skywalker/wander/Runtime.h"

using namespace sky;

int runStudio(const std::vector<std::string>& raw);  // StudioCommand.cpp
int runSetup(const std::vector<std::string>& raw);   // SetupCommand.cpp
int runBuild(const std::vector<std::string>& raw);   // BuildCommand.cpp
int runMovie(const std::vector<std::string>& raw);   // MovieCommand.cpp

namespace {

struct Args {
    std::vector<std::string> positional;
    std::string get(const std::string& flag, const std::string& fallback = "") const {
        for (size_t i = 0; i + 1 < raw.size(); ++i) {
            if (raw[i] == flag) return raw[i + 1];
        }
        return fallback;
    }
    bool has(const std::string& flag) const {
        for (const auto& r : raw) {
            if (r == flag) return true;
        }
        return false;
    }
    std::vector<std::string> raw;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) a.raw.emplace_back(argv[i]);
    for (size_t i = 0; i < a.raw.size(); ++i) {
        const std::string& r = a.raw[i];
        if (r.rfind("-", 0) == 0) {
            bool takesValue = r == "--project" || r == "--mode" || r == "--scene" || r == "-o" || r == "--width" || r == "--height" ||
                              r == "--ticks" || r == "--as" || r == "--socket";
            if (takesValue) ++i;
            continue;
        }
        a.positional.push_back(r);
    }
    return a;
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path);
    ok = static_cast<bool>(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int usage() {
    std::fprintf(stderr,
                 "skywalker %s — a game engine built for AI agents\n\n"
                 "usage:\n"
                 "  skywalker mcp [--project DIR] [--scene FILE]   MCP server on stdio\n"
                 "  skywalker mcp --attach [SOCKET]                bridge to a running editor\n"
                 "  skywalker mcp --auto [--project DIR]           bridge to the editor if it runs, else headless\n"
                 "  skywalker setup <claude|codex|gemini|cursor|all> [--project DIR] [--global] [--dry-run] [--print] [--no-skills]\n"
                 "                                                 install the MCP server entry, skills, subagents and commands\n"
                 "  skywalker render SCENE -o out.png [--width W] [--height H] [--annotate] [--scene-camera] [--samples N]\n"
                 "  skywalker run SCENE [--ticks N] [-o out.png]\n"
                 "  skywalker check FILE.wander [--project DIR] [--disassemble] [--format]\n"
                 "  skywalker call TOOL [JSON] [--scene FILE] [--project DIR] [-o image.png]\n"
                 "  skywalker call TOOL [JSON] --attach [--as NAME] [--socket PATH]   (on the running editor)\n"
                 "  skywalker tools [--markdown|--json]\n"
                 "  skywalker studio status|agents|board|feedback|loops --project DIR\n"
                 "  skywalker studio run --project DIR --loop NAME [--iterations N] [--dry-run] [--yes]\n"
                 "  skywalker build --project DIR --out DIR [--name N] [--icon F.png] [--release] [--all-assets]   package a macOS app\n"
                 "  skywalker movie SCENE [JSON] -o out.mp4 [--sequence S] [--resolution 1080p] [--fps N] [--samples N]\n"
                 "                    [--shutter F] [--simulate] [--clay|--sketch] [--resume] ...   render a cinematic (movie --help)\n"
                 "  skywalker legal [terms|privacy|license|licensing] [--accept] [--status] [--json]\n"
                 "                                                 Terms of Use, Privacy Notice, license; --accept records acceptance\n"
                 "  skywalker version\n",
                 SKY_VERSION_STRING);
    return 2;
}

std::unique_ptr<Engine> makeEngine(const Args& args) {
    EngineConfig cfg;
    cfg.projectDir = args.get("--project", ".");
    auto engine = std::make_unique<Engine>(cfg);
    std::string scene = args.get("--scene");
    if (!scene.empty()) {
        if (Status s = engine->loadScene(scene); !s) log::error("cli", s.error().message);
    } else {
        (void)engine->newScene("Untitled", true);
    }
    return engine;
}

/// Pipes the MCP stdio session to a running editor over its Unix socket (until either side closes).
int bridgeToEditor(int sock) {
    std::thread downstream([sock] {
        LineReader reader(sock);
        std::string line;
        while (reader.next(line)) {
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
    });
    LineReader in(STDIN_FILENO);
    std::string line;
    while (in.next(line)) {
        if (!writeAll(sock, line + "\n")) break;
    }
    ::shutdown(sock, SHUT_RDWR);
    downstream.join();
    return 0;
}

int runMcp(const Args& args) {
    log::setMinLevel(LogLevel::Warn);  // stdout is the protocol channel; logs go to stderr only
    if (args.has("--attach")) {
        std::string path = Engine::defaultSocketPath();
        for (size_t i = 0; i + 1 < args.raw.size(); ++i) {
            if (args.raw[i] == "--attach" && args.raw[i + 1].rfind("-", 0) != 0) path = args.raw[i + 1];
        }
        auto fd = connectUnixSocket(path);
        if (!fd) {
            std::fprintf(stderr, "skywalker: %s\n  hint: %s\n", fd.error().message.c_str(), fd.error().hint.c_str());
            return 1;
        }
        return bridgeToEditor(fd->get());
    }
    if (args.has("--auto")) {
        // Best of both: co-edit with the running editor when there is one, otherwise run a headless engine
        // on --project (default: the current directory) so agent clients never face a dead server.
        if (auto fd = connectUnixSocket(args.get("--socket", Engine::defaultSocketPath()))) {
            std::fprintf(stderr, "skywalker: attached to the running editor\n");
            return bridgeToEditor(fd->get());
        }
        std::fprintf(stderr, "skywalker: no editor is running, starting a headless engine on '%s'\n", args.get("--project", ".").c_str());
    }

    auto engine = makeEngine(args);
    McpSession session(engine->tools(), [&](const std::string& tool, const Json& a, const std::string& actor) {
        return engine->callTool(tool, a, actor).toMcp();
    });
    LineReader in(STDIN_FILENO);
    std::string line;
    while (in.next(line)) {
        if (line.empty()) continue;
        engine->pump();
        if (auto response = session.handle(line)) {
            std::fwrite(response->data(), 1, response->size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        (void)engine->drainEvents();  // no UI to consume them in headless mode
    }
    return 0;
}

/// The project a scene file belongs to: the nearest folder above it with a game.json, or the parent
/// of its scenes/ folder; "." when neither is found (then paths resolve from the current directory).
std::string projectOfScene(const std::string& scene) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(scene, ec).parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i, dir = dir.parent_path()) {
        if (fs::exists(dir / "game.json", ec)) return dir.string();
        if (dir.filename() == "scenes") return dir.parent_path().string();
        if (dir == dir.parent_path()) break;
    }
    return ".";
}

int runRender(const Args& args, bool simulate) {
    if (args.positional.size() < 2) return usage();
    EngineConfig cfg;
    // Without --project, the scene's own project: art, scripts and sequences resolve as in the editor.
    std::string scene = args.positional[1];
    cfg.projectDir = args.has("--project") ? args.get("--project", ".") : projectOfScene(scene);
    if (!args.has("--project") && cfg.projectDir != ".") scene = std::filesystem::absolute(scene).string();
    Engine engine(cfg);
    if (Status s = engine.loadScene(scene); !s) {
        std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
        return 1;
    }
    if (simulate) {
        int ticks = std::stoi(args.get("--ticks", "60"));
        engine.step(ticks);
        for (const auto& m : engine.recentMessages(1000)) std::printf("%s\n", m.dump().c_str());
        std::printf("simulated %d ticks (%.2fs)\n", ticks, engine.runtime().time());
    }
    std::string out = args.get("-o");
    if (out.empty()) return 0;
    CaptureOptions o;
    o.width = std::stoi(args.get("--width", "1280"));
    o.height = std::stoi(args.get("--height", "720"));
    o.annotate = args.has("--annotate");
    o.useSceneCamera = args.has("--scene-camera");
    o.samples = std::stoi(args.get("--samples", "8"));
    if (!o.useSceneCamera) engine.callTool("camera_set", Json::object({{"frame", "all"}}), "cli");
    auto cap = engine.capture(o);
    if (!cap) {
        std::fprintf(stderr, "error: %s\n", cap.error().message.c_str());
        return 1;
    }
    if (Status s = writePng(cap->image, out); !s) {
        std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d, %zu visible entities, renderer %s)\n", out.c_str(), o.width, o.height,
                cap->visible.size(), engine.renderer().info().backend.c_str());
    return 0;
}

int runCheck(const Args& args) {
    if (args.positional.size() < 2) return usage();
    bool ok = false;
    std::string src = readFile(args.positional[1], ok);
    if (!ok) {
        std::fprintf(stderr, "error: cannot read %s\n", args.positional[1].c_str());
        return 1;
    }
    // Compile exactly like the engine does: every component, engine builtins, and `use`
    // modules from the project folder.
    registerEngineBuiltins();
    Scene scene;
    wander::Runtime rt(scene);
    rt.setProjectDir(args.get("--project", "."));
    auto r = wander::compile(src, rt.compileOptions());
    for (const auto& d : r.diagnostics) {
        std::string file = d.file.empty() ? args.positional[1] : d.file;
        std::printf("%s:%d:%d: %s: %s [%s]%s%s\n", file.c_str(), d.loc.line, d.loc.column,
                    d.severity == wander::Severity::Error ? "error" : "warning", d.message.c_str(), d.code.c_str(),
                    d.hint.empty() ? "" : "\n  hint: ", d.hint.c_str());
    }
    if (args.has("--format") && r.module) std::printf("%s", wander::format(*r.module).c_str());
    if (r.ok() && args.has("--disassemble")) std::printf("%s", r.program->disassemble().c_str());
    if (r.ok()) {
        std::printf("ok: %zu behavior(s), %zu instructions\n", r.program->behaviors.size(), r.program->instructionCount());
    }
    return r.ok() ? 0 : 1;
}

/// `call ... -o FILE`: writes the first image a tool returned (previews, captures, multi-views).
bool saveFirstImage(const Args& args, const std::string& base64) {
    std::string out = args.get("-o");
    if (out.empty() || base64.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!str::base64Decode(base64, bytes)) return false;
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (f) std::printf("wrote %s\n", out.c_str());
    return static_cast<bool>(f);
}

/// `call --attach`: one tool call against the running editor over its MCP socket, as a
/// named client (shows up as `mcp:<name>` in history and the activity feed).
int runCallAttached(const Args& args, const std::string& tool, const Json& toolArgs) {
    auto fd = connectUnixSocket(args.get("--socket", Engine::defaultSocketPath()));
    if (!fd) {
        std::fprintf(stderr, "error: %s\n", fd.error().message.c_str());
        return 1;
    }
    std::string who = args.get("--as", "cli");
    Json init = Json::object({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                              {"params", Json::object({{"protocolVersion", McpSession::latestProtocolVersion()},
                                                       {"clientInfo", Json::object({{"name", who}, {"version", "1"}})}})}});
    Json call = Json::object({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                              {"params", Json::object({{"name", tool}, {"arguments", toolArgs}})}});
    if (!writeAll(fd->get(), init.dump() + "\n" + call.dump() + "\n")) return 1;
    LineReader reader(fd->get());
    std::string line;
    while (reader.next(line)) {
        auto msg = Json::parse(line);
        if (!msg || msg->get("id").asInt() != 2) continue;
        if (msg->contains("error")) {
            std::printf("%s\n", msg->get("error").get("message").asString().c_str());
            return 1;
        }
        const Json& result = msg->get("result");
        for (const auto& c : result.get("content").elements()) {
            if (c.get("type").asString() == "text") std::printf("%s\n", c.get("text").asString().c_str());
            else if (!saveFirstImage(args, c.get("data").asString())) std::printf("[image %zu base64 bytes]\n", c.get("data").asString().size());
        }
        return result.get("isError").asBool() ? 1 : 0;
    }
    return 1;
}

int runCall(const Args& args) {
    if (args.positional.size() < 2) return usage();
    if (args.has("--attach")) {
        Json a = Json::object();
        if (args.positional.size() > 2) {
            auto parsed = Json::parse(args.positional[2]);
            if (!parsed) {
                std::fprintf(stderr, "error: %s\n", parsed.error().message.c_str());
                return 1;
            }
            a = parsed.value();
        }
        return runCallAttached(args, args.positional[1], a);
    }
    auto engine = makeEngine(args);
    if (!args.get("--scene").empty() && engine->scenePath().empty()) return 1;  // load error already reported
    Json a = Json::object();
    if (args.positional.size() > 2) {
        auto parsed = Json::parse(args.positional[2]);
        if (!parsed) {
            std::fprintf(stderr, "error: %s\n", parsed.error().message.c_str());
            return 1;
        }
        a = parsed.value();
    }
    ToolResult r = engine->callTool(args.positional[1], a, "cli");
    for (const auto& c : r.content) {
        if (c.type == ContentBlock::Type::Text) std::printf("%s\n", c.text.c_str());
        else if (!saveFirstImage(args, c.data)) std::printf("[image %s, %zu base64 bytes]\n", c.mimeType.c_str(), c.data.size());
    }
    return r.isError ? 1 : 0;
}

int runTools(const Args& args) {
    Engine engine;
    if (args.has("--json")) {
        std::printf("%s\n", engine.tools().listJson().dump(1).c_str());
        return 0;
    }
    if (args.has("--markdown")) {
        std::printf("%s", engine.tools().catalogueMarkdown().c_str());
        return 0;
    }
    for (const auto& t : engine.tools().all()) std::printf("%-20s %s\n", t.name.c_str(), t.title.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);
    if (args.positional.empty()) return usage();
    const std::string& cmd = args.positional[0];
    if (cmd != "mcp") log::setMinLevel(LogLevel::Warn);
    if (cmd == "mcp") return runMcp(args);
    if (cmd == "render") return runRender(args, false);
    if (cmd == "run") return runRender(args, true);
    if (cmd == "check") return runCheck(args);
    if (cmd == "call") return runCall(args);
    if (cmd == "tools") return runTools(args);
    if (cmd == "studio") return runStudio(args.raw);
    if (cmd == "setup") return runSetup(args.raw);
    if (cmd == "build") return runBuild(args.raw);
    if (cmd == "movie") return runMovie(args.raw);
    if (cmd == "legal") {
        std::string out, err;
        int code = legal::runCommand({args.raw.begin() + 1, args.raw.end()}, legal::defaultRecordPath(), out, err);
        std::fputs(out.c_str(), stdout);
        std::fputs(err.c_str(), stderr);
        return code;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("skywalker %s\n", SKY_VERSION_STRING);
        return 0;
    }
    return usage();
}
