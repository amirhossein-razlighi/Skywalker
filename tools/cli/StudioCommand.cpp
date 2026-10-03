// `skywalker studio` — the studio from the command line (and CI).
//
//   skywalker studio status   --project DIR             roster, board, feedback, loops, usage
//   skywalker studio agents   --project DIR             the roster
//   skywalker studio board    --project DIR             kanban
//   skywalker studio feedback --project DIR             feedback with verdicts and measured effects
//   skywalker studio loops    --project DIR             loops and templates
//   skywalker studio run      --project DIR --loop NAME [--iterations N] [--goal TEXT]
//                             [--dry-run | --mock SCRIPT.json] [--yes] [--scene FILE] [--no-save] [--verbose]
//
// `run` drives a loop headlessly: engine stages (playtests) run in the engine, agent stages
// run through the LLM runner (keys from ANTHROPIC_API_KEY / OPENAI_API_KEY only). Output is
// a live, readable log; Ctrl-C stops the loop cleanly.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/studio/Catalog.h"
#include "skywalker/studio/Runner.h"
#include "skywalker/studio/Studio.h"

using namespace sky;

namespace {

std::atomic<bool> gCancel{false};

void onInterrupt(int) { gCancel.store(true); }

struct Flags {
    std::vector<std::string> raw;
    std::string get(const std::string& flag, const std::string& fallback = "") const {
        for (size_t i = 0; i + 1 < raw.size(); ++i) {
            if (raw[i] == flag) return raw[i + 1];
        }
        return fallback;
    }
    bool has(const std::string& flag) const { return std::find(raw.begin(), raw.end(), flag) != raw.end(); }
};

struct Style {
    bool on = false;
    std::string dim() const { return on ? "\033[2m" : ""; }
    std::string bold() const { return on ? "\033[1m" : ""; }
    std::string green() const { return on ? "\033[32m" : ""; }
    std::string red() const { return on ? "\033[31m" : ""; }
    std::string yellow() const { return on ? "\033[33m" : ""; }
    std::string cyan() const { return on ? "\033[36m" : ""; }
    std::string magenta() const { return on ? "\033[35m" : ""; }
    std::string reset() const { return on ? "\033[0m" : ""; }
};

std::string clip(const std::string& s, size_t n) {
    std::string one = s;
    for (auto& c : one) {
        if (c == '\n') c = ' ';
    }
    return one.size() > n ? one.substr(0, n) + "…" : one;
}

std::string pad(const std::string& s, size_t n) { return s.size() >= n ? s : s + std::string(n - s.size(), ' '); }

void printStatus(studio::Studio& s, const Style& st) {
    Json o = s.overview();
    std::printf("%sStudio%s  %s\n", st.bold().c_str(), st.reset().c_str(), s.projectDir().c_str());
    std::printf("  agents   %zu\n", s.agents().size());
    std::printf("  board    ");
    for (const auto& [k, v] : o.get("board").members()) {
        if (v.asInt()) std::printf("%s %lld  ", k.c_str(), static_cast<long long>(v.asInt()));
    }
    std::printf("\n  feedback ");
    for (const auto& [k, v] : o.get("feedback").members()) {
        if (v.asInt()) std::printf("%s %lld  ", k.c_str(), static_cast<long long>(v.asInt()));
    }
    std::printf("\n");
    if (o.get("needs_decision").size()) {
        std::printf("%sneeds a decision%s\n", st.yellow().c_str(), st.reset().c_str());
        for (const auto& n : o.get("needs_decision").elements()) std::printf("  %s\n", n.asString().c_str());
    }
    for (const auto& l : o.get("loops").elements()) {
        std::printf("  loop %s: %s\n", l.get("name").asString().c_str(), l.get("status").asString().c_str());
    }
    if (!o.get("latest_playtest").asString().empty()) {
        std::printf("  latest playtest %s %s\n", o.get("latest_playtest").asString().c_str(), o.get("latest_metrics").dump().c_str());
    }
    const Json& u = o.get("usage");
    std::printf("  usage    %lld tokens in, %lld out, %lld cached · %lld requests · %lld tool calls · $%.2f\n",
                static_cast<long long>(u.get("input_tokens").asInt()), static_cast<long long>(u.get("output_tokens").asInt()),
                static_cast<long long>(u.get("cache_read_tokens").asInt()), static_cast<long long>(u.get("requests").asInt()),
                static_cast<long long>(u.get("tool_calls").asInt()), u.get("cost_usd").asNumber());
    if (s.agents().empty()) std::printf("\nno agents yet: skywalker call studio_team_template '{\"template\":\"indie_trio\"}' --project DIR\n");
}

void printAgents(studio::Studio& s, const Style& st) {
    for (const auto& a : s.agents()) {
        const auto* role = studio::findRole(a.role);
        auto u = s.usage().find(a.id);
        std::printf("%s@%s%s %s— %s%s%s · %s · %s%s\n", st.bold().c_str(), pad(a.id, 10).c_str(), st.reset().c_str(),
                    "", role ? role->title.c_str() : a.role.c_str(), a.focus.empty() ? "" : " (", a.focus.empty() ? "" : (a.focus + ")").c_str(),
                    studio::toString(a.autonomy), a.model.empty() ? (a.provider + " default").c_str() : a.model.c_str(),
                    u == s.usage().end() ? "" : (" · " + std::to_string(u->second.totalTokens()) + " tokens").c_str());
    }
    if (s.agents().empty()) std::printf("(no agents)\n");
}

void printBoard(studio::Studio& s, const Style& st) {
    for (const auto& status : studio::taskStatuses()) {
        std::vector<const studio::Task*> col;
        for (const auto& t : s.tasks()) {
            if (t.status == status) col.push_back(&t);
        }
        if (col.empty()) continue;
        std::printf("%s%s%s (%zu)\n", st.bold().c_str(), status.c_str(), st.reset().c_str(), col.size());
        for (const auto* t : col) {
            std::printf("  %s %s%s%s%s\n", pad(t->id, 5).c_str(), t->title.c_str(), st.dim().c_str(),
                        (t->assignee.empty() ? "" : "  @" + t->assignee).c_str(), st.reset().c_str());
        }
    }
    if (s.tasks().empty()) std::printf("(empty board)\n");
}

void printFeedback(studio::Studio& s, const Style& st) {
    for (const auto& f : s.feedback()) {
        std::string color = f.status == "verified" ? st.green() : f.status == "regressed" ? st.red() : f.status == "open" ? st.yellow() : "";
        std::printf("%s %s[%s]%s %s/%s %s%s\n", pad(f.id, 5).c_str(), color.c_str(), f.status.c_str(), st.reset().c_str(),
                    f.category.c_str(), f.severity.c_str(), f.summary.c_str(), f.occurrences > 1 ? (" ×" + std::to_string(f.occurrences)).c_str() : "");
        if (const studio::Decision* d = f.decision.empty() ? nullptr : s.decision(f.decision)) {
            std::printf("      %s→ %s by %s: %s%s\n", st.dim().c_str(), d->verdict.c_str(), d->by.c_str(), clip(d->rationale, 110).c_str(),
                        st.reset().c_str());
            std::string eff = d->effect.get("status").asString();
            if (!eff.empty() && eff != "n/a" && eff != "pending") {
                std::string metrics;
                for (const auto& [k, row] : d->effect.get("metrics").members()) {
                    metrics += (metrics.empty() ? "" : ", ") + k + " " + row.get("before").dump() + "→" + row.get("after").dump();
                }
                std::printf("      effect: %s%s%s %s\n", eff == "improved" ? st.green().c_str() : eff == "regressed" ? st.red().c_str() : "",
                            eff.c_str(), st.reset().c_str(), metrics.c_str());
            }
        }
    }
    if (s.feedback().empty()) std::printf("(no feedback)\n");
}

void printLoops(studio::Studio& s) {
    for (const auto& l : s.loops()) {
        Json st = s.loopStatus(l, false);
        std::printf("%s: %s", l.name.c_str(), st.get("status").asString().c_str());
        if (!st.get("iteration").isNull()) std::printf(" · iteration %s/%s", st.get("iteration").dump().c_str(), st.get("max_iterations").dump().c_str());
        if (st.contains("stop_reason")) std::printf(" · %s", st.get("stop_reason").asString().c_str());
        std::printf("\n  goal: %s\n  stages:", l.goal.c_str());
        for (const auto& stage : l.stages) std::printf(" %s", stage.id.c_str());
        std::printf("\n");
    }
    std::printf("templates:\n");
    for (const auto& t : studio::loopTemplateNames()) std::printf("  %s — %s\n", t.c_str(), studio::loopTemplateDescription(t).c_str());
}

/// Prints studio activity events (decisions, feedback, playtests, loop progress).
void printEvent(const Json& e, const Style& st) {
    if (e.get("type").asString() != "studio") return;
    const std::string& kind = e.get("kind").asString();
    const std::string& action = e.get("action").asString();
    const std::string& id = e.get("id").asString();
    std::string summary = e.get("summary").asString();
    if (kind == "playtest") {
        std::printf("  %s●%s %s\n", st.cyan().c_str(), st.reset().c_str(), summary.c_str());
    } else if (kind == "feedback") {
        if (action == "submitted") std::printf("    %s+ %s%s %s\n", st.yellow().c_str(), id.c_str(), st.reset().c_str(), clip(summary, 150).c_str());
        else if (action == "seen_again") std::printf("    %s~ %s seen again%s (×%lld) %s\n", st.dim().c_str(), id.c_str(), st.reset().c_str(),
                                                    static_cast<long long>(e.get("occurrences").asInt()), clip(summary, 120).c_str());
        else if (action == "effect") std::printf("    %s⇢ %s%s %s → %s\n", st.magenta().c_str(), id.c_str(), st.reset().c_str(),
                                                clip(summary, 150).c_str(), e.get("status").asString().c_str());
        else if (action == "fixed") std::printf("    %s✓ %s fixed%s %s\n", st.green().c_str(), id.c_str(), st.reset().c_str(), clip(summary, 120).c_str());
    } else if (kind == "decision") {
        std::printf("    %s⚖ %s%s %s\n", st.magenta().c_str(), id.c_str(), st.reset().c_str(), clip(summary, 170).c_str());
    } else if (kind == "task") {
        if (action == "created") std::printf("    ▢ %s %s\n", id.c_str(), clip(summary, 150).c_str());
        else if (action == "status") std::printf("    ▣ %s %s\n", id.c_str(), clip(summary, 150).c_str());
    } else if (kind == "message") {
        std::printf("    ✉ %s\n", clip(summary, 160).c_str());
    } else if (kind == "loop") {
        if (action == "started") std::printf("%s▶ %s%s\n", st.bold().c_str(), summary.c_str(), st.reset().c_str());
        else if (action == "iteration_started") std::printf("%s— %s%s\n", st.bold().c_str(), summary.c_str(), st.reset().c_str());
        else if (action == "stage_started") std::printf("%s◆ %s%s\n", st.cyan().c_str(), summary.c_str(), st.reset().c_str());
        else if (action == "finished") std::printf("%s■ %s%s\n", st.bold().c_str(), summary.c_str(), st.reset().c_str());
        else if (action == "awaiting_approval") std::printf("%s⏸ %s%s\n", st.yellow().c_str(), summary.c_str(), st.reset().c_str());
    }
}

int runLoop(Engine& engine, const Flags& f, const Style& st) {
    studio::Studio& studio = engine.studio();
    std::string name = f.get("--loop");
    if (name.empty() && studio.loops().size() == 1) name = studio.loops().front().name;
    if (name.empty()) {
        std::fprintf(stderr, "error: which loop? pass --loop NAME\n");
        printLoops(studio);
        return 2;
    }
    if (!studio.loop(name)) {
        auto names = studio::loopTemplateNames();
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            std::fprintf(stderr, "error: no loop '%s'\n", name.c_str());
            printLoops(studio);
            return 2;
        }
        ToolResult r = engine.callTool("studio_loop_define", Json::object({{"template", name}}), "cli");
        if (r.isError) {
            std::fprintf(stderr, "error: %s\n", r.content.front().text.c_str());
            return 1;
        }
        std::printf("defined loop %s from its template\n", name.c_str());
    }

    bool dryRun = f.has("--dry-run");
    bool verbose = f.has("--verbose") || dryRun;
    std::unique_ptr<studio::llm::MockProvider> mock;
    if (dryRun) mock = std::make_unique<studio::llm::MockProvider>(Json::object({{"echo", true}}));
    if (!f.get("--mock").empty()) {
        std::ifstream in(f.get("--mock"));
        std::stringstream ss;
        ss << in.rdbuf();
        auto script = Json::parse(ss.str());
        if (!in || !script) {
            std::fprintf(stderr, "error: cannot read mock script %s\n", f.get("--mock").c_str());
            return 1;
        }
        mock = std::make_unique<studio::llm::MockProvider>(*script);
    }
    std::mutex providersMutex;
    std::map<std::string, std::unique_ptr<studio::llm::Provider>> providers;
    std::mutex out;
    bool yes = f.has("--yes");

    studio::LoopRunner::Options o;
    o.providers = [&](const studio::AgentProfile& a) -> Result<studio::llm::Provider*> {
        if (mock) return static_cast<studio::llm::Provider*>(mock.get());
        std::lock_guard lock(providersMutex);
        std::string kind = str::lower(a.provider);
        auto it = providers.find(kind);
        if (it != providers.end()) return it->second.get();
        auto p = studio::llm::fromEnvironment(kind);
        if (!p) return p.error();
        studio::llm::Provider* raw = p->get();
        providers[kind] = std::move(*p);
        return raw;
    };
    o.approve = [&](const std::string& agent, const std::string& tool, const Json&) {
        if (!yes) {
            std::lock_guard lock(out);
            std::printf("    %s@%s wanted %s — declined (needs approval; pass --yes to allow)%s\n", st.yellow().c_str(), agent.c_str(),
                        tool.c_str(), st.reset().c_str());
        }
        return yes;
    };
    o.approveGates = yes;
    o.cancel = &gCancel;
    o.goal = f.get("--goal");
    if (!f.get("--iterations").empty()) o.maxIterations = std::max(1, std::atoi(f.get("--iterations").c_str()));
    o.log = [&](const studio::RunnerEvent& e) {
        std::lock_guard lock(out);
        std::string who = "@" + e.agent;
        if (e.kind == "agent_start") {
            std::printf("  %s▸ %s%s %s%s%s\n", st.bold().c_str(), who.c_str(), st.reset().c_str(), st.dim().c_str(),
                        e.data.get("model").asString().c_str(), st.reset().c_str());
        } else if (e.kind == "text") {
            std::printf("    %s ✎ %s\n", who.c_str(), clip(e.text, verbose ? 600 : 220).c_str());
        } else if (e.kind == "tool") {
            bool okCall = e.data.get("ok").asBool(true);
            std::printf("    %s %s⚙%s %s %s%s%s\n", who.c_str(), st.dim().c_str(), st.reset().c_str(), clip(e.text, 150).c_str(),
                        okCall ? st.green().c_str() : st.red().c_str(), okCall ? "✓" : "✗", st.reset().c_str());
        } else if (e.kind == "agent_done") {
            std::printf("  %s◂ %s%s %s · %lld rounds · %lld tool calls · %lld/%lld tokens\n", st.dim().c_str(), who.c_str(), st.reset().c_str(),
                        e.data.get("stop").asString().c_str(), static_cast<long long>(e.data.get("rounds").asInt()),
                        static_cast<long long>(e.data.get("tool_calls").asInt()), static_cast<long long>(e.data.get("input_tokens").asInt()),
                        static_cast<long long>(e.data.get("output_tokens").asInt()));
        } else if (e.kind == "error") {
            std::printf("    %s%s ✗ %s%s\n", st.red().c_str(), who.c_str(), e.text.c_str(), st.reset().c_str());
        } else if (e.kind == "stage") {
            std::string agents;
            for (const auto& a : e.data.get("agents").elements()) agents += (agents.empty() ? "" : ", ") + ("@" + a.asString());
            std::printf("  → %s%s\n", agents.c_str(), e.data.get("parallel").asBool() && e.data.get("agents").size() > 1 ? " (in parallel)" : "");
        }
        std::fflush(stdout);
    };

    if (dryRun) std::printf("%sdry run: agents answer with a stub (no LLM calls); playtests run for real%s\n", st.dim().c_str(), st.reset().c_str());
    std::signal(SIGINT, onInterrupt);
    studio::LoopRunner runner(engine, o);
    std::optional<Result<Json>> result;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        result.emplace(runner.run(name));
        done.store(true);
    });
    auto printEvents = [&] {
        auto events = engine.drainEvents();
        if (events.empty()) return;
        std::lock_guard lock(out);
        for (const auto& e : events) printEvent(e, st);
        std::fflush(stdout);
    };
    while (!done.load()) {
        engine.pump();
        printEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();
    engine.pump();
    printEvents();
    std::signal(SIGINT, SIG_DFL);

    if (!*result) {
        std::fprintf(stderr, "error: %s\n", result->error().message.c_str());
        return 1;
    }
    const Json& final = result->value();
    Json o2 = studio.overview();
    const Json& u = o2.get("usage");
    std::printf("\n%s%s%s: %s%s · %lld in / %lld out tokens · $%.2f\n", st.bold().c_str(), name.c_str(), st.reset().c_str(),
                final.get("status").asString().c_str(),
                final.contains("stop_reason") ? (" (" + final.get("stop_reason").asString() + ")").c_str() : "",
                static_cast<long long>(u.get("input_tokens").asInt()), static_cast<long long>(u.get("output_tokens").asInt()),
                u.get("cost_usd").asNumber());
    if (final.get("trends").isObject() && !final.get("trends").members().empty()) {
        for (const char* k : {"completion_rate", "deaths", "time_to_goal", "stuck_seconds"}) {
            if (final.get("trends").contains(k)) std::printf("  %s: %s\n", k, final.get("trends").get(k).dump().c_str());
        }
    }
    if (!dryRun && !f.has("--no-save") && !engine.scenePath().empty()) {
        if (Status s = engine.saveScene(""); s) std::printf("saved %s\n", engine.scenePath().c_str());
        else std::fprintf(stderr, "warning: could not save the scene: %s\n", s.error().message.c_str());
    }
    std::string status = final.get("status").asString();
    return status == "done" || status == "stopped" || status == "awaiting_approval" ? 0 : 1;
}

}  // namespace

int runStudio(const std::vector<std::string>& raw) {
    Flags f{raw};
    std::vector<std::string> positional;
    for (size_t i = 0; i < raw.size(); ++i) {
        const std::string& r = raw[i];
        if (r.rfind("-", 0) == 0) {
            bool takesValue = r == "--project" || r == "--scene" || r == "--loop" || r == "--iterations" || r == "--mock" || r == "--goal";
            if (takesValue) ++i;
            continue;
        }
        positional.push_back(r);
    }
    std::string sub = positional.size() > 1 ? positional[1] : "status";
    Style st;
    st.on = isatty(STDOUT_FILENO) && !f.has("--no-color");

    EngineConfig cfg;
    cfg.projectDir = f.get("--project", ".");
    if (!std::filesystem::is_directory(cfg.projectDir)) {
        std::fprintf(stderr, "error: project directory %s does not exist\n", cfg.projectDir.c_str());
        return 1;
    }
    if (f.has("--null-renderer")) cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    if (sub == "run") {
        std::string scene = f.get("--scene");
        if (scene.empty() && std::filesystem::exists(std::filesystem::path(cfg.projectDir) / "scenes/main.sky.json")) {
            scene = "scenes/main.sky.json";
        }
        if (!scene.empty()) {
            if (Status s = engine.loadScene(scene); !s) {
                std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
                return 1;
            }
            std::printf("scene %s\n", scene.c_str());
        } else {
            std::printf("no scene (pass --scene FILE): playtests need a player\n");
            (void)engine.newScene("Untitled", true);
        }
        (void)engine.drainEvents();
        return runLoop(engine, f, st);
    }
    studio::Studio& s = engine.studio();
    if (sub == "status") printStatus(s, st);
    else if (sub == "agents") printAgents(s, st);
    else if (sub == "board") printBoard(s, st);
    else if (sub == "feedback") printFeedback(s, st);
    else if (sub == "loops") printLoops(s);
    else {
        std::fprintf(stderr,
                     "usage: skywalker studio status|agents|board|feedback|loops --project DIR\n"
                     "       skywalker studio run --project DIR --loop NAME [--iterations N] [--goal TEXT]\n"
                     "                            [--dry-run | --mock SCRIPT.json] [--yes] [--scene FILE] [--no-save] [--verbose]\n");
        return 2;
    }
    return 0;
}
