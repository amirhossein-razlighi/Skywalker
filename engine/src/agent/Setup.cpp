#include "skywalker/agent/Setup.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "skywalker/agent/Resources.h"
#include "skywalker/core/Strings.h"

namespace sky {

namespace fs = std::filesystem;

namespace {

constexpr const char* kServerName = "skywalker";

// ---------------------------------------------------------------------------------------------------------
// Text helpers

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string readText(const fs::path& path, bool& existed) {
    std::ifstream f(path, std::ios::binary);
    existed = static_cast<bool>(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string jsonString(const std::string& s) { return Json(s).dump(); }

std::string tomlArray(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) out += (i ? ", " : "") + jsonString(values[i]);
    return out + "]";
}

// ---------------------------------------------------------------------------------------------------------
// Where things go

fs::path scopeRoot(const SetupOptions& o) { return o.global ? o.homeDir : o.projectDir; }

std::vector<std::string> serverArgs(const SetupOptions& o) {
    if (o.mode == "attach") return {"mcp", "--attach"};
    if (o.mode == "headless") {
        if (o.global) return {"mcp"};
        return {"mcp", "--project", fs::absolute(o.projectDir).lexically_normal().string()};
    }
    return {"mcp", "--auto"};
}

std::string binaryPath(const SetupOptions& o) { return o.binary.empty() ? "skywalker" : o.binary.string(); }

/// Text between the managed markers of an embedded guidance file (markers included).
std::string extractBlock(std::string_view text) {
    size_t a = text.find(kManagedBegin);
    size_t b = text.find(kManagedEnd);
    if (a == std::string_view::npos || b == std::string_view::npos || b < a) return {};
    b += std::string_view(kManagedEnd).size();
    return std::string(text.substr(a, b - a)) + "\n";
}

std::string neutralContextBlock() {
    const auto* ctx = findEmbeddedAsset("integrations/skills-src/context.md");
    if (!ctx) return {};
    std::string body = ctx->content;
    while (!body.empty() && body.back() == '\n') body.pop_back();
    return std::string(kManagedBegin) + "\n" + body + "\n" + kManagedEnd + "\n";
}

void addFile(SetupPlan& plan, const fs::path& path, std::string after, std::string what) {
    SetupChange c;
    c.path = path;
    c.before = readText(path, c.existed);
    c.after = std::move(after);
    c.what = std::move(what);
    plan.changes.push_back(std::move(c));
}

void addTree(SetupPlan& plan, const fs::path& root, const std::string& srcPrefix, const fs::path& dstRel, const std::string& what) {
    for (const auto* a : embeddedAssetsUnder(srcPrefix)) {
        std::string rel = std::string(a->path).substr(srcPrefix.size());
        addFile(plan, root / dstRel / rel, a->content, what);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Merge primitives

Result<std::string> mergeJsonMcpServer(const std::string& existing, const std::string& command, const std::vector<std::string>& args,
                                       const std::vector<std::pair<std::string, Json>>& defaultsIfAbsent) {
    Json doc = Json::object();
    if (!trim(existing).empty()) {
        auto parsed = Json::parse(existing);
        if (!parsed) {
            return Error::make("invalid_json", "existing file is not valid JSON: " + parsed.error().message,
                               "fix or remove it, or add the entry by hand (skywalker setup <tool> --print shows it)");
        }
        doc = parsed.value();
        if (!doc.isObject()) return Error::make("invalid_json", "existing file is not a JSON object", "add the entry by hand (--print)");
    }
    Json& servers = doc["mcpServers"];
    if (servers.isNull()) servers = Json::object();
    if (!servers.isObject()) return Error::make("invalid_json", "'mcpServers' exists but is not an object", "fix it by hand");
    Json& entry = servers[kServerName];
    if (!entry.isObject()) entry = Json::object();
    entry["command"] = command;
    Json argList = Json::array();
    for (const auto& a : args) argList.push(a);
    entry["args"] = argList;
    for (const auto& [key, value] : defaultsIfAbsent) {
        if (!entry.contains(key)) entry[key] = value;
    }
    return doc.dump(2) + "\n";
}

std::string upsertTomlMcpServer(const std::string& existing, const std::string& command, const std::vector<std::string>& args) {
    const std::string header = std::string("[mcp_servers.") + kServerName + "]";
    const std::string quotedHeader = std::string("[mcp_servers.\"") + kServerName + "\"]";
    const std::string commandLine = "command = " + jsonString(command);
    const std::string argsLine = "args = " + tomlArray(args);
    std::vector<std::string> lines = splitLines(existing);

    size_t headerAt = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = trim(lines[i]);
        if (t == header || t == quotedHeader) {
            headerAt = i;
            break;
        }
    }
    if (headerAt == lines.size()) {
        std::string out = existing;
        if (!out.empty() && out.back() != '\n') out += '\n';
        if (!out.empty()) out += '\n';
        out += header + "\n" + commandLine + "\n" + argsLine + "\nstartup_timeout_sec = 30\ntool_timeout_sec = 600\n";
        return out;
    }

    // The table body runs until the next table header; other keys (env_vars, enabled, ...) and sub-tables stay as they are.
    size_t end = headerAt + 1;
    while (end < lines.size() && trim(lines[end]).rfind('[', 0) != 0) ++end;
    auto findKey = [&](const std::string& key) -> size_t {
        for (size_t i = headerAt + 1; i < end; ++i) {
            std::string t = trim(lines[i]);
            if (t.rfind(key, 0) == 0 && trim(t.substr(key.size())).rfind('=', 0) == 0) return i;
        }
        return lines.size();
    };
    auto replaceKey = [&](const std::string& key, const std::string& line) {
        size_t at = findKey(key);
        if (at == lines.size()) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(headerAt) + 1, line);
            ++end;
            return;
        }
        // A multi-line array value: drop its continuation lines.
        size_t last = at;
        if (lines[at].find('[') != std::string::npos && lines[at].find(']') == std::string::npos) {
            while (last + 1 < end && lines[last].find(']') == std::string::npos) ++last;
        }
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at) + 1, lines.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        end -= last - at;
        lines[at] = line;
    };
    // Insert in reverse so the final order after the header is command, args, timeouts.
    if (findKey("tool_timeout_sec") == lines.size()) replaceKey("tool_timeout_sec", "tool_timeout_sec = 600");
    if (findKey("startup_timeout_sec") == lines.size()) replaceKey("startup_timeout_sec", "startup_timeout_sec = 30");
    replaceKey("args", argsLine);
    replaceKey("command", commandLine);
    return joinLines(lines);
}

std::string upsertManagedBlock(const std::string& existing, const std::string& block) {
    size_t a = existing.find(kManagedBegin);
    size_t b = existing.find(kManagedEnd);
    if (a != std::string::npos && b != std::string::npos && b > a) {
        b += std::string_view(kManagedEnd).size();
        if (b < existing.size() && existing[b] == '\n') ++b;
        return existing.substr(0, a) + block + existing.substr(b);
    }
    std::string out = existing;
    if (!trim(out).empty()) {
        if (out.back() != '\n') out += '\n';
        out += '\n';
    } else {
        out.clear();
    }
    return out + block;
}

// ---------------------------------------------------------------------------------------------------------
// Diff

std::string textDiff(const std::string& before, const std::string& after) {
    if (before == after) return {};
    std::vector<std::string> a = splitLines(before), b = splitLines(after);
    const size_t n = a.size(), m = b.size();
    if (n * m > 4'000'000) return "  (large change: " + std::to_string(n) + " -> " + std::to_string(m) + " lines)\n";
    std::vector<std::vector<uint32_t>> lcs(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    }
    struct Op { char kind; const std::string* text; };
    std::vector<Op> ops;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) { ops.push_back({' ', &a[i]}); ++i; ++j; }
        else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) { ops.push_back({'+', &b[j]}); ++j; }
        else { ops.push_back({'-', &a[i]}); ++i; }
    }
    // Keep two lines of context around changes.
    std::vector<bool> keep(ops.size(), false);
    for (size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].kind == ' ') continue;
        for (size_t c = (k >= 2 ? k - 2 : 0); c < std::min(ops.size(), k + 3); ++c) keep[c] = true;
    }
    std::string out;
    bool skipped = false;
    for (size_t k = 0; k < ops.size(); ++k) {
        if (!keep[k]) { skipped = true; continue; }
        if (skipped) { out += "  ...\n"; skipped = false; }
        out += std::string(1, ops[k].kind) + " " + *ops[k].text + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Public API

fs::path currentExecutablePath() {
    std::error_code ec;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    return fs::weakly_canonical(fs::path(buf.c_str()), ec);
#elif defined(__linux__)
    char buf[4096];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return fs::weakly_canonical(fs::path(buf), ec);
#else
    return {};
#endif
}

const char* setupToolName(SetupTool tool) {
    switch (tool) {
        case SetupTool::Claude: return "claude";
        case SetupTool::Codex: return "codex";
        case SetupTool::Gemini: return "gemini";
        case SetupTool::Cursor: return "cursor";
    }
    return "";
}

Result<std::vector<SetupTool>> parseSetupTools(const std::string& name) {
    static const std::vector<std::string> names = {"claude", "codex", "gemini", "cursor", "all"};
    if (name == "all") return std::vector<SetupTool>{SetupTool::Claude, SetupTool::Codex, SetupTool::Gemini, SetupTool::Cursor};
    if (name == "claude" || name == "claude-code") return std::vector<SetupTool>{SetupTool::Claude};
    if (name == "codex") return std::vector<SetupTool>{SetupTool::Codex};
    if (name == "gemini") return std::vector<SetupTool>{SetupTool::Gemini};
    if (name == "cursor") return std::vector<SetupTool>{SetupTool::Cursor};
    std::string guess = str::closest(name, names);
    return Error::make("unknown_tool", "unknown tool '" + name + "'", guess.empty() ? "use claude, codex, gemini, cursor or all" : "did you mean '" + guess + "'?");
}

std::string setupSnippet(SetupTool tool, const SetupOptions& o) {
    const std::string cmd = binaryPath(o);
    const std::vector<std::string> args = serverArgs(o);
    auto jsonEntry = [&](std::vector<std::pair<std::string, Json>> extra) {
        auto merged = mergeJsonMcpServer("", cmd, args, extra);
        return merged ? merged.value() : std::string();
    };
    switch (tool) {
        case SetupTool::Claude: {
            std::string line = "claude mcp add --scope " + std::string(o.global ? "user" : "project") + " " + kServerName + " -- " + cmd;
            for (const auto& a : args) line += " " + a;
            return "# .mcp.json (project scope), or run:\n" + line + "\n" + jsonEntry({});
        }
        case SetupTool::Codex:
            return std::string(o.global ? "# ~/.codex/config.toml\n" : "# .codex/config.toml (trusted projects) or ~/.codex/config.toml\n") +
                   upsertTomlMcpServer("", cmd, args);
        case SetupTool::Gemini:
            return "# .gemini/settings.json or ~/.gemini/settings.json\n" + jsonEntry({{"timeout", Json(600000)}});
        case SetupTool::Cursor:
            return "# .cursor/mcp.json or ~/.cursor/mcp.json\n" + jsonEntry({});
    }
    return {};
}

Result<SetupPlan> planSetup(const SetupOptions& options) {
    SetupOptions o = options;
    if (o.homeDir.empty()) {
        const char* home = std::getenv("HOME");
        o.homeDir = home ? home : "";
    }
    if (o.global && o.homeDir.empty()) return Error::make("no_home", "cannot find the home directory", "set HOME or run without --global");
    if (o.mode != "auto" && o.mode != "attach" && o.mode != "headless") {
        return Error::make("bad_mode", "unknown mode '" + o.mode + "'", "use auto, attach or headless");
    }
    if (o.tools.empty()) return Error::make("no_tool", "no tool selected", "use claude, codex, gemini, cursor or all");
    if (embeddedAssets().empty()) return Error::make("no_assets", "this build has no embedded integrations", "rebuild from a source tree that contains integrations/");

    SetupPlan plan;
    const fs::path root = scopeRoot(o);
    const std::string cmd = binaryPath(o);
    const std::vector<std::string> args = serverArgs(o);
    std::vector<SetupTool> done;
    for (SetupTool tool : o.tools) {
        if (std::find(done.begin(), done.end(), tool) != done.end()) continue;
        done.push_back(tool);
        auto mergeJson = [&](const fs::path& path, const std::vector<std::pair<std::string, Json>>& defaults) -> Status {
            bool existed = false;
            std::string before = readText(path, existed);
            auto merged = mergeJsonMcpServer(before, cmd, args, defaults);
            if (!merged) return Error::make(merged.error().code, path.string() + ": " + merged.error().message, merged.error().hint);
            addFile(plan, path, merged.value(), "mcp server");
            return Status::success();
        };
        auto addGuidance = [&](const fs::path& path, const std::string& block) {
            if (block.empty()) return;
            bool existed = false;
            addFile(plan, path, upsertManagedBlock(readText(path, existed), block), "guidance block");
        };

        switch (tool) {
            case SetupTool::Claude: {
                if (o.global) {
                    // ~/.claude.json is owned and rewritten by Claude Code itself: use its CLI instead of editing it.
                    std::vector<std::string> argv = {"claude", "mcp", "add", "--scope", "user", kServerName, "--", cmd};
                    argv.insert(argv.end(), args.begin(), args.end());
                    plan.commands.push_back(std::move(argv));
                } else if (Status s = mergeJson(root / ".mcp.json", {}); !s) {
                    return s.error();
                } else {
                    plan.notes.push_back("Claude Code asks you to approve a project .mcp.json server the first time (or run `claude mcp list`).");
                }
                if (o.skills) {
                    addTree(plan, root, "integrations/claude-code/skills/", ".claude/skills", "skill");
                    addTree(plan, root, "integrations/claude-code/agents/", ".claude/agents", "subagent");
                    addTree(plan, root, "integrations/claude-code/commands/", ".claude/commands", "command");
                    addGuidance(o.global ? root / ".claude" / "CLAUDE.md" : root / "CLAUDE.md", neutralContextBlock());
                    plan.notes.push_back("Claude Code: if you also install the plugin (/plugin marketplace add), skip these copies with --no-skills.");
                }
                break;
            }
            case SetupTool::Codex: {
                const fs::path config = root / ".codex" / "config.toml";
                bool existed = false;
                std::string before = readText(config, existed);
                addFile(plan, config, upsertTomlMcpServer(before, cmd, args), "mcp server");
                if (!o.global) plan.notes.push_back("Codex reads a project's .codex/config.toml only for trusted projects; use --global to put the server in ~/.codex/config.toml.");
                if (o.skills) {
                    addTree(plan, root, "integrations/claude-code/skills/", ".agents/skills", "skill");
                    addTree(plan, root, "integrations/codex/.agents/skills/", ".agents/skills", "workflow skill");
                    addTree(plan, root, "integrations/codex/.codex/agents/", ".codex/agents", "custom agent");
                    const auto* agents = findEmbeddedAsset("integrations/codex/AGENTS.md");
                    addGuidance(o.global ? root / ".codex" / "AGENTS.md" : root / "AGENTS.md", agents ? extractBlock(agents->content) : std::string());
                }
                break;
            }
            case SetupTool::Gemini: {
                if (Status s = mergeJson(root / ".gemini" / "settings.json", {{"timeout", Json(600000)}}); !s) return s.error();
                if (o.skills) {
                    addTree(plan, root, "integrations/claude-code/skills/", ".gemini/skills", "skill");
                    addTree(plan, root, "integrations/gemini/agents/", ".gemini/agents", "subagent");
                    addTree(plan, root, "integrations/gemini/commands/", ".gemini/commands", "command");
                    const auto* ctx = findEmbeddedAsset("integrations/gemini/GEMINI.md");
                    addGuidance(o.global ? root / ".gemini" / "GEMINI.md" : root / "GEMINI.md", ctx ? extractBlock(ctx->content) : std::string());
                }
                break;
            }
            case SetupTool::Cursor: {
                if (Status s = mergeJson(root / ".cursor" / "mcp.json", {}); !s) return s.error();
                if (o.skills) {
                    addTree(plan, root, "integrations/claude-code/skills/", ".cursor/skills", "skill");
                    addTree(plan, root, "integrations/cursor/.cursor/agents/", ".cursor/agents", "subagent");
                    addTree(plan, root, "integrations/cursor/.cursor/commands/", ".cursor/commands", "command");
                    if (!o.global) addTree(plan, root, "integrations/cursor/.cursor/rules/", ".cursor/rules", "rule");
                }
                plan.notes.push_back("Cursor: restart it (or toggle the server in Settings > MCP) to pick up the new server.");
                break;
            }
        }
    }
    return plan;
}

Result<SetupApplied> applySetup(const SetupPlan& plan, const SetupOptions& options) {
    SetupApplied applied;
    fs::path root = options.global ? options.homeDir : options.projectDir;
    if (root.empty()) root = options.global ? fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") : fs::path(".");
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path backupDir = root / ".skywalker" / "setup-backups" / stamp;

    for (const auto& c : plan.changes) {
        if (!c.changes()) { ++applied.unchanged; continue; }
        std::error_code ec;
        if (c.existed) {
            std::error_code relEc;
            fs::path rel = fs::relative(c.path, root, relEc);
            if (relEc || rel.empty() || *rel.begin() == "..") rel = c.path.relative_path();
            fs::path dest = backupDir / rel;
            fs::create_directories(dest.parent_path(), ec);
            std::ofstream bak(dest, std::ios::binary);
            bak << c.before;
            if (!bak) return Error::make("backup_failed", "cannot back up " + c.path.string() + " to " + dest.string(), "nothing was changed for this file");
            applied.backupDir = backupDir;
        }
        fs::create_directories(c.path.parent_path(), ec);
        if (ec) return Error::make("write_failed", "cannot create " + c.path.parent_path().string() + ": " + ec.message());
        fs::path tmp = c.path;
        tmp += ".skywalker-tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f << c.after;
            if (!f) return Error::make("write_failed", "cannot write " + tmp.string());
        }
        fs::rename(tmp, c.path, ec);
        if (ec) return Error::make("write_failed", "cannot replace " + c.path.string() + ": " + ec.message());
        ++applied.written;
    }
    return applied;
}

}  // namespace sky
