#pragma once
// Model Context Protocol (MCP) server session.
//
// Transport-agnostic: feed it one JSON-RPC message (a line) and it returns the response
// line, if any. The CLI wires it to stdio (newline-delimited JSON, per the MCP stdio
// transport); the editor wires it to a Unix domain socket so external agents can attach
// to a live editor session. Implements: initialize, ping, tools/list, tools/call,
// resources/list, prompts/list, logging/setLevel. Spec: https://modelcontextprotocol.io

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/core/Json.h"

namespace sky {

class McpSession {
public:
    /// Executes a tool and returns an MCP CallToolResult. May block (e.g. hop threads).
    using Executor = std::function<Json(const std::string& tool, const Json& args, const std::string& actor)>;

    McpSession(const ToolRegistry& registry, Executor executor);

    /// Handles one message. Returns the serialized response, or nullopt for notifications.
    std::optional<std::string> handle(std::string_view message);

    const std::string& clientName() const { return clientName_; }
    bool initialized() const { return initialized_; }
    const std::string& protocolVersion() const { return protocolVersion_; }

    static const char* latestProtocolVersion();
    static const char* instructions();

private:
    Json dispatch(const Json& request);
    Json handleOne(const Json& request, bool& isNotification);

    const ToolRegistry& registry_;
    Executor executor_;
    std::string clientName_ = "mcp-client";
    std::string protocolVersion_;
    bool initialized_ = false;
};

}  // namespace sky
