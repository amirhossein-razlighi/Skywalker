#include "skywalker/studio/Runner.h"

#include <chrono>
#include <optional>
#include <set>
#include <thread>

#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/studio/Catalog.h"

namespace sky::studio {

namespace {

/// Runs `fn` on the engine's thread and waits. False if the engine refused the job.
bool onMain(Engine& engine, const std::function<void()>& fn) {
    auto f = engine.post([&fn] {
        fn();
        return Json::object({{"ran", true}});
    });
    return f.get().get("ran").asBool();
}

std::string firstLine(const std::string& s, size_t max = 120) {
    std::string line = s.substr(0, s.find('\n'));
    return line.size() > max ? line.substr(0, max) + "…" : line;
}

std::string brief(const Json& args, size_t max = 140) {
    std::string s = args.dump();
    return s.size() > max ? s.substr(0, max) + "…" : s;
}

const std::set<std::string>& loopControlTools() {
    static const std::set<std::string> s = {"studio_loop_start",   "studio_loop_advance", "studio_loop_stop",   "studio_loop_define",
                                            "studio_team_template", "studio_agent_remove", "studio_agent_define"};
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// AgentRunner
// ---------------------------------------------------------------------------

AgentRunner::AgentRunner(Engine& engine, llm::Provider& provider, AgentProfile profile, Options options)
    : engine_(engine), provider_(provider), profile_(std::move(profile)), options_(std::move(options)) {}

void AgentRunner::emit(const std::string& kind, const std::string& text, Json data) {
    if (options_.log) options_.log(RunnerEvent{kind, profile_.id, text, std::move(data)});
}

std::string AgentRunner::systemPrompt(const AgentProfile& a, const std::vector<AgentProfile>& roster) {
    const RoleInfo* role = findRole(a.role);
    std::string title = role ? role->title : a.role;
    std::string mission = !a.mission.empty() ? a.mission : role ? role->mission : "";
    std::string s = "You are " + a.name + " (@" + a.id + "), the " + title + " at a small game studio of AI agents and humans " +
                    "building a game inside the Skywalker game engine. " + mission + "\n";
    s += "Discipline: " + a.discipline + ".";
    if (!a.focus.empty()) s += " Your focus: " + a.focus + ".";
    if (!a.focusTags.empty()) {
        s += " Tags:";
        for (const auto& t : a.focusTags) s += " " + t;
        s += ".";
    }
    if (!a.reportsTo.empty()) s += " You report to @" + a.reportsTo + ".";
    s += "\nPersonality: " + (a.persona.empty() ? std::string("friendly, precise and concise") : a.persona) + "\n";
    if (!str::trim(a.instructions).empty()) s += "\nStanding instructions from the human:\n" + a.instructions + "\n";
    if (!a.memory.empty()) {
        s += "\nYour long-term memory (manage it with studio_memory):\n";
        for (size_t i = 0; i < a.memory.size(); ++i) s += std::to_string(i + 1) + ". " + a.memory[i] + "\n";
    }
    s += "\nThe team:\n";
    for (const auto& m : roster) {
        if (m.id == a.id) continue;
        const RoleInfo* r = findRole(m.role);
        s += "- @" + m.id + " " + m.name + " — " + (r ? r->title : m.role) + (m.focus.empty() ? "" : " (" + m.focus + ")") + "\n";
    }
    s += R"(
How the studio works:
- The board (studio_task_list, studio_task_claim, studio_task_update), feedback (studio_feedback_submit, studio_feedback_list), director decisions (studio_decide — direction and production only) and messages (studio_message_send, studio_inbox) are shared with the whole team and the human. Mention teammates with @id or @role.
- Stay within your discipline and focus. If something belongs to a teammate, message them or file feedback instead of doing their job.
- You change the live game through engine tools. Every change is undoable and shown as done by you. Work loop: understand (scene_overview, studio_inbox) -> act (batch for many edits) -> look (viewport_capture; boxes are labelled with #ids) -> verify (sim_control step, playtest_run) -> report.
- Conventions: meters, +Y up, entities face -Z, rotations are Euler degrees [pitch, yaw, roll], colors "#rrggbb". Behaviors are Wander code: read wander_reference before writing any. Tag the player "player", goals "goal", hazards "hazard", and emit "death" / "fail" / "damage" / "objective" events so playtests can measure the game.
- When you finish, reply with a short report: what you changed or found, and anything still open.)";
    return s;
}

std::vector<AgentRunner::AllowedTool> AgentRunner::toolsFor(const AgentProfile& agent, const ToolRegistry& registry, bool loopMember) {
    std::vector<AllowedTool> out;
    auto consider = [&](const ToolDef& def) {
        if (def.quiet) return;  // plumbing for external harnesses (event polls, tool-host traffic)
        if (loopMember && loopControlTools().count(def.name)) return;
        Access access = agent.access(def.category, !def.mutates, def.openWorld);
        if (access == Access::Off) return;
        out.push_back({llm::ToolSpec{def.name, def.description, def.inputSchema}, def.category, access});
    };
    for (const auto& def : registry.all()) consider(def);
    for (const auto& def : registry.dynamicTools()) consider(def);  // py_* tools served by external processes
    return out;
}

llm::ToolOutcome AgentRunner::execute(const llm::ToolCall& call, const std::vector<AllowedTool>& tools) {
    llm::ToolOutcome out{call.id, call.name, "", {}, false};
    auto it = std::find_if(tools.begin(), tools.end(), [&](const AllowedTool& t) { return t.spec.name == call.name; });
    if (it == tools.end()) {
        out.isError = true;
        out.text = "The tool " + call.name + " is not available to you (permissions or autonomy). Ask the human, or message the owner.";
        emit("tool", call.name + " (not permitted)", Json::object({{"ok", false}}));
        return out;
    }
    if (call.invalidJson) {
        out.isError = true;
        out.text = Json::object({{"INVALID_JSON", call.input}}).dump();
        emit("tool", call.name + " (invalid JSON arguments)", Json::object({{"ok", false}}));
        return out;
    }
    if (it->access == Access::Ask) {
        bool approved = options_.approve && options_.approve(profile_.id, call.name, call.input);
        if (!approved) {
            out.isError = true;
            out.text = "The human declined " + call.name + " (or no human was available to approve it). Try something else or ask.";
            emit("tool", call.name + " (declined)", Json::object({{"ok", false}}));
            return out;
        }
    }
    Json mcp;
    std::string actor = profile_.actor();
    bool ran = onMain(engine_, [&] { mcp = engine_.callTool(call.name, call.input, actor).toMcp(); });
    if (!ran) {
        out.isError = true;
        out.text = "The engine is not accepting requests (shutting down).";
        return out;
    }
    for (const auto& block : mcp.get("content").elements()) {
        if (block.get("type").asString() == "text") {
            out.text += (out.text.empty() ? "" : "\n") + block.get("text").asString();
        } else if (block.get("type").asString() == "image") {
            out.imagesPng.push_back(block.get("data").asString());
        }
    }
    if (out.text.size() > options_.maxToolText) {
        out.text = out.text.substr(0, options_.maxToolText) + "\n… (truncated; ask for less, e.g. with filters or limits)";
    }
    out.isError = mcp.get("isError").asBool();
    std::string summary = out.text.substr(0, out.text.find('\n'));
    if (summary.size() > 120) summary = summary.substr(0, 120) + "…";
    emit("tool", call.name + " " + brief(call.input), Json::object({{"ok", !out.isError}, {"result", summary}, {"tool", call.name}}));
    return out;
}

AgentRunResult AgentRunner::run(const std::string& prompt) {
    AgentRunResult res;
    std::vector<AllowedTool> tools;
    std::vector<AgentProfile> roster;
    bool ok = onMain(engine_, [&] {
        Studio& s = engine_.studio();
        if (const AgentProfile* p = s.agent(profile_.id)) profile_ = *p;  // latest definition
        roster = s.agents();
        tools = toolsFor(profile_, engine_.tools(), options_.loopMember);
        s.setPresence(profile_.id, "working", firstLine(prompt));
    });
    if (!ok) {
        res.ok = false;
        res.stop = "error";
        res.error = "the engine is not accepting requests";
        return res;
    }
    llm::SessionConfig cfg;
    cfg.agentId = profile_.id;
    cfg.model = profile_.model.empty() ? provider_.defaultModel() : profile_.model;
    cfg.system = systemPrompt(profile_, roster);
    for (const auto& t : tools) cfg.tools.push_back(t.spec);
    emit("agent_start", firstLine(prompt),
         Json::object({{"model", cfg.model}, {"tools", static_cast<int64_t>(tools.size())}, {"prompt", prompt}}));

    std::unique_ptr<llm::Session> session = provider_.open(cfg);
    std::vector<llm::ToolOutcome> outcomes;
    std::string text = prompt;
    std::string lastText;
    bool resumeNext = false;
    int pauses = 0;
    int maxRounds = options_.maxRounds > 0 ? options_.maxRounds : std::max(1, profile_.maxRounds);
    for (int round = 0; round < maxRounds; ++round) {
        if (options_.cancel && options_.cancel->load()) {
            res.stop = "cancelled";
            break;
        }
        auto turn = resumeNext ? session->resume() : session->send(outcomes, text);
        resumeNext = false;
        text.clear();
        outcomes.clear();
        ++res.rounds;
        if (!turn) {
            res.ok = false;
            res.stop = "error";
            res.error = turn.error().message + (turn.error().hint.empty() ? "" : " (" + turn.error().hint + ")");
            emit("error", res.error);
            break;
        }
        res.usage += turn->usage;
        std::string model = turn->model.empty() ? cfg.model : turn->model;
        llm::TokenUsage u = turn->usage;
        onMain(engine_, [&] { engine_.studio().recordUsage(profile_.id, model, u.input, u.output, u.cacheRead, u.cacheWrite); });
        if (turn->stop == llm::Stop::Refusal) {
            // Partial output of a declined turn is discarded, and its tools never run.
            res.stop = "refusal";
            res.error = "the model declined: " + turn->stopDetail;
            emit("error", res.error);
            break;
        }
        if (!turn->text.empty()) {
            lastText = turn->text;
            emit("text", turn->text);
        }
        if (turn->stop == llm::Stop::PauseTurn) {
            if (++pauses > 5) {
                res.ok = false;
                res.stop = "error";
                res.error = "the model paused too many times";
                break;
            }
            resumeNext = true;
            continue;
        }
        if (turn->calls.empty()) {
            res.stop = turn->stop == llm::Stop::MaxTokens ? "max_tokens" : "done";
            break;
        }
        for (const auto& call : turn->calls) {
            if (options_.cancel && options_.cancel->load()) {
                outcomes.push_back({call.id, call.name, "Cancelled by the human.", {}, true});
            } else if (turn->stop == llm::Stop::MaxTokens) {
                // The reply was cut off mid-call: never run a possibly truncated input.
                outcomes.push_back({call.id, call.name,
                                    "Your reply was cut off (max_tokens) before this call was complete, so it was not run. "
                                    "Re-issue it with smaller arguments.",
                                    {},
                                    true});
            } else {
                outcomes.push_back(execute(call, tools));
                ++res.toolCalls;
            }
        }
    }
    if (res.stop.empty()) {
        res.stop = "max_rounds";
        lastText += (lastText.empty() ? "" : "\n") + std::string("(stopped after reaching the round limit)");
    }
    res.report = lastText.empty() ? "(no report)" : lastText;
    onMain(engine_, [&] { engine_.studio().setPresence(profile_.id, "idle", ""); });
    emit("agent_done", res.report,
         Json::object({{"stop", res.stop},
                       {"rounds", res.rounds},
                       {"tool_calls", res.toolCalls},
                       {"input_tokens", res.usage.input},
                       {"output_tokens", res.usage.output}}));
    return res;
}

// ---------------------------------------------------------------------------
// LoopRunner
// ---------------------------------------------------------------------------

LoopRunner::LoopRunner(Engine& engine, Options options) : engine_(engine), options_(std::move(options)) {}

void LoopRunner::emit(const std::string& kind, const std::string& agent, const std::string& text, Json data) {
    if (options_.log) options_.log(RunnerEvent{kind, agent, text, std::move(data)});
}

Json LoopRunner::call(const std::string& tool, const Json& args, bool& ok, std::string& error) {
    Json mcp;
    bool ran = onMain(engine_, [&] { mcp = engine_.callTool(tool, args, "studio:runner").toMcp(); });
    if (!ran) {
        ok = false;
        error = "the engine is not accepting requests";
        return {};
    }
    ok = !mcp.get("isError").asBool();
    if (!ok) error = mcp.get("content")[size_t{0}].get("text").asString();
    return mcp.get("structuredContent");
}

Result<Json> LoopRunner::run(const std::string& loopName) {
    Json args = Json::object({{"loop", loopName}});
    if (!options_.goal.empty()) args["goal"] = options_.goal;
    if (options_.maxIterations > 0) args["max_iterations"] = options_.maxIterations;
    bool ok = false;
    std::string error;
    Json st = call("studio_loop_start", args, ok, error);
    for (int guard = 0; guard < 100000; ++guard) {
        if (!ok) return Error::make("loop_error", error);
        std::string status = st.get("status").asString();
        if (options_.cancel && options_.cancel->load()) {
            Json stopped = call("studio_loop_stop", Json::object({{"loop", loopName}, {"reason", "cancelled"}}), ok, error);
            return ok ? stopped : st;
        }
        if (status == "awaiting_approval") {
            if (!options_.approveGates) {
                emit("loop", "", "waiting for a human approval (studio_loop_advance {approve: true})", st);
                return st;
            }
            st = call("studio_loop_advance", Json::object({{"loop", loopName}, {"approve", true}}), ok, error);
            continue;
        }
        if (status != "running") return st;

        const Json assignments = st.get("assignments");
        bool parallel = st.get("parallel").asBool(true);
        emit("stage", "", st.get("stage").asString(),
             Json::object({{"iteration", st.get("iteration")}, {"agents", st.get("waiting_for")}, {"parallel", parallel}}));
        std::vector<Json> reports(assignments.size());
        auto work = [&](size_t i) {
            const Json& a = assignments[i];
            std::string id = a.get("agent").asString();
            std::optional<AgentProfile> profile;
            onMain(engine_, [&] {
                if (const AgentProfile* p = engine_.studio().agent(id)) profile = *p;
            });
            if (!profile) {
                reports[i] = Json::object({{"agent", id}, {"report", "(agent no longer on the roster)"}});
                return;
            }
            auto provider = options_.providers ? options_.providers(*profile)
                                               : Result<llm::Provider*>(Error::make("no_provider", "no provider configured"));
            if (!provider || !*provider) {
                std::string why = provider ? "no provider" : provider.error().message;
                emit("error", id, why);
                reports[i] = Json::object({{"agent", id}, {"report", "(could not run: " + why + ")"}});
                return;
            }
            AgentRunner::Options o;
            o.approve = options_.approve;
            o.log = options_.log;
            o.loopMember = true;
            o.cancel = options_.cancel;
            AgentRunner runner(engine_, **provider, *profile, o);
            AgentRunResult r = runner.run(a.get("prompt").asString());
            std::string report = r.report;
            if (!r.ok) report = "(failed: " + r.error + ") " + report;
            else if (r.stop == "refusal") report = "(declined: " + r.error + ") " + report;
            reports[i] = Json::object({{"agent", id}, {"report", report}});
        };
        if (parallel && assignments.size() > 1) {
            std::vector<std::thread> threads;
            threads.reserve(assignments.size());
            for (size_t i = 0; i < assignments.size(); ++i) threads.emplace_back(work, i);
            for (auto& t : threads) t.join();
        } else {
            for (size_t i = 0; i < assignments.size(); ++i) work(i);
        }
        if (options_.cancel && options_.cancel->load()) continue;  // handled at the top
        Json arr = Json::array();
        for (auto& r : reports) arr.push(std::move(r));
        st = call("studio_loop_advance", Json::object({{"loop", loopName}, {"reports", arr}}), ok, error);
    }
    return Error::make("loop_error", "the loop did not finish");
}

Result<Json> runWhilePumping(Engine& engine, const std::function<Result<Json>()>& fn) {
    std::optional<Result<Json>> out;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        try {
            out.emplace(fn());
        } catch (const std::exception& e) {
            out.emplace(Error::make("internal_error", e.what()));
        } catch (...) {
            out.emplace(Error::make("internal_error", "unknown exception"));
        }
        done.store(true);
    });
    while (!done.load()) {
        engine.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    engine.pump();
    return std::move(*out);
}

}  // namespace sky::studio
