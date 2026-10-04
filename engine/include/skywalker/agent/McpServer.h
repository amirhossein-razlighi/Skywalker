#pragma once
// Model Context Protocol (MCP) server session.
//
// Transport-agnostic: feed it one JSON-RPC message (a line) and it returns the response
// line, if any. The CLI wires it to stdio (newline-delimited JSON, per the MCP stdio
// transport); the editor wires it to a Unix domain socket so external agents can attach
// to a live editor session. Implements: initialize, ping, tools/list, tools/call,
// resources/list, resources/templates/list, resources/read (docs, skills, the tool catalogue and live
// studio/scene state), prompts/list, prompts/get (studio roles and workflows), logging/setLevel,
// notifications/tools/list_changed (custom and external tools come and go), and the
// skywalker/tools/* methods that let a client host tools (skywalker/agent/ExternalHost.h).
// Spec: https://modelcontextprotocol.io

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "skywalker/agent/ExternalHost.h"
#include "skywalker/agent/ToolRegistry.h"
#include "skywalker/core/Json.h"

namespace sky {

class McpSession {
public:
    /// Executes a tool and returns an MCP CallToolResult. May block (e.g. hop threads).
    using Executor = std::function<Json(const std::string& tool, const Json& args, const std::string& actor)>;
    /// Like Executor, with the full call context (actor and, for callbacks of an external tool,
    /// the call they serve).
    using ContextExecutor = std::function<Json(const std::string& tool, const Json& args, const ToolContext& ctx)>;

    McpSession(const ToolRegistry& registry, Executor executor);
    McpSession(const ToolRegistry& registry, ContextExecutor executor);

    /// Lets the client host tools (`skywalker/tools/register`); without it those methods report
    /// that the transport does not support external tools.
    void setHost(std::shared_ptr<ExternalHost> host, HostHandlers handlers);
    const std::shared_ptr<ExternalHost>& host() const { return host_; }
    /// Tells the engine the client is gone (its external tools go offline). Idempotent.
    void closeHost();

    /// Handles one message. Returns the serialized response, or nullopt for notifications.
    std::optional<std::string> handle(std::string_view message);
    /// `notifications/tools/list_changed` when the tool set changed since the client last
    /// listed (or was told). Thread-safe; transports send it whenever it is not empty.
    std::optional<std::string> pendingNotification();

    const std::string& clientName() const { return clientName_; }
    bool initialized() const { return initialized_.load(); }
    const std::string& protocolVersion() const { return protocolVersion_; }

    static const char* latestProtocolVersion();
    static const char* instructions();

private:
    Json dispatch(const Json& request);
    Json listResources() const;
    Json readResource(const Json& id, const std::string& uri);
    Json listPrompts() const;
    Json getPrompt(const Json& id, const std::string& name, const Json& arguments);
    Json handleOne(const Json& request, bool& isNotification);

    Json callTool(const std::string& tool, const Json& args, const std::string& parentCall = {});

    const ToolRegistry& registry_;
    ContextExecutor executor_;
    std::string clientName_ = "mcp-client";
    std::string protocolVersion_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> hostClosed_{false};
    std::atomic<uint64_t> notifiedRevision_{0};
    std::shared_ptr<ExternalHost> host_;
    HostHandlers handlers_;
};

}  // namespace sky
