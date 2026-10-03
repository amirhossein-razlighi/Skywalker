#pragma once
// LLM providers for the headless studio runner.
//
// Two wire protocols cover nearly every model: the Anthropic Messages API and the
// OpenAI-compatible Chat Completions API (OpenAI, DeepSeek, OpenRouter, Groq, and local
// servers such as Ollama, LM Studio, vLLM, llama.cpp). Neither has an official C++ SDK, so
// both speak raw HTTPS JSON over libcurl. A scripted mock provider makes tests and dry runs
// deterministic and offline.
//
// Sessions own their provider-native history, which is strictly append-only: assistant
// content is stored exactly as returned (thinking blocks included), which preserved
// thinking and prompt caching both rely on.
//
// API keys come from the environment only (ANTHROPIC_API_KEY, OPENAI_API_KEY) and are never
// written anywhere.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::studio::llm {

struct ToolSpec {
    std::string name;
    std::string description;
    Json inputSchema;
};

struct ToolCall {
    std::string id;
    std::string name;
    Json input;
    bool invalidJson = false;  // the model produced arguments that are not JSON (raw text in input)
};

struct ToolOutcome {
    std::string id;
    std::string name;
    std::string text;
    std::vector<std::string> imagesPng;  // base64 PNG
    bool isError = false;
};

struct TokenUsage {
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite = 0;
    TokenUsage& operator+=(const TokenUsage& o) {
        input += o.input;
        output += o.output;
        cacheRead += o.cacheRead;
        cacheWrite += o.cacheWrite;
        return *this;
    }
};

enum class Stop { EndTurn, ToolUse, MaxTokens, Refusal, PauseTurn, Other };
const char* toString(Stop s);

struct Turn {
    std::string text;
    std::vector<ToolCall> calls;
    Stop stop = Stop::EndTurn;
    std::string stopDetail;  // refusal category/explanation, raw stop reason for Other
    TokenUsage usage;
    std::string model;  // the model that produced the turn (a server-side fallback may differ)
};

struct HttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeoutSeconds = 600;  // long agentic turns are normal
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string retryAfter;  // Retry-After header, if any
};

using HttpFn = std::function<Result<HttpResponse>(const HttpRequest&)>;
/// libcurl POST (thread-safe; one easy handle per request).
HttpFn curlHttp();

struct SessionConfig {
    std::string agentId;
    std::string model;
    std::string system;
    std::vector<ToolSpec> tools;
    int maxTokens = 16000;
    std::string effort = "high";
    bool vision = true;
};

/// One conversation with one model.
class Session {
public:
    virtual ~Session() = default;
    /// The next user turn: results for every tool call of the previous turn (in order),
    /// then optional text.
    virtual Result<Turn> send(const std::vector<ToolOutcome>& outcomes, const std::string& userText) = 0;
    /// Continues after Stop::PauseTurn (re-sends the history unchanged).
    virtual Result<Turn> resume() = 0;
    /// Provider-native message history (for debugging and tests).
    virtual Json history() const = 0;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string name() const = 0;
    virtual std::string defaultModel() const = 0;
    virtual std::unique_ptr<Session> open(const SessionConfig& config) = 0;
};

// --- Anthropic Messages API --------------------------------------------------------
namespace anthropic {
constexpr const char* kDefaultModel = "claude-opus-5-5";
/// Request body for the current history.
Json buildRequest(const SessionConfig& config, const Json& messages);
/// The user message carrying tool results (with images) and optional text.
Json userMessage(const std::vector<ToolOutcome>& outcomes, const std::string& text);
Result<Turn> parseResponse(const Json& response);
/// Models that accept adaptive thinking and output_config.effort.
bool supportsAdaptiveThinking(std::string_view model);
/// Models that accept server-side refusal fallbacks (`fallbacks: "default"`).
bool supportsServerFallback(std::string_view model);
}  // namespace anthropic

// --- OpenAI-compatible Chat Completions -------------------------------------------------
namespace openai {
constexpr const char* kDefaultModel = "gpt-5";
Json buildRequest(const SessionConfig& config, const Json& messages);
/// Appends tool messages (and a follow-up user message with images, if any) plus text.
void appendUser(Json& messages, const std::vector<ToolOutcome>& outcomes, const std::string& text, bool vision);
Result<Turn> parseResponse(const Json& response);
}  // namespace openai

std::unique_ptr<Provider> makeAnthropic(std::string apiKey, std::string baseUrl, HttpFn http);
std::unique_ptr<Provider> makeOpenAI(std::string apiKey, std::string baseUrl, std::string defaultModel, HttpFn http,
                                     bool vision = true);

/// Scripted provider for tests and dry runs.
///   {"agents": {"<agent id>": [turn, ...]}, "default": [turn, ...], "echo": true}
///   turn = {"text": "...", "tool_calls": [{"name": "...", "input": {...}}],
///           "stop": "end_turn|max_tokens|refusal|pause_turn", "usage": {"input": n, "output": n}}
/// Each agent consumes its own queue (then "default"); when both are empty the turn is a
/// short text report ("(dry run) ..." when echo is set). Thread-safe.
class MockProvider : public Provider {
public:
    explicit MockProvider(Json script);
    std::string name() const override { return "mock"; }
    std::string defaultModel() const override { return "mock"; }
    std::unique_ptr<Session> open(const SessionConfig& config) override;
    /// Everything the mock received: [{agent, kind: "prompt"|"tool_result", text, tool, is_error}].
    Json log() const;

    // Used by sessions.
    Json nextTurn(const std::string& agent);
    void record(Json entry);
    bool echo() const { return echo_; }

private:
    mutable std::mutex mutex_;
    Json script_;
    bool echo_ = false;
    Json log_ = Json::array();
};

/// A provider for an agent's `provider` field, configured from the environment:
/// "anthropic"/"claude" -> ANTHROPIC_API_KEY (+ ANTHROPIC_BASE_URL), "openai" or any other
/// OpenAI-compatible name -> OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_MODEL; "mock" -> echo.
Result<std::unique_ptr<Provider>> fromEnvironment(const std::string& kind, HttpFn http = curlHttp());

}  // namespace sky::studio::llm
