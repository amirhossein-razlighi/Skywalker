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

struct ToolResult {
    std::vector<ContentBlock> content;
    Json structured;  // machine-readable payload (also mirrored as text for older clients)
    bool isError = false;

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
    std::string category;  // scene, entity, wander, sim, view, history, asset, render
    Json inputSchema;
    bool mutates = false;
    bool destructive = false;
    std::function<ToolResult(const Json& args, ToolContext& ctx)> handler;
};

class ToolRegistry {
public:
    void add(ToolDef def);
    const ToolDef* find(std::string_view name) const;
    const std::vector<ToolDef>& all() const { return tools_; }

    /// Validates arguments against the schema, then invokes the handler. Never throws.
    ToolResult call(std::string_view name, const Json& args, ToolContext& ctx) const;

    /// MCP `tools/list` payload.
    Json listJson() const;

private:
    std::vector<ToolDef> tools_;
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
Json any(std::string description);
Json array(Json items, std::string description);
/// An entity reference: numeric id or name.
Json entity(std::string description = "Entity id (number) or exact name");
}  // namespace schema

}  // namespace sky
