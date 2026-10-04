// The tool_* tools: agents define, test, inspect and manage their own tools (docs/CUSTOM_TOOLS.md).

#include "ToolHelpers.h"
#include "skywalker/agent/CustomTools.h"

namespace sky::tools {

namespace {

using namespace schema;

ToolResult reply(Result<Json> r, const std::string& summary = {}) {
    if (!r) return ToolResult::error(r.error());
    return ToolResult::json(std::move(r.value()), summary);
}

Json definitionSchema() {
    Json caps = object({{"read_scene", boolean("Query entities, components and vars (default true; false runs on an empty scene)")},
                        {"mutate", boolean("Edit the scene, directly or through mutating tools (default false; needs a human's approval)")},
                        {"calls", array(string("Tool name or glob"), "Tools it may call (call_tool / steps), e.g. [\"scene_query\", \"entity_*\"]")},
                        {"files", object({{"read", array(string("path"), "Project folders/files it may read (\"data/\", \"levels/*.json\")")},
                                          {"write", array(string("path"), "...and write (never tools/, game.json, .skywalker/, agents/, studio/)")}})},
                        {"network", boolean("External tools only")}});
    caps["description"] = "What the tool may do: an explicit allowlist (least privilege)";
    Json limits = object({{"instructions", integer("Wander instruction budget per call (default 1000000, max 50000000)")},
                          {"timeout_ms", integer("Wall time per call (default 5000, max 120000)")},
                          {"max_output_bytes", integer("Largest result (default 65536, max 1048576)")},
                          {"max_calls", integer("Nested tool calls per call (default 100)")}});
    limits["description"] = "Hard limits";
    Json test = object({{"name", string("What it checks")},
                        {"args", Json::object({{"type", "object"}, {"description", "Arguments for the call"}})},
                        {"setup", array(object({{"tool", string("Tool")}, {"args", Json::object({{"type", "object"}})}}, {"tool"}),
                                        "Tool calls run first (rolled back with the test)")},
                        {"expect", Json::object({{"type", "object"},
                                                 {"description", "{ok, error: code, result: subset (with $gte $lte $gt $lt $len $contains $type), "
                                                                 "contains: text, max_ms}"}})}});
    return object(
        {{"name", string("Tool name; it becomes user_<name> (lowercase, digits, underscores)")},
         {"title", string("Short title (default: from the name)")},
         {"description", string("For language models: what it does, when to use it, key arguments, an example")},
         {"category", string("Permission category, e.g. scene, world, custom (default custom)")},
         {"kind", enumeration({"wander", "composite"}, "Implementation (inferred: \"wander\" code or \"steps\")")},
         {"input_schema", Json::object({{"type", "object"}, {"description", "JSON schema of the arguments ({type: object, properties, required}); "
                                                                            "property \"default\"s are filled in"}})},
         {"output_schema", Json::object({{"type", "object"}, {"description", "Optional JSON schema the result must match"}})},
         {"capabilities", caps},
         {"limits", limits},
         {"wander", string("kind wander: Wander code with `fn run(args) ... return {...} end` (saved to tools/<name>.wander). Builtins: "
                           "call_tool, try_tool, tool_warn, tool_fail, read_file, write_file, list_files, file_exists, json_parse, json_text, tool_actor")},
         {"steps", array(object({{"id", string("Name for later templates ({{steps.<id>}})")},
                                 {"tool", string("Tool to call")},
                                 {"args", Json::object({{"type", "object"}, {"description", "Arguments; {{args.x}}, {{steps.id.field}}, {{item}}, {{index}}, {{prev}}, "
                                                                                             "{{x ?? default}}"}})},
                                 {"for_each", any("Run once per element: a template giving an array, or a count")},
                                 {"as", string("Loop variable name (default item)")},
                                 {"when", any("Template; the step is skipped when it is false/empty")},
                                 {"continue_on_error", boolean("Keep going when this step fails")},
                                 {"note", string("Comment")}},
                                {"tool"}),
                         "kind composite: tool calls in order (one undo step when the tool mutates)")},
         {"result", any("kind composite: result mapping template (default: the last step's result)")},
         {"tests", array(test, "Checks run by tool_define and tool_test as dry runs")},
         {"enabled", boolean("Register it (default true)")},
         {"from_library", string("Install a tool from the user library (tool_promote put it there)")},
         {"run_tests", boolean("Run the tests before saving (default true); failing tests block the save")},
         {"validate_only", boolean("Check and test without saving")}},
        {});
}

}  // namespace

void addCustomToolTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"tool_define", "Define a custom tool",
             "Create or update your OWN tool when no engine tool does what you need. Kinds: \"wander\" — Wander code `fn run(args) "
             "... end` that queries the scene and calls allowlisted tools (call_tool) under an instruction budget; \"composite\" — "
             "a pipeline of tool calls with templated args ({{args.x}}, {{steps.find.entities}}), for_each loops and a result "
             "mapping. Declare capabilities (read_scene, mutate, calls, files) as a minimal allowlist and add tests "
             "(args -> expect) — they run as dry runs before saving. Read-only tools become callable at once (as user_<name>, "
             "listed in tools/list); tools that mutate or write files wait for a human's approval (tool_approve) under the default "
             "policy. Updating merges your fields over the current definition. Example: {\"name\":\"enemies_near\",\"description\":"
             "\"Enemies within a radius of a point with their hp...\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"radius\":"
             "{\"type\":\"number\",\"default\":10}}},\"wander\":\"fn run(args)\\n  let out = []\\n  for e in find_all(\\\"enemy\\\")\\n    if "
             "distance(e, (0,0,0)) <= args.radius then out.push({name: e.name, hp: e.hp}) end\\n  end\\n  return {enemies: out}\\nend\","
             "\"tests\":[{\"args\":{\"radius\":5},\"expect\":{\"result\":{\"enemies\":{\"$type\":\"array\"}}}}]}. docs/CUSTOM_TOOLS.md has recipes.",
             "tools", definitionSchema(), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto r = engine.customTools().define(a, ctx.actor);
                 if (!r) return ToolResult::error(r.error());
                 bool failedTests = r->get("status").asString() == "tests_failed";
                 std::string summary = r->get("name").asString() + ": " + r->get("status").asString() +
                                       (failedTests ? " (nothing saved; fix the code or the tests)" : "");
                 ToolResult out = ToolResult::json(std::move(r.value()), summary);
                 out.isError = failedTests;
                 return out;
             }});

    reg.add({"tool_test", "Test a custom tool",
             "Run a custom tool's tests, or one call with \"args\", as a DRY RUN: scene edits are rolled back and file writes "
             "skipped, so it is safe on tools that are not approved yet. Pass \"definition\" (same fields as tool_define) to try "
             "a draft without saving it. Returns per-test pass/fail with the compared values, logs and timing. Example: "
             "{\"name\":\"user_enemies_near\",\"args\":{\"radius\":8}}",
             "tools",
             object({{"name", string("Custom tool name")},
                     {"definition", Json::object({{"type", "object"}, {"description", "An unsaved definition to try instead"}})},
                     {"args", Json::object({{"type", "object"}, {"description", "Run one call with these arguments instead of the tests"}})},
                     {"setup", array(object({{"tool", string("Tool")}, {"args", Json::object({{"type", "object"}})}}, {"tool"}),
                                     "With args: tool calls run first (rolled back)")},
                     {"filter", string("Only tests whose name contains this")}}),
             false, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto r = engine.customTools().test(a, ctx.actor);
                 if (!r) return ToolResult::error(r.error());
                 std::string summary = r->contains("total")
                                           ? std::to_string(r->get("passed").asInt()) + "/" + std::to_string(r->get("total").asInt()) + " tests passed"
                                           : std::string(r->get("ok").asBool() ? "ok (dry run)" : "failed (dry run)");
                 return ToolResult::json(std::move(r.value()), summary);
             }});

    reg.add({"tool_list_custom", "List custom tools",
             "Every custom tool of the project (and external tools hosted by connected clients) with its status — active, "
             "pending_approval, rejected, disabled, offline, invalid — kind, version, author and call counts, plus the project's "
             "policy. include_library lists your user library (tool_promote); reload rescans tools/ for hand-edited definitions.",
             "tools",
             object({{"include_library", boolean("Also list the user library (~/.skywalker/tools)")},
                     {"reload", boolean("Rescan tools/*.tool.json first")}}),
             false, false,
             [&engine](const Json& a, ToolContext&) {
                 Json r = engine.customTools().list(a.get("include_library").asBool(false), a.get("reload").asBool(false));
                 std::string summary = std::to_string(r.get("count").asInt()) + " custom tool(s), policy " + r.get("policy").asString();
                 return ToolResult::json(std::move(r), summary);
             }});

    reg.add({"tool_inspect", "Inspect a custom tool",
             "Full definition of a custom tool: code or steps, schemas, capabilities, limits, tests, status and why, approval "
             "(by whom, for which hash), stats (calls, failures, timing) and its recent calls and errors. Use it to debug a "
             "tool or before changing it.",
             "tools", object({{"name", string("Custom tool name")}}, {"name"}), false, false,
             [&engine](const Json& a, ToolContext&) { return reply(engine.customTools().inspect(a.get("name").asString())); }});

    reg.add({"tool_remove", "Remove a custom tool",
             "Unregister a custom tool and delete its files (tools/<name>.tool.json and .wander) unless delete_files is false. "
             "External tools are unregistered until their client registers them again.",
             "tools", object({{"name", string("Custom tool name")}, {"delete_files", boolean("Delete its files (default true)")}}, {"name"}),
             true, true,
             [&engine](const Json& a, ToolContext& ctx) {
                 return reply(engine.customTools().remove(a.get("name").asString(), a.get("delete_files").asBool(true), ctx.actor));
             }});

    reg.add({"tool_approve", "Approve a custom tool (human)",
             "A HUMAN approves (or rejects with approve:false) the current definition of a custom tool that needs approval "
             "(it mutates, writes files or reaches the network, or the policy is \"ask\"). The approval is bound to the "
             "definition's hash: changing the tool needs approval again. Agents cannot approve; ask the human to use the "
             "editor's Studio > Tools panel or `skywalker call tool_approve '{\"name\":\"user_x\"}' --project DIR`.",
             "tools",
             object({{"name", string("Custom tool name")},
                     {"approve", boolean("true approves (default), false rejects")},
                     {"note", string("Why (kept with the approval)")}},
                    {"name"}),
             true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 return reply(engine.customTools().approve(a.get("name").asString(), a.get("approve").asBool(true),
                                                            a.get("note").asString(), ctx.actor));
             }});

    reg.add({"tool_enable", "Enable or disable a custom tool",
             "Turn a custom tool off (it disappears from tools/list but keeps its files and approval) or back on.",
             "tools", object({{"name", string("Custom tool name")}, {"enabled", boolean("true or false")}}, {"name", "enabled"}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 return reply(engine.customTools().setEnabled(a.get("name").asString(), a.get("enabled").asBool(), ctx.actor));
             }});

    reg.add({"tool_promote", "Promote a tool to the user library",
             "Copy a project's custom tool into your user library (~/.skywalker/tools) so other projects can install it with "
             "tool_define {\"from_library\": \"user_x\"} (their approval rules apply).",
             "tools", object({{"name", string("Custom tool name")}}, {"name"}), false, false,
             [&engine](const Json& a, ToolContext& ctx) { return reply(engine.customTools().promote(a.get("name").asString(), ctx.actor)); }});

    reg.add({"tool_policy", "Custom tool policy",
             "Read or set who approves custom tools in this project (game.json customTools.policy): off (disabled), ask (every "
             "tool needs a human's approval), auto (default: read-only tools run at once, tools that mutate or write files need "
             "approval), trust (no approvals). Agents may only make it stricter.",
             "tools", object({{"policy", enumeration({"off", "ask", "auto", "trust"}, "New policy (omit to read it)")}}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 if (!a.contains("policy")) {
                     return ToolResult::json(Json::object({{"policy", toString(engine.customTools().policy())}}));
                 }
                 return reply(engine.customTools().setPolicy(*parseToolPolicy(a.get("policy").asString()), ctx.actor));
             }});
}

}  // namespace sky::tools
