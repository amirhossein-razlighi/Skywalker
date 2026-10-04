#include "skywalker/agent/McpServer.h"

#include <algorithm>

#include "skywalker/agent/Resources.h"

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

constexpr const char* kDocsPrefix = "docs/";
constexpr const char* kSkillsPrefix = "integrations/claude-code/skills/";
constexpr const char* kAgentsPrefix = "integrations/claude-code/agents/";
constexpr const char* kCommandsPrefix = "integrations/claude-code/commands/";

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string stem(std::string_view path) {
    size_t slash = path.find_last_of('/');
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    size_t dot = file.rfind('.');
    return std::string(dot == std::string_view::npos ? file : file.substr(0, dot));
}

/// First markdown heading of a document, used as the resource title.
std::string firstHeading(std::string_view text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (startsWith(line, "# ")) return std::string(line.substr(2));
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return {};
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

Json resourceEntry(const std::string& uri, const std::string& name, const std::string& title, const std::string& description,
                   const char* mime) {
    Json r = Json::object({{"uri", uri}, {"name", name}});
    if (!title.empty()) r["title"] = title;
    if (!description.empty()) r["description"] = description;
    r["mimeType"] = mime;
    return r;
}

/// Live state served as resources: it is read through the same tools agents call, so it reflects whatever
/// the attached editor or headless project currently holds.
struct LiveResource {
    const char* uri;
    const char* name;
    const char* description;
    const char* tool;
    const char* args;
    const char* mime;
};
constexpr LiveResource kLive[] = {
    {"skywalker://scene/overview", "scene_overview", "The live scene outline: every entity as one line, environment and selection", "scene_overview", "{}", "text/plain"},
    {"skywalker://studio/overview", "studio_overview", "Studio at a glance: roster with live status, board counts, feedback awaiting a decision, loops, playtest metrics, usage", "studio_overview", "{}", "application/json"},
    {"skywalker://studio/roster", "studio_roster", "The studio roster with full agent profiles (ids are used for assignees and the `as` argument)", "studio_agent_list", R"({"include_profiles":true})", "application/json"},
    {"skywalker://studio/board", "studio_board", "The task board: every task with status, assignee, acceptance criteria and links", "studio_task_list", "{}", "application/json"},
    {"skywalker://studio/feedback", "studio_feedback", "All feedback with director verdicts and measured effects", "studio_feedback_list", "{}", "application/json"},
    {"skywalker://studio/loops", "studio_loops", "Every loop with its state and the available loop templates", "studio_loop_status", "{}", "application/json"},
};

/// The result text of a tool call: structured JSON when asked for and present, else the first text block.
std::string toolPayload(const Json& result, bool preferStructured, bool& isError) {
    isError = result.get("isError").asBool();
    if (preferStructured && result.get("structuredContent").isObject()) return result.get("structuredContent").dump(2);
    for (const auto& block : result.get("content").elements()) {
        if (block.get("type").asString() == "text") return block.get("text").asString();
    }
    return {};
}

}  // namespace

const char* McpSession::latestProtocolVersion() { return kVersions[0]; }

const char* McpSession::instructions() {
    return "Skywalker is a game engine built for agents. Typical loop: scene_overview -> make changes "
           "(entity_create / entity_update / transform / batch) -> viewport_capture (annotate=true labels every "
           "entity with its #id) -> iterate. Behaviors are written in Wander (call wander_reference once). Use "
           "behavior_set to attach behaviors; it returns compiler diagnostics. Test gameplay deterministically with "
           "sim_control step. Every edit is undoable (history) and attributed to you. Prefer batch for many edits. "
           "Resources: skywalker://docs/<name> (engine docs), skywalker://skills/<name> (the skywalker-* agent skills), "
           "skywalker://tools (live tool catalogue), skywalker://studio/* and skywalker://scene/overview (live state). "
           "Prompts: studio roles (role_*) and workflows (new_game, look_dev, playtest_loop, ...). To work as a studio "
           "roster member pass as:\"<agent id>\" on studio_* calls.";
}

McpSession::McpSession(const ToolRegistry& registry, Executor executor)
    : registry_(registry),
      executor_([fn = std::move(executor)](const std::string& tool, const Json& args, const ToolContext& ctx) {
          return fn(tool, args, ctx.actor);
      }) {
    notifiedRevision_ = registry_.revision();
}

McpSession::McpSession(const ToolRegistry& registry, ContextExecutor executor)
    : registry_(registry), executor_(std::move(executor)) {
    notifiedRevision_ = registry_.revision();
}

void McpSession::setHost(std::shared_ptr<ExternalHost> host, HostHandlers handlers) {
    host_ = std::move(host);
    handlers_ = std::move(handlers);
}

void McpSession::closeHost() {
    if (!host_ || hostClosed_.exchange(true)) return;
    if (handlers_.closed) handlers_.closed(host_);
}

std::optional<std::string> McpSession::pendingNotification() {
    if (!initialized_.load()) return std::nullopt;
    uint64_t now = registry_.revision();
    if (notifiedRevision_.exchange(now) == now) return std::nullopt;
    return Json::object({{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}).dump();
}

Json McpSession::callTool(const std::string& tool, const Json& args, const std::string& parentCall) {
    ToolContext ctx;
    ctx.actor = "mcp:" + clientName_;
    ctx.parentCall = parentCall;
    return executor_(tool, args, ctx);
}

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
        if (host_) host_->setActor("mcp:" + clientName_);
        Json capabilities = Json::object({{"tools", Json::object({{"listChanged", true}})},
                                          {"resources", Json::object()},
                                          {"prompts", Json::object()},
                                          {"logging", Json::object()}});
        if (host_) {
            // Clients can host tools on this connection (docs/CUSTOM_TOOLS.md, "External tools").
            capabilities["experimental"] = Json::object(
                {{"skywalker/externalTools",
                  Json::object({{"version", 1},
                                {"methods", Json::array({"skywalker/tools/register", "skywalker/tools/unregister",
                                                         "skywalker/tools/list"})},
                                {"callMethod", "skywalker/tools/call"}})}});
        }
        return rpcResult(id, Json::object({
            {"protocolVersion", protocolVersion_},
            {"capabilities", capabilities},
            {"serverInfo", Json::object({{"name", "skywalker"},
                                         {"title", "Skywalker Game Engine"},
                                         {"version", SKY_VERSION_STRING}})},
            {"instructions", instructions()},
        }));
    }
    if (method == "ping") return rpcResult(id, Json::object());
    if (method == "tools/list") {
        notifiedRevision_ = registry_.revision();
        return rpcResult(id, registry_.listJson());
    }
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
        // A callback made while this client serves one of its external tools.
        const std::string& parentCall = params.get("_meta").get("skywalker/call_id").asString();
        return rpcResult(id, callTool(name, args, parentCall));
    }
    if (startsWith(method, "skywalker/tools/")) {
        if (!host_ || !handlers_.request) {
            return rpcError(id, -32601, "Method not found: " + method + " (this connection cannot host tools)");
        }
        Result<Json> r = handlers_.request(method, params, host_);
        if (!r) {
            std::string message = "[" + r.error().code + "] " + r.error().message;
            if (!r.error().hint.empty()) message += " (hint: " + r.error().hint + ")";
            return rpcError(id, r.error().code == "unknown_method" ? -32601 : -32602, message);
        }
        return rpcResult(id, std::move(r.value()));
    }
    if (method == "resources/list") return rpcResult(id, listResources());
    if (method == "resources/templates/list") {
        return rpcResult(id, Json::object({{"resourceTemplates", Json::array({
            Json::object({{"uriTemplate", "skywalker://docs/{name}"}, {"name", "docs"}, {"description", "Engine documentation page, e.g. STUDIO, RENDERING, AGENTS, legal/TERMS, legal/PRIVACY"}, {"mimeType", "text/markdown"}}),
            Json::object({{"uriTemplate", "skywalker://skills/{name}"}, {"name", "skills"}, {"description", "A skywalker-* agent skill (SKILL.md)"}, {"mimeType", "text/markdown"}})})}}));
    }
    if (method == "resources/read") {
        if (!params.get("uri").isString()) return rpcError(id, -32602, "resources/read requires params.uri");
        return readResource(id, params.get("uri").asString());
    }
    if (method == "prompts/list") return rpcResult(id, listPrompts());
    if (method == "prompts/get") {
        if (!params.get("name").isString()) return rpcError(id, -32602, "prompts/get requires params.name");
        return getPrompt(id, params.get("name").asString(), params.get("arguments"));
    }
    if (method == "logging/setLevel") return rpcResult(id, Json::object());
    return rpcError(id, -32601, "Method not found: " + method);
}

Json McpSession::listResources() const {
    Json list = Json::array();
    list.push(resourceEntry("skywalker://tools", "tools", "Tool catalogue",
                            "Every engine tool by category with its description (generated from the live registry)", "text/markdown"));
    for (const auto& r : kLive) list.push(resourceEntry(r.uri, r.name, "", r.description, r.mime));
    for (const auto* a : embeddedAssetsUnder(kDocsPrefix)) {
        // Path below docs/ without ".md": "STUDIO", "legal/TERMS".
        std::string rel = std::string(a->path).substr(std::string_view(kDocsPrefix).size());
        std::string name = rel.substr(0, rel.size() - std::min<size_t>(rel.size(), 3));
        list.push(resourceEntry("skywalker://docs/" + name, name, firstHeading(a->content), "", "text/markdown"));
    }
    for (const auto* a : embeddedAssetsUnder(kSkillsPrefix)) {
        std::string rel = std::string(a->path).substr(std::string_view(kSkillsPrefix).size());
        size_t slash = rel.find('/');
        std::string skill = rel.substr(0, slash);
        if (rel.substr(slash + 1) == "SKILL.md") {
            list.push(resourceEntry("skywalker://skills/" + skill, skill, "", parseMarkdownDoc(a->content).get("description"), "text/markdown"));
        } else {
            list.push(resourceEntry("skywalker://skills/" + rel, skill + "/" + rel.substr(slash + 1), firstHeading(a->content), "", "text/markdown"));
        }
    }
    return Json::object({{"resources", list}});
}

Json McpSession::readResource(const Json& id, const std::string& uri) {
    auto contents = [&](const std::string& text, const char* mime) {
        return rpcResult(id, Json::object({{"contents", Json::array({Json::object({{"uri", uri}, {"mimeType", mime}, {"text", text}})})}}));
    };
    auto notFound = [&] { return rpcError(id, -32002, "Resource not found: " + uri + " (resources/list shows what exists)"); };
    if (uri == "skywalker://tools") return contents(registry_.catalogueMarkdown(), "text/markdown");
    for (const auto& r : kLive) {
        if (uri != r.uri) continue;
        auto args = Json::parse(r.args);
        if (!registry_.find(r.tool) || !args) return notFound();
        bool isError = false;
        std::string text = toolPayload(callTool(r.tool, args.value()), std::string_view(r.mime) == "application/json", isError);
        if (isError) return rpcError(id, -32603, "Reading " + uri + " failed: " + text);
        return contents(text, r.mime);
    }
    constexpr std::string_view docs = "skywalker://docs/";
    constexpr std::string_view skills = "skywalker://skills/";
    if (startsWith(uri, docs)) {
        std::string name = uri.substr(docs.size());
        if (name.empty() || name.find("..") != std::string::npos) return notFound();
        if (const auto* a = findEmbeddedAsset(std::string(kDocsPrefix) + name + ".md")) return contents(a->content, "text/markdown");
        return notFound();
    }
    if (startsWith(uri, skills)) {
        std::string rel = uri.substr(skills.size());
        if (rel.find("..") != std::string::npos) return notFound();
        if (rel.find('/') == std::string::npos) rel += "/SKILL.md";
        if (const auto* a = findEmbeddedAsset(std::string(kSkillsPrefix) + rel)) return contents(a->content, "text/markdown");
    }
    return notFound();
}

Json McpSession::listPrompts() const {
    Json list = Json::array();
    auto argumentList = [](const std::string& description) {
        return Json::array({Json::object({{"name", "arguments"}, {"description", description}, {"required", false}})});
    };
    for (const auto* a : embeddedAssetsUnder(kAgentsPrefix)) {
        MarkdownDoc doc = parseMarkdownDoc(a->content);
        list.push(Json::object({{"name", "role_" + replaceAll(doc.get("name"), "-", "_")},
                                {"title", "Studio role: " + doc.get("name")},
                                {"description", doc.get("description")},
                                {"arguments", argumentList("The assignment for this role (optional; otherwise claim a task from the board)")}}));
    }
    for (const auto* a : embeddedAssetsUnder(kCommandsPrefix)) {
        MarkdownDoc doc = parseMarkdownDoc(a->content);
        std::string hint = doc.get("argument-hint");
        list.push(Json::object({{"name", replaceAll(stem(a->path), "-", "_")},
                                {"title", "Workflow: " + stem(a->path)},
                                {"description", doc.get("description")},
                                {"arguments", argumentList(hint.empty() ? "Optional arguments" : hint)}}));
    }
    list.push(Json::object(
        {{"name", "studio_agent"},
         {"title", "Studio agent brief"},
         {"description", "The system prompt and permitted tools of a roster member of this project's studio (live, from studio_agent_brief); adopt it to act as that agent"},
         {"arguments", Json::array({Json::object({{"name", "agent"}, {"description", "Agent id (see studio_agent_list)"}, {"required", true}}),
                                    Json::object({{"name", "arguments"}, {"description", "The assignment (optional)"}, {"required", false}})})}}));
    return Json::object({{"prompts", list}});
}

Json McpSession::getPrompt(const Json& id, const std::string& name, const Json& promptArgs) {
    const std::string given = promptArgs.get("arguments").asString();
    const std::string assignment =
        "\n## Your assignment\n" + (given.empty() ? std::string("Check studio_inbox and claim a task with studio_task_claim.") : given) + "\n";
    auto result = [&](const std::string& description, const std::string& text) {
        return rpcResult(id, Json::object({{"description", description},
                                           {"messages", Json::array({Json::object({{"role", "user"}, {"content", Json::object({{"type", "text"}, {"text", text}})}})})}}));
    };
    if (name == "studio_agent") {
        std::string agent = promptArgs.get("agent").asString();
        if (agent.empty()) return rpcError(id, -32602, "prompt studio_agent requires the argument 'agent'");
        Json brief = callTool("studio_agent_brief", Json::object({{"agent", agent}, {"loop_member", true}}));
        bool isError = false;
        std::string text = toolPayload(brief, false, isError);
        if (isError) return rpcError(id, -32602, text);
        const Json& prompt = brief.get("structuredContent").get("system_prompt");
        if (prompt.isString()) text = prompt.asString();
        text += "\n\nIdentify as this agent by passing as:\"" + agent + "\" on studio_* calls.\n" + assignment;
        return result("Studio agent " + agent, text);
    }
    if (startsWith(name, "role_")) {
        for (const auto* a : embeddedAssetsUnder(kAgentsPrefix)) {
            MarkdownDoc doc = parseMarkdownDoc(a->content);
            if ("role_" + replaceAll(doc.get("name"), "-", "_") == name) return result(doc.get("description"), doc.body + assignment);
        }
    }
    for (const auto* a : embeddedAssetsUnder(kCommandsPrefix)) {
        if (replaceAll(stem(a->path), "-", "_") != name) continue;
        MarkdownDoc doc = parseMarkdownDoc(a->content);
        return result(doc.get("description"), replaceAll(doc.body, "$ARGUMENTS", given.empty() ? "(none given: ask the human what they want)" : given));
    }
    return rpcError(id, -32602, "Unknown prompt '" + name + "' (prompts/list shows what exists)");
}

}  // namespace sky
