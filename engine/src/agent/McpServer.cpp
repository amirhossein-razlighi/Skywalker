#include "skywalker/agent/McpServer.h"

#include <algorithm>

namespace sky {

namespace {

constexpr const char* kVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

Json rpcError(const Json& id, int code, std::string message) {
    return Json::object({{"jsonrpc", "2.0"},
                         {"id", id},
                         {"error", Json::object({{"code", code}, {"message", std::move(message)}})}});
}

Json rpcResult(const Json& id, Json result) {
    return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

std::string sanitizeActor(std::string name) {
    std::replace_if(name.begin(), name.end(), [](char c) { return c == ' ' || c == '\n'; }, '-');
    if (name.size() > 48) name.resize(48);
    return name.empty() ? "mcp-client" : name;
}

}  // namespace

const char* McpSession::latestProtocolVersion() { return kVersions[0]; }

const char* McpSession::instructions() {
    return "Skywalker is a game engine built for agents. Typical loop: scene_overview -> make changes "
           "(entity_create / entity_update / transform / batch) -> viewport_capture (annotate=true labels every "
           "entity with its #id) -> iterate. Behaviors are written in Wander (call wander_reference once). Use "
           "behavior_set to attach behaviors; it returns compiler diagnostics. Test gameplay deterministically with "
           "sim_control step. Every edit is undoable (history) and attributed to you. Prefer batch for many edits.";
}

McpSession::McpSession(const ToolRegistry& registry, Executor executor)
    : registry_(registry), executor_(std::move(executor)) {}

std::optional<std::string> McpSession::handle(std::string_view message) {
    auto parsed = Json::parse(message);
    if (!parsed) return rpcError(Json(), -32700, "Parse error: " + parsed.error().message).dump();
    const Json& msg = parsed.value();
    if (msg.isArray()) {  // JSON-RPC batch (allowed by older protocol revisions)
        Json responses = Json::array();
        for (const auto& item : msg.elements()) {
            bool note = false;
            Json r = handleOne(item, note);
            if (!note) responses.push(std::move(r));
        }
        if (responses.size() == 0) return std::nullopt;
        return responses.dump();
    }
    bool isNotification = false;
    Json response = handleOne(msg, isNotification);
    if (isNotification) return std::nullopt;
    return response.dump();
}

Json McpSession::handleOne(const Json& request, bool& isNotification) {
    if (!request.isObject() || request.get("jsonrpc").asString() != "2.0") {
        isNotification = false;
        return rpcError(request.get("id"), -32600, "Invalid Request");
    }
    // Responses from the client (we never send requests) and notifications have no reply.
    if (!request.contains("method")) {
        isNotification = true;
        return {};
    }
    isNotification = !request.contains("id");
    if (isNotification) {
        if (request.get("method").asString() == "notifications/initialized") initialized_ = true;
        return {};
    }
    return dispatch(request);
}

Json McpSession::dispatch(const Json& req) {
    const Json& id = req.get("id");
    const std::string& method = req.get("method").asString();
    const Json& params = req.get("params");

    if (method == "initialize") {
        std::string requested = params.get("protocolVersion").asString();
        protocolVersion_ = latestProtocolVersion();
        for (const char* v : kVersions) {
            if (requested == v) protocolVersion_ = v;
        }
        const Json& info = params.get("clientInfo");
        clientName_ = sanitizeActor(info.get("name").asString("mcp-client"));
        return rpcResult(id, Json::object({
            {"protocolVersion", protocolVersion_},
            {"capabilities", Json::object({{"tools", Json::object({{"listChanged", false}})},
                                           {"resources", Json::object()},
                                           {"prompts", Json::object()},
                                           {"logging", Json::object()}})},
            {"serverInfo", Json::object({{"name", "skywalker"},
                                         {"title", "Skywalker Game Engine"},
                                         {"version", SKY_VERSION_STRING}})},
            {"instructions", instructions()},
        }));
    }
    if (method == "ping") return rpcResult(id, Json::object());
    if (method == "tools/list") return rpcResult(id, registry_.listJson());
    if (method == "tools/call") {
        if (!params.get("name").isString()) return rpcError(id, -32602, "tools/call requires params.name");
        const std::string& name = params.get("name").asString();
        if (!registry_.find(name)) {
            // Unknown tools are a protocol error per spec; include a hint in the message.
            ToolContext ctx;
            ToolResult r = registry_.call(name, Json::object(), ctx);
            return rpcError(id, -32602, r.content.empty() ? "Unknown tool" : r.content.front().text);
        }
        Json args = params.get("arguments").isObject() ? params.get("arguments") : Json::object();
        return rpcResult(id, executor_(name, args, "mcp:" + clientName_));
    }
    if (method == "resources/list") return rpcResult(id, Json::object({{"resources", Json::array()}}));
    if (method == "resources/templates/list") return rpcResult(id, Json::object({{"resourceTemplates", Json::array()}}));
    if (method == "prompts/list") return rpcResult(id, Json::object({{"prompts", Json::array()}}));
    if (method == "logging/setLevel") return rpcResult(id, Json::object());
    return rpcError(id, -32601, "Method not found: " + method);
}

}  // namespace sky
