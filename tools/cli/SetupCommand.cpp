// `skywalker setup <claude|codex|gemini|cursor|all>`: install the MCP server entry, skills, subagents and
// guidance for an agent tool. The work happens in sky::planSetup / sky::applySetup (engine/src/agent/Setup.cpp).

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "skywalker/agent/Setup.h"

extern char** environ;

namespace {

namespace fs = std::filesystem;

int setupUsage() {
    std::fprintf(stderr,
                 "usage: skywalker setup <claude|codex|gemini|cursor|all> [options]\n\n"
                 "Installs the MCP server entry (pointing at this binary), the skywalker-* skills, studio-role subagents,\n"
                 "workflow commands and a guidance block for the tool. Existing configs are merged, never replaced;\n"
                 "files that change are backed up under <root>/.skywalker/setup-backups/.\n\n"
                 "options:\n"
                 "  --project DIR   project to configure (default: current directory)\n"
                 "  --global        configure your user-level config (~/.claude, ~/.codex, ~/.gemini, ~/.cursor) instead\n"
                 "  --dry-run       show what would change (with diffs); write nothing\n"
                 "  --print         print the MCP entry to paste by hand; write nothing\n"
                 "  --no-skills     only the MCP server entry\n"
                 "  --mode MODE     auto (default: editor if running, else headless), attach, or headless\n"
                 "  --binary PATH   path to write for the skywalker executable (default: this binary)\n"
                 "  --home DIR      use DIR as the home directory for --global (for tests); external commands are printed, not run\n"
                 "  --verbose       list every file instead of a summary\n");
    return 2;
}

bool hasFlag(const std::vector<std::string>& raw, const std::string& flag) {
    for (const auto& r : raw) {
        if (r == flag) return true;
    }
    return false;
}

std::string flagValue(const std::vector<std::string>& raw, const std::string& flag, const std::string& fallback = "") {
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] == flag) return raw[i + 1];
    }
    return fallback;
}

bool onPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string all = path;
    size_t start = 0;
    while (start <= all.size()) {
        size_t colon = all.find(':', start);
        std::string dir = all.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        std::error_code ec;
        if (!dir.empty() && fs::exists(fs::path(dir) / program, ec)) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

int runCommand(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string commandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& a : argv) {
        bool plain = a.find_first_of(" \t'\"\\$") == std::string::npos && !a.empty();
        out += (out.empty() ? "" : " ") + (plain ? a : "'" + a + "'");
    }
    return out;
}

}  // namespace

int runSetup(const std::vector<std::string>& raw) {
    // raw[0] == "setup"; the tool name is the first non-flag argument after it.
    std::string toolName;
    for (size_t i = 1; i < raw.size(); ++i) {
        if (raw[i].rfind("-", 0) == 0) {
            if (raw[i] == "--project" || raw[i] == "--mode" || raw[i] == "--binary" || raw[i] == "--home") ++i;
            continue;
        }
        toolName = raw[i];
        break;
    }
    if (toolName.empty() || hasFlag(raw, "--help") || hasFlag(raw, "-h")) return setupUsage();

    auto tools = sky::parseSetupTools(toolName);
    if (!tools) {
        std::fprintf(stderr, "error: %s\n  hint: %s\n", tools.error().message.c_str(), tools.error().hint.c_str());
        return 2;
    }
    sky::SetupOptions options;
    options.tools = tools.value();
    options.projectDir = flagValue(raw, "--project", ".");
    options.global = hasFlag(raw, "--global");
    options.skills = !hasFlag(raw, "--no-skills");
    options.mode = flagValue(raw, "--mode", "auto");
    options.homeDir = flagValue(raw, "--home");
    std::string binary = flagValue(raw, "--binary");
    options.binary = binary.empty() ? sky::currentExecutablePath() : fs::absolute(binary);
    if (!options.global) {
        std::error_code ec;
        options.projectDir = fs::absolute(options.projectDir, ec).lexically_normal();
        if (!fs::is_directory(options.projectDir, ec)) {
            std::fprintf(stderr, "error: project directory '%s' does not exist\n", options.projectDir.string().c_str());
            return 1;
        }
    }
    const bool dryRun = hasFlag(raw, "--dry-run");
    const bool verbose = hasFlag(raw, "--verbose");

    if (hasFlag(raw, "--print")) {
        for (sky::SetupTool tool : options.tools) std::printf("=== %s ===\n%s\n", sky::setupToolName(tool), sky::setupSnippet(tool, options).c_str());
        return 0;
    }

    auto plan = sky::planSetup(options);
    if (!plan) {
        std::fprintf(stderr, "error: %s\n", plan.error().message.c_str());
        if (!plan.error().hint.empty()) std::fprintf(stderr, "  hint: %s\n", plan.error().hint.c_str());
        return 1;
    }

    std::printf("skywalker setup %s (%s scope%s)\n", toolName.c_str(), options.global ? "user" : "project", dryRun ? ", dry run" : "");
    std::printf("  server: %s mcp (%s)\n", options.binary.empty() ? "skywalker" : options.binary.string().c_str(), options.mode.c_str());
    int created = 0, updated = 0, same = 0;
    for (const auto& c : plan->changes) {
        const bool detailed = verbose || c.what == "mcp server" || c.what == "guidance block";
        if (!c.changes()) { ++same; }
        else if (c.existed) { ++updated; }
        else { ++created; }
        if (!detailed) continue;
        std::printf("  [%s] %s  (%s)\n", !c.changes() ? "same  " : (c.existed ? "update" : "create"), c.path.string().c_str(), c.what.c_str());
        if (dryRun && c.changes()) {
            std::string diff = sky::textDiff(c.before, c.after);
            // Cap long diffs (a new guidance file or a rewritten config) so the summary stays readable.
            size_t lines = 0, cut = 0;
            for (size_t i = 0; i < diff.size() && cut == 0; ++i) {
                if (diff[i] == '\n' && ++lines == 40) cut = i + 1;
            }
            if (cut != 0 && cut < diff.size()) {
                std::printf("%s    ... (%zu more diff lines)\n", diff.substr(0, cut).c_str(), std::count(diff.begin() + static_cast<std::ptrdiff_t>(cut), diff.end(), '\n'));
            } else if (!diff.empty()) {
                std::printf("%s", diff.c_str());
            }
        }
    }
    std::printf("  files: %d to create, %d to update, %d unchanged\n", created, updated, same);
    for (const auto& argv : plan->commands) std::printf("  command: %s\n", commandLine(argv).c_str());

    if (!dryRun) {
        auto applied = sky::applySetup(plan.value(), options);
        if (!applied) {
            std::fprintf(stderr, "error: %s\n", applied.error().message.c_str());
            if (!applied.error().hint.empty()) std::fprintf(stderr, "  hint: %s\n", applied.error().hint.c_str());
            return 1;
        }
        std::printf("  wrote %d file(s), %d already up to date\n", applied->written, applied->unchanged);
        if (!applied->backupDir.empty()) std::printf("  backups of replaced files: %s\n", applied->backupDir.string().c_str());
        for (const auto& argv : plan->commands) {
            if (!options.homeDir.empty()) {
                std::printf("  run this yourself:\n    %s\n", commandLine(argv).c_str());
                continue;
            }
            if (!onPath(argv[0])) {
                std::printf("  '%s' is not on PATH; run this yourself:\n    %s\n", argv[0].c_str(), commandLine(argv).c_str());
                continue;
            }
            int code = runCommand(argv);
            if (code != 0) {
                std::printf("  `%s` exited with %d (an existing 'skywalker' server? check `claude mcp list`)\n", commandLine(argv).c_str(), code);
            } else {
                std::printf("  ran: %s\n", commandLine(argv).c_str());
            }
        }
    }
    for (const auto& note : plan->notes) std::printf("  note: %s\n", note.c_str());
    if (dryRun) std::printf("  (dry run: nothing was written)\n");
    return 0;
}
