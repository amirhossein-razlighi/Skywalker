// Dialogue: parsing, running (lines, choices, conditions, variables, jumps, commands), lint,
// preview, and live conversations through Wander and the default dialogue UI.

#include <doctest/doctest.h>

#include "skywalker/engine/Engine.h"
#include "skywalker/ui/Dialogue.h"
#include "skywalker/ui/World2D.h"

using namespace sky;

namespace {

const char* kScript = R"(title: Start
tags: intro
---
// A comment
<<declare $trust = 1>>
Narrator: The council chamber falls silent. #mood:tense
Vale: You're late, {$name}. #portrait:vale_cold #angry
-> I was delayed by the riots.
    <<set $trust += 1>>
    Vale: Riots. Of course.
-> That is none of your concern. <<if $trust > 5>>
    <<set $trust to 0>>
-> (Say nothing)
    <<jump Silence>>
<<if $trust >= 2>>
    Vale: Perhaps we can talk.
<<elseif $trust == 1>>
    Vale: Sit.
<<else>>
    Vale: Leave.
<<endif>>
<<lights_dim 0.5 "slow fade">>
Vale: Visits: {visited_count("Start")}
===
title: Silence
---
Vale: Silence suits you.
<<stop>>
Vale: (never shown)
===
)";

Json run(const char* script, const Json& choices, const Json& vars = Json::object()) {
    return dialogue::preview(dialogue::parse(script), "Start", choices, vars);
}

std::vector<std::string> lines(const Json& preview) {
    std::vector<std::string> out;
    for (const auto& t : preview.get("transcript").elements()) {
        if (t.get("type").asString() == "line") out.push_back(t.get("speaker").asString() + ": " + t.get("text").asString());
    }
    return out;
}

}  // namespace

TEST_CASE("dialogue: parse nodes, lines, tags, choices and commands") {
    auto s = dialogue::parse(kScript);
    REQUIRE(s->ok());
    REQUIRE(s->nodes.size() == 2);
    CHECK(s->nodes[0].title == "Start");
    CHECK(s->nodes[0].headers.get("tags").asString() == "intro");
    const auto& code = s->nodes[0].code;
    // declare, 2 lines, choices group, if/elseif/else ..., command, line
    bool sawChoices = false, sawCommand = false;
    for (const auto& in : code) {
        if (in.op == dialogue::Instr::Op::Choices) {
            sawChoices = true;
            CHECK(in.options.size() == 3);
            CHECK(in.options[1].condition != nullptr);
        }
        if (in.op == dialogue::Instr::Op::Command) {
            sawCommand = true;
            CHECK(in.command == "lights_dim");
            REQUIRE(in.args.size() == 2);
        }
    }
    CHECK(sawChoices);
    CHECK(sawCommand);
    CHECK(s->varsRead.count("trust"));
    CHECK(s->varsRead.count("name"));
    CHECK(s->declared.at("trust").asNumber() == doctest::Approx(1.0));
    CHECK(s->commands.count("lights_dim"));
}

TEST_CASE("dialogue: branches, conditions, variables, interpolation and jumps") {
    Json first = run(kScript, Json::array({0}), Json::object({{"name", "Chancellor"}}));
    auto l = lines(first);
    REQUIRE(l.size() == 5);
    CHECK(l[0] == "Narrator: The council chamber falls silent.");
    CHECK(l[1] == "Vale: You're late, Chancellor.");
    CHECK(l[2] == "Vale: Riots. Of course.");
    CHECK(l[3] == "Vale: Perhaps we can talk.");  // trust 1 + 1
    CHECK(l[4] == "Vale: Visits: 1");
    CHECK(first.get("vars").get("trust").asNumber() == doctest::Approx(2.0));
    CHECK(first.get("stopped").asString() == "ended");
    // The second option is hidden (trust is not > 5): only two choices offered; "nothing" picks by text.
    Json silent = run(kScript, Json::array({"nothing"}));
    auto choices = silent.get("transcript")[size_t{3}];
    REQUIRE(choices.get("type").asString() == "choices");
    CHECK(choices.get("options").size() == 2);
    auto ls = lines(silent);
    CHECK(ls.back() == "Vale: Silence suits you.");  // <<stop>> ends before the next line
    CHECK(silent.get("visited").get("Silence").asInt() == 1);
    // Declared defaults don't overwrite provided variables; the elseif branch.
    Json sit = run(kScript, Json::array({"riots"}), Json::object({{"trust", -5}}));
    CHECK(lines(sit)[3] == "Vale: Leave.");
    // Out of choices: stops and lists them.
    Json pending = run(kScript, Json::array());
    CHECK(pending.get("stopped").asString() == "awaiting_choice");
    CHECK(pending.get("pendingChoices").size() == 2);
    // Tags are parsed into the transcript.
    CHECK(first.get("transcript")[size_t{2}].get("tags").get("portrait").asString() == "vale_cold");
    CHECK(first.get("transcript")[size_t{2}].get("tags").get("angry").asBool());
    // Commands are reported with their arguments.
    bool cmd = false;
    for (const auto& t : first.get("transcript").elements()) {
        if (t.get("type").asString() == "command") {
            cmd = true;
            CHECK(t.get("args")[1].asString() == "slow fade");
            CHECK(t.get("event").asString() == "dialogue:lights_dim");
        }
    }
    CHECK(cmd);
}

TEST_CASE("dialogue: expressions") {
    const char* src = R"(title: Start
---
<<set $a = 2 + 3 * 4>>
<<set $b = ($a - 4) / 2>>
<<set $s = "x" + $a>>
<<set $c = not ($a > 10 and $b eq 5) || false>>
<<set $d = $a % 5>>
<<set $e = max(3, 9) - min(1, 2)>>
Result: {$a} {$b} {$s} {$c} {$d} {$e} {round(2.6)}
===
)";
    Json r = run(src, Json::array());
    CHECK(lines(r)[0] == "Result: 14 5 x14 false 4 8 3");
}

TEST_CASE("dialogue: lint finds errors and warnings with hints") {
    const char* bad = R"(title: Start
---
Vale: Hello
<<jump Endnig>>
<<if $x > 1>>
  Vale: no endif here
===
title: Ending
---
Vale: Bye {$missing}
===
title: Orphan
---
===
title: Start
---
Vale: duplicate
===
)";
    auto s = dialogue::parse(bad);
    Json lint = dialogue::lint(*s, "Start");
    CHECK_FALSE(lint.get("ok").asBool());
    std::string all = lint.dump();
    CHECK(all.find("unknown_node") != std::string::npos);
    CHECK(all.find("did you mean \\\"Ending\\\"") != std::string::npos);
    CHECK(all.find("missing_endif") != std::string::npos);
    CHECK(all.find("duplicate_node") != std::string::npos);
    CHECK(all.find("unset_variable") != std::string::npos);
    CHECK(all.find("empty_node") != std::string::npos);
    CHECK(all.find("unreachable") != std::string::npos);
    // Syntax errors carry line numbers.
    auto syntax = dialogue::parse("title: A\n---\n<<set trust to 1>>\nX: {1 +}\n<<if>>\n===\n");
    Json l2 = dialogue::lint(*syntax, "A");
    CHECK(l2.get("errors").asInt() >= 2);
    CHECK(l2.get("diagnostics")[size_t{0}].get("line").asInt() == 3);
    // Missing structure.
    CHECK(dialogue::lint(*dialogue::parse("Vale: no header\n"), "Start").get("errors").asInt() >= 1);
    // A clean script.
    CHECK(dialogue::lint(*dialogue::parse(kScript), "Start").get("ok").asBool());
}

TEST_CASE("dialogue: runner guards infinite loops and bad choices") {
    auto loop = dialogue::parse("title: Start\n---\n<<jump Start>>\n===\n");
    dialogue::Runner r(loop);
    Json vars = Json::object();
    dialogue::VarStore store{[&](const std::string& n) { return vars.get(n); }, [&](const std::string& n, const Json& v) { vars[n] = v; }};
    Status st = r.start("Start", store, {});
    CHECK_FALSE(st.ok());
    CHECK(st.error().code == "dialogue_loop");
    auto ok = dialogue::parse(kScript);
    dialogue::Runner r2(ok);
    REQUIRE(r2.start("Start", store, {}).ok());
    CHECK(r2.state() == dialogue::Runner::State::Line);
    CHECK_FALSE(r2.choose(0, store, {}).ok());  // not at a choice yet
    REQUIRE(r2.advance(store, {}).ok());
    REQUIRE(r2.advance(store, {}).ok());
    REQUIRE(r2.state() == dialogue::Runner::State::Choices);
    CHECK_FALSE(r2.choose(7, store, {}).ok());
    CHECK_FALSE(r2.start("Nope", store, {}).ok());
}

TEST_CASE("dialogue: live conversation through Wander, events, typewriter and the default UI") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine e(cfg);
    (void)e.newScene("Talk", false);
    Scene& s = e.scene();
    EntityId npc = s.create("Vale");
    REQUIRE(s.patchComponent(npc, "dialogue", Json::object({{"source", kScript}, {"typewriter", 30}})).ok());
    REQUIRE(s.setBehaviors(npc, Json::parse(R"([{"name": "Talk", "source": "behavior Talk\n  var ended = 0\n  var dimmed = 0\n  var picked = 0\n  on start\n    dialogue_var(\"name\", \"Chancellor\")\n    start_dialogue(\"Start\")\n  end\n  on dialogue \"lights_dim\"\n    dimmed = 1\n  end\n  on dialogue \"choice\"\n    picked = picked + 1\n  end\n  on dialogue \"end\"\n    ended = 1\n  end\nend"}])")
                                         .value())
                .ok());
    e.step(1);
    const DialogueRunner* d = s.get<DialogueRunner>(npc);
    REQUIRE(d->running);
    CHECK(d->speaker == "Narrator");
    CHECK(d->line == "The council chamber falls silent.");
    CHECK(d->tags.get("mood").asString() == "tense");
    // The default dialogue UI exists and shows the line, typed in over time.
    EntityId line = s.find("Dialogue Line");
    REQUIRE(line);
    CHECK(s.get<UIElement>(line)->text == "The council chamber falls silent.");
    CHECK(s.get<UIElement>(s.find("Dialogue Box"))->visible);
    Json state = e.world2d().dialogueState(s, npc);
    CHECK(state.get("typing").asBool());
    // Space finishes the typewriter, then advances.
    e.input().pressed.insert("space");
    e.step(1);
    CHECK_FALSE(e.world2d().dialogueState(s, npc).get("typing").asBool());
    e.input().pressed.insert("space");
    e.step(1);
    CHECK(s.get<DialogueRunner>(npc)->line == "You're late, Chancellor.");
    CHECK(s.get<UIElement>(s.find("Dialogue Portrait"))->visible);
    CHECK(s.get<UIElement>(s.find("Dialogue Portrait"))->image == "portraits/vale_cold.png");
    e.step(120);  // finish typing
    e.input().pressed.insert("enter");
    e.step(1);
    REQUIRE(s.get<DialogueRunner>(npc)->choices.size() == 2);
    CHECK(s.get<UIElement>(s.find("Dialogue Choices"))->visible);
    EntityId choice1 = s.find("Choice 1");
    REQUIRE(choice1);
    CHECK(s.get<UIElement>(choice1)->text.find("riots") != std::string::npos);
    // Clicking a choice button (as a player would) picks it.
    Json r = e.callTool("ui_interact", Json::parse(R"({"element": "Choice 1"})").value(), "agent:test").structured;
    CHECK(r.get("events")[size_t{0}].asString() == "dialogue:choice");
    e.step(1);
    CHECK(s.record(npc)->vars.get("picked").asInt() == 1);
    CHECK(s.record(npc)->vars.get("trust").asNumber() == doctest::Approx(2.0));
    CHECK(s.get<DialogueRunner>(npc)->line == "Riots. Of course.");
    // Drive the rest with dialogue_control.
    for (int i = 0; i < 12 && s.get<DialogueRunner>(npc)->running; ++i) {
        e.callTool("dialogue_control", Json::parse(R"({"action": "advance"})").value(), "agent:test");
        e.callTool("dialogue_control", Json::parse(R"({"action": "advance"})").value(), "agent:test");
    }
    e.step(1);
    CHECK_FALSE(s.get<DialogueRunner>(npc)->running);
    CHECK(s.record(npc)->vars.get("dimmed").asInt() == 1);
    CHECK(s.record(npc)->vars.get("ended").asInt() == 1);
    CHECK_FALSE(s.get<UIElement>(s.find("Dialogue Box"))->visible);
    e.stop();
    CHECK_FALSE(s.find("Dialogue UI"));  // runtime UI is gone after stop
    CHECK_FALSE(s.get<DialogueRunner>(npc)->running);
}

TEST_CASE("dialogue: tools check and preview scripts") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine e(cfg);
    (void)e.newScene("T", false);
    Json check = e.callTool("dialogue_check", Json::object({{"source", "title: Start\n---\nA: hi\n<<jump Nowhere>>\n===\n"}}), "t").structured;
    CHECK_FALSE(check.get("ok").asBool());
    Json prev = e.callTool("dialogue_preview", Json::object({{"source", kScript}, {"choices", Json::array({0})},
                                                           {"vars", Json::object({{"name", "X"}})}}),
                           "t")
                    .structured;
    CHECK(prev.get("stopped").asString() == "ended");
    CHECK(prev.get("vars").get("trust").asNumber() == doctest::Approx(2.0));
    ToolResult bad = e.callTool("dialogue_preview", Json::object({{"source", "title: S\n---\n<<jump X>>\n===\n"}}), "t");
    CHECK(bad.isError);
}
