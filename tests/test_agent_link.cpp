// Agent link: the followable event log (events_poll), tools served by external processes
// (tool_host_*, py_* tools) and the studio additions external harnesses use (structured
// messages, threads, presence). See docs/PYTHON_AGENTS.md.

#include <doctest/doctest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

#include "skywalker/agent/EventLog.h"
#include "skywalker/agent/SocketServer.h"
#include "skywalker/agent/ToolHost.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path dir;
    TempDir() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("sky_link_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::unique_ptr<Engine> makeEngine(const std::string& project = ".") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = project;
    cfg.audio = audio::AudioMode::Null;
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Link", true);
    return e;
}

/// A minimal MCP client over the agent socket (one request at a time).
struct Client {
    UniqueFd fd;
    std::unique_ptr<LineReader> reader;
    int nextId = 1;
    explicit Client(const std::string& path, const std::string& name) {
        auto r = connectUnixSocket(path);
        if (r.ok()) fd = std::move(*r);
        reader = std::make_unique<LineReader>(fd.get());
        Json init = Json::object({{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", "initialize"},
                                  {"params", Json::object({{"protocolVersion", "2025-06-18"},
                                                           {"clientInfo", Json::object({{"name", name}, {"version", "1"}})}})}});
        (void)request(init);
    }
    /// Null when the server went away (no REQUIRE here: clients run on their own threads).
    Json request(const Json& msg) {
        if (!writeAll(fd.get(), msg.dump() + "\n")) return Json();
        std::string line;
        if (!reader->next(line)) return Json();
        auto parsed = Json::parse(line);
        return parsed ? parsed.value() : Json();
    }
    /// tools/call; returns the CallToolResult.
    Json call(const std::string& tool, const Json& args) {
        Json msg = Json::object({{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", "tools/call"},
                                 {"params", Json::object({{"name", tool}, {"arguments", args}})}});
        Json r = request(msg);
        return r.get("result");
    }
};

/// Pumps the engine (the editor's frame loop) until `done`.
void pumpUntil(Engine& engine, const std::atomic<bool>& done, int maxMs = 10000) {
    for (int i = 0; i < maxMs / 2 && !done; ++i) {
        engine.update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::string socketPath(const char* tag) {
    return (fs::temp_directory_path() / ("sky-link-" + std::string(tag) + std::to_string(::getpid()) + ".sock")).string();
}

}  // namespace

TEST_CASE("agent link: the event log numbers, filters, bounds and resumes") {
    EventLog log(16);
    for (int i = 0; i < 5; ++i) log.append(Json::object({{"type", i % 2 ? "tool" : "studio"}, {"i", i}}));
    auto all = log.since(0, 100);
    REQUIRE(all.events.size() == 5);
    CHECK(all.events[0].get("seq").asInt() == 1);
    CHECK(all.events[0].contains("time"));
    CHECK(all.next == 5);
    CHECK_FALSE(all.truncated);

    auto limited = log.since(0, 2);
    CHECK(limited.events.size() == 2);
    CHECK(limited.more);
    CHECK(limited.next == 2);

    auto studio = log.since(0, 100, [](const Json& e) { return e.get("type").asString() == "studio"; });
    CHECK(studio.events.size() == 3);
    CHECK(studio.next == 5);  // the cursor moves past filtered-out events too

    CHECK(log.since(5, 100).events.empty());
    // A cursor from an earlier engine session restarts from the oldest retained event.
    auto reset = log.since(999, 100);
    CHECK(reset.reset);
    CHECK(reset.events.size() == 5);

    for (int i = 0; i < 30; ++i) log.append(Json::object({{"type", "tool"}}));
    CHECK(log.size() == 16);
    CHECK(log.oldestSeq() == 20);
    auto late = log.since(3, 100);
    CHECK(late.truncated);
    CHECK(late.events.size() == 16);
}

TEST_CASE("agent link: waiting readers wake on new events, time out, and can be cancelled") {
    EventLog log;
    auto t0 = std::chrono::steady_clock::now();
    auto none = log.wait(0, 10, std::chrono::milliseconds(30));
    CHECK(none.events.empty());
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(25));

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        log.append(Json::object({{"type", "tool"}}));
        log.append(Json::object({{"type", "studio"}}));
    });
    auto got = log.wait(0, 10, std::chrono::seconds(5), [](const Json& e) { return e.get("type").asString() == "studio"; });
    writer.join();
    REQUIRE(got.events.size() == 1);
    CHECK(got.events[0].get("type").asString() == "studio");

    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cancel = true;
        log.wakeAll();
    });
    auto t1 = std::chrono::steady_clock::now();
    auto cancelled = log.wait(log.lastSeq(), 10, std::chrono::seconds(10), {}, &cancel);
    canceller.join();
    CHECK(cancelled.events.empty());
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(5));
}

TEST_CASE("agent link: events_poll follows the activity stream and quiet tools stay out of it") {
    auto engine = makeEngine();
    ToolResult first = engine->callTool("events_poll", Json::object(), "mcp:test");
    REQUIRE_FALSE(first.isError);
    uint64_t cursor = static_cast<uint64_t>(first.structured.get("next").asInt());
    engine->callTool("entity_create", Json::object({{"name", "Probe"}, {"mesh", "cube"}}), "mcp:alice");
    engine->callTool("events_poll", Json::object({{"since", static_cast<int64_t>(cursor)}}), "mcp:test");  // quiet: no event
    ToolResult r = engine->callTool("events_poll", Json::object({{"since", static_cast<int64_t>(cursor)}, {"types", Json::array({"tool"})}}),
                                    "mcp:test");
    REQUIRE_FALSE(r.isError);
    const Json& events = r.structured.get("events");
    REQUIRE(events.size() == 1);
    CHECK(events[0].get("tool").asString() == "entity_create");
    CHECK(events[0].get("actor").asString() == "mcp:alice");
    // Actor filters.
    ToolResult mine = engine->callTool(
        "events_poll", Json::object({{"since", static_cast<int64_t>(cursor)}, {"exclude_actors", Json::array({"mcp:alice"})}}), "mcp:test");
    for (const auto& e : mine.structured.get("events").elements()) CHECK(e.get("actor").asString() != "mcp:alice");
    // On the engine's own thread a long poll cannot wait (nothing could be emitted meanwhile): it returns at once.
    auto t0 = std::chrono::steady_clock::now();
    ToolResult waited = engine->callTool("events_poll", Json::object({{"since", r.structured.get("next")}, {"wait_ms", 20000}}), "mcp:test");
    CHECK_FALSE(waited.isError);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

TEST_CASE("agent link: tool hosts register py_* tools, validate names and unregister") {
    auto engine = makeEngine();
    Json spec = Json::object({{"tools", Json::array({Json::object({{"name", "memory_recall"},
                                                                    {"description", "Search memory"},
                                                                    {"input_schema", Json::object({{"type", "object"},
                                                                                                   {"properties", Json::object({{"query", Json::object({{"type", "string"}})}})},
                                                                                                   {"required", Json::array({"query"})}})}})})},
                              {"label", "test harness"}});
    ToolResult reg = engine->callTool("tool_host_register", spec, "mcp:harness");
    REQUIRE_FALSE(reg.isError);
    std::string host = reg.structured.get("host").asString();
    CHECK(host == "H-1");
    const ToolDef* def = engine->tools().find("py_memory_recall");
    REQUIRE(def != nullptr);
    CHECK(def->category == "python");
    bool listed = false;
    Json catalogue = engine->tools().listJson();
    for (const auto& t : catalogue.get("tools").elements()) listed = listed || t.get("name").asString() == "py_memory_recall";
    CHECK(listed);

    // Arguments are validated against the host's schema before anything is queued.
    ToolResult bad = engine->callTool("py_memory_recall", Json::object(), "mcp:other");
    CHECK(bad.isError);
    // Called on the engine's own thread, a py_* tool cannot wait for its host: it fails fast with a hint.
    ToolResult inline_ = engine->callTool("py_memory_recall", Json::object({{"query", "lava"}}), "mcp:other");
    CHECK(inline_.isError);
    CHECK(inline_.content.front().text.find("agent socket") != std::string::npos);

    // Another host cannot silently take the name; invalid names are refused.
    CHECK(engine->callTool("tool_host_register", spec, "mcp:intruder").isError);
    Json badName = Json::object({{"tools", Json::array({Json::object({{"name", "has space"}})})}});
    CHECK(engine->callTool("tool_host_register", badName, "mcp:harness").isError);
    // Re-registering the same host replaces its tool set.
    Json renamed = Json::object({{"host", host}, {"tools", Json::array({Json::object({{"name", "memory_search"}})})}});
    REQUIRE_FALSE(engine->callTool("tool_host_register", renamed, "mcp:harness").isError);
    CHECK(engine->tools().find("py_memory_recall") == nullptr);
    CHECK(engine->tools().find("py_memory_search") != nullptr);

    ToolResult list = engine->callTool("tool_host_list", Json::object(), "mcp:test");
    CHECK(list.structured.get("hosts").size() == 1);
    CHECK(list.structured.get("events").get("last_seq").asInt() > 0);

    REQUIRE_FALSE(engine->callTool("tool_host_unregister", Json::object({{"host", host}}), "mcp:harness").isError);
    CHECK(engine->tools().find("py_memory_search") == nullptr);
    CHECK(engine->toolHost().stats().get("hosts").asInt() == 0);
    CHECK(engine->callTool("tool_host_unregister", Json::object({{"host", host}}), "mcp:harness").isError);
}

TEST_CASE("agent link: a py_* call over the socket is served by another connection") {
    auto engine = makeEngine();
    std::string path = socketPath("host");
    REQUIRE(engine->startAgentServer(path).ok());
    std::atomic<bool> hostReady{false}, done{false};
    std::string hostId;
    Json callerResult;
    Json polledCall;
    Json eventsSeen;

    std::thread hostThread([&] {
        Client host(path, "sky-agents/host");
        Json reg = host.call("tool_host_register",
                             Json::object({{"label", "test"}, {"tools", Json::array({Json::object({{"name", "echo"}, {"description", "Echo back"}})})}}));
        hostId = reg.get("structuredContent").get("host").asString();
        hostReady = true;
        Json polled = host.call("tool_host_poll", Json::object({{"host", hostId}, {"wait_ms", 5000}}));
        const Json& calls = polled.get("structuredContent").get("calls");
        if (calls.size() == 1) {
            polledCall = calls[0];
            host.call("tool_host_reply", Json::object({{"host", hostId},
                                                       {"call", calls[0].get("call")},
                                                       {"text", "echo: " + calls[0].get("args").get("word").asString()},
                                                       {"structured", Json::object({{"word", calls[0].get("args").get("word")}})}}));
        }
    });
    std::thread callerThread([&] {
        while (!hostReady) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Client caller(path, "claude-code/mira");
        callerResult = caller.call("py_echo", Json::object({{"word", "lava"}}));
        Json ev = caller.call("events_poll", Json::object({{"since", 0}, {"types", Json::array({"tool"})}}));
        eventsSeen = ev.get("structuredContent").get("events");
        done = true;
    });
    pumpUntil(*engine, done);
    hostThread.join();
    callerThread.join();
    CHECK(polledCall.get("actor").asString() == "mcp:claude-code/mira");
    CHECK(callerResult.get("isError").asBool() == false);
    CHECK(callerResult.get("content")[0].get("text").asString() == "echo: lava");
    CHECK(callerResult.get("structuredContent").get("word").asString() == "lava");
    // The caller's py_echo is in the activity stream, attributed; the host's plumbing is not.
    bool sawEcho = false;
    for (const auto& e : eventsSeen.elements()) {
        std::string tool = e.get("tool").asString();
        CHECK(tool.rfind("tool_host_", 0) != 0);
        sawEcho = sawEcho || (tool == "py_echo" && e.get("actor").asString() == "mcp:claude-code/mira");
    }
    CHECK(sawEcho);
    CHECK(engine->toolHost().stats().get("served").asInt() == 1);
    engine->stopAgentServer();
}

TEST_CASE("agent link: a long poll on a connection returns as soon as an event arrives, and stopping the server ends waits") {
    TempDir dir;  // the studio writes its message log into the project
    auto engine = makeEngine(dir.dir.string());
    std::string path = socketPath("poll");
    REQUIRE(engine->startAgentServer(path).ok());
    std::atomic<bool> done{false};
    Json got;
    uint64_t cursor = engine->eventLog().lastSeq();
    std::thread follower([&] {
        Client c(path, "sky-agents/follower");
        Json r = c.call("events_poll", Json::object({{"since", static_cast<int64_t>(cursor)}, {"types", Json::array({"studio.message"})}, {"wait_ms", 8000}}));
        got = r.get("structuredContent");
        done = true;
    });
    // Let the follower start waiting, then post a message from "the editor".
    for (int i = 0; i < 50; ++i) {
        engine->update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto t0 = std::chrono::steady_clock::now();
    engine->callTool("studio_message_send", Json::object({{"text", "hello agents"}, {"kind", "inform"}, {"data", Json::object({{"k", 1}})}}), "user");
    pumpUntil(*engine, done);
    follower.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
    REQUIRE(got.get("events").size() == 1);
    const Json& msg = got.get("events")[0].get("message");
    CHECK(msg.get("text").asString() == "hello agents");
    CHECK(msg.get("kind").asString() == "inform");
    CHECK(msg.get("data").get("k").asInt() == 1);

    // A waiting host poll does not hold up stopping the server.
    std::atomic<bool> polled{false};
    std::thread waiter([&] {
        Client c(path, "sky-agents/host");
        Json reg = c.call("tool_host_register", Json::object({{"tools", Json::array({Json::object({{"name", "slow"}})})}}));
        polled = true;
        (void)c.call("tool_host_poll", Json::object({{"host", reg.get("structuredContent").get("host")}, {"wait_ms", 25000}}));
    });
    pumpUntil(*engine, polled);
    for (int i = 0; i < 25; ++i) {
        engine->update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto t1 = std::chrono::steady_clock::now();
    engine->stopAgentServer();
    waiter.join();
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(5));
}

TEST_CASE("agent link: structured studio messages, threads and presence") {
    TempDir dir;
    auto engine = makeEngine(dir.dir.string());
    REQUIRE_FALSE(engine->callTool("studio_team_template", Json::object({{"template", "indie_trio"}}), "user").isError);
    ToolResult root = engine->callTool("studio_message_send",
                                       Json::object({{"text", "Can you widen the bridge?"},
                                                     {"to", Json::array({"stratus"})},
                                                     {"kind", "request"},
                                                     {"data", Json::object({{"task", "widen"}, {"width", 3}})},
                                                     {"as", "nimbus"}}),
                                       "mcp:sky-agents");
    REQUIRE_FALSE(root.isError);
    std::string rootId = root.structured.get("id").asString();
    CHECK(root.structured.get("kind").asString() == "request");
    CHECK(root.structured.get("data").get("width").asInt() == 3);
    ToolResult answer = engine->callTool("studio_message_send",
                                         Json::object({{"text", "done"}, {"reply_to", rootId}, {"kind", "result"}, {"as", "stratus"}}),
                                         "mcp:sky-agents");
    REQUIRE_FALSE(answer.isError);
    engine->callTool("studio_message_send", Json::object({{"text", "unrelated"}}), "user");
    ToolResult thread = engine->callTool("studio_inbox", Json::object({{"thread", rootId}}), "user");
    REQUIRE(thread.structured.get("messages").size() == 2);
    CHECK(thread.structured.get("messages")[1].get("kind").asString() == "result");
    ToolResult after = engine->callTool("studio_inbox", Json::object({{"thread", rootId}, {"after", rootId}}), "user");
    CHECK(after.structured.get("messages").size() == 1);
    ToolResult chan = engine->callTool("studio_inbox", Json::object({{"channel", "general"}, {"after", answer.structured.get("id")}}), "user");
    CHECK(chan.structured.get("messages").size() == 1);

    // Messages survive a reload with their kind and payload.
    engine.reset();
    engine = makeEngine(dir.dir.string());
    ToolResult reloaded = engine->callTool("studio_inbox", Json::object({{"thread", rootId}}), "user");
    REQUIRE(reloaded.structured.get("messages").size() == 2);
    CHECK(reloaded.structured.get("messages")[0].get("data").get("task").asString() == "widen");

    ToolResult p = engine->callTool("studio_presence", Json::object({{"status", "working"}, {"activity", "T-1 bridge"}, {"as", "stratus"}}), "mcp:sky-agents");
    REQUIRE_FALSE(p.isError);
    CHECK(p.structured.get("presence").get("status").asString() == "working");
    ToolResult roster = engine->callTool("studio_agent_list", Json::object(), "user");
    bool working = false;
    for (const auto& a : roster.structured.get("agents").elements()) {
        if (a.get("id").asString() == "stratus") working = a.get("status").get("status").asString() == "working" || a.dump().find("working") != std::string::npos;
    }
    CHECK(working);
    CHECK(engine->callTool("studio_presence", Json::object({{"status", "idle"}}), "user").isError);  // who?
}
