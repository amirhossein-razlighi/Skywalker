// Agent link tools: follow the engine's event stream (events_poll) and serve tools from another
// process (tool_host_*), so external harnesses such as the Python agent layer (python/,
// docs/PYTHON_AGENTS.md) can watch the studio live and offer their own tools to every agent.
//
// Waiting (long polls) happens in the DeferredWork part of a tool, which runs on the agent
// connection's thread: the engine's main thread is never blocked. Called on the main thread
// (stdio `skywalker mcp`, the CLI runner), the polls simply do not wait.

#include <algorithm>

#include "ToolHelpers.h"
#include "skywalker/agent/EventLog.h"
#include "skywalker/agent/ToolHost.h"

namespace sky::tools {

namespace {

using namespace schema;

Json strings(std::string description) { return array(Json::object({{"type", "string"}}), std::move(description)); }

std::vector<std::string> toStrings(const Json& j) {
    std::vector<std::string> out;
    for (const auto& v : j.elements()) {
        if (v.isString()) out.push_back(v.asString());
    }
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

/// "studio" matches every studio event; "studio.message" only studio events of kind "message".
bool typeMatches(const std::vector<std::string>& types, const Json& e) {
    const std::string& type = e.get("type").asString();
    const std::string& kind = e.get("kind").asString();
    for (const auto& t : types) {
        if (t == type || (!kind.empty() && t == type + "." + kind)) return true;
    }
    return false;
}

bool actorMatches(const std::vector<std::string>& actors, const Json& e) {
    const std::string& actor = e.get("actor").asString();
    for (const auto& a : actors) {
        if (actor == a || startsWith(actor, a)) return true;
    }
    return false;
}

std::chrono::milliseconds waitFor(const Json& a, int64_t fallback) {
    return std::chrono::milliseconds(std::clamp<int64_t>(a.get("wait_ms").asInt(fallback), 0, 25000));
}

}  // namespace

void addAgentLinkTools(Engine& engine, ToolRegistry& reg) {
    std::shared_ptr<EventLog> log = engine.eventLogShared();
    std::shared_ptr<ToolHost> host = engine.toolHostShared();
    // Every agent-link tool is plumbing for external harnesses: no activity-feed entry per call, and the
    // studio crew does not get them (registration announces itself with a "tool_host" event).
    auto addQuiet = [&reg](ToolDef def) {
        def.quiet = true;
        reg.add(std::move(def));
    };

    ToolDef poll{
        "events_poll", "Follow engine events",
        "Read the engine's live event stream (the same events the editor's Activity feed shows: tool calls with their actor, "
        "studio changes such as messages, tasks, feedback, decisions and loops, play state, selection, scene loads/saves, "
        "asset changes) after a cursor, without consuming them. Pass the returned `next` as `since` on the next call; "
        "since=0 starts with everything still retained (the last 4096 events). Filter with types (\"tool\", \"studio\", "
        "\"studio.message\", \"play_state\", ...), actors (prefix match, e.g. \"mcp:sky-agents\") or exclude_actors. "
        "wait_ms > 0 long-polls until a matching event arrives (agent socket connections only; use a dedicated "
        "connection, the wait blocks that connection). Example: {since: 120, types: [\"studio.message\"], wait_ms: 20000}.",
        "agent",
        object({{"since", integer("Cursor: the `next` of the previous call (0 = from the oldest retained event)")},
                {"types", strings("Only these event types (\"type\" or \"type.kind\", e.g. \"studio.task\")")},
                {"actors", strings("Only events whose actor starts with one of these")},
                {"exclude_actors", strings("Skip events whose actor starts with one of these (e.g. your own)")},
                {"limit", integer("Max events returned (default 200, max 1000)")},
                {"wait_ms", integer("Long-poll: wait up to this long for a matching event (max 25000, default 0)")}}),
        false, false,
        [log, host](const Json& a, ToolContext&) -> ToolResult {
            auto since = static_cast<uint64_t>(std::max<int64_t>(0, a.get("since").asInt()));
            size_t limit = static_cast<size_t>(std::clamp<int64_t>(a.get("limit").asInt(200), 1, 1000));
            std::vector<std::string> types = toStrings(a.get("types"));
            std::vector<std::string> actors = toStrings(a.get("actors"));
            std::vector<std::string> exclude = toStrings(a.get("exclude_actors"));
            EventLog::Filter filter;
            if (!types.empty() || !actors.empty() || !exclude.empty()) {
                filter = [types, actors, exclude](const Json& e) {
                    if (!types.empty() && !typeMatches(types, e)) return false;
                    if (!actors.empty() && !actorMatches(actors, e)) return false;
                    if (!exclude.empty() && actorMatches(exclude, e)) return false;
                    return true;
                };
            }
            auto wait = waitFor(a, 0);
            auto batch = std::make_shared<EventLog::Batch>();
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            auto finish = [log, batch] {
                Json events = Json::array();
                for (auto& e : batch->events) events.push(std::move(e));
                size_t n = events.size();
                Json out = Json::object({{"events", std::move(events)},
                                         {"next", batch->next},
                                         {"last_seq", log->lastSeq()},
                                         {"truncated", batch->truncated},
                                         {"more", batch->more},
                                         {"reset", batch->reset}});
                std::string summary = std::to_string(n) + " event(s), next=" + std::to_string(batch->next);
                if (batch->truncated) summary += " (older events were dropped before you read them)";
                return ToolResult::json(std::move(out), summary);
            };
            return ToolResult::defer(
                [log, host, batch, since, limit, filter, wait, cancelled] {
                    // On the main thread nothing can be emitted while we wait: just read.
                    bool canWait = wait.count() > 0 && !host->onMainThread();
                    *batch = canWait ? log->wait(since, limit, wait, filter, cancelled.get()) : log->since(since, limit, filter);
                },
                finish,
                [log, cancelled] {
                    cancelled->store(true);
                    log->wakeAll();
                });
        }};
    poll.quiet = true;
    reg.add(std::move(poll));

    // ---------------------------------------------------------------- tool host
    ToolDef registerTools{
        "tool_host_register", "Serve tools from your process",
        "Offer tools implemented in your own process (a Python agent harness, a script) to every agent connected to this "
        "engine: each becomes an engine tool named py_<name> that appears in tools/list. Calls are queued for you; fetch "
        "them with tool_host_poll on a dedicated connection and answer each with tool_host_reply. Keep polling: a host "
        "that has not polled for ttl_seconds (default 60) is dropped with its tools. Re-register with the returned host "
        "id to change the tool set. Example: {label: \"sky-agents memory\", tools: [{name: \"memory_recall\", description: "
        "\"Search the team's shared memory\", input_schema: {type: \"object\", properties: {query: {type: \"string\"}}, "
        "required: [\"query\"]}}]}.",
        "agent",
        object({{"tools", array(Json::object({{"type", "object"},
                                               {"properties", Json::object({{"name", string("Tool name (py_ is prepended)")},
                                                                            {"title", string("Short title")},
                                                                            {"description", string("What it does, for language models")},
                                                                            {"input_schema", Json::object({{"type", "object"}, {"description", "JSON Schema of the arguments"}})},
                                                                            {"mutates", boolean("Changes the project or game (default false)")}})},
                                               {"required", Json::array({"name"})}}),
                                 "The tools to serve")},
                {"host", string("Your host id from an earlier registration (to update its tool set)")},
                {"label", string("Who serves these tools, shown in descriptions and tool_host_list")},
                {"ttl_seconds", number("Drop this host after this long without a poll (30-3600, default 60)")},
                {"replace", boolean("Take over names another host serves (default false)")}},
               {"tools"}),
        true, false,
        [&engine, host](const Json& a, ToolContext& ctx) -> ToolResult {
            std::vector<ToolHost::ToolSpec> specs;
            for (const auto& t : a.get("tools").elements()) {
                specs.push_back({t.get("name").asString(), t.get("title").asString(), t.get("description").asString(),
                                 t.get("input_schema"), t.get("mutates").asBool()});
            }
            auto id = host->registerTools(a.get("host").asString(), ctx.actor, a.get("label").asString(), specs,
                                          a.get("ttl_seconds").asNumber(0), a.get("replace").asBool());
            if (!id) return ToolResult::error(id.error());
            Json names = Json::array();
            for (const auto& s : specs) names.push(ToolHost::publicName(s.name));
            engine.emitEvent(Json::object({{"type", "tool_host"}, {"action", "register"}, {"host", *id}, {"actor", ctx.actor}, {"tools", names}}));
            return ToolResult::json(Json::object({{"host", *id}, {"tools", names}}),
                                    "serving " + std::to_string(specs.size()) + " tool(s) as host " + *id + "; now poll with tool_host_poll");
        }};
    registerTools.quiet = true;  // announces itself with a tool_host event instead
    reg.add(std::move(registerTools));

    ToolDef hostPoll{
        "tool_host_poll", "Fetch calls for your tools",
        "Fetch the calls other agents made to your py_* tools (see tool_host_register): [{call, tool, args, actor}]. "
        "wait_ms long-polls until a call arrives (default 20000, max 25000); use a connection dedicated to polling. Answer "
        "every call with tool_host_reply. Polling also keeps your host alive.",
        "agent",
        object({{"host", string("Your host id")},
                {"wait_ms", integer("Wait up to this long for a call (default 20000, max 25000)")},
                {"max", integer("Max calls returned (default 8, max 64)")}},
               {"host"}),
        false, false,
        [host](const Json& a, ToolContext&) -> ToolResult {
            std::string id = a.get("host").asString();
            size_t max = static_cast<size_t>(std::clamp<int64_t>(a.get("max").asInt(8), 1, 64));
            auto wait = waitFor(a, 20000);
            auto out = std::make_shared<Result<std::vector<ToolHost::Call>>>(std::vector<ToolHost::Call>{});
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            return ToolResult::defer(
                [host, id, max, wait, out, cancelled] {
                    *out = host->poll(id, max, host->onMainThread() ? std::chrono::milliseconds(0) : wait, cancelled.get());
                },
                [out] {
                    if (!*out) return ToolResult::error(out->error());
                    Json calls = Json::array();
                    for (const auto& c : out->value()) calls.push(c.toJson());
                    size_t n = calls.size();
                    return ToolResult::json(Json::object({{"calls", std::move(calls)}}), std::to_string(n) + " call(s)");
                },
                [host, cancelled] {
                    cancelled->store(true);
                    host->wakeAll();
                });
        }};
    hostPoll.quiet = true;
    reg.add(std::move(hostPoll));

    ToolDef reply{
        "tool_host_reply", "Answer a tool call",
        "Answer one call fetched with tool_host_poll. Give text (what the calling model reads), optionally structured "
        "JSON and PNG images (base64), and is_error=true when the call failed (say why in text, with a hint).",
        "agent",
        object({{"host", string("Your host id")},
                {"call", integer("The call id from tool_host_poll")},
                {"text", string("The result as text")},
                {"structured", Json::object({{"type", "object"}, {"description", "Machine-readable result"}})},
                {"images", strings("PNG images, base64")},
                {"is_error", boolean("The call failed")}},
               {"host", "call"}),
        false, false,
        [host](const Json& a, ToolContext&) -> ToolResult {
            Json content = Json::array();
            std::string text = a.get("text").asString();
            if (text.empty() && a.get("structured").isObject()) text = a.get("structured").dump();
            content.push(Json::object({{"type", "text"}, {"text", text}}));
            for (const auto& img : a.get("images").elements()) {
                content.push(Json::object({{"type", "image"}, {"data", img.asString()}, {"mimeType", "image/png"}}));
            }
            Json mcp = Json::object({{"content", content}, {"isError", a.get("is_error").asBool()}});
            if (a.get("structured").isObject()) mcp["structuredContent"] = a.get("structured");
            if (Status s = host->reply(a.get("host").asString(), static_cast<uint64_t>(a.get("call").asInt()), std::move(mcp)); !s) {
                return ToolResult::error(s.error());
            }
            return ToolResult::text("delivered");
        }};
    reply.quiet = true;
    reg.add(std::move(reply));

    addQuiet({"tool_host_unregister", "Stop serving tools",
             "Stop serving the py_* tools of a host (they disappear from tools/list; calls still waiting fail). Hosts that "
             "stop polling expire on their own after ttl_seconds.",
             "agent", object({{"host", string("Host id")}}, {"host"}), true, false,
             [&engine, host](const Json& a, ToolContext& ctx) -> ToolResult {
                 std::string id = a.get("host").asString();
                 if (!host->unregister(id, "the host unregistered")) {
                     return ToolResult::error(Error::make("not_found", "no tool host " + id, "tool_host_list shows the live hosts"));
                 }
                 engine.emitEvent(Json::object({{"type", "tool_host"}, {"action", "unregister"}, {"host", id}, {"actor", ctx.actor}}));
                 return ToolResult::text("host " + id + " stopped serving tools");
             }});

    addQuiet({"tool_host_list", "Tool hosts",
             "Which external processes serve py_* tools: each host's label, owner (actor), tools, queued and in-flight calls, "
             "calls served and seconds since its last poll; plus the event stream's cursor range (events_poll).",
             "agent", object({}), false, false,
             [host, log](const Json&, ToolContext&) -> ToolResult {
                 host->expireStale();
                 Json out = host->info();
                 out["events"] = Json::object({{"last_seq", log->lastSeq()}, {"oldest_seq", log->oldestSeq()},
                                               {"retained", log->size()}, {"capacity", log->capacity()}});
                 std::string text;
                 for (const auto& h : out.get("hosts").elements()) {
                     text += h.get("host").asString() + " " + h.get("label").asString() + ": " +
                             std::to_string(h.get("tools").size()) + " tool(s), " + std::to_string(h.get("served").asInt()) + " served\n";
                 }
                 return ToolResult::json(std::move(out), text.empty() ? "no tool hosts" : text);
             }});
}

}  // namespace sky::tools
