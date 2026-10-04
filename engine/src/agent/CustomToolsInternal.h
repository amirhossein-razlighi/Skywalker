#pragma once
// Custom tools internals shared by CustomTools.cpp (definitions, persistence, approval, hosts),
// CustomToolRun.cpp (execution: Wander, composite, external, nested calls, tests) and
// CustomToolTools.cpp (the tool_* lifecycle tools). Not part of the public API.

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/agent/CustomTools.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Builtins.h"
#include "skywalker/wander/Bytecode.h"
#include "skywalker/wander/Runtime.h"

namespace sky {

struct ToolStats {
    int64_t calls = 0;
    int64_t failures = 0;
    double totalMs = 0;
    double lastMs = 0;
    double maxMs = 0;
    std::string lastActor;
    std::string lastAt;
    std::deque<Json> recentErrors;  // newest last, at most 5
    std::deque<Json> recentCalls;   // newest last, at most 10

    void record(const std::string& actor, bool ok, double ms, const std::string& error, const Json& nested);
    Json toJson() const;
};

struct ToolEntry {
    CustomToolDef def;
    /// active | pending_approval | rejected | disabled | offline | invalid | policy_off
    std::string status = "invalid";
    std::string reason;  // why it is not active
    std::string hash;
    std::shared_ptr<const wander::Program> program;  // kind wander
    std::vector<std::string> warnings;
    ToolStats stats;
    std::shared_ptr<ExternalHost> host;  // kind external, while its client is connected
    /// Kind external through the poll-based ToolHost (tool_host_register): its transport definition,
    /// whose handler queues the call for the host process.
    std::shared_ptr<const ToolDef> hosted;
    std::string file;                // project-relative definition file; empty for unsaved external tools
    std::string loadError;           // the file could not be parsed or validated

    Json summary() const;
};

/// One running call of a custom tool.
struct Invocation {
    std::string callId;
    std::shared_ptr<const ToolEntry> tool;
    std::string actor;  // the caller: permissions and attribution
    int depth = 0;      // nesting (1 = called directly)
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point deadline;
    int calls = 0;            // nested tool calls so far
    bool dryRun = false;      // tests: scene edits are rolled back by the caller, file writes skipped
    bool askedHuman = false;  // the caller's access to this tool required a human's OK
    std::vector<std::string> warnings;
    std::vector<std::string> logs;
    Json nested = Json::array();  // [{tool, ok, ms}]
    std::optional<Error> failure;  // tool_fail() from Wander

    bool expired() const { return std::chrono::steady_clock::now() > deadline; }
    double elapsedMs() const;
};

struct CustomTools::Impl {
    explicit Impl(Engine& e);

    Engine& engine;
    std::map<std::string, std::shared_ptr<ToolEntry>> entries;
    std::vector<Invocation*> stack;  // synchronous nesting on the main thread
    std::map<std::string, std::shared_ptr<Invocation>> external;  // external calls in flight, by call id
    uint64_t nextCall = 1;
    std::unique_ptr<wander::BuiltinRegistry> builtins;  // call_tool & co. on top of the engine's builtins
    std::unique_ptr<Scene> sandbox;                     // empty scene for tools without read_scene
    std::vector<std::unique_ptr<wander::Runtime>> scenePool;
    std::vector<std::unique_ptr<wander::Runtime>> sandboxPool;
    Json approvals = Json::object();  // tools/approvals.json "approvals"
    bool hasToolFiles = false;  // the project has tools/*.tool.json (or will: audit logs go to real projects only)
    mutable int64_t gameJsonTime = -2;
    mutable ToolPolicy cachedPolicy = ToolPolicy::Auto;

    static constexpr int kMaxDepth = 8;
    static constexpr int64_t kMaxInstructions = 50'000'000;
    static constexpr int kMaxTimeoutMs = 120'000;
    static constexpr int64_t kMaxOutputBytes = 1024 * 1024;
    static constexpr int kMaxCallsLimit = 10'000;

    // --- definitions (CustomTools.cpp) --------------------------------------------------------
    std::string projectDir() const;
    /// Parses and validates a definition (merged over `previous` when updating).
    Result<CustomToolDef> parse(const Json& spec, const std::string& actor, const CustomToolDef* previous,
                                std::vector<std::string>& warnings);
    Status validate(CustomToolDef& def, std::vector<std::string>& warnings, std::shared_ptr<const wander::Program>* program);
    Result<std::shared_ptr<const wander::Program>> compile(const CustomToolDef& def, std::vector<std::string>& warnings);
    /// Whether a program calls a builtin that could change the scene (spawn, move, add_tag...): a read-only
    /// call then compares the scene before and after; otherwise the runtime's write guard suffices.
    static bool callsSceneBuiltins(const wander::Program& program);
    /// Decides the status (policy, approval, enabled, host) and (un)registers the tool.
    void refresh(ToolEntry& entry);
    void publish(const ToolEntry& entry);
    void unpublish(const std::string& name);
    bool needsApproval(const CustomToolDef& def) const;
    bool approved(const ToolEntry& entry) const;
    Status save(ToolEntry& entry);
    void loadApprovals();
    Status saveApprovals();
    void loadAll();
    Result<std::shared_ptr<ToolEntry>> loadFile(const std::string& relPath);
    ToolPolicy policy() const;
    std::shared_ptr<ToolEntry> find(const std::string& name) const;
    Error unknownTool(const std::string& name) const;
    void audit(Json event);

    // --- execution (CustomToolRun.cpp) --------------------------------------------------------
    /// The registry handler of every custom tool.
    ToolResult run(const std::string& name, const Json& args, ToolContext& ctx);
    ToolResult execute(const std::shared_ptr<const ToolEntry>& tool, const Json& args, ToolContext& ctx, bool dryRun);
    ToolResult runWander(Invocation& inv, const Json& args);
    ToolResult runComposite(Invocation& inv, const Json& args);
    ToolResult runExternal(const std::shared_ptr<Invocation>& inv, const Json& args);
    ToolResult runHosted(const std::shared_ptr<Invocation>& inv, const Json& args);
    /// One external tool from a client (both transports): validated like any definition, then status and registration.
    Result<std::shared_ptr<ToolEntry>> adoptExternal(Json spec, const std::shared_ptr<ExternalHost>& host,
                                                    std::shared_ptr<const ToolDef> hosted = nullptr);
    /// A hosted tool's transport is gone (main thread): the entry is dropped.
    void dropHosted(const std::string& name, const std::string& hostId);
    /// A nested call made by a running custom tool (call_tool, a composite step, an external
    /// tool's callback): capability, permission, depth and limit checks, then the call.
    ToolResult callNested(Invocation& inv, const std::string& tool, const Json& args);
    Status checkNested(const Invocation& inv, const ToolDef& target) const;
    /// Gate for external tools' callbacks (ToolContext::parentCall).
    std::optional<ToolResult> gate(const ToolDef& tool, const Json& args, ToolContext& ctx);
    /// A project path the invocation may access ("" + error if not).
    Result<std::string> checkPath(const Invocation& inv, const std::string& path, bool write) const;
    /// Finishes a result: output schema, output cap, logs/warnings, stats, audit, activity event.
    ToolResult finish(Invocation& inv, ToolResult result);
    Json runTests(const std::shared_ptr<const ToolEntry>& tool, const std::string& actor, const std::string& filter);
    Json testOnce(const std::shared_ptr<const ToolEntry>& tool, const Json& args, const Json& setup, const std::string& actor);
    void registerBuiltins();
    std::unique_ptr<wander::Runtime> acquireRuntime(bool scene);
    void releaseRuntime(bool scene, std::unique_ptr<wander::Runtime> rt);
};

namespace customtools {
/// Normalizes a tool name ("user.foo", "foo" -> "user_foo"); errors explain the rules.
Result<std::string> normalizeName(std::string_view name, std::string_view defaultNamespace);
/// Checks that a JSON schema is one tools can use (an object with typed properties).
Status checkSchema(const Json& schema, const std::string& where);
/// Fills `default` values of top-level properties that the arguments leave out.
Json withDefaults(const Json& schema, const Json& args);
/// Whether a glob list ("entity_*", "scene_query") allows a tool name.
bool allows(const std::vector<std::string>& patterns, std::string_view name);
/// Resolves {{...}} templates in a composite step (see docs/CUSTOM_TOOLS.md).
Result<Json> resolveTemplate(const Json& tmpl, const Json& scope);
/// Whether `actual` matches the expectation `expected` (subset match with $gte/$lte/$len/... operators).
bool matches(const Json& expected, const Json& actual, std::string& why, const std::string& path = "result");
/// Wander value -> tool JSON (entities become their numeric id).
Json toToolJson(const wander::Value& v);
std::string nowIso();
}  // namespace customtools

}  // namespace sky
