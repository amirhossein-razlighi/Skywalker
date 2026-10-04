// Custom tools defined by agents (docs/CUSTOM_TOOLS.md): validation, Wander and composite execution,
// capabilities, limits, approval policy, undo, persistence, list_changed notifications, recursion,
// and tools hosted by external clients over a socket and over stdio.

#include <doctest/doctest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "skywalker/agent/CustomTools.h"
#include "skywalker/agent/McpServer.h"
#include "skywalker/agent/SocketServer.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

/// A throwaway project folder (removed at the end).
struct Project {
    Project() {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("sky-custom-tools-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel) << text;
    }
    std::unique_ptr<Engine> engine() const {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.audio = audio::AudioMode::Null;
        cfg.projectDir = dir.string();
        auto e = std::make_unique<Engine>(cfg);
        (void)e->newScene("Tools", false);
        return e;
    }
    fs::path dir;
};

ToolResult call(Engine& e, const std::string& tool, const Json& args, const std::string& actor = "agent:test") {
    return e.callTool(tool, args, actor);
}

std::string text(const ToolResult& r) { return r.content.empty() ? std::string() : r.content.front().text; }

Json parse(const std::string& s) { return Json::parse(s).value(); }

void addEnemies(Engine& e) {
    REQUIRE_FALSE(call(e, "entity_create", parse(R"({"name":"Goblin","mesh":"cube","position":[2,0,0],"tags":["enemy"],"vars":{"hp":3}})"), "user").isError);
    REQUIRE_FALSE(call(e, "entity_create", parse(R"({"name":"Orc","mesh":"cube","position":[20,0,0],"tags":["enemy"],"vars":{"hp":9}})"), "user").isError);
    REQUIRE_FALSE(call(e, "entity_create", parse(R"({"name":"Coin","mesh":"sphere","position":[1,0,0],"tags":["pickup"],"vars":{"rarity":"rare","value":1}})"), "user").isError);
}

const char* kEnemiesNear = R"({
  "name": "enemies_near",
  "description": "Enemies (tag enemy) within a radius of a point, with their hp. Use it before planning an encounter.",
  "input_schema": {"type": "object", "properties": {
      "radius": {"type": "number", "default": 10},
      "center": {"type": "array", "items": {"type": "number"}}}},
  "wander": "fn run(args)\n  let c = (0, 0, 0)\n  if args.center then c = args.center end\n  let out = []\n  for e in find_all(\"enemy\")\n    if distance(e, c) <= args.radius then out.push({name: e.name, hp: e.hp}) end\n  end\n  return {count: out.length, enemies: out}\nend\n",
  "tests": [{"name": "finds the goblin", "setup": [{"tool": "entity_create", "args": {"name": "G2", "tags": ["enemy"], "position": [1, 0, 0], "vars": {"hp": 1}}}],
             "args": {"radius": 5}, "expect": {"result": {"count": {"$gte": 1}}}}]
})";

}  // namespace

TEST_CASE("custom tools: a read-only Wander tool is validated, tested, registered, called and persisted") {
    Project p;
    {
        auto e = p.engine();
        addEnemies(*e);
        ToolResult def = call(*e, "tool_define", parse(kEnemiesNear));
        INFO(text(def));
        REQUIRE_FALSE(def.isError);
        CHECK(def.structured.get("name").asString() == "user_enemies_near");
        CHECK(def.structured.get("status").asString() == "active");
        CHECK(def.structured.get("tests").get("passed").asInt() == 1);
        CHECK(fs::exists(p.dir / "tools/enemies_near.tool.json"));
        CHECK(fs::exists(p.dir / "tools/enemies_near.wander"));
        // The test's setup entity was rolled back.
        CHECK(e->scene().find("G2") == kNoEntity);

        const ToolDef* t = e->tools().find("user_enemies_near");
        REQUIRE(t);
        CHECK(t->origin == "custom");
        CHECK_FALSE(t->mutates);
        bool listed = false;
        const Json catalogue = e->tools().listJson();
        for (const auto& item : catalogue.get("tools").elements()) listed = listed || item.get("name").asString() == "user_enemies_near";
        CHECK(listed);

        ToolResult r = call(*e, "user_enemies_near", parse(R"({"radius": 5})"));
        INFO(text(r));
        REQUIRE_FALSE(r.isError);
        CHECK(r.structured.get("result").get("count").asInt() == 1);
        CHECK(r.structured.get("result").get("enemies")[0].get("name").asString() == "Goblin");
        CHECK(r.structured.get("result").get("enemies")[0].get("hp").asInt() == 3);
        // Defaults from the schema; unknown arguments get did-you-mean errors.
        CHECK(call(*e, "user_enemies_near", Json::object()).structured.get("result").get("count").asInt() == 1);
        ToolResult typo = call(*e, "user_enemies_near", parse(R"({"radus": 5})"));
        CHECK(typo.isError);
        CHECK(text(typo).find("radius") != std::string::npos);
        // No history entry for a read-only call.
        CHECK(e->history().lastCommitted()->actor == "user");

        Json inspect = call(*e, "tool_inspect", parse(R"({"name": "enemies_near"})")).structured;
        CHECK(inspect.get("stats").get("calls").asInt() == 2);  // the misspelled call never reached the tool
        CHECK(inspect.get("code").asString().find("fn run") != std::string::npos);
    }
    // Persistence: a new engine on the same project registers it again.
    auto e2 = p.engine();
    REQUIRE(e2->tools().find("user_enemies_near"));
    CHECK(call(*e2, "tool_list_custom", Json::object()).structured.get("tools")[0].get("status").asString() == "active");
    std::ifstream audit(p.dir / ".skywalker/logs/custom_tools.jsonl");
    std::string line;
    bool sawDefine = false, sawCall = false;
    while (std::getline(audit, line)) {
        sawDefine = sawDefine || line.find("\"define\"") != std::string::npos;
        sawCall = sawCall || line.find("\"call\"") != std::string::npos;
    }
    CHECK(sawDefine);
    CHECK(sawCall);
}

TEST_CASE("custom tools: definitions are validated with clear errors") {
    Project p;
    auto e = p.engine();
    auto define = [&](const std::string& json) { return call(*e, "tool_define", parse(json)); };
    ToolResult shortDesc = define(R"({"name": "x", "description": "too short", "wander": "fn run(args) return 1 end"})");
    CHECK(shortDesc.isError);
    CHECK(text(shortDesc).find("description") != std::string::npos);
    ToolResult badName = define(R"({"name": "Bad Name!", "description": "A tool with an invalid name for testing", "wander": "fn run(args) return 1 end"})");
    CHECK(badName.isError);
    ToolResult dotted = define(R"({"name": "user.dotted", "description": "Dots become underscores in tool names here", "wander": "fn run(args)\n  return 1\nend"})");
    CHECK_FALSE(dotted.isError);
    CHECK(dotted.structured.get("name").asString() == "user_dotted");
    ToolResult schemaType = define(R"({"name": "s", "description": "A tool whose schema has a bad type in it", "input_schema": {"type": "object", "properties": {"n": {"type": "numbr"}}}, "wander": "fn run(args) return 1 end"})");
    CHECK(schemaType.isError);
    CHECK(text(schemaType).find("number") != std::string::npos);  // did you mean
    ToolResult required = define(R"({"name": "s", "description": "Requires a property it does not declare", "input_schema": {"type": "object", "properties": {}, "required": ["n"]}, "wander": "fn run(args) return 1 end"})");
    CHECK(required.isError);
    ToolResult unknownKey = define(R"({"name": "s", "description": "Has a misspelled field in its definition", "capabilites": {}, "wander": "fn run(args) return 1 end"})");
    CHECK(unknownKey.isError);
    CHECK(text(unknownKey).find("capabilities") != std::string::npos);
    ToolResult noRun = define(R"({"name": "s", "description": "Wander code without the run function", "wander": "fn walk(args)\n  return 1\nend"})");
    CHECK(noRun.isError);
    CHECK(text(noRun).find("fn run") != std::string::npos);
    ToolResult compile = define(R"({"name": "s", "description": "Wander code that does not compile at all", "wander": "fn run(args)\n  return nope(\nend"})");
    CHECK(compile.isError);
    CHECK(compile.structured.get("error").asString() == "compile_error");
    ToolResult builtin = define(R"({"name": "s", "description": "Lists a tool that does not exist in calls", "capabilities": {"calls": ["scene_qurey"]}, "wander": "fn run(args) return 1 end"})");
    CHECK(builtin.isError);
    CHECK(text(builtin).find("scene_query") != std::string::npos);
    ToolResult mutating = define(R"({"name": "s", "description": "Lists a mutating tool without the capability", "capabilities": {"calls": ["entity_update"]}, "wander": "fn run(args) return 1 end"})");
    CHECK(mutating.isError);
    CHECK(text(mutating).find("mutate") != std::string::npos);
    ToolResult network = define(R"({"name": "s", "description": "Asks for network access as a Wander tool", "capabilities": {"network": true}, "wander": "fn run(args) return 1 end"})");
    CHECK(network.isError);
    ToolResult writeTools = define(R"({"name": "s", "description": "Wants to write the tools folder itself", "capabilities": {"files": {"write": ["tools/"]}}, "wander": "fn run(args) return 1 end"})");
    CHECK(writeTools.isError);
    ToolResult escape = define(R"({"name": "s", "description": "Wants to read outside the project folder", "capabilities": {"files": {"read": ["../secrets"]}}, "wander": "fn run(args) return 1 end"})");
    CHECK(escape.isError);
    ToolResult lifecycle = define(R"({"name": "s", "description": "Wants to call the tool lifecycle tools", "capabilities": {"calls": ["tool_*"]}, "wander": "fn run(args) return 1 end"})");
    CHECK(lifecycle.isError);
    ToolResult shadow = define(R"({"name": "user_dotted", "kind": "external", "description": "Tries to define an external tool by hand"})");
    CHECK(shadow.isError);
    // Failing tests block the save.
    ToolResult failing = define(R"({"name": "f", "description": "A tool whose own test does not pass", "wander": "fn run(args)\n  return {n: 1}\nend", "tests": [{"name": "wrong", "expect": {"result": {"n": 2}}}]})");
    CHECK(failing.isError);
    CHECK(failing.structured.get("status").asString() == "tests_failed");
    CHECK(failing.structured.get("tests").get("tests")[0].get("failures")[0].asString().find("expected 2") != std::string::npos);
    CHECK_FALSE(fs::exists(p.dir / "tools/f.tool.json"));
    CHECK(e->tools().find("user_f") == nullptr);
}

TEST_CASE("custom tools: capabilities are enforced at run time") {
    Project p;
    auto e = p.engine();
    addEnemies(*e);
    // Allowed to call scene_query only.
    ToolResult def = call(*e, "tool_define", parse(R"({
        "name": "probe", "description": "Calls tools to probe what capabilities allow at run time",
        "input_schema": {"type": "object", "properties": {"tool": {"type": "string"}}},
        "capabilities": {"calls": ["scene_query"]},
        "wander": "fn run(args)\n  return call_tool(args.tool, {})\nend"})"));
    REQUIRE_FALSE(def.isError);
    ToolResult ok = call(*e, "user_probe", parse(R"({"tool": "scene_query"})"));
    INFO(text(ok));
    CHECK_FALSE(ok.isError);
    CHECK(ok.structured.get("result").get("matches").size() == 3);
    ToolResult denied = call(*e, "user_probe", parse(R"({"tool": "entity_get"})"));
    CHECK(denied.isError);
    CHECK(text(denied).find("capability_denied") != std::string::npos);
    ToolResult mutate = call(*e, "user_probe", parse(R"({"tool": "scene_new"})"));
    CHECK(mutate.isError);

    // A read-only tool that writes the scene directly is rolled back and fails.
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "sneaky", "description": "Moves an entity although it is not allowed to mutate",
        "wander": "fn run(args)\n  let g = find(\"Goblin\")\n  g.position = (9, 9, 9)\n  g.hp = 100\n  return 1\nend"})")).isError);
    Json before = e->scene().toJson();
    ToolResult sneaky = call(*e, "user_sneaky", Json::object());
    CHECK(sneaky.isError);
    CHECK(sneaky.structured.get("error").asString() == "capability_denied");
    CHECK(e->scene().toJson() == before);

    // Files: only the declared folders, never outside the project.
    p.write("data/loot.json", R"({"tiers": ["common", "rare"]})");
    p.write("secret/keys.txt", "nope");
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "files", "description": "Reads and writes project files inside its declared folders",
        "input_schema": {"type": "object", "properties": {"read": {"type": "string"}, "write": {"type": "string"}}},
        "capabilities": {"files": {"read": ["data/"], "write": ["exports/"]}},
        "wander": "fn run(args)\n  let out = {}\n  if args.read then out.data = json_parse(read_file(args.read)) end\n  if args.write then write_file(args.write, \"hello\") end\n  return out\nend"})")).isError);
    // Writing files is privileged: it waits for a human under the default policy.
    CHECK(e->tools().find("user_files") == nullptr);
    CHECK(call(*e, "tool_approve", parse(R"({"name": "user_files"})"), "user").structured.get("status").asString() == "active");
    ToolResult read = call(*e, "user_files", parse(R"({"read": "data/loot.json"})"), "user");
    INFO(text(read));
    REQUIRE_FALSE(read.isError);
    CHECK(read.structured.get("result").get("data").get("tiers")[1].asString() == "rare");
    CHECK(call(*e, "user_files", parse(R"({"read": "secret/keys.txt"})"), "user").isError);
    CHECK(call(*e, "user_files", parse(R"({"read": "../etc/passwd"})"), "user").isError);
    CHECK_FALSE(call(*e, "user_files", parse(R"({"write": "exports/out.txt"})"), "user").isError);
    CHECK(fs::exists(p.dir / "exports/out.txt"));
}

TEST_CASE("custom tools: budgets, output caps, call limits and the recursion guard") {
    Project p;
    auto e = p.engine();
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "spin", "description": "Loops forever to check the instruction budget is enforced",
        "limits": {"instructions": 20000},
        "wander": "fn run(args)\n  let n = 0\n  while true\n    n += 1\n  end\n  return n\nend"})")).isError);
    ToolResult spin = call(*e, "user_spin", Json::object());
    CHECK(spin.isError);
    CHECK(text(spin).find("instruction budget") != std::string::npos);

    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "big", "description": "Returns a large result to check the output cap is enforced",
        "limits": {"max_output_bytes": 1000},
        "wander": "fn run(args)\n  let out = []\n  for i in 0..500\n    out.push(\"item number {i}\")\n  end\n  return out\nend"})")).isError);
    ToolResult big = call(*e, "user_big", Json::object());
    CHECK(big.isError);
    CHECK(big.structured.get("error").asString() == "output_too_large");

    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "chatty", "description": "Calls a tool many times to check the nested call limit",
        "capabilities": {"calls": ["engine_info"]}, "limits": {"max_calls": 3},
        "wander": "fn run(args)\n  for i in 0..10\n    call_tool(\"engine_info\")\n  end\n  return 1\nend"})")).isError);
    ToolResult chatty = call(*e, "user_chatty", Json::object());
    CHECK(chatty.isError);
    CHECK(text(chatty).find("call_limit") != std::string::npos);

    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "slow", "description": "Computes for longer than its wall-time limit allows",
        "limits": {"instructions": 50000000, "timeout_ms": 10},
        "wander": "fn run(args)\n  let n = 0\n  for i in 0..2000000\n    n += i % 7\n  end\n  return n\nend"})")).isError);
    ToolResult slow = call(*e, "user_slow", Json::object());
    CHECK(slow.isError);
    CHECK(slow.structured.get("error").asString() == "timeout");

    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "fails", "description": "Stops with its own error message and a hint for the caller",
        "wander": "fn run(args)\n  tool_warn(\"about to fail\")\n  tool_fail(\"radius must be positive\", \"try 10\")\n  return 1\nend"})")).isError);
    ToolResult fails = call(*e, "user_fails", Json::object());
    CHECK(fails.isError);
    CHECK(fails.structured.get("error").asString() == "tool_failed");
    CHECK(fails.structured.get("hint").asString() == "try 10");

    // ping -> pong -> ping ...: cut off at the depth limit.
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({"name": "pong", "description": "Half of a pair of tools that call each other",
        "wander": "fn run(args)\n  return 1\nend"})")).isError);
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({"name": "ping", "description": "Half of a pair of tools that call each other",
        "capabilities": {"calls": ["user_pong"]}, "wander": "fn run(args)\n  return call_tool(\"user_pong\")\nend"})")).isError);
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({"name": "pong", "capabilities": {"calls": ["user_ping"]},
        "wander": "fn run(args)\n  return call_tool(\"user_ping\")\nend"})")).isError);
    ToolResult loop = call(*e, "user_ping", Json::object());
    CHECK(loop.isError);
    CHECK(text(loop).find("recursion_limit") != std::string::npos);
}

TEST_CASE("custom tools: mutating tools need approval, edit as one undoable step, and re-approval after changes") {
    Project p;
    auto e = p.engine();
    addEnemies(*e);
    const char* rebalance = R"({
        "name": "rebalance_pickups", "description": "Sets every pickup's value from its rarity (common 1, rare 5, epic 20).",
        "capabilities": {"mutate": true},
        "wander": "fn run(args)\n  let n = 0\n  for c in find_all(\"pickup\")\n    if c.rarity == \"rare\" then c.value = 5 else c.value = 1 end\n    c.position = c.position + (0, 1, 0)\n    n += 1\n  end\n  return {changed: n}\nend",
        "tests": [{"name": "changes the coin", "expect": {"result": {"changed": 1}}}]})";
    ToolResult def = call(*e, "tool_define", parse(rebalance));
    INFO(text(def));
    REQUIRE_FALSE(def.isError);
    CHECK(def.structured.get("status").asString() == "pending_approval");
    CHECK(def.structured.get("tests").get("passed").asInt() == 1);
    EntityId coin = e->scene().find("Coin");
    CHECK(e->scene().record(coin)->vars.get("value").asInt() == 1);  // the test was a dry run
    CHECK(e->tools().find("user_rebalance_pickups") == nullptr);
    CHECK(call(*e, "tool_list_custom", Json::object()).structured.get("tools")[0].get("status").asString() == "pending_approval");

    // Agents cannot approve; humans can.
    ToolResult agentApproves = call(*e, "tool_approve", parse(R"({"name": "rebalance_pickups"})"), "agent:mira");
    CHECK(agentApproves.isError);
    CHECK(agentApproves.structured.get("error").asString() == "approval_requires_human");
    CHECK(call(*e, "tool_approve", parse(R"({"name": "rebalance_pickups", "note": "looks right"})"), "user").structured.get("status").asString() == "active");

    ToolResult r = call(*e, "user_rebalance_pickups", Json::object(), "agent:mira");
    INFO(text(r));
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("result").get("changed").asInt() == 1);
    CHECK(e->scene().record(coin)->vars.get("value").asInt() == 5);
    CHECK(e->history().lastCommitted()->actor == "agent:mira");
    REQUIRE(call(*e, "history", parse(R"({"action": "undo"})"), "user").isError == false);
    CHECK(e->scene().record(coin)->vars.get("value").asInt() == 1);

    // Changing the code invalidates the approval.
    ToolResult changed = call(*e, "tool_define", parse(R"({"name": "rebalance_pickups", "wander": "fn run(args)\n  return {changed: 0}\nend", "tests": []})"));
    CHECK(changed.structured.get("status").asString() == "pending_approval");
    CHECK(changed.structured.get("version").asInt() == 2);
    CHECK(e->tools().find("user_rebalance_pickups") == nullptr);
    // Disable / enable keep the approval of the current definition.
    call(*e, "tool_approve", parse(R"({"name": "rebalance_pickups"})"), "cli");
    CHECK(call(*e, "tool_enable", parse(R"({"name": "rebalance_pickups", "enabled": false})")).structured.get("status").asString() == "disabled");
    CHECK(e->tools().find("user_rebalance_pickups") == nullptr);
    CHECK(call(*e, "tool_enable", parse(R"({"name": "rebalance_pickups", "enabled": true})")).structured.get("status").asString() == "active");
    // Removal deletes the files.
    CHECK_FALSE(call(*e, "tool_remove", parse(R"({"name": "rebalance_pickups"})")).isError);
    CHECK_FALSE(fs::exists(p.dir / "tools/rebalance_pickups.tool.json"));
    CHECK(e->tools().find("user_rebalance_pickups") == nullptr);
}

TEST_CASE("custom tools: composite pipelines with templates, loops and atomic rollback") {
    Project p;
    auto e = p.engine();
    addEnemies(*e);
    ToolResult def = call(*e, "tool_define", parse(R"({
        "name": "tag_enemies", "description": "Adds a tag to every entity that has another tag, as one undo step.",
        "input_schema": {"type": "object", "properties": {"from": {"type": "string"}, "add": {"type": "string"}}, "required": ["from", "add"]},
        "capabilities": {"mutate": true},
        "steps": [
          {"id": "found", "tool": "scene_query", "args": {"tag": "{{args.from}}"}},
          {"tool": "entity_update", "for_each": "{{steps.found.matches}}", "as": "m",
           "args": {"entity": "{{m.id}}", "tags": ["{{args.from}}", "{{args.add}}"]}}
        ],
        "result": {"tagged": "{{steps.found.matches.length}}", "first": "{{steps.found.matches[0].name}}", "note": "added {{args.add}}"}})"));
    INFO(text(def));
    REQUIRE_FALSE(def.isError);
    CHECK(def.structured.get("capabilities").get("calls").size() == 2);  // derived from the steps
    call(*e, "tool_approve", parse(R"({"name": "tag_enemies"})"), "user");
    ToolResult r = call(*e, "user_tag_enemies", parse(R"({"from": "enemy", "add": "hostile"})"));
    INFO(text(r));
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("result").get("tagged").asInt() == 2);
    CHECK(r.structured.get("result").get("first").asString() == "Goblin");
    CHECK(r.structured.get("result").get("note").asString() == "added hostile");
    CHECK(e->scene().findTagged("hostile").size() == 2);
    size_t entries = e->history().entries().size();
    call(*e, "history", parse(R"({"action": "undo"})"), "user");
    CHECK(e->scene().findTagged("hostile").empty());  // one undo step for the whole pipeline
    CHECK(e->history().cursor() == entries - 1);

    // A failing step rolls back everything before it.
    REQUIRE_FALSE(call(*e, "tool_define", parse(R"({
        "name": "half", "description": "Creates an entity, then fails, to check the whole pipeline rolls back",
        "capabilities": {"mutate": true},
        "steps": [{"tool": "entity_create", "args": {"name": "Temp"}}, {"tool": "entity_update", "args": {"entity": "NoSuchThing", "name": "x"}}]})")).isError);
    call(*e, "tool_approve", parse(R"({"name": "half"})"), "user");
    ToolResult half = call(*e, "user_half", Json::object());
    CHECK(half.isError);
    CHECK(text(half).find("step 1") != std::string::npos);
    CHECK(e->scene().find("Temp") == kNoEntity);

    // Template errors name the missing field.
    ToolResult bad = call(*e, "tool_define", parse(R"({"name": "bad", "description": "Refers to a step that does not exist in its templates",
        "steps": [{"tool": "scene_query", "args": {"tag": "{{steps.nope.x}}"}}]})"));
    CHECK(bad.isError);
}

TEST_CASE("custom tools: policy (off, ask, trust) and who may change it") {
    Project p;
    auto e = p.engine();
    const char* simple = R"({"name": "hello", "description": "Says hello; a trivial read-only tool for policy checks", "wander": "fn run(args)\n  return \"hi\"\nend"})";
    CHECK(call(*e, "tool_policy", Json::object()).structured.get("policy").asString() == "auto");
    // Agents may tighten, not loosen.
    CHECK_FALSE(call(*e, "tool_policy", parse(R"({"policy": "ask"})")).isError);
    CHECK(call(*e, "tool_define", parse(simple)).structured.get("status").asString() == "pending_approval");
    CHECK(call(*e, "tool_policy", parse(R"({"policy": "trust"})")).isError);
    CHECK(call(*e, "game_settings", parse(R"({"operation": "set", "settings": {"customTools": {"policy": "trust"}}})")).isError);
    CHECK_FALSE(call(*e, "tool_policy", parse(R"({"policy": "trust"})"), "user").isError);
    CHECK(e->tools().find("user_hello") != nullptr);  // re-evaluated at once
    CHECK_FALSE(call(*e, "tool_policy", parse(R"({"policy": "off"})")).isError);
    CHECK(e->tools().find("user_hello") == nullptr);
    CHECK(call(*e, "tool_define", parse(simple)).structured.get("error").asString() == "policy_off");
}

TEST_CASE("custom tools: tool_test runs dry, drafts can be tried unsaved, and the library round-trips") {
    Project p;
    auto e = p.engine();
    addEnemies(*e);
    Json draft = parse(R"({"definition": {"name": "draft", "description": "Moves the goblin; tried as a dry run without saving",
        "capabilities": {"mutate": true}, "wander": "fn run(args)\n  find(\"Goblin\").position = (5, 5, 5)\n  return 1\nend"}, "args": {}})");
    ToolResult r = call(*e, "tool_test", draft);
    INFO(text(r));
    CHECK(r.structured.get("ok").asBool());
    CHECK(r.structured.get("dry_run").asBool());
    CHECK(e->scene().worldMatrix(e->scene().find("Goblin")).translation().x == doctest::Approx(2));
    CHECK_FALSE(fs::exists(p.dir / "tools/draft.tool.json"));

    REQUIRE_FALSE(call(*e, "tool_define", parse(kEnemiesNear)).isError);
    fs::path lib = p.dir / "library";
    ::setenv("SKYWALKER_TOOL_LIBRARY", lib.c_str(), 1);
    ToolResult promoted = call(*e, "tool_promote", parse(R"({"name": "enemies_near"})"));
    REQUIRE_FALSE(promoted.isError);
    CHECK(fs::exists(lib / "enemies_near.tool.json"));
    Project other;
    auto e2 = other.engine();
    CHECK(call(*e2, "tool_list_custom", parse(R"({"include_library": true})")).structured.get("library").size() == 1);
    ToolResult installed = call(*e2, "tool_define", parse(R"({"from_library": "enemies_near"})"));
    INFO(text(installed));
    CHECK(installed.structured.get("status").asString() == "active");
    ::unsetenv("SKYWALKER_TOOL_LIBRARY");
}

TEST_CASE("custom tools: MCP clients get notifications/tools/list_changed") {
    Project p;
    auto e = p.engine();
    McpSession session(e->tools(), [&](const std::string& t, const Json& a, const ToolContext& ctx) { return e->callTool(t, a, ctx).toMcp(); });
    session.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"t"}}})");
    session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    Json init = parse(*session.handle(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})"));
    CHECK(init.get("result").get("capabilities").get("tools").get("listChanged").asBool());
    session.handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    CHECK_FALSE(session.pendingNotification().has_value());
    REQUIRE_FALSE(call(*e, "tool_define", parse(kEnemiesNear)).isError);
    auto note = session.pendingNotification();
    REQUIRE(note.has_value());
    CHECK(parse(*note).get("method").asString() == "notifications/tools/list_changed");
    CHECK_FALSE(session.pendingNotification().has_value());  // once per change
}

namespace {

/// A minimal MCP client for tests: sends requests, answers the engine's skywalker/tools/call
/// requests with `serve`, and collects responses by id.
struct FakeHost {
    int fd = -1;
    std::function<Json(const Json& params, FakeHost& self)> serve;
    std::string buffer;

    bool readLine(std::string& line) {
        while (true) {
            size_t nl = buffer.find('\n');
            if (nl != std::string::npos) {
                line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                return true;
            }
            char chunk[4096];
            ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n <= 0) return false;
            buffer.append(chunk, static_cast<size_t>(n));
        }
    }
    void send(const Json& j) { writeAll(fd, j.dump() + "\n"); }
    /// Sends a request and waits for its response, serving engine requests meanwhile.
    Json request(int id, const std::string& method, const Json& params) {
        send(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}));
        std::string line;
        while (readLine(line)) {
            auto msg = Json::parse(line);
            if (!msg) continue;
            if (msg->get("method").asString() == "skywalker/tools/call") {
                Json result = serve(msg->get("params"), *this);
                send(Json::object({{"jsonrpc", "2.0"}, {"id", msg->get("id")}, {"result", result}}));
                continue;
            }
            if (msg->get("id").isNumber() && msg->get("id").asInt() == id) return msg.value();
        }
        return Json();
    }
    /// Serves engine requests until the connection closes or `stop` is set.
    void serveUntil(const std::atomic<bool>& stop) {
        std::string line;
        while (!stop && readLine(line)) {
            auto msg = Json::parse(line);
            if (msg && msg->get("method").asString() == "skywalker/tools/call") {
                Json result = serve(msg->get("params"), *this);
                send(Json::object({{"jsonrpc", "2.0"}, {"id", msg->get("id")}, {"result", result}}));
            }
        }
    }
};

const char* kRegister = R"({"namespace": "ext", "tools": [{"name": "echo", "description": "Echoes its word back, after looking at the scene",
    "inputSchema": {"type": "object", "properties": {"word": {"type": "string"}}},
    "capabilities": {"calls": ["scene_overview"]}}]})";

/// The external tool's implementation: one allowed and one forbidden callback, then the answer.
Json echoServe(const Json& params, FakeHost& self) {
    std::string callId = params.get("call_id").asString();
    Json meta = Json::object({{"skywalker/call_id", callId}});
    Json allowed = self.request(100, "tools/call", Json::object({{"name", "scene_overview"}, {"arguments", Json::object()}, {"_meta", meta}}));
    Json denied = self.request(101, "tools/call", Json::object({{"name", "entity_create"}, {"arguments", Json::object({{"name", "X"}})}, {"_meta", meta}}));
    std::string word = params.get("arguments").get("word").asString();
    return Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", "echo " + word}})})},
                         {"structuredContent", Json::object({{"word", word},
                                                             {"overview_ok", !allowed.get("result").get("isError").asBool()},
                                                             {"create_denied", denied.get("result").get("isError").asBool()},
                                                             {"actor", params.get("actor")}})}});
}

}  // namespace

TEST_CASE("custom tools: a client hosts a tool over the socket; callbacks are capability-checked; it goes offline on disconnect") {
    Project p;
    auto e = p.engine();
    std::string path = (fs::temp_directory_path() / ("sky-ct-" + std::to_string(::getpid()) + ".sock")).string();
    REQUIRE(e->startAgentServer(path).ok());
    auto fd = connectUnixSocket(path);
    REQUIRE(fd.ok());
    FakeHost host;
    host.fd = fd->get();
    host.serve = echoServe;
    std::atomic<bool> registered{false}, stop{false};
    Json regResult;
    std::thread client([&] {
        host.request(1, "initialize", parse(R"({"protocolVersion":"2025-11-25","clientInfo":{"name":"pyhost"}})"));
        regResult = host.request(2, "skywalker/tools/register", parse(kRegister));
        registered = true;
        host.serveUntil(stop);
    });
    for (int i = 0; i < 2000 && !registered; ++i) {
        e->update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(registered);
    INFO(regResult.dump());
    CHECK(regResult.get("result").get("tools")[0].get("status").asString() == "active");
    const ToolDef* t = e->tools().find("ext_echo");
    REQUIRE(t);
    CHECK(t->origin == "external");

    // Called on the main thread: the engine serves the tool's callbacks while it waits.
    ToolResult r = e->callTool("ext_echo", parse(R"({"word": "lava"})"), "agent:mira");
    INFO(text(r));
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("word").asString() == "lava");
    CHECK(r.structured.get("overview_ok").asBool());
    CHECK(r.structured.get("create_denied").asBool());
    CHECK(r.structured.get("actor").asString() == "agent:mira");
    CHECK(e->scene().find("X") == kNoEntity);
    Json inspect = e->callTool("tool_inspect", parse(R"({"name": "ext_echo"})"), "user").structured;
    CHECK(inspect.get("stats").get("calls").asInt() == 1);

    stop = true;
    ::shutdown(fd->get(), SHUT_RDWR);
    client.join();
    for (int i = 0; i < 500 && e->tools().find("ext_echo"); ++i) {
        e->update(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(e->tools().find("ext_echo") == nullptr);  // disappears with its client
    e->stopAgentServer();
}

TEST_CASE("custom tools: hand-written definitions load in dependency order and tool_list_custom reload picks up edits") {
    Project p;
    // a_caller (loaded first) calls z_helper (defined in a later file).
    p.write("tools/a_caller.tool.json", R"({"name": "user_a_caller", "description": "Calls the helper tool defined in another file",
        "capabilities": {"calls": ["user_z_helper"]}, "wander": "fn run(args)\n  return call_tool(\"user_z_helper\")\nend"})");
    p.write("tools/z_helper.tool.json", R"({"name": "user_z_helper", "description": "Returns a constant for the caller tool",
        "source": "tools/z_helper.wander"})");
    p.write("tools/z_helper.wander", "fn run(args)\n  return {answer: 42}\nend\n");
    p.write("tools/broken.tool.json", "{ not json");
    auto e = p.engine();
    ToolResult r = call(*e, "user_a_caller", Json::object());
    INFO(text(r));
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("result").get("result").get("answer").asInt() == 42);
    Json list = call(*e, "tool_list_custom", Json::object()).structured;
    bool sawBroken = false;
    for (const auto& t : list.get("tools").elements()) {
        if (t.get("name").asString() == "user_broken") sawBroken = t.get("status").asString() == "invalid";
    }
    CHECK(sawBroken);  // listed with its error, so agents can fix it
    // Edit the code by hand, then reload.
    p.write("tools/z_helper.wander", "fn run(args)\n  return {answer: 7}\nend\n");
    Json reloaded = call(*e, "tool_list_custom", parse(R"({"reload": true})")).structured;
    CHECK(reloaded.get("reloaded").get("updated").size() == 1);
    CHECK(call(*e, "user_z_helper", Json::object()).structured.get("result").get("answer").asInt() == 7);
}

TEST_CASE("custom tools: an external tool over stdio (re-entrant callbacks on one thread), and unknown call ids") {
    Project p;
    auto e = p.engine();
    int toServer[2], toClient[2];
    REQUIRE(::pipe(toServer) == 0);
    REQUIRE(::pipe(toClient) == 0);
    McpSession session(e->tools(), [&](const std::string& t, const Json& a, const ToolContext& ctx) { return e->callTool(t, a, ctx).toMcp(); });
    McpStreamServer server(session, toServer[0], toClient[1]);
    session.setHost(server.peer(), e->customTools().hostHandlers());
    Json callResult, badCallback;
    std::thread client([&] {
        FakeHost host;
        host.fd = toClient[0];
        host.serve = echoServe;
        // FakeHost writes to its own fd; route writes to the server's input.
        int out = toServer[1];
        auto sendTo = [&](const Json& j) { writeAll(out, j.dump() + "\n"); };
        auto request = [&](int id, const std::string& method, const Json& params) {
            sendTo(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}));
            std::string line;
            while (host.readLine(line)) {
                auto msg = Json::parse(line);
                if (!msg) continue;
                if (msg->get("method").asString() == "skywalker/tools/call") {
                    std::string callId = msg->get("params").get("call_id").asString();
                    Json meta = Json::object({{"skywalker/call_id", callId}});
                    sendTo(Json::object({{"jsonrpc", "2.0"}, {"id", 50}, {"method", "tools/call"},
                                         {"params", Json::object({{"name", "scene_overview"}, {"arguments", Json::object()}, {"_meta", meta}})}}));
                    std::string cb;
                    while (host.readLine(cb)) {
                        auto m = Json::parse(cb);
                        if (m && m->get("id").isNumber() && m->get("id").asInt() == 50) break;
                    }
                    sendTo(Json::object({{"jsonrpc", "2.0"}, {"id", msg->get("id")},
                                         {"result", Json::object({{"content", Json::array({Json::object({{"type", "text"}, {"text", "pong"}})})}})}}));
                    continue;
                }
                if (msg->get("id").isNumber() && msg->get("id").asInt() == id) return msg.value();
            }
            return Json();
        };
        request(1, "initialize", parse(R"({"protocolVersion":"2025-11-25","clientInfo":{"name":"stdio-host"}})"));
        request(2, "skywalker/tools/register", parse(kRegister));
        callResult = request(3, "tools/call", parse(R"({"name": "ext_echo", "arguments": {"word": "x"}})"));
        badCallback = request(4, "tools/call", parse(R"({"name": "scene_overview", "arguments": {}, "_meta": {"skywalker/call_id": "call-999"}})"));
        ::close(toServer[1]);
    });
    server.run([&] { e->pump(); });
    client.join();
    ::close(toServer[0]);
    ::close(toClient[0]);
    ::close(toClient[1]);
    INFO(callResult.dump());
    CHECK(callResult.get("result").get("content")[0].get("text").asString() == "pong");
    CHECK(badCallback.get("result").get("isError").asBool());
    CHECK(badCallback.dump().find("unknown_call") != std::string::npos);
    CHECK(e->tools().find("ext_echo") == nullptr);  // the stream closed
}

TEST_CASE("custom tools: tool_host_register (py_*) tools follow the same policy") {
    Project p;
    auto e = p.engine();
    Json spec = parse(R"({"label": "harness", "tools": [
        {"name": "recall", "description": "Search the team's shared memory for notes"},
        {"name": "wipe", "description": "Deletes notes from the team's shared memory", "mutates": true}]})");
    ToolResult reg = e->callTool("tool_host_register", spec, "mcp:harness");
    INFO(text(reg));
    REQUIRE_FALSE(reg.isError);
    CHECK(reg.structured.get("status").get("py_recall").get("status").asString() == "active");
    CHECK(reg.structured.get("status").get("py_wipe").get("status").asString() == "pending_approval");
    CHECK(e->tools().find("py_recall") != nullptr);
    CHECK(e->tools().find("py_wipe") == nullptr);
    CHECK_FALSE(e->callTool("tool_approve", parse(R"({"name": "py_wipe"})"), "user").isError);
    CHECK(e->tools().find("py_wipe") != nullptr);
    CHECK(e->tools().find("py_wipe")->origin == "external");
    CHECK_FALSE(e->callTool("tool_host_unregister", Json::object({{"host", reg.structured.get("host")}}), "mcp:harness").isError);
    CHECK(e->tools().find("py_recall") == nullptr);
    CHECK(e->tools().find("py_wipe") == nullptr);
}
