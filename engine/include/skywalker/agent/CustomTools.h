#pragma once
// Custom tools: tools that AI agents define for themselves, at runtime, on top of the engine's own.
//
// Three kinds share one registration path (the ToolRegistry, so they appear in tools/list, MCP
// list_changed notifications, `skywalker tools --json` and the crew's tool lists):
//
//   wander     a Wander function `fn run(args) ... end`, run in-process by the VM with an
//              instruction budget; it reads the scene and calls allowlisted tools (call_tool)
//   composite  a declarative pipeline of tool calls with templated arguments ({{args.x}},
//              {{steps.find.entities}}), for_each loops and a result mapping
//   external   hosted by a connected client (the Python agent layer, another process) over MCP;
//              the engine forwards calls to it and enforces what it may call back into
//
// Definitions are project assets: tools/<name>.tool.json (+ tools/<name>.wander). Safety model:
//   * capabilities are an explicit allowlist (read the scene, mutate it, call these tools, read or
//     write these project paths, network); every call runs with the caller's permissions
//     intersected with them (least privilege)
//   * limits bound instructions, wall time, output size and nested calls; nesting depth is capped
//   * mutations go through Engine::edit (one undo step, attributed to the caller); a tool that
//     edits the scene without the mutate capability is rolled back and fails
//   * the project policy (game.json "customTools": {"policy": off|ask|auto|trust}) decides which
//     tools need a human's approval (tool_approve); approval is bound to the definition's hash
//   * every lifecycle event and call is logged (.skywalker/logs/custom_tools.jsonl) and shown in
//     the activity feed
// See docs/CUSTOM_TOOLS.md.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/agent/ExternalHost.h"
#include "skywalker/agent/ToolHost.h"
#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

class Engine;

namespace wander {
class BuiltinRegistry;
class Runtime;
struct Program;
}  // namespace wander

/// Who may register what (game.json "customTools": {"policy": ...}). Strictest first.
enum class ToolPolicy {
    Off,    // custom and external tools are disabled
    Ask,    // every tool needs a human's approval
    Auto,   // read-only tools are approved automatically; tools that mutate, write files or reach the network need approval (default)
    Trust,  // everything is approved automatically (trusted single-user setups, CI)
};
const char* toString(ToolPolicy p);
std::optional<ToolPolicy> parseToolPolicy(std::string_view s);

struct ToolCapabilities {
    bool readScene = true;               // query entities, components, vars
    bool mutate = false;                 // edit the scene (directly or through mutating tools)
    std::vector<std::string> calls;      // tools it may call; "entity_*" globs allowed
    std::vector<std::string> readPaths;  // project-relative files/folders it may read ("data/", "levels/*.json")
    std::vector<std::string> writePaths; // ... and write
    bool network = false;                // reach outside the project (external tools only)

    Json toJson() const;
};

struct ToolLimits {
    int64_t instructions = 1'000'000;  // Wander instruction budget per call
    int timeoutMs = 5'000;             // wall time (external tools default to 30 s)
    int64_t maxOutputBytes = 64 * 1024;
    int maxCalls = 100;                // nested tool calls per call

    Json toJson() const;
};

/// A parsed, validated tool definition.
struct CustomToolDef {
    std::string name;  // user_<x> (wander, composite) or <namespace>_<x> (external)
    std::string title;
    std::string description;
    std::string category = "custom";
    std::string kind;  // wander | composite | external
    Json inputSchema;
    Json outputSchema;  // optional
    ToolCapabilities caps;
    ToolLimits limits;
    std::string source;      // Wander source (kind wander)
    std::string sourceFile;  // project-relative file holding it ("tools/x.wander"); empty = inline
    Json steps;              // composite pipeline
    Json result;             // composite result mapping
    Json tests = Json::array();
    bool enabled = true;
    int version = 1;
    std::string author;  // actor that first defined it
    Json provenance = Json::object();
    bool persist = false;  // external: keep the definition (offline) when the host disconnects

    /// The file format (tools/<name>.tool.json).
    Json toJson() const;
    /// Whether the tool can change anything (scene, files) or reach outside the project.
    bool privileged() const;
    /// Hash of everything that decides what the tool does (approval is bound to it).
    std::string hash() const;
};

class CustomTools {
public:
    explicit CustomTools(Engine& engine);
    ~CustomTools();
    CustomTools(const CustomTools&) = delete;
    CustomTools& operator=(const CustomTools&) = delete;

    // --- Lifecycle (the tool_* tools) ----------------------------------------------------------
    /// Creates or updates a tool: validates the definition (schema, capabilities, limits, code),
    /// compiles it, runs its tests, saves it and registers it if the policy allows.
    Result<Json> define(const Json& spec, const std::string& actor);
    /// Runs a tool's tests, or one ad-hoc call ("args"), as a dry run: scene edits are rolled back
    /// and file writes skipped. `spec` may hold an unsaved definition instead of a name.
    Result<Json> test(const Json& request, const std::string& actor);
    Json list(bool includeLibrary, bool reload);
    Result<Json> inspect(const std::string& name) const;
    Result<Json> remove(const std::string& name, bool deleteFiles, const std::string& actor);
    /// Approves (or rejects) the current definition of a tool. Humans only ("user", "cli").
    Result<Json> approve(const std::string& name, bool approve, const std::string& note, const std::string& actor);
    Result<Json> setEnabled(const std::string& name, bool enabled, const std::string& actor);
    /// Copies a project tool into the user library (~/.skywalker/tools) for other projects.
    Result<Json> promote(const std::string& name, const std::string& actor);
    ToolPolicy policy() const;
    /// Changes the policy. Loosening it is reserved to humans; anyone may tighten it.
    Result<Json> setPolicy(ToolPolicy policy, const std::string& actor);
    /// Rescans tools/ (definitions edited by hand). Returns what changed.
    Json reload();

    // --- External hosts (MCP connections) ----------------------------------------------------
    /// `skywalker/tools/register | unregister | list` from a client. Main thread.
    Result<Json> hostRequest(const std::string& method, const Json& params, const std::shared_ptr<ExternalHost>& host);
    /// The client disconnected. Main thread.
    void hostClosed(const std::shared_ptr<ExternalHost>& host);
    /// Handlers for a transport on another thread: they hop to the engine's main thread.
    HostHandlers hostHandlers();

    // --- Hosted tools (the poll-based ToolHost: tool_host_register, py_* tools) ---------------
    /// Takes over publishing for the ToolHost so its tools follow the same policy, approvals,
    /// capabilities, limits, stats and audit as every other dynamic tool (one registration path).
    ToolHost::Publisher hostedPublisher();
    /// {name: {status, reason?}} for tools of any kind (tool_host_register reports it).
    Json statusOf(const Json& names) const;

    /// Human actors (who may approve tools and loosen the policy).
    static bool isHuman(std::string_view actor);
    /// "tools/<short>.tool.json" for a tool name.
    static std::string fileFor(const std::string& name);
    /// The user library folder (~/.skywalker/tools, or $SKYWALKER_TOOL_LIBRARY).
    static std::string libraryDir();
    /// Adds the builtins only tool code has (call_tool, read_file, tool_fail...) to a registry, e.g. one
    /// chained to the global registry for checking tool code outside a tool call.
    static void registerBuiltins(wander::BuiltinRegistry& registry);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sky
