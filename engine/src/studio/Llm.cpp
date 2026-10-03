#include "skywalker/studio/Llm.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"

namespace sky::studio::llm {

const char* toString(Stop s) {
    switch (s) {
        case Stop::EndTurn: return "end_turn";
        case Stop::ToolUse: return "tool_use";
        case Stop::MaxTokens: return "max_tokens";
        case Stop::Refusal: return "refusal";
        case Stop::PauseTurn: return "pause_turn";
        case Stop::Other: return "other";
    }
    return "other";
}

// ---------------------------------------------------------------------------
// HTTP (libcurl)
// ---------------------------------------------------------------------------

namespace {

size_t onBody(char* ptr, size_t size, size_t n, void* user) {
    auto* out = static_cast<std::string*>(user);
    size_t bytes = size * n;
    if (out->size() + bytes > (64u << 20)) return 0;  // 64 MB cap
    out->append(ptr, bytes);
    return bytes;
}

size_t onHeader(char* ptr, size_t size, size_t n, void* user) {
    auto* retryAfter = static_cast<std::string*>(user);
    std::string line(ptr, size * n);
    if (str::startsWith(str::lower(line), "retry-after:")) *retryAfter = str::trim(line.substr(12));
    return size * n;
}

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

/// Retries 429 / 5xx / 529 (overloaded) and network errors with backoff.
Result<HttpResponse> postWithRetry(const HttpFn& http, const HttpRequest& req) {
    int attempts = 0;
    for (;;) {
        auto r = http(req);
        bool retryable = !r || r->status == 429 || r->status == 408 || r->status == 409 || r->status >= 500;
        if (!retryable || ++attempts > 3) return r;
        double wait = 2.0 * (1 << (attempts - 1));
        if (r && !r->retryAfter.empty()) {
            double ra = 0;
            if (str::parseDouble(r->retryAfter, ra)) wait = std::clamp(ra, 1.0, 60.0);
        }
        log::warn("studio", "LLM request failed (" + (r ? "HTTP " + std::to_string(r->status) : r.error().message) +
                                "), retrying in " + std::to_string(static_cast<int>(wait)) + " s");
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(wait * 1000)));
    }
}

Error httpError(const HttpResponse& r, const std::string& provider) {
    std::string msg;
    if (auto j = Json::parse(r.body)) msg = j->get("error").get("message").asString();
    if (msg.empty()) msg = r.body.substr(0, 400);
    std::string hint;
    if (r.status == 401 || r.status == 403) hint = "check the API key environment variable for " + provider;
    else if (r.status == 404) hint = "check the model id and base URL";
    else if (r.status == 400) hint = "the request was rejected; the message says why";
    return Error::make("provider_error", provider + " HTTP " + std::to_string(r.status) + ": " + msg, hint);
}

}  // namespace

HttpFn curlHttp() {
    return [](const HttpRequest& req) -> Result<HttpResponse> {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        CURL* c = curl_easy_init();
        if (!c) return Error::make("network_error", "could not initialize the HTTP client");
        HttpResponse resp;
        struct curl_slist* headers = nullptr;
        for (const auto& [k, v] : req.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
        curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeoutSeconds);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // worker threads
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Skywalker/" SKY_VERSION_STRING " (studio)");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp.retryAfter);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        CURLcode rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK) return Error::make("network_error", std::string("request failed: ") + curl_easy_strerror(rc));
        return resp;
    };
}

// ---------------------------------------------------------------------------
// Anthropic Messages API
// ---------------------------------------------------------------------------

namespace anthropic {

bool supportsAdaptiveThinking(std::string_view model) {
    // Adaptive thinking + effort: Opus/Sonnet 4.6 and later, all 5.x, Fable, Mythos.
    // Haiku 4.5 and the 4.5-and-older generation use budget_tokens instead (not sent).
    std::string m = str::lower(model);
    if (m.find("haiku") != std::string::npos) return false;
    if (m.find("-4-5") != std::string::npos || m.find("claude-3") != std::string::npos) return false;
    if (m.find("-4-0") != std::string::npos || m.find("-4-1") != std::string::npos) return false;
    return str::startsWith(m, "claude-");
}

bool supportsServerFallback(std::string_view model) {
    return model == "claude-opus-5-5" || model == "claude-opus-5" || model == "claude-fable-5-1" || model == "claude-sonnet-5-5";
}

Json buildRequest(const SessionConfig& cfg, const Json& messages) {
    Json tools = Json::array();
    for (const auto& t : cfg.tools) {
        tools.push(Json::object({{"name", t.name}, {"description", t.description}, {"input_schema", t.inputSchema}}));
    }
    std::string model = cfg.model.empty() ? kDefaultModel : cfg.model;
    Json body = Json::object({{"model", model}, {"max_tokens", cfg.maxTokens}});
    // Render order is tools -> system -> messages: a breakpoint on the (stable) system block
    // caches tools + system together; top-level automatic caching follows the growing history.
    body["system"] = Json::array({Json::object(
        {{"type", "text"}, {"text", cfg.system}, {"cache_control", Json::object({{"type", "ephemeral"}})}})});
    if (tools.size()) body["tools"] = tools;
    body["messages"] = messages;
    if (supportsAdaptiveThinking(model)) {
        body["thinking"] = Json::object({{"type", "adaptive"}});
        body["output_config"] = Json::object({{"effort", cfg.effort.empty() ? "high" : cfg.effort}});
    }
    body["cache_control"] = Json::object({{"type", "ephemeral"}});
    if (supportsServerFallback(model)) body["fallbacks"] = "default";
    return body;
}

Json userMessage(const std::vector<ToolOutcome>& outcomes, const std::string& text) {
    Json content = Json::array();
    // Every tool result of a turn goes back in one user message, results first.
    for (const auto& o : outcomes) {
        Json blocks = Json::array({Json::object({{"type", "text"}, {"text", o.text.empty() ? "(no output)" : o.text}})});
        for (const auto& img : o.imagesPng) {
            blocks.push(Json::object(
                {{"type", "image"},
                 {"source", Json::object({{"type", "base64"}, {"media_type", "image/png"}, {"data", img}})}}));
        }
        Json r = Json::object({{"type", "tool_result"}, {"tool_use_id", o.id}, {"content", blocks}});
        if (o.isError) r["is_error"] = true;
        content.push(r);
    }
    if (!text.empty()) content.push(Json::object({{"type", "text"}, {"text", text}}));
    return Json::object({{"role", "user"}, {"content", content}});
}

Result<Turn> parseResponse(const Json& r) {
    if (r.get("type").asString() == "error") {
        return Error::make("provider_error", "Anthropic: " + r.get("error").get("message").asString());
    }
    if (!r.get("content").isArray()) return Error::make("provider_error", "Anthropic: response has no content");
    Turn t;
    t.model = r.get("model").asString();
    for (const auto& b : r.get("content").elements()) {
        const std::string& type = b.get("type").asString();
        if (type == "text") {
            t.text += b.get("text").asString();
        } else if (type == "tool_use") {
            t.calls.push_back({b.get("id").asString(), b.get("name").asString(), b.get("input"), false});
        }
        // thinking / redacted_thinking / fallback blocks stay in the history untouched.
    }
    const std::string& stop = r.get("stop_reason").asString();
    if (stop == "tool_use") t.stop = Stop::ToolUse;
    else if (stop == "end_turn" || stop == "stop_sequence") t.stop = Stop::EndTurn;
    else if (stop == "max_tokens" || stop == "model_context_window_exceeded") t.stop = Stop::MaxTokens;
    else if (stop == "refusal") t.stop = Stop::Refusal;
    else if (stop == "pause_turn") t.stop = Stop::PauseTurn;
    else t.stop = Stop::Other;
    if (t.stop == Stop::Refusal) {
        // stop_details is informational and may be null.
        const Json& d = r.get("stop_details");
        std::string category = d.get("category").asString();
        std::string why = d.get("explanation").asString();
        t.stopDetail = category.empty() && why.empty() ? "declined" : category + (why.empty() ? "" : ": " + why);
    } else if (t.stop == Stop::Other) {
        t.stopDetail = stop;
    }
    if (t.stop == Stop::EndTurn && !t.calls.empty()) t.stop = Stop::ToolUse;
    const Json& u = r.get("usage");
    t.usage.input = u.get("input_tokens").asInt();
    t.usage.output = u.get("output_tokens").asInt();
    t.usage.cacheRead = u.get("cache_read_input_tokens").asInt();
    t.usage.cacheWrite = u.get("cache_creation_input_tokens").asInt();
    return t;
}

}  // namespace anthropic

namespace {

class AnthropicSession : public Session {
public:
    AnthropicSession(std::string key, std::string base, HttpFn http, SessionConfig cfg)
        : key_(std::move(key)), base_(std::move(base)), http_(std::move(http)), cfg_(std::move(cfg)) {
        if (cfg_.model.empty()) cfg_.model = anthropic::kDefaultModel;
    }

    Result<Turn> send(const std::vector<ToolOutcome>& outcomes, const std::string& userText) override {
        if (broken_) return Error::make("invalid_state", "this conversation ended (refusal); start a new session");
        messages_.push(anthropic::userMessage(outcomes, userText));
        return post();
    }

    Result<Turn> resume() override { return post(); }

    Json history() const override { return messages_; }

private:
    Result<Turn> post() {
        HttpRequest req;
        req.url = base_ + "/v1/messages";
        req.headers = {{"content-type", "application/json"}, {"x-api-key", key_}, {"anthropic-version", "2023-06-01"}};
        if (anthropic::supportsServerFallback(cfg_.model)) req.headers.emplace_back("anthropic-beta", "server-side-fallback-2026-07-01");
        req.body = anthropic::buildRequest(cfg_, messages_).dump();
        auto resp = postWithRetry(http_, req);
        if (!resp) return resp.error();
        if (resp->status < 200 || resp->status >= 300) return httpError(*resp, "Anthropic");
        auto json = Json::parse(resp->body);
        if (!json) return Error::make("provider_error", "Anthropic: invalid JSON response");
        auto turn = anthropic::parseResponse(*json);
        if (!turn) return turn;
        if (turn->stop == Stop::Refusal) {
            // Never run a refused turn's tools, and don't build on its partial output.
            broken_ = true;
            return turn;
        }
        // Append the assistant content exactly as returned (thinking blocks included).
        messages_.push(Json::object({{"role", "assistant"}, {"content", json->get("content")}}));
        return turn;
    }

    std::string key_, base_;
    HttpFn http_;
    SessionConfig cfg_;
    Json messages_ = Json::array();
    bool broken_ = false;
};

class AnthropicProvider : public Provider {
public:
    AnthropicProvider(std::string key, std::string base, HttpFn http)
        : key_(std::move(key)), base_(trimSlash(std::move(base))), http_(std::move(http)) {}
    std::string name() const override { return "anthropic"; }
    std::string defaultModel() const override { return anthropic::kDefaultModel; }
    std::unique_ptr<Session> open(const SessionConfig& cfg) override {
        return std::make_unique<AnthropicSession>(key_, base_, http_, cfg);
    }

private:
    std::string key_, base_;
    HttpFn http_;
};

}  // namespace

// ---------------------------------------------------------------------------
// OpenAI-compatible Chat Completions
// ---------------------------------------------------------------------------

namespace openai {

Json buildRequest(const SessionConfig& cfg, const Json& messages) {
    Json body = Json::object({{"model", cfg.model.empty() ? kDefaultModel : cfg.model}, {"messages", messages}});
    if (!cfg.tools.empty()) {
        Json tools = Json::array();
        for (const auto& t : cfg.tools) {
            tools.push(Json::object({{"type", "function"},
                                     {"function", Json::object({{"name", t.name},
                                                                {"description", t.description},
                                                                {"parameters", t.inputSchema}})}}));
        }
        body["tools"] = tools;
    }
    return body;
}

void appendUser(Json& messages, const std::vector<ToolOutcome>& outcomes, const std::string& text, bool vision) {
    std::vector<std::string> images;
    for (const auto& o : outcomes) {
        messages.push(Json::object({{"role", "tool"},
                                    {"tool_call_id", o.id},
                                    {"content", (o.isError ? "ERROR: " : "") + (o.text.empty() ? std::string("(no output)") : o.text)}}));
        for (const auto& img : o.imagesPng) images.push_back(img);
    }
    // Tool messages cannot carry images in this protocol: screenshots follow as a user message.
    if (!images.empty() && vision) {
        Json parts = Json::array({Json::object({{"type", "text"}, {"text", "Images returned by the tools above:"}})});
        for (const auto& img : images) {
            parts.push(Json::object({{"type", "image_url"}, {"image_url", Json::object({{"url", "data:image/png;base64," + img}})}}));
        }
        messages.push(Json::object({{"role", "user"}, {"content", parts}}));
    }
    if (!text.empty()) messages.push(Json::object({{"role", "user"}, {"content", text}}));
}

Result<Turn> parseResponse(const Json& r) {
    if (r.contains("error")) {
        const Json& e = r.get("error");
        return Error::make("provider_error", "OpenAI-compatible: " + (e.isString() ? e.asString() : e.get("message").asString()));
    }
    const Json& choice = r.get("choices")[size_t{0}];
    const Json& msg = choice.get("message");
    if (!msg.isObject()) return Error::make("provider_error", "OpenAI-compatible: response has no choices");
    Turn t;
    t.model = r.get("model").asString();
    t.text = msg.get("content").asString();
    for (const auto& c : msg.get("tool_calls").elements()) {
        ToolCall call{c.get("id").asString(), c.get("function").get("name").asString(), Json::object(), false};
        const Json& args = c.get("function").get("arguments");
        if (args.isObject()) {
            call.input = args;
        } else {
            auto parsed = Json::parse(args.asString().empty() ? "{}" : args.asString());
            if (parsed && parsed->isObject()) {
                call.input = *parsed;
            } else {
                call.invalidJson = true;
                call.input = Json(args.asString());
            }
        }
        t.calls.push_back(std::move(call));
    }
    const std::string& finish = choice.get("finish_reason").asString();
    if (finish == "tool_calls" || !t.calls.empty()) t.stop = Stop::ToolUse;
    else if (finish == "length") t.stop = Stop::MaxTokens;
    else if (finish == "content_filter") {
        t.stop = Stop::Refusal;
        t.stopDetail = "content filter";
    } else {
        t.stop = Stop::EndTurn;
    }
    if (msg.get("refusal").isString() && !msg.get("refusal").asString().empty()) {
        t.stop = Stop::Refusal;
        t.stopDetail = msg.get("refusal").asString();
    }
    const Json& u = r.get("usage");
    int64_t cached = u.get("prompt_tokens_details").get("cached_tokens").asInt();
    t.usage.input = std::max<int64_t>(0, u.get("prompt_tokens").asInt() - cached);
    t.usage.cacheRead = cached;
    t.usage.output = u.get("completion_tokens").asInt();
    return t;
}

}  // namespace openai

namespace {

class OpenAISession : public Session {
public:
    OpenAISession(std::string key, std::string base, HttpFn http, SessionConfig cfg, bool vision)
        : key_(std::move(key)), base_(std::move(base)), http_(std::move(http)), cfg_(std::move(cfg)), vision_(vision && cfg_.vision) {
        messages_.push(Json::object({{"role", "system"}, {"content", cfg_.system}}));
    }

    Result<Turn> send(const std::vector<ToolOutcome>& outcomes, const std::string& userText) override {
        openai::appendUser(messages_, outcomes, userText, vision_);
        return post();
    }

    Result<Turn> resume() override { return post(); }
    Json history() const override { return messages_; }

private:
    Result<Turn> post() {
        HttpRequest req;
        req.url = base_ + "/chat/completions";
        req.headers = {{"content-type", "application/json"}};
        if (!key_.empty()) req.headers.emplace_back("authorization", "Bearer " + key_);
        req.body = openai::buildRequest(cfg_, messages_).dump();
        auto resp = postWithRetry(http_, req);
        if (!resp) return resp.error();
        if (resp->status < 200 || resp->status >= 300) return httpError(*resp, "OpenAI-compatible");
        auto json = Json::parse(resp->body);
        if (!json) return Error::make("provider_error", "OpenAI-compatible: invalid JSON response");
        auto turn = openai::parseResponse(*json);
        if (!turn) return turn;
        messages_.push(json->get("choices")[size_t{0}].get("message"));
        return turn;
    }

    std::string key_, base_;
    HttpFn http_;
    SessionConfig cfg_;
    bool vision_;
    Json messages_ = Json::array();
};

class OpenAIProvider : public Provider {
public:
    OpenAIProvider(std::string key, std::string base, std::string model, HttpFn http, bool vision)
        : key_(std::move(key)), base_(trimSlash(std::move(base))), model_(std::move(model)), http_(std::move(http)), vision_(vision) {}
    std::string name() const override { return "openai"; }
    std::string defaultModel() const override { return model_; }
    std::unique_ptr<Session> open(const SessionConfig& cfg) override {
        SessionConfig c = cfg;
        if (c.model.empty()) c.model = model_;
        return std::make_unique<OpenAISession>(key_, base_, http_, c, vision_);
    }

private:
    std::string key_, base_, model_;
    HttpFn http_;
    bool vision_;
};

}  // namespace

std::unique_ptr<Provider> makeAnthropic(std::string apiKey, std::string baseUrl, HttpFn http) {
    return std::make_unique<AnthropicProvider>(std::move(apiKey), baseUrl.empty() ? "https://api.anthropic.com" : std::move(baseUrl),
                                               std::move(http));
}

std::unique_ptr<Provider> makeOpenAI(std::string apiKey, std::string baseUrl, std::string defaultModel, HttpFn http, bool vision) {
    return std::make_unique<OpenAIProvider>(std::move(apiKey), baseUrl.empty() ? "https://api.openai.com/v1" : std::move(baseUrl),
                                            defaultModel.empty() ? openai::kDefaultModel : std::move(defaultModel), std::move(http),
                                            vision);
}

// ---------------------------------------------------------------------------
// Mock
// ---------------------------------------------------------------------------

namespace {

class MockSession : public Session {
public:
    MockSession(MockProvider& p, SessionConfig cfg) : provider_(p), cfg_(std::move(cfg)) {}

    Result<Turn> send(const std::vector<ToolOutcome>& outcomes, const std::string& userText) override {
        for (const auto& o : outcomes) {
            provider_.record(Json::object({{"agent", cfg_.agentId},
                                           {"kind", "tool_result"},
                                           {"tool", o.name},
                                           {"text", o.text},
                                           {"is_error", o.isError}}));
        }
        if (!userText.empty()) {
            provider_.record(Json::object({{"agent", cfg_.agentId}, {"kind", "prompt"}, {"text", userText}}));
            lastPrompt_ = userText;
        }
        history_.push(Json::object({{"role", "user"}, {"text", userText}, {"tool_results", static_cast<int64_t>(outcomes.size())}}));
        return next();
    }

    Result<Turn> resume() override { return next(); }
    Json history() const override { return history_; }

private:
    Result<Turn> next() {
        Json spec = provider_.nextTurn(cfg_.agentId);
        Turn t;
        t.model = "mock";
        if (spec.isNull()) {
            std::string first = lastPrompt_.substr(0, lastPrompt_.find('\n'));
            if (provider_.echo()) {
                // Second line of a loop prompt is the goal; the stage is in the first.
                t.text = "(dry run) @" + cfg_.agentId + " received: " + first;
            } else {
                t.text = "(mock) @" + cfg_.agentId + " done.";
            }
            t.usage = {static_cast<int64_t>(lastPrompt_.size() / 4), 12, 0, 0};
            history_.push(Json::object({{"role", "assistant"}, {"text", t.text}}));
            return t;
        }
        t.text = spec.get("text").asString();
        int n = 0;
        for (const auto& c : spec.get("tool_calls").elements()) {
            t.calls.push_back({"mock_" + cfg_.agentId + "_" + std::to_string(++callCounter_) + "_" + std::to_string(n++),
                               c.get("name").asString(), c.get("input").isObject() ? c.get("input") : Json::object(), false});
        }
        std::string stop = spec.get("stop").asString(t.calls.empty() ? "end_turn" : "tool_use");
        t.stop = stop == "tool_use"     ? Stop::ToolUse
                 : stop == "max_tokens" ? Stop::MaxTokens
                 : stop == "refusal"    ? Stop::Refusal
                 : stop == "pause_turn" ? Stop::PauseTurn
                                        : Stop::EndTurn;
        if (t.stop == Stop::Refusal) t.stopDetail = spec.get("stop_details").asString("declined");
        if (t.stop == Stop::EndTurn && !t.calls.empty()) t.stop = Stop::ToolUse;
        t.usage = {spec.get("usage").get("input").asInt(100), spec.get("usage").get("output").asInt(20), 0, 0};
        history_.push(Json::object({{"role", "assistant"}, {"text", t.text}, {"tool_calls", static_cast<int64_t>(t.calls.size())}}));
        return t;
    }

    MockProvider& provider_;
    SessionConfig cfg_;
    Json history_ = Json::array();
    std::string lastPrompt_;
    int callCounter_ = 0;
};

}  // namespace

MockProvider::MockProvider(Json script) : script_(std::move(script)) {
    if (!script_.isObject()) script_ = Json::object();
    echo_ = script_.get("echo").asBool(false);
}

std::unique_ptr<Session> MockProvider::open(const SessionConfig& config) { return std::make_unique<MockSession>(*this, config); }

Json MockProvider::nextTurn(const std::string& agent) {
    std::lock_guard lock(mutex_);
    for (Json* queue : {&script_["agents"][agent], &script_["default"]}) {
        if (queue->isArray() && queue->size() > 0) {
            Json t = (*queue)[size_t{0}];
            queue->elements().erase(queue->elements().begin());
            return t;
        }
    }
    return Json();
}

void MockProvider::record(Json entry) {
    std::lock_guard lock(mutex_);
    log_.push(std::move(entry));
}

Json MockProvider::log() const {
    std::lock_guard lock(mutex_);
    return log_;
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Provider>> fromEnvironment(const std::string& kindIn, HttpFn http) {
    std::string kind = str::lower(kindIn);
    auto env = [](const char* k) {
        const char* v = std::getenv(k);
        return std::string(v ? v : "");
    };
    if (kind == "mock" || kind == "dry-run") {
        return std::unique_ptr<Provider>(std::make_unique<MockProvider>(Json::object({{"echo", true}})));
    }
    if (kind.empty() || kind == "anthropic" || kind == "claude" || str::startsWith(kind, "anthropic")) {
        std::string key = env("ANTHROPIC_API_KEY");
        if (key.empty()) {
            return Error::make("missing_key", "ANTHROPIC_API_KEY is not set",
                               "export ANTHROPIC_API_KEY=... (keys are read from the environment only), or run with --dry-run");
        }
        return makeAnthropic(key, env("ANTHROPIC_BASE_URL"), std::move(http));
    }
    // Everything else speaks the OpenAI-compatible protocol (OpenAI, DeepSeek, OpenRouter,
    // Groq, Ollama, LM Studio, vLLM, llama.cpp...). Local servers need no key.
    std::string base = env("OPENAI_BASE_URL");
    std::string key = env("OPENAI_API_KEY");
    if (base.empty() && key.empty()) {
        return Error::make("missing_key", "OPENAI_API_KEY (or OPENAI_BASE_URL for a local server) is not set",
                           "export OPENAI_API_KEY=... or OPENAI_BASE_URL=http://localhost:11434/v1");
    }
    return makeOpenAI(key, base, env("OPENAI_MODEL"), std::move(http), env("OPENAI_VISION") != "0");
}

}  // namespace sky::studio::llm
