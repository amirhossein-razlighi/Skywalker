// Agent integrations (workstream K): MCP resources and prompts, `skywalker setup` merging, and the checks
// that the generated Claude Code / Codex / Gemini / Cursor files are current and match the real tool schemas.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/agent/McpServer.h"
#include "skywalker/agent/Resources.h"
#include "skywalker/agent/Setup.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct McpFixture {
    McpFixture() {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = tempDir("project").string();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("Integrations", true);
        session = std::make_unique<McpSession>(engine->tools(), [this](const std::string& t, const Json& a, const std::string& actor) {
            return engine->callTool(t, a, actor).toMcp();
        });
        send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"claude-code"}}})");
    }
    ~McpFixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path tempDir(const std::string& name) {
        if (dir.empty()) {
            dir = fs::temp_directory_path() / ("sky_integrations_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(dir);
        }
        fs::path p = dir / name;
        fs::create_directories(p);
        return p;
    }
    Json send(const std::string& msg) {
        auto r = session->handle(msg);
        REQUIRE(r.has_value());
        return Json::parse(*r).value();
    }
    Json rpc(const std::string& method, const Json& params = Json::object()) {
        return send(Json::object({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}).dump());
    }
    fs::path dir;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<McpSession> session;
};

bool listHas(const Json& list, const char* key, const std::string& value) {
    for (const auto& item : list.elements()) {
        if (item.get(key).asString() == value) return true;
    }
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

struct TempRoot {
    TempRoot() {
        path = fs::temp_directory_path() / ("sky_setup_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path / "project");
        fs::create_directories(path / "home");
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path project() const { return path / "project"; }
    fs::path home() const { return path / "home"; }
    fs::path path;
};

SetupOptions optionsFor(const TempRoot& root, SetupTool tool) {
    SetupOptions o;
    o.tools = {tool};
    o.projectDir = root.project();
    o.homeDir = root.home();
    o.binary = "/opt/skywalker/bin/skywalker";
    return o;
}

bool pythonAvailable() { return std::system("python3 --version > /dev/null 2>&1") == 0; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Embedded assets

TEST_CASE("integrations: docs and the generated agent files are embedded") {
    CHECK(findEmbeddedAsset("docs/STUDIO.md") != nullptr);
    CHECK(findEmbeddedAsset("integrations/claude-code/skills/skywalker-core/SKILL.md") != nullptr);
    CHECK(findEmbeddedAsset("integrations/claude-code/.claude-plugin/plugin.json") != nullptr);
    CHECK(findEmbeddedAsset("integrations/codex/AGENTS.md") != nullptr);
    CHECK(findEmbeddedAsset("integrations/codex/.agents/skills/skywalker-cmd-new-game/SKILL.md") != nullptr);
    CHECK(findEmbeddedAsset("integrations/gemini/gemini-extension.json") != nullptr);
    CHECK(findEmbeddedAsset("integrations/cursor/.cursor/rules/skywalker.mdc") != nullptr);
    // Skills are identical for every tool: they are embedded once.
    CHECK(findEmbeddedAsset("integrations/gemini/skills/skywalker-core/SKILL.md") == nullptr);
    CHECK(findEmbeddedAsset("integrations/codex/.agents/skills/skywalker-core/SKILL.md") == nullptr);

    MarkdownDoc doc = parseMarkdownDoc(findEmbeddedAsset("integrations/claude-code/skills/skywalker-core/SKILL.md")->content);
    CHECK(doc.get("name") == "skywalker-core");
    CHECK(doc.get("description").find("Skywalker") != std::string::npos);
    CHECK(doc.body.rfind("# Skywalker core", 0) == 0);
}

// ---------------------------------------------------------------------------------------------------------
// MCP resources

TEST_CASE("mcp: resources/list exposes docs, skills, the tool catalogue and live studio state") {
    McpFixture f;
    CHECK(f.rpc("initialize", Json::object({{"protocolVersion", "2025-06-18"}})).get("result").get("capabilities").contains("resources"));
    Json list = f.rpc("resources/list").get("result").get("resources");
    CHECK(listHas(list, "uri", "skywalker://tools"));
    CHECK(listHas(list, "uri", "skywalker://docs/STUDIO"));
    CHECK(listHas(list, "uri", "skywalker://docs/RENDERING"));
    CHECK(listHas(list, "uri", "skywalker://skills/skywalker-core"));
    CHECK(listHas(list, "uri", "skywalker://skills/skywalker-look-dev/references/recipes.md"));
    CHECK(listHas(list, "uri", "skywalker://studio/roster"));
    CHECK(listHas(list, "uri", "skywalker://studio/board"));
    CHECK(listHas(list, "uri", "skywalker://scene/overview"));
    for (const auto& r : list.elements()) {
        CHECK_FALSE(r.get("uri").asString().empty());
        CHECK_FALSE(r.get("mimeType").asString().empty());
    }
    Json templates = f.rpc("resources/templates/list").get("result").get("resourceTemplates");
    CHECK(listHas(templates, "uriTemplate", "skywalker://docs/{name}"));
}

TEST_CASE("mcp: resources/read serves docs, skills and the live tool catalogue") {
    McpFixture f;
    Json doc = f.rpc("resources/read", Json::object({{"uri", "skywalker://docs/STUDIO"}})).get("result").get("contents")[0];
    CHECK(doc.get("mimeType").asString() == "text/markdown");
    CHECK(doc.get("text").asString().find("# The Studio") != std::string::npos);

    Json skill = f.rpc("resources/read", Json::object({{"uri", "skywalker://skills/skywalker-studio"}})).get("result").get("contents")[0];
    CHECK(skill.get("text").asString().find("name: skywalker-studio") != std::string::npos);
    Json ref = f.rpc("resources/read", Json::object({{"uri", "skywalker://skills/skywalker-studio/references/loops.md"}})).get("result").get("contents")[0];
    CHECK(ref.get("text").asString().find("# Loops") != std::string::npos);

    Json tools = f.rpc("resources/read", Json::object({{"uri", "skywalker://tools"}})).get("result").get("contents")[0];
    const std::string catalogue = tools.get("text").asString();
    CHECK(catalogue.find("`viewport_capture`") != std::string::npos);
    CHECK(catalogue.find("`studio_decide`") != std::string::npos);
    CHECK(catalogue == f.engine->tools().catalogueMarkdown());

    Json missing = f.rpc("resources/read", Json::object({{"uri", "skywalker://docs/NOPE"}}));
    CHECK(missing.get("error").get("code").asInt() == -32002);
    CHECK(f.rpc("resources/read", Json::object({{"uri", "skywalker://skills/../../etc/passwd"}})).get("error").get("code").asInt() == -32002);
    CHECK(f.rpc("resources/read", Json::object({{"uri", "file:///etc/passwd"}})).get("error").get("code").asInt() == -32002);
    CHECK(f.rpc("resources/read").get("error").get("code").asInt() == -32602);
}

TEST_CASE("mcp: live studio resources reflect the project") {
    McpFixture f;
    Json before = f.rpc("resources/read", Json::object({{"uri", "skywalker://studio/roster"}})).get("result").get("contents")[0];
    CHECK(before.get("mimeType").asString() == "application/json");
    CHECK(Json::parse(before.get("text").asString()).value().get("agents").size() == 0);

    f.engine->callTool("studio_team_template", Json::object({{"template", "indie_trio"}}), "test");
    f.engine->callTool("studio_task_create", Json::object({{"title", "Build the hub"}, {"assignee", "@environment_artist"}}), "test");

    Json roster = Json::parse(f.rpc("resources/read", Json::object({{"uri", "skywalker://studio/roster"}})).get("result").get("contents")[0].get("text").asString()).value();
    CHECK(roster.get("agents").size() == 3);
    CHECK(listHas(roster.get("agents"), "id", "aurora"));
    Json board = Json::parse(f.rpc("resources/read", Json::object({{"uri", "skywalker://studio/board"}})).get("result").get("contents")[0].get("text").asString()).value();
    CHECK(board.dump().find("Build the hub") != std::string::npos);
    Json scene = f.rpc("resources/read", Json::object({{"uri", "skywalker://scene/overview"}})).get("result").get("contents")[0];
    CHECK(scene.get("text").asString().find("Scene") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------
// MCP prompts

TEST_CASE("mcp: prompts/list exposes studio roles and workflows") {
    McpFixture f;
    Json prompts = f.rpc("prompts/list").get("result").get("prompts");
    for (const char* name : {"role_creative_director", "role_level_designer", "role_environment_artist", "role_gameplay_programmer", "role_technical_artist",
                             "role_sound_designer", "role_playtester", "role_critic", "new_game", "look_dev", "playtest_loop", "studio_status", "studio_setup",
                             "studio_agent"}) {
        CHECK_MESSAGE(listHas(prompts, "name", name), name);
    }
    for (const auto& p : prompts.elements()) CHECK_FALSE(p.get("description").asString().empty());
}

TEST_CASE("mcp: prompts/get renders roles, workflows and live agent briefs") {
    McpFixture f;
    Json role = f.rpc("prompts/get", Json::object({{"name", "role_level_designer"},
                                                    {"arguments", Json::object({{"arguments", "Widen the lava bridge"}})}})).get("result");
    std::string text = role.get("messages")[0].get("content").get("text").asString();
    CHECK(text.find("Level Designer") != std::string::npos);
    CHECK(text.find("as:\"level_designer\"") != std::string::npos);
    CHECK(text.find("Widen the lava bridge") != std::string::npos);
    CHECK(role.get("messages")[0].get("role").asString() == "user");

    Json flow = f.rpc("prompts/get", Json::object({{"name", "new_game"}, {"arguments", Json::object({{"arguments", "a cozy boat game"}})}})).get("result");
    std::string flowText = flow.get("messages")[0].get("content").get("text").asString();
    CHECK(flowText.find("a cozy boat game") != std::string::npos);
    CHECK(flowText.find("$ARGUMENTS") == std::string::npos);

    f.engine->callTool("studio_team_template", Json::object({{"template", "indie_trio"}}), "test");
    Json brief = f.rpc("prompts/get", Json::object({{"name", "studio_agent"}, {"arguments", Json::object({{"agent", "aurora"}})}}));
    REQUIRE(brief.contains("result"));
    std::string briefText = brief.get("result").get("messages")[0].get("content").get("text").asString();
    CHECK(briefText.find("Aurora") != std::string::npos);
    CHECK(briefText.find("as:\"aurora\"") != std::string::npos);

    CHECK(f.rpc("prompts/get", Json::object({{"name", "nope"}})).get("error").get("code").asInt() == -32602);
    CHECK(f.rpc("prompts/get", Json::object({{"name", "studio_agent"}, {"arguments", Json::object({{"agent", "ghost"}})}})).get("error").get("code").asInt() == -32602);
    CHECK(f.rpc("prompts/get").get("error").get("code").asInt() == -32602);
}

// ---------------------------------------------------------------------------------------------------------
// Merge primitives

TEST_CASE("setup: JSON merge keeps the user's settings and refuses broken files") {
    auto fresh = mergeJsonMcpServer("", "/bin/sky", {"mcp", "--auto"});
    REQUIRE(fresh);
    Json doc = Json::parse(fresh.value()).value();
    CHECK(doc.get("mcpServers").get("skywalker").get("command").asString() == "/bin/sky");
    CHECK(doc.get("mcpServers").get("skywalker").get("args")[1].asString() == "--auto");

    const std::string existing = R"({"theme":"dark","mcpServers":{"other":{"command":"x"},"skywalker":{"command":"/old","args":["mcp"],"env":{"K":"V"},"timeout":5}},"zzz":1})";
    auto merged = mergeJsonMcpServer(existing, "/new/sky", {"mcp", "--attach"}, {{"timeout", Json(600000)}});
    REQUIRE(merged);
    Json out = Json::parse(merged.value()).value();
    CHECK(out.get("theme").asString() == "dark");
    CHECK(out.get("zzz").asInt() == 1);
    CHECK(out.get("mcpServers").get("other").get("command").asString() == "x");
    const Json& entry = out.get("mcpServers").get("skywalker");
    CHECK(entry.get("command").asString() == "/new/sky");
    CHECK(entry.get("args")[1].asString() == "--attach");
    CHECK(entry.get("env").get("K").asString() == "V");   // user additions survive
    CHECK(entry.get("timeout").asInt() == 5);            // defaults never override a user's value
    CHECK(out.members().front().first == "theme");        // key order preserved

    CHECK_FALSE(mergeJsonMcpServer("{not json", "/x", {}));
    CHECK_FALSE(mergeJsonMcpServer("[1,2]", "/x", {}));
    CHECK_FALSE(mergeJsonMcpServer(R"({"mcpServers":[]})", "/x", {}));
}

TEST_CASE("setup: TOML upsert edits only the skywalker table") {
    const std::string args2 = R"(["mcp", "--auto"])";
    std::string created = upsertTomlMcpServer("model = \"gpt\"\n", "/bin/sky", {"mcp", "--auto"});
    CHECK(created.find("model = \"gpt\"") == 0);
    CHECK(created.find("[mcp_servers.skywalker]\ncommand = \"/bin/sky\"\nargs = " + args2) != std::string::npos);
    CHECK(created.find("tool_timeout_sec = 600") != std::string::npos);

    const std::string existing =
        "model = \"gpt\"\n\n[mcp_servers.other]\ncommand = \"o\"\n\n[mcp_servers.skywalker]\ncommand = \"/old\"\nargs = [\n  \"mcp\",\n  \"--attach\",\n]\n"
        "tool_timeout_sec = 90\nenabled = true\n\n[mcp_servers.skywalker.env]\nA = \"B\"\n\n[profiles.x]\nk = 1\n";
    std::string updated = upsertTomlMcpServer(existing, "/new/sky", {"mcp", "--auto"});
    CHECK(updated.find("command = \"/old\"") == std::string::npos);
    CHECK(updated.find("command = \"/new/sky\"") != std::string::npos);
    CHECK(updated.find("args = " + args2) != std::string::npos);
    CHECK(updated.find("\"--attach\"") == std::string::npos);        // the multi-line array was replaced whole
    CHECK(updated.find("tool_timeout_sec = 90") != std::string::npos);  // user timeout kept
    CHECK(updated.find("startup_timeout_sec = 30") != std::string::npos);
    CHECK(updated.find("enabled = true") != std::string::npos);
    CHECK(updated.find("[mcp_servers.skywalker.env]\nA = \"B\"") != std::string::npos);
    CHECK(updated.find("[mcp_servers.other]\ncommand = \"o\"") != std::string::npos);
    CHECK(updated.find("[profiles.x]\nk = 1") != std::string::npos);
    CHECK(upsertTomlMcpServer(updated, "/new/sky", {"mcp", "--auto"}) == updated);  // idempotent
}

TEST_CASE("setup: managed blocks are replaced in place, appended otherwise") {
    const std::string block = std::string(kManagedBegin) + "\nHello\n" + kManagedEnd + "\n";
    CHECK(upsertManagedBlock("", block) == block);
    std::string with = upsertManagedBlock("# My notes\n\nKeep me.\n", block);
    CHECK(with == "# My notes\n\nKeep me.\n\n" + block);
    const std::string next = std::string(kManagedBegin) + "\nHello v2\n" + kManagedEnd + "\n";
    std::string replaced = upsertManagedBlock(with + "\nAfter.\n", next);
    CHECK(replaced.find("Hello v2") != std::string::npos);
    CHECK(replaced.find("Hello\n") == std::string::npos);
    CHECK(replaced.find("Keep me.") != std::string::npos);
    CHECK(replaced.find("After.") != std::string::npos);
    CHECK(upsertManagedBlock(replaced, next) == replaced);
}

TEST_CASE("setup: textDiff marks additions and removals") {
    CHECK(textDiff("a\nb\n", "a\nb\n").empty());
    std::string diff = textDiff("a\nb\nc\n", "a\nB\nc\nd\n");
    CHECK(diff.find("- b") != std::string::npos);
    CHECK(diff.find("+ B") != std::string::npos);
    CHECK(diff.find("+ d") != std::string::npos);
    CHECK(diff.find("  a") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------
// Whole-tool setup

TEST_CASE("setup: parseSetupTools") {
    CHECK(parseSetupTools("all").value().size() == 4);
    CHECK(parseSetupTools("claude").value()[0] == SetupTool::Claude);
    auto bad = parseSetupTools("cursr");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().hint.find("cursor") != std::string::npos);
}

TEST_CASE("setup: claude project scope writes .mcp.json, skills, subagents, commands and CLAUDE.md, idempotently") {
    TempRoot root;
    spit(root.project() / "CLAUDE.md", "# Mine\n\nKeep this.\n");
    SetupOptions o = optionsFor(root, SetupTool::Claude);
    auto plan = planSetup(o);
    REQUIRE(plan);
    // Planning writes nothing.
    CHECK_FALSE(fs::exists(root.project() / ".mcp.json"));
    auto applied = applySetup(plan.value(), o);
    REQUIRE(applied);
    CHECK(applied->written > 20);

    Json mcp = Json::parse(slurp(root.project() / ".mcp.json")).value();
    CHECK(mcp.get("mcpServers").get("skywalker").get("command").asString() == "/opt/skywalker/bin/skywalker");
    CHECK(mcp.get("mcpServers").get("skywalker").get("args")[1].asString() == "--auto");
    CHECK(fs::exists(root.project() / ".claude/skills/skywalker-core/SKILL.md"));
    CHECK(fs::exists(root.project() / ".claude/skills/skywalker-look-dev/references/recipes.md"));
    CHECK(fs::exists(root.project() / ".claude/agents/level-designer.md"));
    CHECK(fs::exists(root.project() / ".claude/commands/new-game.md"));
    std::string claudeMd = slurp(root.project() / "CLAUDE.md");
    CHECK(claudeMd.find("Keep this.") != std::string::npos);
    CHECK(claudeMd.find("Skywalker game engine") != std::string::npos);
    // The existing CLAUDE.md was backed up before it changed.
    REQUIRE_FALSE(applied->backupDir.empty());
    CHECK(slurp(applied->backupDir / "CLAUDE.md") == "# Mine\n\nKeep this.\n");

    // Second run: nothing to do.
    auto again = planSetup(o);
    REQUIRE(again);
    for (const auto& c : again->changes) CHECK_MESSAGE(!c.changes(), c.path.string());
    auto second = applySetup(again.value(), o);
    REQUIRE(second);
    CHECK(second->written == 0);
}

TEST_CASE("setup: merging into an existing .mcp.json keeps other servers; --no-skills writes only the server entry") {
    TempRoot root;
    spit(root.project() / ".mcp.json", R"({"mcpServers":{"github":{"type":"http","url":"https://example.invalid/mcp"}}})");
    SetupOptions o = optionsFor(root, SetupTool::Claude);
    o.skills = false;
    auto plan = planSetup(o);
    REQUIRE(plan);
    CHECK(plan->changes.size() == 1);
    REQUIRE(applySetup(plan.value(), o));
    Json mcp = Json::parse(slurp(root.project() / ".mcp.json")).value();
    CHECK(mcp.get("mcpServers").contains("github"));
    CHECK(mcp.get("mcpServers").contains("skywalker"));
    CHECK_FALSE(fs::exists(root.project() / ".claude"));

    spit(root.project() / ".mcp.json", "{ broken");
    auto refused = planSetup(o);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find(".mcp.json") != std::string::npos);
}

TEST_CASE("setup: claude --global uses the claude CLI instead of editing ~/.claude.json") {
    TempRoot root;
    SetupOptions o = optionsFor(root, SetupTool::Claude);
    o.global = true;
    auto plan = planSetup(o);
    REQUIRE(plan);
    REQUIRE(plan->commands.size() == 1);
    CHECK(plan->commands[0][0] == "claude");
    CHECK(plan->commands[0][3] == "--scope");
    CHECK(plan->commands[0][4] == "user");
    CHECK(plan->commands[0].back() == "--auto");
    for (const auto& c : plan->changes) CHECK(c.path.string().rfind((root.home() / ".claude").string(), 0) == 0);
}

TEST_CASE("setup: codex merges config.toml, installs skills, custom agents and AGENTS.md") {
    TempRoot root;
    spit(root.home() / ".codex/config.toml", "model = \"gpt\"\n\n[mcp_servers.keep]\ncommand = \"k\"\n");
    spit(root.home() / ".codex/AGENTS.md", "My global rules.\n");
    SetupOptions o = optionsFor(root, SetupTool::Codex);
    o.global = true;
    auto plan = planSetup(o);
    REQUIRE(plan);
    auto applied = applySetup(plan.value(), o);
    REQUIRE(applied);
    std::string toml = slurp(root.home() / ".codex/config.toml");
    CHECK(toml.find("model = \"gpt\"") == 0);
    CHECK(toml.find("[mcp_servers.keep]\ncommand = \"k\"") != std::string::npos);
    CHECK(toml.find("[mcp_servers.skywalker]") != std::string::npos);
    CHECK(toml.find("command = \"/opt/skywalker/bin/skywalker\"") != std::string::npos);
    CHECK(fs::exists(root.home() / ".agents/skills/skywalker-core/SKILL.md"));
    CHECK(fs::exists(root.home() / ".agents/skills/skywalker-cmd-new-game/agents/openai.yaml"));
    CHECK(fs::exists(root.home() / ".codex/agents/level_designer.toml"));
    std::string agentsMd = slurp(root.home() / ".codex/AGENTS.md");
    CHECK(agentsMd.find("My global rules.") == 0);
    CHECK(agentsMd.find("Codex specifics") != std::string::npos);
    CHECK_FALSE(applied->backupDir.empty());
    CHECK(slurp(applied->backupDir / ".codex/config.toml").find("[mcp_servers.keep]") != std::string::npos);
}

TEST_CASE("setup: gemini and cursor write their own layouts; headless mode pins the project") {
    TempRoot root;
    SetupOptions g = optionsFor(root, SetupTool::Gemini);
    g.mode = "headless";
    REQUIRE(applySetup(planSetup(g).value(), g));
    Json settings = Json::parse(slurp(root.project() / ".gemini/settings.json")).value();
    const Json& entry = settings.get("mcpServers").get("skywalker");
    CHECK(entry.get("args")[1].asString() == "--project");
    CHECK(entry.get("args")[2].asString() == fs::absolute(root.project()).lexically_normal().string());
    CHECK(entry.get("timeout").asInt() == 600000);
    CHECK(fs::exists(root.project() / ".gemini/skills/skywalker-core/SKILL.md"));
    CHECK(fs::exists(root.project() / ".gemini/agents/critic.md"));
    CHECK(fs::exists(root.project() / ".gemini/commands/playtest-loop.toml"));
    CHECK(slurp(root.project() / "GEMINI.md").find("Gemini CLI specifics") != std::string::npos);

    SetupOptions c = optionsFor(root, SetupTool::Cursor);
    REQUIRE(applySetup(planSetup(c).value(), c));
    CHECK(Json::parse(slurp(root.project() / ".cursor/mcp.json")).value().get("mcpServers").contains("skywalker"));
    CHECK(fs::exists(root.project() / ".cursor/rules/skywalker.mdc"));
    CHECK(fs::exists(root.project() / ".cursor/skills/skywalker-studio/references/loops.md"));
    CHECK(fs::exists(root.project() / ".cursor/agents/playtester.md"));
    CHECK(fs::exists(root.project() / ".cursor/commands/look-dev.md"));
    // Global Cursor has no file-based rules.
    SetupOptions cg = optionsFor(root, SetupTool::Cursor);
    cg.global = true;
    for (const auto& ch : planSetup(cg).value().changes) CHECK(ch.path.string().find("/rules/") == std::string::npos);
}

TEST_CASE("setup: snippets for --print") {
    SetupOptions o;
    o.binary = "/opt/sky";
    CHECK(setupSnippet(SetupTool::Codex, o).find("[mcp_servers.skywalker]") != std::string::npos);
    CHECK(setupSnippet(SetupTool::Claude, o).find("claude mcp add") != std::string::npos);
    CHECK(setupSnippet(SetupTool::Gemini, o).find("\"timeout\": 600000") != std::string::npos);
    CHECK(setupSnippet(SetupTool::Cursor, o).find("\"mcpServers\"") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------
// The generated files are current and consistent with the engine

TEST_CASE("integrations: generated files are up to date with integrations/skills-src") {
    if (!pythonAvailable()) {
        MESSAGE("python3 not available: skipping");
        return;
    }
    std::string cmd = std::string("python3 \"") + SKY_SOURCE_DIR + "/integrations/generate.py\" --check > /dev/null 2>&1";
    CHECK_MESSAGE(std::system(cmd.c_str()) == 0, "run `python3 integrations/generate.py` and commit the result");
}

TEST_CASE("integrations: the SDK examples are valid Python") {
    if (!pythonAvailable()) {
        MESSAGE("python3 not available: skipping");
        return;
    }
    // py_compile checks syntax without importing the (uninstalled) SDKs; the cache goes to a temp dir.
    for (const char* rel : {"/integrations/examples/openai_agents_sdk/skywalker_agents.py",
                            "/integrations/examples/anthropic_tool_runner/skywalker_claude.py",
                            "/integrations/generate.py", "/integrations/check_skills.py"}) {
        std::string cmd = std::string("python3 -c \"import ast,sys; ast.parse(open(sys.argv[1]).read())\" \"") + SKY_SOURCE_DIR + rel + "\" > /dev/null 2>&1";
        CHECK_MESSAGE(std::system(cmd.c_str()) == 0, rel);
    }
}

TEST_CASE("integrations: skill sources only use tools and arguments that exist") {
    if (!pythonAvailable()) {
        MESSAGE("python3 not available: skipping");
        return;
    }
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    fs::path tmp = fs::temp_directory_path() / ("sky_tools_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    spit(tmp, engine.tools().listJson().dump());
    std::string cmd = std::string("python3 \"") + SKY_SOURCE_DIR + "/integrations/check_skills.py\" --tools \"" + tmp.string() + "\" > /dev/null 2>&1";
    int code = std::system(cmd.c_str());
    std::error_code ec;
    fs::remove(tmp, ec);
    CHECK_MESSAGE(code == 0, "run `skywalker tools --json > t.json && python3 integrations/check_skills.py --tools t.json` to see the problems");
}
