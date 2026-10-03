// Dialogue tools: lint scripts, simulate conversations without UI, and drive live ones.

#include <fstream>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;

Result<std::shared_ptr<const dialogue::Script>> loadScript(Engine& engine, const Json& a) {
    std::string source;
    if (a.contains("source")) {
        source = a.get("source").asString();
    } else if (a.contains("path")) {
        std::ifstream f(engine.resolvePath(a.get("path").asString()));
        if (!f) return Error::make("not_found", "cannot open " + a.get("path").asString());
        std::stringstream ss;
        ss << f.rdbuf();
        source = ss.str();
    } else if (a.contains("entity")) {
        auto e = resolve(engine, a.get("entity"));
        if (!e) return e.error();
        const DialogueRunner* d = engine.scene().get<DialogueRunner>(*e);
        if (!d) return Error::make("invalid_arguments", "the entity has no dialogue component");
        return engine.world2d().script(*d);
    } else {
        return Error::make("invalid_arguments", "pass path, source or entity");
    }
    return dialogue::parse(source);
}

}  // namespace

void addDialogueTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"dialogue_check", "Check dialogue",
             "Lint a .dialogue script (Yarn-style): syntax errors with line numbers, jumps to missing nodes (with did-you-mean), "
             "a missing start node, unreachable nodes, variables read but never set, empty nodes. Also lists nodes (lines, "
             "choices, jumps), variables and custom commands (each becomes the Wander event \"dialogue:<command>\"). Run it "
             "after writing or editing dialogue. Format: nodes are `title: Name` / `---` / lines / `===`; lines are "
             "`Speaker: text #tag #portrait:name`; choices are `-> text` with indented bodies; commands <<set $x to 1>>, "
             "<<if $x > 1>>..<<elseif>>..<<else>>..<<endif>>, <<jump Node>>, <<wait 1>>, <<stop>>, <<any_command args>>.",
             "dialogue",
             object({{"path", string("Script file (project-relative .dialogue)")},
                     {"source", string("Script text (instead of path)")},
                     {"entity", entity("Entity whose dialogue component's script to check")},
                     {"start", string("Start node (default \"Start\")")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto s = loadScript(engine, a);
                 if (!s) return ToolResult::error(s.error());
                 Json r = dialogue::lint(*s.value(), a.get("start").asString("Start"));
                 std::string summary = std::to_string(r.get("errors").asInt()) + " error(s), " + std::to_string(r.get("warnings").asInt()) +
                                       " warning(s), " + std::to_string(r.get("nodes").size()) + " node(s)";
                 for (const auto& d : r.get("diagnostics").elements()) {
                     summary += "\n  line " + std::to_string(d.get("line").asInt()) + " " + d.get("severity").asString() + ": " +
                                d.get("message").asString();
                     if (d.contains("hint")) summary += " (" + d.get("hint").asString() + ")";
                 }
                 return ToolResult::json(r, summary);
             }});

    reg.add({"dialogue_preview", "Preview dialogue",
             "Simulate a conversation without UI or play mode and get the transcript: every line (speaker, text, tags), each "
             "choice offered and taken, commands sent, node jumps, final variables and visit counts. `choices` picks options in "
             "order by 0-based index or by a text fragment; when they run out the preview stops at the pending choice and "
             "lists it. Use it to test branches and variable logic. Example: {\"path\": \"story/intro.dialogue\", \"choices\": "
             "[0, \"refuse\"], \"vars\": {\"trust\": 2}}",
             "dialogue",
             object({{"path", string("Script file")},
                     {"source", string("Script text")},
                     {"entity", entity("Entity with a dialogue component")},
                     {"start", string("Start node (default \"Start\" or the component's startNode)")},
                     {"choices", array(Json::object({{"type", Json::array({"integer", "string"})}}), "Choices to take, in order")},
                     {"vars", Json::object({{"type", "object"}, {"description", "Initial variables (names without $)"}})},
                     {"max_steps", integer("Safety limit (default 500)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto s = loadScript(engine, a);
                 if (!s) return ToolResult::error(s.error());
                 if (!s.value()->ok()) {
                     Json lint = dialogue::lint(*s.value(), "");
                     return ToolResult::error(Error::make("dialogue_error", "the script has errors: " +
                                                                                lint.get("diagnostics")[size_t{0}].get("message").asString(),
                                                          "run dialogue_check for the full list"));
                 }
                 std::string start = a.get("start").asString("Start");
                 if (!a.contains("start") && a.contains("entity")) {
                     if (auto e = resolve(engine, a.get("entity")); e) {
                         if (const DialogueRunner* d = engine.scene().get<DialogueRunner>(*e)) start = d->startNode;
                     }
                 }
                 Json r = dialogue::preview(s.value(), start, a.get("choices"), a.get("vars"),
                                            static_cast<int>(std::clamp<int64_t>(a.get("max_steps").asInt(500), 1, 100000)));
                 std::string text;
                 for (const auto& t : r.get("transcript").elements()) {
                     const std::string type = t.get("type").asString();
                     if (type == "line") {
                         text += (t.get("speaker").asString().empty() ? "" : t.get("speaker").asString() + ": ") + t.get("text").asString() + "\n";
                     } else if (type == "choices") {
                         text += "  [choices";
                         for (const auto& o : t.get("options").elements()) text += " | " + o.asString();
                         text += "] -> " + t.get("chosenText").asString() + "\n";
                     } else if (type == "command") {
                         text += "  <<" + t.get("name").asString() + ">>\n";
                     } else if (type == "node") {
                         text += "== " + t.get("title").asString() + "\n";
                     } else if (type == "error") {
                         text += "  ERROR: " + t.get("message").asString() + "\n";
                     }
                 }
                 text += "stopped: " + r.get("stopped").asString();
                 if (r.contains("pendingChoices")) {
                     text += " (choose:";
                     for (const auto& o : r.get("pendingChoices").elements()) text += " | " + o.asString();
                     text += ")";
                 }
                 return ToolResult::json(r, text);
             }});

    reg.add({"dialogue_control", "Drive dialogue",
             "Drive a live conversation in play mode (enters play-paused when editing, like sim_control step): start a node, "
             "advance (finishes the typewriter first), choose an option (0-based), stop, or read the state (speaker, line, "
             "choices, typing). Events reach Wander on the next tick (on dialogue \"choice\", \"end\", \"<command>\").",
             "dialogue",
             object({{"action", enumeration({"start", "advance", "choose", "stop", "state"}, "What to do")},
                     {"entity", entity("Dialogue runner entity (default: the first with a dialogue component)")},
                     {"node", string("start: node to start at (default: the component's startNode)")},
                     {"choice", integer("choose: 0-based option index")}},
                    {"action"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 EntityId runner = kNoEntity;
                 if (a.contains("entity")) {
                     auto e = resolve(engine, a.get("entity"));
                     if (!e) return ToolResult::error(e.error());
                     runner = *e;
                 }
                 const std::string action = a.get("action").asString();
                 if (action != "state" && engine.playState() == PlayState::Editing) {
                     engine.play();
                     engine.pause();
                 }
                 Scene& s = engine.scene();
                 Status st;
                 if (action == "start") st = engine.world2d().startDialogue(s, runner, a.get("node").asString(), &engine.runtime());
                 else if (action == "advance") st = engine.world2d().advanceDialogue(s, runner, &engine.runtime());
                 else if (action == "choose") {
                     if (!a.contains("choice")) return ToolResult::error(Error::make("invalid_arguments", "choose needs choice (0-based)"));
                     st = engine.world2d().chooseDialogue(s, runner, static_cast<int>(a.get("choice").asInt()), &engine.runtime());
                 } else if (action == "stop") {
                     st = engine.world2d().stopDialogue(s, runner, &engine.runtime());
                 }
                 if (!st) return fail(st);
                 Json state = engine.world2d().dialogueState(s, runner);
                 std::string summary = state.get("running").asBool() ? state.get("speaker").asString() + ": " + state.get("line").asString()
                                                                    : "no conversation running";
                 if (state.get("choices").size()) {
                     summary += "\nchoices:";
                     int i = 0;
                     for (const auto& c : state.get("choices").elements()) summary += "\n  " + std::to_string(i++) + ". " + c.asString();
                 }
                 return ToolResult::json(state, summary);
             }});
}

}  // namespace sky::tools
