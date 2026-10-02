#include <doctest/doctest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <thread>

#include "skywalker/agent/McpServer.h"
#include "skywalker/agent/SocketServer.h"
#include "skywalker/engine/Engine.h"

using namespace sky;

namespace {

struct Fixture {
    Fixture() {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("MCP", true);
        session = std::make_unique<McpSession>(engine->tools(), [this](const std::string& t, const Json& a, const std::string& actor) {
            return engine->callTool(t, a, actor).toMcp();
        });
    }
    Json send(const std::string& msg) {
        auto r = session->handle(msg);
        REQUIRE(r.has_value());
        return Json::parse(*r).value();
    }
    std::unique_ptr<Engine> engine;
    std::unique_ptr<McpSession> session;
};

}  // namespace

TEST_CASE("mcp: initialize negotiates protocol version and reports capabilities") {
    Fixture f;
    Json r = f.send(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude code","version":"1"}}})");
    CHECK(r.get("id").asInt() == 1);
    CHECK(r.get("result").get("protocolVersion").asString() == "2025-06-18");
    CHECK(r.get("result").get("capabilities").contains("tools"));
    CHECK(r.get("result").get("serverInfo").get("name").asString() == "skywalker");
    CHECK(f.session->clientName() == "claude-code");

    Json unknown = f.send(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})");
    CHECK(unknown.get("result").get("protocolVersion").asString() == McpSession::latestProtocolVersion());
}

TEST_CASE("mcp: notifications get no response; ping works; unknown methods error") {
    Fixture f;
    CHECK_FALSE(f.session->handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
    CHECK(f.session->initialized());
    CHECK(f.send(R"({"jsonrpc":"2.0","id":"a","method":"ping"})").get("result").isObject());
    Json e = f.send(R"({"jsonrpc":"2.0","id":3,"method":"nope"})");
    CHECK(e.get("error").get("code").asInt() == -32601);
    Json p = f.send("{not json");
    CHECK(p.get("error").get("code").asInt() == -32700);
}

TEST_CASE("mcp: tools/list and tools/call (text, structured, image content)") {
    Fixture f;
    f.send(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"tester"}}})");
    Json list = f.send(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    CHECK(list.get("result").get("tools").size() >= 30);

    Json call = f.send(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"entity_create","arguments":{"name":"Tree","mesh":"cone"}}})");
    CHECK_FALSE(call.get("result").get("isError").asBool());
    CHECK(call.get("result").get("structuredContent").get("name").asString() == "Tree");
    CHECK(f.engine->history().lastCommitted()->actor == "mcp:tester");

    Json cap = f.send(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"viewport_capture","arguments":{"width":64,"height":64}}})");
    bool image = false;
    for (const auto& c : cap.get("result").get("content").elements()) image = image || c.get("type").asString() == "image";
    CHECK(image);

    Json err = f.send(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"entity_get","arguments":{"entity":"Nope"}}})");
    CHECK(err.get("result").get("isError").asBool());

    Json bad = f.send(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"no_such_tool"}})");
    CHECK(bad.get("error").get("code").asInt() == -32602);
}

TEST_CASE("mcp: socket server round trip with main-thread hop") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    (void)engine.newScene("Socket", true);
    std::string path = (std::filesystem::temp_directory_path() / ("sky-test-" + std::to_string(::getpid()) + ".sock")).string();
    REQUIRE(engine.startAgentServer(path).ok());

    auto fd = connectUnixSocket(path);
    REQUIRE(fd.ok());
    std::atomic<bool> done{false};
    std::string response;
    std::thread client([&] {
        writeAll(fd->get(), R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scene_overview","arguments":{}}})" "\n");
        LineReader reader(fd->get());
        reader.next(response);
        done = true;
    });
    // Emulate the editor's frame loop pumping jobs on the main thread.
    for (int i = 0; i < 500 && !done; ++i) {
        engine.update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    auto r = Json::parse(response);
    REQUIRE(r.ok());
    CHECK(r->get("result").get("content")[0].get("text").asString().find("Cube") != std::string::npos);
    engine.stopAgentServer();
    CHECK_FALSE(std::filesystem::exists(path));
}
