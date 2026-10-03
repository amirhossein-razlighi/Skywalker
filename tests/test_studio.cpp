// Studio: roster, board, feedback, decisions, loops, playtests, runner (docs/STUDIO.md).

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/studio/Catalog.h"
#include "skywalker/studio/Llm.h"
#include "skywalker/studio/Playtest.h"
#include "skywalker/studio/Runner.h"
#include "skywalker/studio/Studio.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

/// A throwaway project directory, removed at the end of the test.
struct TempProject {
    fs::path dir;
    TempProject() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("sky_studio_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string path() const { return dir.string(); }
};

std::unique_ptr<Engine> makeEngine(const TempProject& p) {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = p.path();
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Test", false);
    double t = 1790000000;  // deterministic, monotonic studio clock
    e->studio().setClock([t]() mutable { return t += 1; });
    return e;
}

Json call(Engine& e, const char* tool, const Json& args, bool expectOk = true, const char* actor = "user") {
    ToolResult r = e.callTool(tool, args, actor);
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

Json J(const char* text) { return Json::parse(text).value(); }

const char* kControls = R"(behavior Controls
  intent "WASD moves the player at 4 m/s."
  var speed = 4
  on tick
    let dir = (0, 0, 0)
    if key("w") then dir = dir + (0, 0, -1) end
    if key("s") then dir = dir + (0, 0, 1) end
    if key("a") then dir = dir + (-1, 0, 0) end
    if key("d") then dir = dir + (1, 0, 0) end
    move self by normalize(dir) * speed * dt
  end
end
)";

const char* kGoal = R"(behavior Goal
  intent "Reaching the flag completes the level."
  on tick
    let p = nearest("player")
    if exists(p) and distance(self, p) < 1 then
      emit "objective"
    end
  end
end
)";

const char* kLava = R"(behavior Lava
  intent "Touching the lava kills the player: back to the start."
  on tick
    let p = nearest("player")
    if exists(p) and abs(p.position.x - self.position.x) < 15 and abs(p.position.z - self.position.z) < 0.9 then
      emit "death"
      p.position = (0, 0.5, 8)
    end
  end
end
)";

/// A tiny generated level: start at z=8, flag at z=-6, optional full-width lava at z=2.
Json tinyLevel(bool lava) {
    Json ents = Json::array();
    auto behavior = [](const char* name, const char* src) {
        return Json::array({Json::object({{"name", name}, {"source", src}, {"enabled", true}})});
    };
    ents.push(J(R"({"id":1,"name":"Ground","tags":["static"],"components":{"transform":{"scale":[30,1,30]},"mesh":{"mesh":"plane"}}})"));
    Json player = J(R"({"id":2,"name":"Player","tags":["player"],"components":{"transform":{"position":[0,0.5,8]},"mesh":{"mesh":"capsule"}}})");
    player["behaviors"] = behavior("Controls", kControls);
    ents.push(player);
    Json flag = J(R"({"id":3,"name":"Flag","tags":["goal"],"components":{"transform":{"position":[0,0.5,-6]},"mesh":{"mesh":"cube","color":"#40d080"}}})");
    flag["behaviors"] = behavior("Goal", kGoal);
    ents.push(flag);
    if (lava) {
        Json l = J(R"({"id":4,"name":"Lava","tags":["hazard"],"components":{"transform":{"position":[0,0.1,2],"scale":[30,0.2,1.5]},"mesh":{"mesh":"cube","color":"#ff4010"}}})");
        l["behaviors"] = behavior("Lava", kLava);
        ents.push(l);
    }
    return Json::object({{"format", "skywalker.scene"}, {"version", 1}, {"name", "Tiny"}, {"seed", 3}, {"entities", ents}});
}

Json strip(Json metrics) {  // drop wall-clock dependent fields
    for (const char* k : {"avg_tick_ms", "p95_tick_ms", "est_fps"}) metrics.erase(k);
    return metrics;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("studio: catalog templates are valid and spawn cleanly") {
    TempProject p;
    auto e = makeEngine(p);
    for (const auto& name : studio::teamTemplateNames()) {
        INFO(name);
        Json r = call(*e, "studio_team_template", Json::object({{"template", name}}));
        CHECK(r.get("agents").size() == studio::teamTemplate(name).size());
    }
    CHECK(e->studio().agents().size() >= 22);
    for (const auto& name : studio::loopTemplateNames()) {
        INFO(name);
        Json r = call(*e, "studio_loop_define", Json::object({{"template", name}}));
        CHECK(r.get("stages").size() > 0);
    }
    for (const auto& r : studio::roles()) CHECK(!r.mission.empty());
    // did-you-mean on bad input
    ToolResult bad = e->callTool("studio_agent_define", J(R"({"name":"X","role":"levl_designer"})"), "user");
    CHECK(bad.isError);
    CHECK(bad.content.front().text.find("level_designer") != std::string::npos);
}

TEST_CASE("studio: board, feedback and decision lifecycle with persistence round trip") {
    TempProject p;
    {
        auto e = makeEngine(p);
        call(*e, "studio_team_template", J(R"({"template":"indie_trio"})"));
        call(*e, "studio_agent_define", J(R"({"name":"Drizzle","role":"playtester","playtest":{"skill":0.3}})"));
        // feedback from a playtester (identified as a roster member)
        Json f1 = call(*e, "studio_feedback_submit", J(R"({"category":"difficulty","severity":"high","summary":"Lava jump too hard","evidence":{"positions":[[0,0,2]]}})"),
                       true, "agent:drizzle");
        CHECK(f1.get("id").asString() == "F-1");
        CHECK(f1.get("by").asString() == "drizzle");
        Json f2 = call(*e, "studio_feedback_submit", J(R"({"category":"visuals","summary":"Flag is ugly","fingerprint":"flag-look"})"));
        // fingerprint dedup
        Json f2b = call(*e, "studio_feedback_submit", J(R"({"category":"visuals","summary":"Flag still ugly","fingerprint":"flag-look"})"));
        CHECK(f2b.get("id").asString() == f2.get("id").asString());
        CHECK(f2b.get("occurrences").asInt() == 2);

        // only direction/production may decide
        ToolResult denied = e->callTool("studio_decide", J(R"({"feedback":"F-1","verdict":"drop","rationale":"no"})"), "agent:stratus");
        CHECK(denied.isError);
        CHECK(denied.content.front().text.find("permission_denied") != std::string::npos);
        // a rationale is required
        call(*e, "studio_decide", J(R"({"feedback":"F-1","verdict":"drop","rationale":""})"), false, "agent:nimbus");

        Json d1 = call(*e, "studio_decide",
                       J(R"({"feedback":"F-1","verdict":"act","rationale":"Unfair spike","targets":{"deaths":{"max":1}},
                             "tasks":[{"title":"Widen the bridge","assignee":"@gameplay_programmer","acceptance":["3 m wide"]},
                                      {"title":"Glow on lava","assignee":"aurora"}]})"),
                       true, "agent:nimbus");
        CHECK(d1.get("tasks").size() == 2);
        Json d2 = call(*e, "studio_decide", J(R"({"feedback":"F-2","verdict":"drop","rationale":"Placeholder art; final pass later"})"), true,
                       "agent:nimbus");
        CHECK(d2.get("verdict").asString() == "drop");
        // re-deciding needs redecide
        call(*e, "studio_decide", J(R"({"feedback":"F-1","verdict":"drop","rationale":"x"})"), false, "agent:nimbus");

        // dependencies block; claim via `as` for an external agent
        call(*e, "studio_task_update", J(R"({"task":"T-2","depends_on":["T-1"]})"));
        call(*e, "studio_task_claim", J(R"({"task":"T-2","as":"aurora"})"), false, "mcp:claude-code");
        Json claimed = call(*e, "studio_task_claim", J(R"({"as":"stratus"})"), true, "mcp:claude-code");
        CHECK(claimed.get("id").asString() == "T-1");
        CHECK(claimed.get("status").asString() == "doing");
        CHECK(e->studio().feedbackItem("F-1")->status == "in_progress");
        call(*e, "studio_task_update", J(R"({"task":"T-1","status":"done","comment":"widened to 3 m"})"), true, "mcp:claude-code/stratus");
        CHECK(e->studio().feedbackItem("F-1")->status == "in_progress");  // T-2 still open
        call(*e, "studio_task_update", J(R"({"task":"T-2","status":"done"})"), true, "agent:aurora");
        CHECK(e->studio().feedbackItem("F-1")->status == "fixed");

        // effect: regression reopens the tasks
        auto& s = e->studio();
        auto eff = s.recordEffect("F-1", J(R"({"deaths":1,"completion_rate":0.5})"), J(R"({"deaths":3,"completion_rate":0.2})"), "P-1", "P-2",
                                  "user");
        REQUIRE(eff.ok());
        CHECK(eff->get("status").asString() == "regressed");
        CHECK(s.feedbackItem("F-1")->status == "regressed");
        CHECK(s.task("T-1")->status == "todo");
        // fix again, then an improvement verifies it
        call(*e, "studio_task_update", J(R"({"task":"T-1","status":"done"})"), true, "agent:stratus");
        call(*e, "studio_task_update", J(R"({"task":"T-2","status":"done"})"), true, "agent:aurora");
        CHECK(s.feedbackItem("F-1")->status == "fixed");
        eff = s.recordEffect("F-1", J(R"({"deaths":3,"completion_rate":0.2})"), J(R"({"deaths":0,"completion_rate":1})"), "P-2", "P-3", "user");
        CHECK(eff->get("status").asString() == "improved");
        CHECK(eff->get("targets_met").asBool());
        CHECK(s.feedbackItem("F-1")->status == "verified");

        // messages with @mentions, threads and inbox read cursors
        Json m1 = call(*e, "studio_message_send", J(R"({"text":"@engineering can you look at T-1? cc @nimbus","task":"T-1"})"), true,
                       "agent:aurora");
        CHECK(m1.get("mentions").size() == 2);
        call(*e, "studio_message_send", J(R"({"text":"on it","reply_to":"M-1"})"), true, "agent:stratus");
        Json inbox = call(*e, "studio_inbox", J("{}"), true, "agent:stratus");
        CHECK(inbox.get("messages").size() == 1);  // its own reply is not in its inbox
        CHECK(call(*e, "studio_inbox", J("{}"), true, "agent:stratus").get("messages").size() == 0);  // now read
        Json aur = call(*e, "studio_inbox", J("{}"), true, "agent:aurora");
        REQUIRE(aur.get("messages").size() == 1);
        CHECK(aur.get("messages")[size_t{0}].get("thread").asString() == "M-1");

        call(*e, "studio_memory", J(R"({"action":"note","text":"Lava must glow"})"), true, "agent:aurora");
        s.recordUsage("nimbus", "claude-opus-5-5", 1000000, 100000, 0, 0);
        CHECK(s.usage().at("nimbus").costUsd == doctest::Approx(4.0 + 2.0));
    }
    // Round trip: a fresh engine reads everything back from the project files.
    {
        auto e = makeEngine(p);
        auto& s = e->studio();
        CHECK(s.agents().size() == 4);
        CHECK(s.agent("drizzle")->playtest.get("skill").asNumber() == doctest::Approx(0.3));
        CHECK(s.agent("aurora")->memory.size() == 1);
        CHECK(s.tasks().size() == 2);
        CHECK(s.task("T-1")->comments.size() >= 3);
        CHECK(s.feedbackItem("F-1")->status == "verified");
        CHECK(s.feedbackItem("F-2")->status == "dropped");
        CHECK(s.decisions().size() == 2);
        CHECK(s.decision("D-1")->effect.get("status").asString() == "improved");
        CHECK(s.messages().size() == 2);
        CHECK(s.usage().at("nimbus").inputTokens == 1000000);
        Json f = call(*e, "studio_feedback_list", J(R"({"status":"verified"})"));
        CHECK(f.get("feedback")[size_t{0}].get("verdict").get("rationale").asString() == "Unfair spike");
        // ids keep counting after a reload
        CHECK(call(*e, "studio_task_create", J(R"({"title":"Next"})")).get("id").asString() == "T-3");
        // files are human-readable JSON
        std::ifstream board(p.dir / "studio" / "board.json");
        std::stringstream ss;
        ss << board.rdbuf();
        CHECK(ss.str().find("\"title\": \"Widen the bridge\"") != std::string::npos);
    }
}

TEST_CASE("studio: external edits on disk are picked up") {
    TempProject p;
    auto a = makeEngine(p);
    auto b = makeEngine(p);
    call(*a, "studio_team_template", J(R"({"template":"indie_trio"})"));
    call(*a, "studio_task_create", J(R"({"title":"From A"})"));
    Json list = call(*b, "studio_task_list", J("{}"));
    CHECK(list.get("tasks").size() == 1);
    CHECK(call(*b, "studio_agent_list", J("{}")).get("agents").size() == 3);
    call(*b, "studio_agent_remove", J(R"({"agent":"aurora"})"));
    CHECK(call(*a, "studio_agent_list", J("{}")).get("agents").size() == 2);
}

TEST_CASE("studio: actor identity and permission filtering") {
    TempProject p;
    auto e = makeEngine(p);
    call(*e, "studio_team_template", J(R"({"template":"qa_squad"})"));
    auto& s = e->studio();
    CHECK(s.memberForActor("agent:stratus") == "stratus");
    CHECK(s.memberForActor("mcp:claude-code/sentinel") == "sentinel");
    CHECK(s.memberForActor("mcp:Bolt") == "bolt");
    CHECK(s.memberForActor("mcp:claude-code").empty());
    CHECK(s.memberForActor("user").empty());
    // `as` cannot be used to impersonate someone else
    ToolResult r = e->callTool("studio_task_create", J(R"({"title":"x","as":"stratus"})"), "agent:bolt");
    CHECK(r.isError);

    const ToolRegistry& reg = e->tools();
    auto names = [](const std::vector<studio::AgentRunner::AllowedTool>& v) {
        std::set<std::string> n;
        for (const auto& t : v) n.insert(t.spec.name);
        return n;
    };
    // Playtesters observe: read-only tools plus studio coordination, never scene edits.
    auto observer = studio::AgentRunner::toolsFor(*s.agent("bolt"), reg, true);
    auto on = names(observer);
    CHECK(on.count("scene_overview"));
    CHECK(on.count("playtest_run"));
    CHECK(on.count("studio_feedback_submit"));
    CHECK_FALSE(on.count("entity_update"));
    CHECK_FALSE(on.count("asset_download"));
    CHECK_FALSE(on.count("studio_loop_advance"));  // loop control is withheld inside loops
    // Autonomous engineer: edits allowed, downloads ask, explicit off removes a category.
    call(*e, "studio_agent_define", J(R"({"id":"stratus","permissions":{"wander":"off"}})"));
    auto eng = studio::AgentRunner::toolsFor(*s.agent("stratus"), reg, false);
    auto en = names(eng);
    CHECK(en.count("entity_update"));
    CHECK_FALSE(en.count("behavior_set"));
    CHECK(en.count("studio_loop_advance"));
    for (const auto& t : eng) {
        if (t.spec.name == "entity_update") CHECK((t.access == studio::Access::Allow));
        if (t.spec.name == "asset_download") CHECK((t.access == studio::Access::Ask));
    }
    // Ask autonomy: mutations need approval, reads don't.
    call(*e, "studio_agent_define", J(R"({"id":"stratus","autonomy":"ask"})"));
    for (const auto& t : studio::AgentRunner::toolsFor(*s.agent("stratus"), reg, false)) {
        if (t.spec.name == "entity_update") CHECK((t.access == studio::Access::Ask));
        if (t.spec.name == "scene_overview") CHECK((t.access == studio::Access::Allow));
        if (t.spec.name == "studio_task_update") CHECK((t.access == studio::Access::Allow));
    }
}

TEST_CASE("studio: metrics comparison, effect verdicts and targets") {
    Json before = J(R"({"deaths":4,"completion_rate":0.25,"time_to_goal":null,"coverage":0.5})");
    Json after = J(R"({"deaths":1,"completion_rate":1,"time_to_goal":12.5,"coverage":0.5})");
    Json c = studio::compareMetrics(before, after);
    CHECK(c.get("deaths").get("verdict").asString() == "better");
    CHECK(c.get("completion_rate").get("verdict").asString() == "better");
    CHECK(c.get("time_to_goal").get("verdict").asString() == "better");  // never -> finished
    CHECK(c.get("coverage").get("verdict").asString() == "same");
    CHECK(studio::effectVerdict(c, studio::metricsForCategory("difficulty"), Json::object()) == "improved");
    CHECK(studio::effectVerdict(c, {}, J(R"({"deaths":{"max":0}})")) == "mixed");
    CHECK(studio::effectVerdict(c, {}, J(R"({"deaths":{"max":1}})")) == "improved");
    CHECK(studio::effectVerdict(studio::compareMetrics(after, before), studio::metricsForCategory("difficulty"), Json::object()) ==
          "regressed");
    CHECK(studio::effectVerdict(c, {"loudness"}, Json::object()) == "unmeasured");
    std::string unmet;
    CHECK_FALSE(studio::targetsMet(before, J(R"({"completion_rate":{"min":0.8}})"), &unmet));
    CHECK(unmet.find("completion_rate") != std::string::npos);
    CHECK(studio::targetsMet(after, J(R"({"completion_rate":{"min":0.8},"deaths":2})")));
}

TEST_CASE("studio: playtest goal seeker reaches the goal deterministically, scene untouched") {
    TempProject p;
    auto e = makeEngine(p);
    REQUIRE(e->scene().loadJson(tinyLevel(false)).ok());
    Json before = e->scene().toJson();
    Json r1 = call(*e, "playtest_run", J(R"({"policy":"goal_seeker","seconds":20,"runs":2,"seed":5})"));
    Json r2 = call(*e, "playtest_run", J(R"({"policy":"goal_seeker","seconds":20,"runs":2,"seed":5})"));
    CHECK(e->scene().toJson() == before);  // the editor scene is never touched
    CHECK((e->playState() == PlayState::Editing));
    const Json& m = r1.get("metrics");
    CHECK(m.get("completion_rate").asNumber() == 1.0);
    CHECK(m.get("deaths").asNumber() == 0);
    REQUIRE(m.get("time_to_goal").isNumber());
    CHECK(m.get("time_to_goal").asNumber() > 2.0);
    CHECK(m.get("time_to_goal").asNumber() < 8.0);
    CHECK(r1.get("findings").size() == 0);
    CHECK(strip(r1.get("metrics")) == strip(r2.get("metrics")));
    CHECK(r1.get("id").asString() == "P-1");
    CHECK(r2.get("id").asString() == "P-2");
    // Full reports on disk with identical trajectories.
    auto s1 = e->studio().loadPlaytestReport("P-1");
    auto s2 = e->studio().loadPlaytestReport("P-2");
    REQUIRE(s1.ok());
    REQUIRE(s2.ok());
    CHECK(s1->get("runs")[size_t{0}].get("trajectory") == s2->get("runs")[size_t{0}].get("trajectory"));
    CHECK(s1->get("runs")[size_t{0}].get("objectives").size() >= 1);
    CHECK(fs::exists(p.dir / "studio/playtests/P-1/heatmap.png"));
    CHECK(fs::exists(p.dir / "studio/playtests/P-1/report.json"));
    CHECK(e->studio().latestPlaytest() == "P-2");

    Json cmp = call(*e, "playtest_compare", J(R"({"before":"P-1","after":"P-2"})"));
    CHECK(cmp.get("verdict").asString() != "regressed");
}

TEST_CASE("studio: playtest records deaths, causes, quits and files findings") {
    TempProject p;
    auto e = makeEngine(p);
    REQUIRE(e->scene().loadJson(tinyLevel(true)).ok());
    Json r = call(*e, "playtest_run",
                  J(R"({"policy":"goal_seeker","seconds":12,"seed":2,"persona":{"skill":0,"patience":30},"screenshots":true,"max_screenshots":3})"));
    const Json& m = r.get("metrics");
    CHECK(m.get("deaths").asNumber() >= 2);
    CHECK(m.get("completion_rate").asNumber() == 0);
    CHECK(r.get("death_causes").contains("Lava"));
    bool deathFinding = false;
    for (const auto& f : r.get("findings").elements()) {
        if (f.get("fingerprint").asString() == "deaths:Lava") {
            deathFinding = true;
            CHECK(f.get("category").asString() == "difficulty");
            CHECK(f.get("evidence").get("playtest").asString() == r.get("id").asString());
            CHECK(f.get("evidence").get("captures").size() >= 1);
        }
    }
    CHECK(deathFinding);
    CHECK(r.get("screenshots").size() >= 1);
    // Scripted input goes through sim_input: holding nothing for 2 s leaves the player in place.
    Json s = call(*e, "playtest_run", J(R"({"policy":"scripted","seconds":2,"script":[{"t":0,"hold":["d"]},{"t":1,"release":["d"]}],"screenshots":false})"));
    auto rep = e->studio().loadPlaytestReport(s.get("id").asString());
    REQUIRE(rep.ok());
    const Json& traj = rep->get("runs")[size_t{0}].get("trajectory");
    REQUIRE(traj.size() > 2);
    double x = traj[traj.size() - 1][size_t{1}].asNumber();
    CHECK(x == doctest::Approx(4.0).epsilon(0.1));  // 1 s at 4 m/s to the right
    // Helpful errors
    REQUIRE(e->newScene("Empty", false).ok());
    ToolResult none = e->callTool("playtest_run", J(R"({"seconds":1})"), "user");
    CHECK(none.isError);
    CHECK(none.content.front().text.find("player") != std::string::npos);
}

TEST_CASE("studio: anthropic request and response JSON") {
    using namespace studio::llm;
    SessionConfig cfg;
    cfg.agentId = "nimbus";
    cfg.system = "You are Nimbus.";
    cfg.tools = {{"scene_overview", "Overview", J(R"({"type":"object","properties":{}})")}};
    Json req = anthropic::buildRequest(cfg, Json::array());
    CHECK(req.get("model").asString() == "claude-opus-5-5");
    CHECK(req.get("max_tokens").asInt() == 16000);
    CHECK(req.get("thinking").get("type").asString() == "adaptive");
    CHECK(req.get("output_config").get("effort").asString() == "high");
    CHECK(req.get("system")[size_t{0}].get("cache_control").get("type").asString() == "ephemeral");
    CHECK(req.get("cache_control").get("type").asString() == "ephemeral");
    CHECK(req.get("tools")[size_t{0}].get("input_schema").get("type").asString() == "object");
    CHECK(req.get("fallbacks").asString() == "default");
    CHECK_FALSE(req.contains("tool_choice"));
    cfg.model = "claude-haiku-4-5";
    Json haiku = anthropic::buildRequest(cfg, Json::array());
    CHECK_FALSE(haiku.contains("thinking"));
    CHECK_FALSE(haiku.contains("fallbacks"));
    cfg.model.clear();

    Json msg = anthropic::userMessage({{"toolu_1", "viewport_capture", "ok", {"iVBORw0KGgo="}, false}, {"toolu_2", "x", "", {}, true}},
                                      "continue");
    const Json& content = msg.get("content");
    REQUIRE(content.size() == 3);
    CHECK(content[size_t{0}].get("type").asString() == "tool_result");
    CHECK(content[size_t{0}].get("tool_use_id").asString() == "toolu_1");
    CHECK(content[size_t{0}].get("content")[size_t{1}].get("type").asString() == "image");
    CHECK(content[size_t{0}].get("content")[size_t{1}].get("source").get("media_type").asString() == "image/png");
    CHECK(content[size_t{1}].get("is_error").asBool());
    CHECK(content[size_t{1}].get("content")[size_t{0}].get("text").asString() == "(no output)");
    CHECK(content[size_t{2}].get("type").asString() == "text");

    auto turn = anthropic::parseResponse(J(R"({"model":"claude-opus-5-5","stop_reason":"tool_use","content":[
        {"type":"thinking","thinking":"","signature":"sig"},{"type":"text","text":"Looking."},
        {"type":"tool_use","id":"toolu_9","name":"scene_overview","input":{}}],
        "usage":{"input_tokens":10,"output_tokens":5,"cache_read_input_tokens":100,"cache_creation_input_tokens":7}})"));
    REQUIRE(turn.ok());
    CHECK((turn->stop == Stop::ToolUse));
    CHECK(turn->text == "Looking.");
    REQUIRE(turn->calls.size() == 1);
    CHECK(turn->calls[0].id == "toolu_9");
    CHECK(turn->usage.cacheRead == 100);
    CHECK(turn->usage.cacheWrite == 7);
    auto refusal = anthropic::parseResponse(J(R"({"stop_reason":"refusal","content":[],"stop_details":{"type":"refusal","category":"cyber","explanation":"no"}})"));
    CHECK((refusal->stop == Stop::Refusal));
    CHECK(refusal->stopDetail == "cyber: no");
    auto nullDetails = anthropic::parseResponse(J(R"({"stop_reason":"refusal","content":[],"stop_details":null})"));
    CHECK(nullDetails->stopDetail == "declined");
    CHECK((anthropic::parseResponse(J(R"({"stop_reason":"pause_turn","content":[]})"))->stop == Stop::PauseTurn));
    CHECK((anthropic::parseResponse(J(R"({"stop_reason":"max_tokens","content":[]})"))->stop == Stop::MaxTokens));

    // A session over a fake transport: headers, retries, append-only history.
    std::vector<HttpRequest> sent;
    int calls = 0;
    HttpFn http = [&](const HttpRequest& r) -> Result<HttpResponse> {
        sent.push_back(r);
        if (++calls == 1) return HttpResponse{529, R"({"type":"error","error":{"type":"overloaded_error","message":"busy"}})", "0"};
        if (calls == 2) {
            return HttpResponse{200, R"({"model":"claude-opus-5-5","stop_reason":"tool_use","content":[{"type":"thinking","thinking":"","signature":"abc"},
                {"type":"tool_use","id":"toolu_1","name":"scene_overview","input":{}}],"usage":{"input_tokens":1,"output_tokens":1}})", ""};
        }
        return HttpResponse{200, R"({"model":"claude-opus-5-5","stop_reason":"end_turn","content":[{"type":"text","text":"Done."}],"usage":{}})", ""};
    };
    auto provider = makeAnthropic("sk-test", "", http);
    auto session = provider->open(cfg);
    auto t1 = session->send({}, "Build a level");
    REQUIRE(t1.ok());
    CHECK(calls == 2);  // retried after 529
    REQUIRE(sent.size() == 2);
    std::map<std::string, std::string> headers(sent[1].headers.begin(), sent[1].headers.end());
    CHECK(sent[1].url == "https://api.anthropic.com/v1/messages");
    CHECK(headers["x-api-key"] == "sk-test");
    CHECK(headers["anthropic-version"] == "2023-06-01");
    CHECK(headers["anthropic-beta"] == "server-side-fallback-2026-07-01");
    auto t2 = session->send({{"toolu_1", "scene_overview", "3 entities", {}, false}}, "");
    REQUIRE(t2.ok());
    CHECK(t2->text == "Done.");
    Json last = Json::parse(sent.back().body).value();
    const Json& history = last.get("messages");
    REQUIRE(history.size() == 3);
    // The thinking block is echoed back unchanged.
    CHECK(history[size_t{1}].get("content")[size_t{0}].get("signature").asString() == "abc");
    CHECK(history[size_t{2}].get("content")[size_t{0}].get("tool_use_id").asString() == "toolu_1");
    CHECK(session->history().size() == 4);
}

TEST_CASE("studio: openai-compatible request and response JSON") {
    using namespace studio::llm;
    SessionConfig cfg;
    cfg.system = "sys";
    cfg.model = "gpt-5";
    cfg.tools = {{"scene_overview", "Overview", J(R"({"type":"object","properties":{}})")}};
    Json messages = Json::array({Json::object({{"role", "system"}, {"content", "sys"}})});
    openai::appendUser(messages, {{"call_1", "viewport_capture", "ok", {"AAAA"}, false}, {"call_2", "x", "bad", {}, true}}, "go on", true);
    REQUIRE(messages.size() == 5);
    CHECK(messages[size_t{1}].get("role").asString() == "tool");
    CHECK(messages[size_t{1}].get("tool_call_id").asString() == "call_1");
    CHECK(messages[size_t{2}].get("content").asString() == "ERROR: bad");
    CHECK(messages[size_t{3}].get("content")[size_t{1}].get("image_url").get("url").asString() == "data:image/png;base64,AAAA");
    CHECK(messages[size_t{4}].get("content").asString() == "go on");
    Json req = openai::buildRequest(cfg, messages);
    CHECK(req.get("tools")[size_t{0}].get("type").asString() == "function");
    CHECK(req.get("tools")[size_t{0}].get("function").get("parameters").get("type").asString() == "object");

    auto t = openai::parseResponse(J(R"({"model":"gpt-5","choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,
        "tool_calls":[{"id":"c1","type":"function","function":{"name":"scene_overview","arguments":"{\"max_entities\":5}"}},
                      {"id":"c2","type":"function","function":{"name":"entity_update","arguments":"{oops"}}]}}],
        "usage":{"prompt_tokens":120,"completion_tokens":30,"prompt_tokens_details":{"cached_tokens":100}}})"));
    REQUIRE(t.ok());
    CHECK((t->stop == Stop::ToolUse));
    REQUIRE(t->calls.size() == 2);
    CHECK(t->calls[0].input.get("max_entities").asInt() == 5);
    CHECK(t->calls[1].invalidJson);
    CHECK(t->usage.input == 20);
    CHECK(t->usage.cacheRead == 100);
    CHECK((openai::parseResponse(J(R"({"choices":[{"finish_reason":"length","message":{"content":"x"}}]})"))->stop == Stop::MaxTokens));
    CHECK((openai::parseResponse(J(R"({"choices":[{"finish_reason":"content_filter","message":{"content":""}}]})"))->stop == Stop::Refusal));

    std::vector<HttpRequest> sent;
    HttpFn http = [&](const HttpRequest& r) -> Result<HttpResponse> {
        sent.push_back(r);
        return HttpResponse{200, R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"hi"}}]})", ""};
    };
    auto provider = makeOpenAI("", "http://localhost:11434/v1/", "qwen3", http, false);
    auto session = provider->open(cfg);
    REQUIRE(session->send({}, "hello").ok());
    CHECK(sent[0].url == "http://localhost:11434/v1/chat/completions");
    for (const auto& [k, v] : sent[0].headers) CHECK(k != "authorization");  // local servers: no key
}

TEST_CASE("studio: agent runner handles tools, approvals, max_tokens and refusals") {
    TempProject p;
    auto e = makeEngine(p);
    call(*e, "studio_team_template", J(R"({"template":"indie_trio"})"));
    call(*e, "studio_agent_define", J(R"({"id":"aurora","autonomy":"ask"})"));
    studio::llm::MockProvider mock(J(R"({"agents":{
        "aurora":[{"tool_calls":[{"name":"entity_create","input":{"name":"Lamp","mesh":"sphere"}},{"name":"scene_overview","input":{}}]},
                  {"tool_calls":[{"name":"entity_create","input":{"name":"Cut"}}],"stop":"max_tokens"},
                  {"text":"Asked for approval; added nothing."}],
        "stratus":[{"text":"","stop":"pause_turn"},{"tool_calls":[{"name":"entity_create","input":{"name":"Crate","mesh":"cube"}}]},{"text":"Made a crate."}],
        "nimbus":[{"text":"partial","stop":"refusal"}]}})"));
    auto runAgent = [&](const std::string& id) {
        auto res = studio::runWhilePumping(*e, [&]() -> Result<Json> {
            studio::AgentRunner::Options o;
            o.approve = [](const std::string&, const std::string& tool, const Json&) { return tool != "entity_create"; };
            const studio::AgentProfile* prof = nullptr;
            studio::AgentProfile copy;
            // profile snapshot (taken on the worker thread through the engine thread)
            auto f = e->post([&] {
                prof = e->studio().agent(id);
                copy = *prof;
                return Json();
            });
            (void)f.get();
            studio::AgentRunner r(*e, mock, copy, o);
            auto out = r.run("Do your thing");
            return Json::object({{"report", out.report}, {"stop", out.stop}, {"tool_calls", out.toolCalls}});
        });
        REQUIRE(res.ok());
        return *res;
    };
    Json s = runAgent("stratus");
    CHECK(s.get("stop").asString() == "done");
    CHECK(s.get("report").asString() == "Made a crate.");
    CHECK(e->scene().find("Crate") != kNoEntity);
    CHECK(e->history().lastCommitted()->actor == "agent:stratus");

    Json a = runAgent("aurora");
    CHECK(e->scene().find("Lamp") == kNoEntity);  // declined by the approver
    CHECK(e->scene().find("Cut") == kNoEntity);   // never run: the reply was cut off
    CHECK(a.get("report").asString() == "Asked for approval; added nothing.");
    bool declined = false, cut = false;
    const Json mockLog = mock.log();  // keep the copy alive while iterating
    for (const auto& entry : mockLog.elements()) {
        if (entry.get("agent").asString() != "aurora" || entry.get("kind").asString() != "tool_result") continue;
        if (entry.get("text").asString().find("declined") != std::string::npos) declined = true;
        if (entry.get("text").asString().find("cut off") != std::string::npos) cut = true;
    }
    CHECK(declined);
    CHECK(cut);

    Json n = runAgent("nimbus");
    CHECK(n.get("stop").asString() == "refusal");
    CHECK(e->studio().usage().at("stratus").requests == 3);  // pause_turn resumed transparently
    CHECK(e->studio().usage().at("stratus").toolCalls >= 1);
}

TEST_CASE("studio: loop with a mock provider — drop one, act on another, fix, verify, measure") {
    TempProject p;
    auto e = makeEngine(p);
    REQUIRE(e->scene().loadJson(tinyLevel(true)).ok());
    call(*e, "studio_team_template", J(R"({"template":"indie_trio"})"));
    // A critic note filed before the loop: the director will drop it.
    call(*e, "studio_feedback_submit", J(R"({"category":"visuals","severity":"low","summary":"The flag is a plain cube"})"));
    call(*e, "studio_loop_define", J(R"({"name":"fix_the_level","goal":"Players reach the flag without dying.",
        "stages":[
          {"id":"playtest","kind":"playtest","playtest":{"policy":"goal_seeker","seconds":12,"runs":2,"persona":{"skill":0,"patience":30}},
           "gate":{"skip_if":"not_first_iteration"}},
          {"id":"triage","assignees":["@creative_director"],"gate":{"skip_if":"no_open_feedback"},
           "instruction":"Triage:\n{{open_feedback}}\nMetrics: {{metrics}}"},
          {"id":"fix","assignees":["@engineering"],"only_with_tasks":true,"gate":{"skip_if":"no_todo_tasks"},
           "instruction":"Your tasks:\n{{my_tasks}}"},
          {"id":"verify","kind":"playtest","playtest":{"policy":"goal_seeker","seconds":12,"runs":2,"persona":{"skill":0,"patience":30}},
           "verify_fixed":true}],
        "stop":{"max_iterations":2,"metric_targets":{"completion_rate":{"min":1}}}})"));

    studio::llm::MockProvider mock(J(R"({"agents":{
      "nimbus":[
        {"tool_calls":[
          {"name":"studio_decide","input":{"feedback":"F-1","verdict":"drop","rationale":"Visual polish is out of scope for this loop."}},
          {"name":"studio_decide","input":{"feedback":"F-2","verdict":"act","rationale":"Unavoidable lava is unfair.",
             "targets":{"deaths":{"max":0}},
             "tasks":[{"title":"Remove the unavoidable lava strip","assignee":"stratus","acceptance":["no deaths on the critical path"]}]}},
          {"name":"studio_decide","input":{"feedback":"F-3","verdict":"merge_into","merge_into":"F-2","rationale":"Same root cause."}}]},
        {"text":"Dropped F-1, acting on F-2, merged F-3."}],
      "stratus":[
        {"tool_calls":[{"name":"studio_task_claim","input":{"task":"T-1"}},{"name":"entity_update","input":{"entity":"Lava","enabled":false}}]},
        {"tool_calls":[{"name":"studio_task_update","input":{"task":"T-1","status":"done","comment":"Disabled the lava strip."}}]},
        {"text":"Lava disabled; T-1 done."}]}})"));

    std::vector<std::string> log;
    std::mutex logMutex;
    studio::LoopRunner::Options o;
    o.providers = [&](const studio::AgentProfile&) -> Result<studio::llm::Provider*> { return static_cast<studio::llm::Provider*>(&mock); };
    o.log = [&](const studio::RunnerEvent& ev) {
        std::lock_guard lock(logMutex);
        log.push_back(ev.kind + " " + ev.agent + " " + ev.text);
    };
    studio::LoopRunner runner(*e, o);
    auto result = studio::runWhilePumping(*e, [&] { return runner.run("fix_the_level"); });
    REQUIRE(result.ok());
    const Json& st = *result;
    INFO(st.dump(2));
    CHECK(st.get("status").asString() == "done");
    CHECK(st.get("stop_reason").asString() == "metric targets met");
    CHECK(st.get("iteration").asInt() == 1);

    auto& s = e->studio();
    // Feedback filed by the bot playtest: F-2 deaths at the lava, F-3 completion.
    REQUIRE(s.feedbackItem("F-2"));
    CHECK(s.feedbackItem("F-2")->fingerprint == "deaths:Lava");
    CHECK(s.feedbackItem("F-1")->status == "dropped");
    CHECK(s.decision(s.feedbackItem("F-1")->decision)->rationale == "Visual polish is out of scope for this loop.");
    CHECK(s.feedbackItem("F-3")->mergedInto == "F-2");
    // Act -> task -> fix -> verify -> effect.
    CHECK(s.task("T-1")->status == "done");
    CHECK(s.task("T-1")->assignee == "stratus");
    CHECK(s.feedbackItem("F-2")->status == "verified");
    const studio::Decision* d = s.decision(s.feedbackItem("F-2")->decision);
    REQUIRE(d);
    CHECK(d->effect.get("status").asString() == "improved");
    CHECK(d->effect.get("metrics").get("deaths").get("after").asNumber() == 0);
    CHECK(d->effect.get("metrics").get("deaths").get("before").asNumber() > 0);
    CHECK(d->effect.get("targets_met").asBool());
    // The scene edit is attributed to the agent and undoable.
    CHECK(e->history().lastCommitted()->actor == "agent:stratus");
    CHECK_FALSE(e->scene().record(e->scene().find("Lava"))->enabled);
    // Trends and history are recorded per iteration.
    CHECK(st.get("trends").get("completion_rate")[size_t{0}].asNumber() == 1.0);
    auto status = call(*e, "studio_loop_status", J(R"({"loop":"fix_the_level"})"));
    const Json& stages = status.get("iterations")[size_t{0}].get("stages");
    REQUIRE(stages.size() == 4);
    CHECK(stages[size_t{1}].get("reports")[size_t{0}].get("agent").asString() == "nimbus");
    CHECK(stages[size_t{3}].get("verified")[size_t{0}].get("effect").asString() == "improved");
    // The director's prompt carried the open feedback and the metrics.
    bool sawPrompt = false;
    const Json mockLog = mock.log();  // keep the copy alive while iterating
    for (const auto& entry : mockLog.elements()) {
        if (entry.get("agent").asString() == "nimbus" && entry.get("kind").asString() == "prompt") {
            sawPrompt = entry.get("text").asString().find("F-2 [difficulty/") != std::string::npos &&
                        entry.get("text").asString().find("completion_rate") != std::string::npos;
        }
    }
    CHECK(sawPrompt);
    CHECK(s.usage().at("nimbus").requests == 2);
}

TEST_CASE("studio: loops driven by an external executor — gates, sign-off, budgets") {
    TempProject p;
    auto e = makeEngine(p);
    call(*e, "studio_team_template", J(R"({"template":"indie_trio"})"));
    call(*e, "studio_loop_define", J(R"JSON({"name":"polish","goal":"Polish",
        "stages":[{"id":"work","assignees":["@art","@engineering"],"instruction":"Polish {{focus}} for {{goal}} (iteration {{iteration}})"},
                  {"id":"review","assignees":["@creative_director"],"gate":{"approval":"human"},"instruction":"Review:\n{{inputs}}"}],
        "stop":{"max_iterations":3,"director_signoff":true}})JSON"));
    // Unknown loops get did-you-mean hints.
    call(*e, "studio_loop_start", J(R"({"loop":"polsh"})"), false);
    Json st = call(*e, "studio_loop_start", J(R"({"loop":"polish"})"), true, "mcp:claude-code");
    CHECK(st.get("status").asString() == "running");
    REQUIRE(st.get("assignments").size() == 2);
    CHECK(st.get("assignments")[size_t{0}].get("prompt").asString().find("Polish environment") != std::string::npos);
    call(*e, "studio_loop_start", J(R"({"loop":"polish"})"), false);  // already running
    // Reports may arrive one by one; a stranger's report is rejected.
    call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"agent":"nimbus","report":"x"}]})"), false);
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"agent":"aurora","report":"Warmer sky."}]})"));
    CHECK(st.get("assignments").size() == 1);
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"report":"Tighter controls."}]})"), true,
              "mcp:claude-code/stratus");
    CHECK(st.get("status").asString() == "awaiting_approval");
    call(*e, "studio_loop_advance", J(R"({"loop":"polish"})"), false);  // needs approve
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","approve":true})"));
    REQUIRE(st.get("assignments").size() == 1);
    CHECK(st.get("assignments")[size_t{0}].get("prompt").asString().find("Warmer sky.") != std::string::npos);
    // No sign-off: next iteration.
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"agent":"nimbus","report":"Not yet."}]})"));
    CHECK(st.get("iteration").asInt() == 2);
    // A non-director cannot sign off.
    call(*e, "studio_loop_advance", J(R"({"loop":"polish","signoff":true})"), false, "agent:aurora");
    call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"agent":"aurora","report":"ok"},{"agent":"stratus","report":"ok"}]})"));
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","approve":true})"));
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","reports":[{"agent":"nimbus","report":"SIGNOFF — ship it."}]})"));
    CHECK(st.get("status").asString() == "done");
    CHECK(st.get("stop_reason").asString() == "director signed off");

    // Token budget stops a run early.
    call(*e, "studio_loop_define", J(R"({"name":"polish","stop":{"token_budget":1000,"director_signoff":false}})"));
    st = call(*e, "studio_loop_start", J(R"({"loop":"polish"})"));
    e->studio().recordUsage("aurora", "claude-opus-5-5", 2000, 10, 0, 0);
    st = call(*e, "studio_loop_advance", J(R"({"loop":"polish","complete_stage":true})"));
    CHECK(st.get("status").asString() == "stopped");
    CHECK(st.get("stop_reason").asString().find("token budget") != std::string::npos);
    // Stop and status listing
    Json list = call(*e, "studio_loop_status", J("{}"));
    CHECK(list.get("loops").size() == 1);
    CHECK(list.get("templates").size() == studio::loopTemplateNames().size());
}
