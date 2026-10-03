#pragma once
// `skywalker setup`: connect Claude Code, Codex, Gemini CLI and Cursor to this engine.
//
// For each tool it installs (1) the MCP server entry pointing at this binary, (2) the skywalker-* skills,
// subagents (studio roles) and commands in the tool's own layout, and (3) a managed block of project
// guidance (CLAUDE.md / AGENTS.md / GEMINI.md / a Cursor rule). The content comes from the files embedded
// in the binary (see Resources.h), so no source tree is needed.
//
// Safety: existing configs are MERGED, never replaced (JSON keys, TOML tables and markdown outside our
// markers are preserved), invalid existing files are refused, and every file that changes is backed up
// first. planSetup() computes the changes without touching the disk (for --dry-run/--print and tests);
// applySetup() writes them.

#include <filesystem>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

enum class SetupTool { Claude, Codex, Gemini, Cursor };

struct SetupOptions {
    std::vector<SetupTool> tools;
    std::filesystem::path projectDir = ".";  // project scope root
    std::filesystem::path homeDir;           // user scope root (default: $HOME)
    std::filesystem::path binary;            // absolute path of the skywalker executable written into the configs
    bool global = false;                     // user scope instead of project scope
    bool skills = true;                      // false: only the MCP server entry
    std::string mode = "auto";               // auto (editor if running, else headless) | attach | headless
};

struct SetupChange {
    std::filesystem::path path;
    std::string before;
    std::string after;
    bool existed = false;
    std::string what;  // "mcp server", "skill skywalker-core", ...
    bool changes() const { return !existed || before != after; }
};

struct SetupPlan {
    std::vector<SetupChange> changes;
    /// Commands the user (or the CLI) must run because the tool owns its config (argv vectors).
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> notes;
};

struct SetupApplied {
    int written = 0;
    int unchanged = 0;
    std::filesystem::path backupDir;  // empty when nothing needed a backup
};

/// "claude", "codex", "gemini", "cursor" (or "all" -> every tool).
Result<std::vector<SetupTool>> parseSetupTools(const std::string& name);
const char* setupToolName(SetupTool tool);

Result<SetupPlan> planSetup(const SetupOptions& options);
Result<SetupApplied> applySetup(const SetupPlan& plan, const SetupOptions& options);

/// The MCP entry for one tool as text to paste by hand (`skywalker setup <tool> --print`).
std::string setupSnippet(SetupTool tool, const SetupOptions& options);

/// Line diff of two texts in unified-ish form (empty when equal).
std::string textDiff(const std::string& before, const std::string& after);

/// Absolute path of the running executable ("" if unknown).
std::filesystem::path currentExecutablePath();

// Config-merging primitives (exposed for tests).
Result<std::string> mergeJsonMcpServer(const std::string& existing, const std::string& command, const std::vector<std::string>& args,
                                       const std::vector<std::pair<std::string, Json>>& defaultsIfAbsent = {});
std::string upsertTomlMcpServer(const std::string& existing, const std::string& command, const std::vector<std::string>& args);
std::string upsertManagedBlock(const std::string& existing, const std::string& block);

}  // namespace sky
