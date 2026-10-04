#pragma once
// Tool registry: the single, typed command surface of the engine.
//
// Every capability an agent can use is a Tool with a JSON-Schema input, a description
// written *for language models*, and MCP annotations. The same registry backs:
//   * the MCP server (stdio for Claude Code / Codex / Cursor / any MCP client, and a Unix
//     socket that attaches to a running editor),
//   * in-editor agents (Cloudlings) via the C API,
//   * the editor UI itself, which is "just another client" of these tools.
// One surface means humans and agents always see and do exactly the same things.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

struct ContentBlock {
    enum class Type { Text, Image } type = Type::Text;
    std::string text;      // Text
    std::string data;      // Image: base64
    std::string mimeType;  // Image: e.g. "image/png"
};

struct ToolResult;

/// The slow half of a tool call (a design app running for seconds, a download...). `work` runs
/// off the main thread and must not touch engine state; `finish` then runs on the main thread
/// and builds the real result (importing files, editing the scene). Agent connections run the
/// work on their own thread, so the editor stays responsive while it runs.
struct DeferredWork {
    std::function<void()> work;
    std::function<ToolResult()> finish;
    /// Optional, thread-safe: asks `work` to stop early (the agent server is shutting down).
    std::function<void()> cancel;
};

struct ToolResult {
    std::vector<ContentBlock> content;
    Json structured;  // machine-readable payload (also mirrored as text for older clients)
    bool isError = false;
    /// Set by tools that finish asynchronously (see DeferredWork). Everything else ignores it:
    /// ToolRegistry::call() completes it inline, only the agent server splits the steps.
    std::shared_ptr<DeferredWork> deferred;

    /// Runs the deferred work (if any) inline and returns the final result.
    ToolResult complete();
    static ToolResult defer(std::function<void()> work, std::function<ToolResult()> finish,
                            std::function<void()> cancel = nullptr);

    static ToolResult text(std::string t);
    static ToolResult json(Json payload, std::string summary = {});
    static ToolResult error(const Error& e);
    ToolResult& image(std::string base64Png);

    Json toMcp() const;  // MCP CallToolResult
};

struct ToolContext {
    std::string actor = "user";  // "user", "agent:Nimbus", "mcp:claude-code", ...
};

struct ToolDef {
    std::string name;
    std::string title;
    std::string description;
    std::string category;  // scene, entity, world, wander, sim, view, history, asset, render, network
    Json inputSchema;
    bool mutates = false;
    bool destructive = false;
    std::function<ToolResult(const Json& args, ToolContext& ctx)> handler;
    /// Reaches outside the project (e.g. the internet). MCP clients and the in-editor crew
    /// ask the human before running these.
    bool openWorld = false;
    /// Plumbing that should not show up in the activity feed or per-agent tool counts (event
    /// polling, tool-host traffic): recording it would feed the event stream back into itself.
    bool quiet = false;
};

class ToolRegistry {
public:
    void add(ToolDef def);
    /// A built-in or dynamic tool by name (nullptr if none).
    const ToolDef* find(std::string_view name) const;
    /// The built-in tools (registered at startup). Dynamic tools are listed by dynamicTools().
    const std::vector<ToolDef>& all() const { return tools_; }

    /// Dynamic tools come and go while agent connections run (tools hosted by an external process,
    /// see ToolHost.h). Thread-safe. A tool with the same name replaces the previous one; pointers
    /// returned by find() stay valid for the registry's lifetime (replaced and removed definitions
    /// are retired, never freed), so at most kMaxDynamicDefinitions definitions are accepted.
    static constexpr size_t kMaxDynamicDefinitions = 4096;
    Status addDynamic(ToolDef def);
    bool removeDynamic(std::string_view name);
    /// Snapshot of the live dynamic tools, in registration order.
    std::vector<ToolDef> dynamicTools() const;

    /// Validates arguments against the schema, then invokes the handler. Never throws. A tool
    /// that defers its slow half is completed inline (use invoke() to split the steps).
    ToolResult call(std::string_view name, const Json& args, ToolContext& ctx) const;
    /// Like call(), but returns a deferred result as is (see DeferredWork).
    ToolResult invoke(std::string_view name, const Json& args, ToolContext& ctx) const;

    /// MCP `tools/list` payload.
    Json listJson() const;
    /// The tool catalogue as markdown, grouped by category (`skywalker tools --markdown`, MCP resource skywalker://tools).
    std::string catalogueMarkdown() const;

private:
    const ToolDef* findDynamic(std::string_view name) const;

    std::vector<ToolDef> tools_;
    mutable std::mutex dynamicMutex_;
    std::vector<std::shared_ptr<const ToolDef>> dynamic_;  // live, in registration order
    std::vector<std::shared_ptr<const ToolDef>> retired_;  // kept alive: find() may have handed them out
};

/// Lightweight JSON-Schema validation covering what tool schemas use: type, required,
/// properties, enum, items, additionalProperties=false. Unknown keys get did-you-mean hints.
Status validateSchema(const Json& schema, const Json& value, const std::string& path = "args");

// Schema-building helpers to keep tool definitions short and readable.
namespace schema {
Json object(std::initializer_list<Json::Member> properties, std::initializer_list<const char*> required = {});
Json string(std::string description);
Json number(std::string description);
Json integer(std::string description);
Json boolean(std::string description);
Json vec3(std::string description);
Json enumeration(std::initializer_list<const char*> values, std::string description);
Json enumeration(const std::vector<std::string>& values, std::string description);
Json any(std::string description);
Json array(Json items, std::string description);
/// An entity reference: numeric id or name.
Json entity(std::string description = "Entity id (number) or exact name");
}  // namespace schema

}  // namespace sky
