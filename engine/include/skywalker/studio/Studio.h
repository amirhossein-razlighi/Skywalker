#pragma once
// Studio: a multi-agent game studio that lives in the project.
//
// The studio is shared by every client (the editor's crew, the headless runner, Claude
// Code, Codex, Gemini CLI, Cursor) because it lives engine-side and is reached through
// tools. Its state is plain, human-readable, git-friendly files in the project:
//
//   agents/<id>.agent.json          roster: one profile per agent
//   studio/board.json               tasks (kanban)
//   studio/feedback.json            feedback from playtesters, critics, agents, humans
//   studio/decisions.json           director/producer verdicts and their measured effect
//   studio/loops/<name>.loop.json   user-defined loops (definition + runtime state)
//   studio/messages.jsonl           channels and threads (append-only)
//   studio/usage.json               tokens, estimated cost and tool calls per agent
//   studio/playtests/<id>/          playtest reports, heatmaps and screenshots
//
// Threading: like the Engine, the Studio is main-thread only. Runners on worker threads
// reach it through Engine::post. Every mutation is written to disk immediately (atomic
// rename) and announced as an activity event ({"type":"studio", ...}).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::studio {

// ---------------------------------------------------------------------------
// Roster
// ---------------------------------------------------------------------------

enum class Autonomy { Observe, Ask, Autonomous };
enum class Access { Inherit, Allow, Ask, Off };

const char* toString(Autonomy a);
const char* toString(Access a);
std::optional<Autonomy> parseAutonomy(std::string_view s);
std::optional<Access> parseAccess(std::string_view s);

struct AgentProfile {
    std::string id;          // stable slug: file name and mention handle (@id)
    std::string name;        // display name
    std::string discipline;  // direction, production, design, engineering, art, audio, writing, qa
    std::string role;        // creative_director, level_designer, ... (see roles())
    std::string focus;       // what this agent specializes in, in words
    std::vector<std::string> focusTags;
    std::string persona;       // personality / voice
    std::string mission;       // replaces the role's built-in mission when not empty
    std::string instructions;  // standing instructions (style guides, constraints)
    std::string provider = "anthropic";  // anthropic | openai | mock | an editor provider name
    std::string model;                   // empty = provider default
    Autonomy autonomy = Autonomy::Autonomous;
    std::map<std::string, Access> permissions;  // tool category -> access
    std::string reportsTo;
    std::string color = "#7fb3ff";
    std::string face = "happy";
    std::vector<std::string> memory;
    int maxRounds = 40;
    Json playtest = Json::object();  // playtester persona knobs {policy, reaction_time, skill, curiosity, patience}

    /// Effective access for a tool. Studio coordination tools (category "studio") are allowed
    /// at every autonomy level unless explicitly turned off: they never change the game.
    Access access(const std::string& category, bool readOnly, bool openWorld) const;
    std::string actor() const { return "agent:" + id; }

    Json toJson() const;
    static Result<AgentProfile> fromJson(const Json& j);
};

// ---------------------------------------------------------------------------
// Board, feedback, decisions
// ---------------------------------------------------------------------------

struct Comment {
    std::string by, at, text;
};

struct Task {
    std::string id;  // T-12
    std::string title;
    std::string description;
    std::vector<std::string> acceptance;
    std::string discipline;
    std::string assignee;  // agent id, or empty
    std::string status = "todo";      // backlog | todo | doing | review | done | dropped
    std::string priority = "normal";  // low | normal | high | critical
    std::vector<std::string> dependsOn;
    std::map<std::string, std::vector<std::string>> links;  // feedback, entities, assets, captures, playtests, loop
    std::vector<Comment> comments;
    std::string createdBy, createdAt, updatedAt;

    Json toJson() const;
    static Task fromJson(const Json& j);
};

struct Feedback {
    std::string id;  // F-7
    std::string by;  // actor that submitted it
    std::string category;  // fun, difficulty, clarity, visuals, audio, performance, bug, narrative, accessibility
    std::string severity = "medium";  // low | medium | high | critical
    std::string summary;
    std::string details;
    Json evidence = Json::object();  // {captures, metrics, playtest, trace, positions, repro}
    std::string target;              // entity / area / system / asset it is about
    std::string status = "open";     // open accepted dropped deferred in_progress fixed verified regressed
    std::string fingerprint;         // dedup key for automatically filed findings
    int occurrences = 1;
    std::string decision;            // D-id
    std::vector<std::string> tasks;  // T-ids created by the decision
    std::string mergedInto;
    std::string createdAt, updatedAt;
    std::vector<Comment> history;  // status changes with notes

    Json toJson() const;
    static Feedback fromJson(const Json& j);
};

struct Decision {
    std::string id;  // D-3
    std::string feedback;
    std::string verdict;  // act | drop | defer | merge_into
    std::string rationale;
    std::string by, at;
    std::vector<std::string> tasks;
    std::string mergeInto;
    Json metricsBefore = Json::object();  // snapshot when the decision was made
    std::string playtestBefore;
    Json targets = Json::object();  // {"deaths": {"max": 1}} — what "better" means for this item
    Json effect = Json::object();   // {status: pending|improved|regressed|unchanged|mixed|unmeasured, ...}

    Json toJson() const;
    static Decision fromJson(const Json& j);
};

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

struct LoopStage {
    std::string id;
    std::string title;
    std::string kind = "agents";  // agents | playtest
    std::vector<std::string> assignees;  // "@role", "@discipline", agent ids
    std::string instruction;             // template: {{goal}} {{inputs}} {{my_tasks}} ...
    std::vector<std::string> inputs;     // stage ids whose reports feed this stage (default: all earlier)
    bool parallel = true;
    bool onlyWithTasks = false;  // only agents holding open tasks get work (fix stages)
    Json gate = Json::object();  // {skip_if: "...", approval: "human"}
    Json playtest = Json::object();  // playtest_run arguments (kind == playtest)
    bool fileFeedback = true;        // playtest stages: file findings as feedback
    bool verifyFixed = false;        // playtest stages: measure the effect of fixed feedback

    Json toJson() const;
    static Result<LoopStage> fromJson(const Json& j);
};

struct LoopStop {
    int maxIterations = 3;
    Json metricTargets = Json::object();  // {"completion_rate": {"min": 0.8}, "deaths": {"max": 1}}
    bool directorSignoff = false;
    int64_t tokenBudget = 0;         // 0 = unlimited (input + output tokens across the studio)
    double timeBudgetMinutes = 0;    // 0 = unlimited

    Json toJson() const;
    static LoopStop fromJson(const Json& j);
};

struct Loop {
    std::string name;
    std::string goal;
    std::string description;
    std::string templateName;
    std::vector<LoopStage> stages;
    LoopStop stop;
    Json state = Json::object();  // runtime state (status, iteration, stage, pending, history, trends)

    Json toJson() const;
    static Result<Loop> fromJson(const Json& j);
};

// ---------------------------------------------------------------------------
// Messages and usage
// ---------------------------------------------------------------------------

struct Message {
    std::string id;  // M-31
    std::string at;
    std::string from;
    std::string channel = "general";
    std::vector<std::string> to;        // explicit recipients (agent ids)
    std::vector<std::string> mentions;  // @ids/@roles resolved to agent ids
    std::string thread;                 // root message id ("" = new thread)
    std::string text;
    Json refs = Json::object();  // {task, feedback, decision, loop}

    Json toJson() const;
    static Message fromJson(const Json& j);
};

struct Usage {
    int64_t inputTokens = 0;
    int64_t outputTokens = 0;
    int64_t cacheReadTokens = 0;
    int64_t cacheWriteTokens = 0;
    int64_t requests = 0;
    int64_t toolCalls = 0;
    int64_t toolErrors = 0;
    double costUsd = 0;
    std::string lastActive;

    int64_t totalTokens() const { return inputTokens + outputTokens + cacheReadTokens + cacheWriteTokens; }
    Json toJson() const;
    static Usage fromJson(const Json& j);
};

/// Estimated USD per million tokens for a model (input, output, cache read, cache write).
/// Unknown models cost 0 (reported as "unpriced").
struct Price {
    double input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
    bool known = false;
};
Price priceFor(std::string_view model);

// ---------------------------------------------------------------------------
// Studio
// ---------------------------------------------------------------------------

/// One unit of agent work produced by a loop stage.
struct Assignment {
    std::string agent;
    std::string stage;
    std::string prompt;
    std::vector<std::string> tasks;
    Json toJson() const;
};

class Studio {
public:
    using EventSink = std::function<void(Json)>;
    /// Runs a playtest for loop stages (set by the engine): args as for playtest_run.
    using PlaytestFn = std::function<Result<Json>(const Json& args, const std::string& actor)>;

    Studio(std::string projectDir, EventSink sink);
    ~Studio();
    Studio(const Studio&) = delete;
    Studio& operator=(const Studio&) = delete;

    /// Seconds since the Unix epoch (overridable for deterministic tests).
    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }
    double now() const;
    std::string timestamp() const;  // ISO 8601 UTC
    void setPlaytestRunner(PlaytestFn fn) { playtest_ = std::move(fn); }

    const std::string& projectDir() const { return projectDir_; }
    std::string studioDir() const;
    /// Reloads files that changed on disk since we last read or wrote them (another
    /// process — the CLI runner, git, a teammate — may edit the project).
    void syncFromDisk();

    // --- Roster ----------------------------------------------------------------
    const std::vector<AgentProfile>& agents() const { return agents_; }
    const AgentProfile* agent(std::string_view idOrName) const;
    /// Creates or updates (merge-patch by id/name) an agent, applying role defaults.
    Result<AgentProfile> defineAgent(const Json& spec, const std::string& actor);
    Status removeAgent(std::string_view idOrName, const std::string& actor);
    Status addMemory(const std::string& agentId, const std::string& note);
    Status forgetMemory(const std::string& agentId, int number);
    /// The roster member behind an actor: "agent:mira", "mcp:claude-code/mira",
    /// "mcp:mira" -> "mira". Empty if none.
    std::string memberForActor(std::string_view actor) const;
    /// Agents matching "@role", "@discipline", "@all" or an id/name.
    std::vector<std::string> resolveAssignees(const std::vector<std::string>& refs) const;
    /// did-you-mean error for an unknown agent.
    Error unknownAgent(std::string_view ref) const;

    // --- Board -----------------------------------------------------------------
    const std::vector<Task>& tasks() const { return tasks_; }
    Task* task(std::string_view id);
    Result<Task> createTask(const Json& spec, const std::string& actor);
    Result<Task> updateTask(std::string_view id, const Json& patch, const std::string& actor);
    Result<Task> claimTask(std::string_view id, const std::string& agentId, const std::string& actor);

    // --- Feedback & decisions -----------------------------------------------------
    const std::vector<Feedback>& feedback() const { return feedback_; }
    Feedback* feedbackItem(std::string_view id);
    /// Files feedback. If `fingerprint` matches a still-relevant item, that item is updated
    /// (occurrences++, fresh evidence) instead; `merged` tells which happened.
    Result<Feedback> submitFeedback(const Json& spec, const std::string& actor, bool* merged = nullptr);
    Result<Feedback> updateFeedback(std::string_view id, const Json& patch, const std::string& actor);
    const std::vector<Decision>& decisions() const { return decisions_; }
    Decision* decision(std::string_view id);
    /// Director/producer verdict on feedback. "act" creates tasks.
    Result<Decision> decide(const Json& spec, const std::string& actor);
    /// Records the measured effect of a decision (before/after metrics) and moves the
    /// feedback to verified / regressed (a regression reopens its tasks).
    Result<Json> recordEffect(std::string_view feedbackId, const Json& before, const Json& after,
                              const std::string& playtestBefore, const std::string& playtestAfter,
                              const std::string& actor);
    /// Latest aggregate playtest metrics (the baseline for new decisions).
    const Json& latestMetrics() const { return latestMetrics_; }
    const std::string& latestPlaytest() const { return latestPlaytest_; }

    // --- Playtests -------------------------------------------------------------
    std::string nextPlaytestId();
    std::string playtestDir(std::string_view id) const;
    /// Remembers a finished playtest's metrics as the studio baseline.
    void notePlaytest(const std::string& id, const Json& metrics, const std::string& actor, const std::string& summary);
    Result<Json> loadPlaytestReport(std::string_view id) const;

    // --- Loops -----------------------------------------------------------------
    const std::vector<Loop>& loops() const { return loops_; }
    Loop* loop(std::string_view name);
    Result<Loop> defineLoop(const Json& spec, const std::string& actor);
    Status removeLoop(std::string_view name, const std::string& actor);
    /// Starts a loop run. Returns the status payload (see loopStatus) with the first
    /// assignments. `overrides`: {goal, max_iterations}.
    Result<Json> startLoop(std::string_view name, const Json& overrides, const std::string& actor);
    /// Records reports for pending assignments and moves the loop forward through engine
    /// stages (playtests) until it needs agents again, waits for approval, or ends.
    /// args: {reports: [{agent, report, signoff?, usage?}], approve?, signoff?, complete_stage?}
    Result<Json> advanceLoop(std::string_view name, const Json& args, const std::string& actor);
    Result<Json> stopLoop(std::string_view name, const std::string& reason, const std::string& actor);
    Json loopStatus(const Loop& loop, bool withHistory = true) const;

    // --- Messages ----------------------------------------------------------------
    const std::vector<Message>& messages() const { return messages_; }
    Result<Message> sendMessage(const Json& spec, const std::string& actor);
    /// Messages for an agent (addressed, mentioned, or in its channels), newest last.
    std::vector<const Message*> inbox(const std::string& agentId, bool unreadOnly, size_t limit, bool markRead);

    // --- Usage & presence --------------------------------------------------------------
    const std::map<std::string, Usage>& usage() const { return usage_; }
    void recordUsage(const std::string& agentId, const std::string& model, int64_t input, int64_t output,
                     int64_t cacheRead, int64_t cacheWrite);
    void noteToolCall(std::string_view actor, std::string_view tool, bool ok);
    int64_t totalTokens() const;
    /// Live status per agent: {status: idle|working, activity, since}.
    void setPresence(const std::string& agentId, const std::string& status, const std::string& activity);
    Json presence(const std::string& agentId) const;

    Json overview() const;

private:
    void emit(const std::string& kind, const std::string& action, const std::string& id, const std::string& actor,
              const std::string& summary, Json extra = Json());
    void loadAll();
    void loadAgents();
    void loadBoard();
    void loadFeedback();
    void loadDecisions();
    void loadLoops();
    void loadMessages();
    void loadUsage();
    void saveAgent(const AgentProfile& a);
    void saveBoard();
    void saveFeedback();
    void saveDecisions();
    void saveLoop(const Loop& l);
    void appendMessage(const Message& m);
    void saveUsage();
    void writeFile(const std::string& path, const std::string& text);
    bool changedOnDisk(const std::string& path) const;
    void noteFile(const std::string& path);

    std::string nextId(const char* prefix, int& counter);
    // Loop engine
    Json enterStages(Loop& loop, const std::string& actor);
    bool shouldSkip(const Loop& loop, const LoopStage& stage, std::string& why) const;
    std::vector<Assignment> buildAssignments(Loop& loop, const LoopStage& stage, const std::string& actor);
    std::string renderPrompt(const Loop& loop, const LoopStage& stage, const AgentProfile& agent,
                             const std::vector<std::string>& tasks) const;
    Json runPlaytestStage(Loop& loop, const LoopStage& stage, const std::string& actor);
    Json finishIteration(Loop& loop, const std::string& actor, bool& finished);
    bool budgetExhausted(const Loop& loop, std::string& why) const;
    Json& currentIteration(Loop& loop);
    void verifyFixedFeedback(const std::string& playtestId, const Json& metrics, const std::string& actor, Json& out);
    void reopenTasksFor(Feedback& f, const std::string& actor, const std::string& why);
    void syncFeedbackFromTasks(const Task& t, const std::string& actor);
    bool isDecider(const std::string& actor) const;

    std::string projectDir_;
    EventSink sink_;
    std::function<double()> clock_;
    PlaytestFn playtest_;

    std::vector<AgentProfile> agents_;
    std::vector<Task> tasks_;
    std::vector<Feedback> feedback_;
    std::vector<Decision> decisions_;
    std::vector<Loop> loops_;
    std::vector<Message> messages_;
    std::map<std::string, Usage> usage_;
    std::map<std::string, std::string> readCursor_;  // agent -> last read message id
    std::map<std::string, Json> presence_;
    Json latestMetrics_ = Json::object();
    std::string latestPlaytest_;
    int nextTask_ = 1, nextFeedback_ = 1, nextDecision_ = 1, nextMessage_ = 1, nextPlaytest_ = 1;
    std::map<std::string, int64_t> mtimes_;  // file -> mtime we last saw/wrote
    bool usageDirty_ = false;
    double lastUsageSave_ = 0;
};

/// "Ada Lovelace!" -> "ada-lovelace"
std::string slugify(std::string_view s);
/// Numeric part of an id like "T-12" (0 if none).
int idNumber(std::string_view id);

/// Metric direction: +1 higher is better, -1 lower is better, 0 neutral/unknown.
int metricDirection(std::string_view metric);
/// Before/after comparison of two metric objects: {metric: {before, after, delta, change, verdict}}.
Json compareMetrics(const Json& before, const Json& after);
/// Overall verdict over `metrics` (empty = every comparable metric), honoring explicit targets:
/// improved | regressed | unchanged | mixed | unmeasured.
std::string effectVerdict(const Json& comparison, const std::vector<std::string>& metrics, const Json& targets);
/// Metrics that tell whether feedback of a category got better.
std::vector<std::string> metricsForCategory(std::string_view category);
/// True if `metrics` satisfies every target ({"m": {"min": x, "max": y}}).
bool targetsMet(const Json& metrics, const Json& targets, std::string* unmet = nullptr);

}  // namespace sky::studio
