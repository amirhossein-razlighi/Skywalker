#pragma once
// Headless studio runner: runs studio agents and loops without the editor (CLI, CI).
//
//   AgentRunner  one agent, one task: system prompt from its profile, the engine tools it
//                is permitted to use, and a tool loop over an LLM provider.
//   LoopRunner   drives a studio loop through the same tools every client uses
//                (studio_loop_start / studio_loop_advance): engine stages run inside the
//                engine, agent stages run here — in parallel on worker threads when the
//                stage allows it.
//
// Threading: runners work on worker threads. Every engine/studio access goes through
// Engine::post, so the thread that owns the engine must keep pumping it (see
// runWhilePumping, used by the CLI and tests).

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/studio/Llm.h"
#include "skywalker/studio/Studio.h"

namespace sky {
class Engine;
class ToolRegistry;
}  // namespace sky

namespace sky::studio {

/// Live progress for UIs and the CLI. Called from worker threads (serialize output yourself).
struct RunnerEvent {
    std::string kind;   // agent_start, text, tool, tool_result, agent_done, stage, loop, error
    std::string agent;  // agent id (if any)
    std::string text;
    Json data;
};
using RunnerLog = std::function<void(const RunnerEvent&)>;
/// Decides "ask" tool calls: return true to allow. Called from worker threads.
using ApproveFn = std::function<bool(const std::string& agent, const std::string& tool, const Json& args)>;
/// The provider for an agent (borrowed; must outlive the run and be thread-safe).
using ProviderFactory = std::function<Result<llm::Provider*>(const AgentProfile& agent)>;

struct AgentRunResult {
    bool ok = true;
    std::string report;
    std::string stop;  // done | max_rounds | max_tokens | refusal | error | cancelled
    std::string error;
    int rounds = 0;
    int toolCalls = 0;
    llm::TokenUsage usage;
};

class AgentRunner {
public:
    struct Options {
        ApproveFn approve;              // null: "ask" tools are declined
        RunnerLog log;
        int maxRounds = 0;              // 0: the profile's max_rounds
        bool loopMember = false;        // running inside a loop: loop/roster control tools are withheld
        std::atomic<bool>* cancel = nullptr;
        size_t maxToolText = 30000;     // tool output kept per call (characters)
    };

    AgentRunner(Engine& engine, llm::Provider& provider, AgentProfile profile, Options options);

    /// Runs one task to completion. Call from a worker thread.
    AgentRunResult run(const std::string& prompt);

    /// The system prompt for an agent (stable across turns for prompt caching).
    static std::string systemPrompt(const AgentProfile& agent, const std::vector<AgentProfile>& roster);
    struct AllowedTool {
        llm::ToolSpec spec;
        std::string category;
        Access access = Access::Allow;
    };
    /// Engine tools this agent may use (permission category "off" removes them entirely).
    static std::vector<AllowedTool> toolsFor(const AgentProfile& agent, const ToolRegistry& registry, bool loopMember);

private:
    llm::ToolOutcome execute(const llm::ToolCall& call, const std::vector<AllowedTool>& tools);
    void emit(const std::string& kind, const std::string& text, Json data = Json());

    Engine& engine_;
    llm::Provider& provider_;
    AgentProfile profile_;
    Options options_;
};

class LoopRunner {
public:
    struct Options {
        ProviderFactory providers;
        ApproveFn approve;              // tool approvals for agents with "ask" access
        bool approveGates = false;      // auto-approve human approval gates
        RunnerLog log;
        int maxIterations = 0;          // 0: the loop's own stop conditions
        std::string goal;               // overrides the loop's goal for this run
        std::atomic<bool>* cancel = nullptr;
    };

    LoopRunner(Engine& engine, Options options);
    /// Runs the loop until it ends, pauses for approval, or is cancelled. Returns the final
    /// studio_loop_status payload. Call from a worker thread.
    Result<Json> run(const std::string& loop);

private:
    Json call(const std::string& tool, const Json& args, bool& ok, std::string& error);
    void emit(const std::string& kind, const std::string& agent, const std::string& text, Json data = Json());

    Engine& engine_;
    Options options_;
};

/// Runs `fn` on a worker thread while pumping `engine` on the calling thread (which must
/// be the engine's thread), then returns its result.
Result<Json> runWhilePumping(Engine& engine, const std::function<Result<Json>()>& fn);

}  // namespace sky::studio
