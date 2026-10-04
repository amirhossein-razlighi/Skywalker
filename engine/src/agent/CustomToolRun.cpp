// Custom tools: execution. Wander tools run in a pooled VM against the live scene (or an empty one),
// composite tools call their steps, external tools are forwarded to the client that hosts them.
// Every nested call goes through callNested (capabilities, the caller's permissions, depth and
// limits); external tools' callbacks go through the same checks in the registry gate.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "CustomToolsInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/studio/Studio.h"

namespace sky {

namespace fs = std::filesystem;
using namespace customtools;
using wander::CallContext;
using wander::Value;

namespace {

/// Paths custom tools may never write (see CustomTools.cpp).
constexpr const char* kProtectedWrites[] = {"tools/", "game.json", ".skywalker/", "agents/", "studio/"};
constexpr int64_t kMaxReadBytes = 4 * 1024 * 1024;

Json replaceEntities(Json j) {
    if (j.isObject()) {
        if (j.members().size() == 1) {
            if (const Json* e = j.find("$entity"); e && e->isNumber()) return *e;
        }
        for (auto& [k, v] : j.members()) v = replaceEntities(std::move(v));
    } else if (j.isArray()) {
        for (auto& e : j.elements()) e = replaceEntities(std::move(e));
    }
    return j;
}

std::string firstText(const ToolResult& r) {
    for (const auto& c : r.content) {
        if (c.type == ContentBlock::Type::Text) return c.text;
    }
    return {};
}

/// What a tool call produced, as JSON: its structured content, else its text.
Json payload(const ToolResult& r) {
    if (!r.structured.isNull()) return r.structured;
    return Json(firstText(r));
}

Error errorOf(const ToolResult& r) {
    return Error::make(r.structured.get("error").asString("tool_error"),
                       r.structured.get("message").asString(firstText(r)), r.structured.get("hint").asString());
}

bool truthyJson(const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: return false;
        case Json::Type::Bool: return j.asBool();
        case Json::Type::Number: return j.asNumber() != 0.0;
        case Json::Type::String: return !j.asString().empty();
        case Json::Type::Array: return j.size() > 0;
        case Json::Type::Object: return j.size() > 0;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers: JSON for Wander, templates, expectations
// ---------------------------------------------------------------------------

namespace customtools {

Json toToolJson(const Value& v) { return replaceEntities(wander::toJson(v)); }

namespace {

/// Evaluates one template expression: a path (args.x, steps.find.entities[0].id, item.name,
/// index, prev) with an optional `?? <json default>`.
Result<Json> evalExpression(const std::string& exprIn, const Json& scope) {
    std::string expr = str::trim(exprIn);
    std::optional<Json> fallback;
    if (size_t q = expr.find("??"); q != std::string::npos) {
        std::string def = str::trim(expr.substr(q + 2));
        expr = str::trim(expr.substr(0, q));
        auto parsed = Json::parse(def);
        fallback = parsed ? parsed.value() : Json(def);
    }
    const Json* cur = &scope;
    std::string walked;
    size_t i = 0;
    auto missing = [&](const std::string& key) -> Result<Json> {
        if (fallback) return *fallback;
        std::vector<std::string> keys;
        for (const auto& [k, v] : cur->members()) keys.push_back(k);
        std::string guess = str::closest(key, keys, 3);
        std::string fields;
        for (const auto& k : keys) fields += (fields.empty() ? "" : ", ") + k;
        return Error::make("template_error", "{{" + exprIn + "}}: " + (walked.empty() ? std::string("there is no") : walked + " has no") + " \"" + key + "\"",
                           guess.empty() ? (fields.empty() ? std::string("roots: args, steps, prev, item, index, actor") : "available: " + fields)
                                         : "did you mean \"" + guess + "\"?");
    };
    while (i < expr.size()) {
        if (expr[i] == '.') {
            ++i;
            continue;
        }
        if (expr[i] == '[') {
            size_t close = expr.find(']', i);
            if (close == std::string::npos) return Error::make("template_error", "{{" + exprIn + "}}: missing ']'");
            std::string idx = str::trim(expr.substr(i + 1, close - i - 1));
            i = close + 1;
            double n = 0;
            if (!str::parseDouble(idx, n) || !cur->isArray()) {
                if (fallback) return *fallback;
                return Error::make("template_error", "{{" + exprIn + "}}: [" + idx + "] needs an array and a number");
            }
            auto k = static_cast<size_t>(n);
            if (n < 0 || k >= cur->size()) {
                if (fallback) return *fallback;
                return Error::make("template_error", "{{" + exprIn + "}}: index " + idx + " is out of range (" + std::to_string(cur->size()) + " items)");
            }
            cur = &(*cur)[k];
            walked += "[" + idx + "]";
            continue;
        }
        size_t end = expr.find_first_of(".[", i);
        std::string key = str::trim(expr.substr(i, end == std::string::npos ? std::string::npos : end - i));
        i = end == std::string::npos ? expr.size() : end;
        if (key == "length" && (cur->isArray() || cur->isString())) {
            return Json(static_cast<int64_t>(cur->isArray() ? cur->size() : cur->asString().size()));
        }
        const Json* next = cur->isObject() ? cur->find(key) : nullptr;
        if (!next) return missing(key);
        cur = next;
        walked += (walked.empty() ? "" : ".") + key;
    }
    if (cur->isNull() && fallback) return *fallback;
    return *cur;
}

}  // namespace

Result<Json> resolveTemplate(const Json& tmpl, const Json& scope) {
    if (tmpl.isString()) {
        const std::string& s = tmpl.asString();
        size_t open = s.find("{{");
        if (open == std::string::npos) return tmpl;
        size_t close = s.find("}}", open);
        if (open == 0 && close == s.size() - 2 && s.find("{{", 2) == std::string::npos) {
            return evalExpression(s.substr(2, s.size() - 4), scope);  // the whole string: keep the type
        }
        std::string out;
        size_t at = 0;
        while ((open = s.find("{{", at)) != std::string::npos) {
            close = s.find("}}", open);
            if (close == std::string::npos) break;
            out += s.substr(at, open - at);
            auto v = evalExpression(s.substr(open + 2, close - open - 2), scope);
            if (!v) return v.error();
            out += v->isString() ? v->asString() : v->dump();
            at = close + 2;
        }
        out += s.substr(at);
        return Json(out);
    }
    if (tmpl.isArray()) {
        Json out = Json::array();
        for (const auto& e : tmpl.elements()) {
            auto r = resolveTemplate(e, scope);
            if (!r) return r.error();
            out.push(std::move(r.value()));
        }
        return out;
    }
    if (tmpl.isObject()) {
        Json out = Json::object();
        for (const auto& [k, v] : tmpl.members()) {
            auto r = resolveTemplate(v, scope);
            if (!r) return r.error();
            out[k] = std::move(r.value());
        }
        return out;
    }
    return tmpl;
}

bool matches(const Json& expected, const Json& actual, std::string& why, const std::string& path) {
    auto fail = [&](const std::string& msg) {
        why = path + ": " + msg;
        return false;
    };
    if (expected.isObject() && expected.size() > 0) {
        bool ops = true;
        for (const auto& [k, v] : expected.members()) ops = ops && !k.empty() && k[0] == '$';
        if (ops) {
            for (const auto& [op, v] : expected.members()) {
                double a = actual.asNumber(std::nan("")), b = v.asNumber();
                if (op == "$gte" && !(a >= b)) return fail("expected >= " + v.dump() + ", got " + actual.dump());
                if (op == "$lte" && !(a <= b)) return fail("expected <= " + v.dump() + ", got " + actual.dump());
                if (op == "$gt" && !(a > b)) return fail("expected > " + v.dump() + ", got " + actual.dump());
                if (op == "$lt" && !(a < b)) return fail("expected < " + v.dump() + ", got " + actual.dump());
                if (op == "$len") {
                    size_t n = actual.isString() ? actual.asString().size() : actual.size();
                    if (!actual.isArray() && !actual.isString() && !actual.isObject()) return fail("expected something with a length, got " + actual.dump());
                    if (!matches(v, Json(static_cast<int64_t>(n)), why, path + ".length")) return false;
                }
                if (op == "$contains") {
                    bool found = false;
                    if (actual.isString()) {
                        found = actual.asString().find(v.asString()) != std::string::npos;
                    } else if (actual.isArray()) {
                        std::string ignored;
                        for (const auto& e : actual.elements()) found = found || matches(v, e, ignored, path);
                    }
                    if (!found) return fail("expected it to contain " + v.dump() + ", got " + actual.dump().substr(0, 200));
                }
                if (op == "$type" && v.asString() != Json::typeName(actual.type())) {
                    return fail("expected a " + v.asString() + ", got " + Json::typeName(actual.type()));
                }
                if (op == "$exists" && v.asBool() == actual.isNull()) return fail(v.asBool() ? "expected a value" : "expected nothing");
            }
            return true;
        }
        if (!actual.isObject()) return fail("expected an object, got " + actual.dump().substr(0, 200));
        for (const auto& [k, v] : expected.members()) {
            if (!matches(v, actual.get(k), why, path + "." + k)) return false;
        }
        return true;
    }
    if (expected.isArray()) {
        if (!actual.isArray()) return fail("expected an array, got " + actual.dump().substr(0, 200));
        if (actual.size() != expected.size()) {
            return fail("expected " + std::to_string(expected.size()) + " items, got " + std::to_string(actual.size()));
        }
        for (size_t i = 0; i < expected.size(); ++i) {
            if (!matches(expected[i], actual[i], why, path + "[" + std::to_string(i) + "]")) return false;
        }
        return true;
    }
    if (expected.isNumber() && actual.isNumber()) {
        double a = actual.asNumber(), b = expected.asNumber();
        if (std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b))) return true;
        return fail("expected " + expected.dump() + ", got " + actual.dump());
    }
    if (expected != actual) return fail("expected " + expected.dump() + ", got " + actual.dump().substr(0, 200));
    return true;
}

}  // namespace customtools

double Invocation::elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// ---------------------------------------------------------------------------
// Running a tool
// ---------------------------------------------------------------------------

ToolResult CustomTools::Impl::run(const std::string& name, const Json& args, ToolContext& ctx) {
    auto it = entries.find(name);
    if (it == entries.end() || it->second->status != "active") {
        return ToolResult::error(Error::make("unavailable", name + " is not available right now",
                                             it == entries.end() ? "tool_list_custom shows the custom tools" : it->second->reason));
    }
    return execute(it->second, args, ctx, false);
}

ToolResult CustomTools::Impl::execute(const std::shared_ptr<const ToolEntry>& tool, const Json& args, ToolContext& ctx, bool dryRun) {
    const CustomToolDef& def = tool->def;
    int depth = std::max(static_cast<int>(stack.size()), ctx.depth) + 1;
    if (depth > kMaxDepth) {
        std::string chain;
        for (const Invocation* inv : stack) chain += inv->tool->def.name + " -> ";
        return ToolResult::error(Error::make("recursion_limit",
                                             "custom tools nested more than " + std::to_string(kMaxDepth) + " deep (" + chain + def.name + ")",
                                             "a tool calling itself (directly or through other tools) needs a stopping condition"));
    }
    auto inv = std::make_shared<Invocation>();
    inv->callId = "call-" + std::to_string(nextCall++);
    inv->tool = tool;
    inv->actor = ctx.actor;
    inv->depth = depth;
    inv->dryRun = dryRun || std::any_of(stack.begin(), stack.end(), [](const Invocation* i) { return i->dryRun; });
    inv->started = std::chrono::steady_clock::now();
    inv->deadline = inv->started + std::chrono::milliseconds(def.limits.timeoutMs);
    // Nested calls keep the outer deadline when it is sooner.
    if (!stack.empty()) inv->deadline = std::min(inv->deadline, stack.back()->deadline);
    if (str::startsWith(ctx.actor, "agent:")) {
        studio::Studio& s = engine.studio();
        if (const studio::AgentProfile* p = s.agent(s.memberForActor(ctx.actor))) {
            bool mutates = def.caps.mutate || !def.caps.writePaths.empty();
            inv->askedHuman = p->access(def.category, !mutates, def.caps.network) == studio::Access::Ask;
        }
    }
    Json full = withDefaults(def.inputSchema, args);
    if (def.kind == "external") return tool->hosted ? runHosted(inv, full) : runExternal(inv, full);

    stack.push_back(inv.get());
    struct Pop {
        std::vector<Invocation*>& stack;
        ~Pop() { stack.pop_back(); }
    } pop{stack};
    ToolResult r = def.kind == "wander" ? runWander(*inv, full) : runComposite(*inv, full);
    return finish(*inv, std::move(r));
}

std::unique_ptr<wander::Runtime> CustomTools::Impl::acquireRuntime(bool scene) {
    auto& pool = scene ? scenePool : sandboxPool;
    if (!pool.empty()) {
        auto rt = std::move(pool.back());
        pool.pop_back();
        return rt;
    }
    return std::make_unique<wander::Runtime>(scene ? engine.scene() : *sandbox, builtins.get());
}

void CustomTools::Impl::releaseRuntime(bool scene, std::unique_ptr<wander::Runtime> rt) {
    (void)rt->drainMessages();
    auto& pool = scene ? scenePool : sandboxPool;
    if (pool.size() < 4) pool.push_back(std::move(rt));
}

ToolResult CustomTools::Impl::runWander(Invocation& inv, const Json& args) {
    const CustomToolDef& def = inv.tool->def;
    const bool scene = def.caps.readScene;
    const bool mutate = def.caps.mutate && scene;
    if (!inv.tool->program) return ToolResult::error(Error::make("invalid_definition", def.name + " has no compiled code"));
    std::unique_ptr<wander::Runtime> rt = acquireRuntime(scene);
    rt->provide<Invocation>(&inv);
    rt->provide<CustomTools::Impl>(this);
    if (mutate) {
        rt->spawnPrefab = [this](const std::string& ref, Vec3 position, const std::string& name) -> Result<EntityId> {
            PrefabPlacement placement;
            placement.hasPosition = true;
            placement.position = position;
            placement.name = name;
            return engine.instantiatePrefabAsset(ref, placement);
        };
    } else {
        rt->spawnPrefab = nullptr;
    }
    std::vector<Value> params;
    for (const auto& f : inv.tool->program->functions) {
        if (f.name == "run" && f.behavior < 0 && f.params.size() == 1) params.push_back(wander::fromJson(args));
    }
    wander::Runtime::FunctionCall call;
    call.budget = def.limits.instructions;
    call.scriptName = def.name;
    wander::Runtime::FunctionResult fr;
    Json changed;
    // Read-only tools: assignments to entities raise at once (the write guard); only code that calls
    // builtins able to edit the scene (spawn, move, add_tag...) pays for a before/after comparison.
    const std::string guard = def.name + " may not change the scene: it lacks the mutate capability";
    const bool sceneBuiltins = callsSceneBuiltins(*inv.tool->program);
    const bool compare = scene && !mutate && sceneBuiltins;
    rt->setSceneWriteGuard(scene && !mutate ? guard : std::string());
    rt->setRecordSceneWrites(mutate);  // direct assignments notify the undo history as they happen
    auto body = [&]() -> Status {
        Scene& s = engine.scene();
        const bool editing = engine.playState() == PlayState::Editing;
        Json before;
        if (compare) before = s.toJson();  // read-only tools must leave the scene exactly as it was
        // Builtins such as move or rotate edit transforms without change notifications: capture every entity first.
        if (mutate && editing && sceneBuiltins) engine.history().touchAll();
        fr = rt->callFunction(inv.tool->program, "run", std::move(params), call);
        for (const auto& m : rt->drainMessages()) {
            if (m.kind == wander::RuntimeMessage::Kind::Log && inv.logs.size() < 200) inv.logs.push_back(m.text);
        }
        if (!fr.ok && fr.error == guard) {
            return Error::make("capability_denied", guard + " (line " + std::to_string(fr.loc.line) + ")",
                               "declare capabilities.mutate: true (needs a human's approval under the default policy), or only read");
        }
        if (compare) {
            Json after = s.toJson();
            if (after != before) {
                (void)s.loadJson(before);
                changed = Json(true);
                return Error::make("capability_denied", def.name + " changed the scene but lacks the mutate capability; its changes were undone",
                                   "declare capabilities.mutate: true (needs a human's approval under the default policy), or only read");
            }
        }
        if (!fr.ok) {
            std::string where = fr.loc.line > 0 ? " (line " + std::to_string(fr.loc.line) + (fr.file.empty() ? "" : " of " + fr.file) + ")" : "";
            std::string message = fr.error;
            std::string hint = "fix the code with tool_define; tool_test runs it without side effects";
            if (str::startsWith(message, "execution budget exceeded")) {
                message = "instruction budget exceeded: more than " + std::to_string(def.limits.instructions) + " steps (limits.instructions)";
                hint = "an endless loop? Otherwise raise limits.instructions (max " + std::to_string(kMaxInstructions) + ")";
            }
            if (inv.failure) return *inv.failure;  // tool_fail(): the tool's own error
            return Error::make("runtime_error", message + where, hint);
        }
        if (inv.failure) return *inv.failure;
        if (inv.expired()) {
            return Error::make("timeout", def.name + " ran longer than " + std::to_string(def.limits.timeoutMs) + " ms (limits.timeout_ms)",
                               "raise limits.timeout_ms or do less per call");
        }
        return {};
    };
    Status st = mutate ? engine.edit(inv.actor, def.title.empty() ? def.name : def.title, body) : body();
    if (scene && (mutate || !changed.isNull())) engine.scene().markDirty();
    rt->provide<Invocation>(nullptr);
    rt->setSceneWriteGuard({});
    rt->setRecordSceneWrites(false);
    releaseRuntime(scene, std::move(rt));
    if (!st) return ToolResult::error(st.error());
    Json out = Json::object({{"result", toToolJson(fr.value)}});
    std::string summary = def.name + ": " + out.get("result").dump().substr(0, 300);
    return ToolResult::json(std::move(out), summary);
}

ToolResult CustomTools::Impl::runComposite(Invocation& inv, const Json& args) {
    const CustomToolDef& def = inv.tool->def;
    Json scope = Json::object({{"args", args}, {"steps", Json::object()}, {"prev", Json()}, {"actor", inv.actor}});
    Json last;
    int ran = 0;
    Json stepErrors = Json::array();
    std::optional<Error> failed;
    auto body = [&]() -> Status {
        for (size_t i = 0; i < def.steps.size(); ++i) {
            const Json& step = def.steps[i];
            const std::string& tool = step.get("tool").asString();
            std::string where = "step " + std::to_string(i) + " (" + tool + ")";
            if (step.contains("when")) {
                auto cond = resolveTemplate(step.get("when"), scope);
                if (!cond) return Error::make(cond.error().code, where + ": " + cond.error().message, cond.error().hint);
                if (!truthyJson(cond.value())) continue;
            }
            Json items;
            bool loop = step.contains("for_each");
            if (loop) {
                auto each = resolveTemplate(step.get("for_each"), scope);
                if (!each) return Error::make(each.error().code, where + ": " + each.error().message, each.error().hint);
                if (each->isNumber()) {
                    items = Json::array();
                    int64_t n = std::clamp<int64_t>(each->asInt(), 0, def.limits.maxCalls);
                    for (int64_t k = 0; k < n; ++k) items.push(k);
                } else if (each->isArray()) {
                    items = each.value();
                } else {
                    return Error::make("template_error", where + ": for_each must give an array or a count, got " + each->dump().substr(0, 120));
                }
            } else {
                items = Json::array({Json()});
            }
            std::string as = step.get("as").asString("item");
            Json results = Json::array();
            for (size_t k = 0; k < items.size(); ++k) {
                if (loop) {
                    scope[as] = items[k];
                    scope["index"] = static_cast<int64_t>(k);
                }
                auto callArgs = resolveTemplate(step.get("args").isNull() ? Json::object() : step.get("args"), scope);
                if (!callArgs) return Error::make(callArgs.error().code, where + ": " + callArgs.error().message, callArgs.error().hint);
                ToolResult r = callNested(inv, tool, callArgs.value());
                ++ran;
                if (r.isError) {
                    Error e = errorOf(r);
                    if (step.get("continue_on_error").asBool(false)) {
                        stepErrors.push(Json::object({{"step", static_cast<int64_t>(i)}, {"tool", tool}, {"error", e.code}, {"message", e.message}}));
                        results.push(Json());
                        continue;
                    }
                    failed = e;
                    return Error::make("step_failed", where + (loop ? " item " + std::to_string(k) : "") + " failed: [" + e.code + "] " + e.message,
                                       e.hint.empty() ? (def.caps.mutate ? "nothing was applied (the whole tool is one undo step)" : "") : e.hint);
                }
                results.push(payload(r));
            }
            Json value = loop ? results : results[0];
            if (std::string id = step.get("id").asString(); !id.empty()) scope["steps"][id] = value;
            scope["prev"] = value;
            last = value;
        }
        return {};
    };
    Status st = def.caps.mutate ? engine.edit(inv.actor, def.title.empty() ? def.name : def.title, body) : body();
    if (!st) return ToolResult::error(st.error());
    Json result = last;
    if (!def.result.isNull()) {
        auto mapped = resolveTemplate(def.result, scope);
        if (!mapped) return ToolResult::error(Error::make(mapped.error().code, "result: " + mapped.error().message, mapped.error().hint));
        result = mapped.value();
    }
    Json out = Json::object({{"result", result}, {"steps_run", ran}});
    if (stepErrors.size()) out["step_errors"] = stepErrors;
    return ToolResult::json(std::move(out), def.name + ": " + result.dump().substr(0, 300));
}

ToolResult CustomTools::Impl::runExternal(const std::shared_ptr<Invocation>& inv, const Json& args) {
    const CustomToolDef& def = inv->tool->def;
    std::shared_ptr<ExternalHost> host = inv->tool->host;
    if (!host || !host->connected()) {
        return finish(*inv, ToolResult::error(Error::make("tool_offline", def.name + " is offline: the client hosting it is not connected",
                                                          "start the client again; it registers the tool when it connects")));
    }
    // Forget calls whose results were never collected (an abandoned request).
    auto now = std::chrono::steady_clock::now();
    for (auto it = external.begin(); it != external.end();) {
        it = now > it->second->deadline + std::chrono::minutes(1) ? external.erase(it) : std::next(it);
    }
    external[inv->callId] = inv;
    struct Outcome {
        Result<Json> result = Error::make("not_run", "the call did not run");
    };
    auto outcome = std::make_shared<Outcome>();
    Json params = Json::object({{"name", def.name},
                                {"arguments", args},
                                {"call_id", inv->callId},
                                {"actor", inv->actor},
                                {"timeout_ms", def.limits.timeoutMs},
                                {"dry_run", inv->dryRun},
                                {"depth", inv->depth}});
    Engine* eng = &engine;
    std::string lane = inv->callId;
    auto timeout = std::chrono::milliseconds(def.limits.timeoutMs);
    // The wait runs off the main thread when the caller can (agents over MCP, the crew); a caller on
    // the main thread serves the tool's callbacks (its lane) while it waits.
    auto work = [host, params, timeout, outcome, eng, lane] {
        outcome->result = host->request("skywalker/tools/call", params, timeout, [eng, lane] {
            if (eng->onMainThread()) eng->pumpLane(lane);
        });
    };
    auto done = [this, inv, outcome]() -> ToolResult {
        external.erase(inv->callId);
        if (!outcome->result) return finish(*inv, ToolResult::error(outcome->result.error()));
        const Json& r = outcome->result.value();
        ToolResult out;
        for (const auto& block : r.get("content").elements()) {
            if (block.get("type").asString() == "image") {
                out.content.push_back({ContentBlock::Type::Image, {}, block.get("data").asString(), block.get("mimeType").asString("image/png")});
            } else {
                out.content.push_back({ContentBlock::Type::Text, block.get("text").asString(block.dump()), {}, {}});
            }
        }
        out.structured = r.get("structuredContent");
        out.isError = r.get("isError").asBool(false);
        if (out.content.empty()) out.content.push_back({ContentBlock::Type::Text, out.structured.isNull() ? r.dump() : out.structured.dump(), {}, {}});
        if (out.isError && !out.structured.isObject()) {
            out.structured = Json::object({{"error", "tool_error"}, {"message", firstText(out)}});
        }
        return finish(*inv, std::move(out));
    };
    return ToolResult::defer(std::move(work), std::move(done));
}

ToolResult CustomTools::Impl::runHosted(const std::shared_ptr<Invocation>& inv, const Json& args) {
    const ToolDef& transport = *inv->tool->hosted;
    external[inv->callId] = inv;  // callbacks carrying this call id get the tool's capabilities
    ToolContext ctx;
    ctx.actor = inv->actor;
    ctx.depth = inv->depth;
    ctx.callId = inv->callId;
    ToolResult r;
    try {
        r = transport.handler(args, ctx);
    } catch (const std::exception& e) {
        r = ToolResult::error(Error::make("internal_error", std::string("tool crashed: ") + e.what()));
    }
    if (!r.deferred) {
        external.erase(inv->callId);
        return finish(*inv, std::move(r));
    }
    std::shared_ptr<DeferredWork> d = std::move(r.deferred);
    return ToolResult::defer(
        d->work,
        [this, inv, d]() -> ToolResult {
            external.erase(inv->callId);
            return finish(*inv, d->finish ? d->finish() : ToolResult::text(""));
        },
        d->cancel);
}

ToolResult CustomTools::Impl::finish(Invocation& inv, ToolResult result) {
    const CustomToolDef& def = inv.tool->def;
    if (!result.isError && !def.outputSchema.isNull()) {
        const Json& checked = def.kind == "external" ? result.structured : result.structured.get("result");
        if (Status s = validateSchema(def.outputSchema, checked, "result"); !s) {
            result = ToolResult::error(Error::make("invalid_output", def.name + " returned output that does not match its output_schema: " + s.error().message,
                                                   s.error().hint));
        }
    }
    if (!result.isError) {
        size_t bytes = result.structured.isNull() ? 0 : result.structured.dump().size();
        for (const auto& c : result.content) bytes += c.text.size() + c.data.size();
        if (static_cast<int64_t>(bytes) > def.limits.maxOutputBytes) {
            result = ToolResult::error(Error::make("output_too_large",
                                                   def.name + " returned " + std::to_string(bytes) + " bytes; the limit is " +
                                                       std::to_string(def.limits.maxOutputBytes) + " (limits.max_output_bytes)",
                                                   "return less (summaries, ids, a limit argument) or raise the limit"));
        }
    }
    if (result.structured.isObject()) {
        if (!inv.logs.empty()) {
            Json logs = Json::array();
            for (const auto& l : inv.logs) logs.push(l);
            result.structured["logs"] = logs;
        }
        if (!inv.warnings.empty()) {
            Json w = Json::array();
            for (const auto& s : inv.warnings) w.push(s);
            result.structured["warnings"] = w;
        }
        if (inv.dryRun) result.structured["dry_run"] = true;
    }
    double ms = inv.elapsedMs();
    std::string error = result.isError ? firstText(result) : std::string();
    if (!inv.dryRun) {
        if (auto it = entries.find(def.name); it != entries.end() && it->second->hash == inv.tool->hash) {
            it->second->stats.record(inv.actor, !result.isError, ms, error.substr(0, 300), inv.nested);
        }
        Json ev = Json::object({{"event", "call"}, {"tool", def.name}, {"actor", inv.actor}, {"ok", !result.isError}, {"ms", ms}});
        if (inv.nested.size()) ev["calls"] = inv.nested;
        if (result.isError) ev["error"] = error.substr(0, 500);
        audit(std::move(ev));
        engine.emitEvent(Json::object({{"type", "custom_tool"},
                                       {"action", "call"},
                                       {"tool", def.name},
                                       {"actor", inv.actor},
                                       {"ok", !result.isError},
                                       {"ms", ms},
                                       {"calls", static_cast<int64_t>(inv.nested.size())}}));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Nested calls and capability checks
// ---------------------------------------------------------------------------

Status CustomTools::Impl::checkNested(const Invocation& inv, const ToolDef& target) const {
    const CustomToolDef& def = inv.tool->def;
    const ToolCapabilities& caps = def.caps;
    if (target.origin.empty() && target.name.rfind("tool_", 0) == 0) {
        return Error::make("capability_denied", "custom tools may not call " + target.name, "tools cannot define, approve or remove tools");
    }
    if (!allows(caps.calls, target.name)) {
        std::string list;
        for (const auto& c : caps.calls) list += (list.empty() ? "" : ", ") + c;
        return Error::make("capability_denied", def.name + " may not call " + target.name + " (capabilities.calls: [" + list + "])",
                           "add it to capabilities.calls with tool_define (privileged changes need a human's approval again)");
    }
    if (target.mutates && !caps.mutate) {
        return Error::make("capability_denied", def.name + " may not call " + target.name + ", which modifies the project (no mutate capability)",
                           "declare capabilities.mutate: true (needs a human's approval under the default policy)");
    }
    if (target.openWorld && !caps.network) {
        return Error::make("capability_denied", def.name + " may not call " + target.name + ", which reaches outside the project",
                           "only external tools with capabilities.network may");
    }
    // The caller's own permissions still apply (least privilege): a crew agent cannot reach, through a
    // tool, what its permissions turn off or what needs a human's OK it did not get.
    if (str::startsWith(inv.actor, "agent:")) {
        studio::Studio& s = engine.studio();
        if (const studio::AgentProfile* p = s.agent(s.memberForActor(inv.actor))) {
            studio::Access access = p->access(target.category, !target.mutates, target.openWorld);
            if (access == studio::Access::Off) {
                return Error::make("permission_denied", inv.actor + "'s permissions do not allow " + target.category + " tools (" + target.name + ")",
                                   "change the agent's permissions in its profile, or ask a teammate who has them");
            }
            if (access == studio::Access::Ask && !inv.askedHuman) {
                return Error::make("permission_denied", target.name + " needs a human's OK for " + inv.actor + ", which this call did not get",
                                   "call " + target.name + " directly so the human is asked");
            }
        }
    }
    return {};
}

ToolResult CustomTools::Impl::callNested(Invocation& inv, const std::string& tool, const Json& args) {
    const ToolDef* target = engine.tools().find(tool);
    if (!target) {
        if (auto e = find(tool)) {
            return ToolResult::error(Error::make("unavailable", tool + " is a custom tool that is not active (" + e->status + ")", e->reason));
        }
        std::string guess = str::closest(tool, inv.tool->def.caps.calls, 4);
        if (guess.empty()) guess = str::closest(tool, engine.tools().names(), 4);
        return ToolResult::error(Error::make("unknown_tool", "no tool named '" + tool + "'", guess.empty() ? "" : "did you mean '" + guess + "'?"));
    }
    if (Status s = checkNested(inv, *target); !s) return ToolResult::error(s.error());
    if (inv.calls >= inv.tool->def.limits.maxCalls) {
        return ToolResult::error(Error::make("call_limit", inv.tool->def.name + " made more than " + std::to_string(inv.tool->def.limits.maxCalls) +
                                                               " tool calls (limits.max_calls)",
                                             "batch the work (e.g. one scene_query instead of many entity_get) or raise limits.max_calls"));
    }
    if (inv.expired()) {
        return ToolResult::error(Error::make("timeout", inv.tool->def.name + " ran out of time (limits.timeout_ms = " +
                                                            std::to_string(inv.tool->def.limits.timeoutMs) + ")"));
    }
    ++inv.calls;
    auto t0 = std::chrono::steady_clock::now();
    ToolContext ctx;
    ctx.actor = inv.actor;
    ctx.depth = inv.depth;
    ToolResult r = engine.callTool(tool, args, ctx);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (inv.nested.size() < 100) inv.nested.push(Json::object({{"tool", tool}, {"ok", !r.isError}, {"ms", ms}}));
    return r;
}

std::optional<ToolResult> CustomTools::Impl::gate(const ToolDef& tool, const Json&, ToolContext& ctx) {
    if (ctx.parentCall.empty()) return std::nullopt;
    auto it = external.find(ctx.parentCall);
    if (it == external.end()) {
        return ToolResult::error(Error::make("unknown_call", "call id " + ctx.parentCall + " is not running (it finished or never existed)",
                                             "only pass _meta[\"skywalker/call_id\"] while serving that skywalker/tools/call request"));
    }
    Invocation& inv = *it->second;
    if (Status s = checkNested(inv, tool); !s) return ToolResult::error(s.error());
    if (inv.calls >= inv.tool->def.limits.maxCalls) {
        return ToolResult::error(Error::make("call_limit", inv.tool->def.name + " made more than " +
                                                               std::to_string(inv.tool->def.limits.maxCalls) + " callbacks (limits.max_calls)"));
    }
    if (inv.expired()) return ToolResult::error(Error::make("timeout", inv.tool->def.name + " ran out of time"));
    ++inv.calls;
    if (inv.nested.size() < 100) inv.nested.push(Json::object({{"tool", tool.name}, {"callback", true}}));
    ctx.actor = inv.actor;  // attributed to whoever called the external tool
    ctx.depth = inv.depth;
    return std::nullopt;
}

Result<std::string> CustomTools::Impl::checkPath(const Invocation& inv, const std::string& path, bool write) const {
    const ToolCapabilities& caps = inv.tool->def.caps;
    const auto& patterns = write ? caps.writePaths : caps.readPaths;
    std::string rel = path;
    while (str::startsWith(rel, "./")) rel = rel.substr(2);
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos) {
        return Error::make("capability_denied", "\"" + path + "\" is not a path inside the project", "use a project-relative path such as \"data/waves.json\"");
    }
    auto allowed = [&](const std::string& p) {
        if (!p.empty() && p.back() == '/') return str::startsWith(rel, p) || rel + "/" == p;
        return p == rel || str::globMatch(p, rel);
    };
    bool ok = std::any_of(patterns.begin(), patterns.end(), allowed);
    if (!ok && !write) ok = std::any_of(caps.writePaths.begin(), caps.writePaths.end(), allowed);  // what it may write it may read
    if (!ok) {
        std::string list;
        for (const auto& p : patterns) list += (list.empty() ? "" : ", ") + p;
        return Error::make("capability_denied",
                           inv.tool->def.name + " may not " + (write ? "write" : "read") + " " + rel + " (capabilities.files." + (write ? "write" : "read") +
                               ": [" + list + "])",
                           "declare the folder in capabilities.files with tool_define");
    }
    if (write) {
        for (const char* prot : kProtectedWrites) {
            if (str::startsWith(rel, prot) || rel == std::string(prot)) {
                return Error::make("capability_denied", "tools may never write " + rel);
            }
        }
    }
    // Symlinks must not lead outside the project.
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(projectDir()), ec);
    fs::path full = fs::weakly_canonical(root / rel, ec);
    std::string r = root.string(), f = full.string();
    if (ec || f.size() < r.size() || f.compare(0, r.size(), r) != 0) {
        return Error::make("capability_denied", "\"" + path + "\" resolves outside the project");
    }
    return full.string();
}

// ---------------------------------------------------------------------------
// Tests (dry runs)
// ---------------------------------------------------------------------------

Json CustomTools::Impl::testOnce(const std::shared_ptr<const ToolEntry>& tool, const Json& args, const Json& setup, const std::string& actor) {
    Json outcome = Json::object();
    if (engine.history().inTransaction()) {
        return Json::object({{"ok", false}, {"error", "unavailable"}, {"message", "tool tests cannot run inside another edit (batch)"}});
    }
    auto t0 = std::chrono::steady_clock::now();
    (void)engine.edit(actor, "Test " + tool->def.name, [&]() -> Status {
        for (const auto& step : setup.elements()) {
            ToolResult r = engine.callTool(step.get("tool").asString(), step.get("args").isNull() ? Json::object() : step.get("args"), actor);
            if (r.isError) {
                Error e = errorOf(r);
                outcome = Json::object({{"ok", false}, {"error", "setup_failed"}, {"message", "setup " + step.get("tool").asString() + " failed: " + e.message}});
                return Error::make("dry_run", "rolled back");
            }
        }
        ToolContext ctx;
        ctx.actor = actor;
        if (Status s = validateSchema(tool->def.inputSchema, withDefaults(tool->def.inputSchema, args)); !s) {
            outcome = Json::object({{"ok", false}, {"error", s.error().code}, {"message", s.error().message}});
            return Error::make("dry_run", "rolled back");
        }
        ToolResult r = execute(tool, args, ctx, true).complete();
        if (r.isError) {
            Error e = errorOf(r);
            outcome = Json::object({{"ok", false}, {"error", e.code}, {"message", e.message}});
            if (!e.hint.empty()) outcome["hint"] = e.hint;
        } else {
            outcome = Json::object({{"ok", true}, {"result", tool->def.kind == "external" ? r.structured : r.structured.get("result")}});
            if (r.structured.contains("logs")) outcome["logs"] = r.structured.get("logs");
            if (r.structured.contains("warnings")) outcome["warnings"] = r.structured.get("warnings");
        }
        if (r.structured.isObject() && r.structured.contains("step_errors")) outcome["step_errors"] = r.structured.get("step_errors");
        return Error::make("dry_run", "rolled back");  // always: tests never change the project
    });
    outcome["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    outcome["dry_run"] = true;
    return outcome;
}

Json CustomTools::Impl::runTests(const std::shared_ptr<const ToolEntry>& tool, const std::string& actor, const std::string& filter) {
    Json tests = Json::array();
    int passed = 0, failed = 0;
    for (size_t i = 0; i < tool->def.tests.size(); ++i) {
        const Json& t = tool->def.tests[i];
        std::string name = t.get("name").asString("test " + std::to_string(i + 1));
        if (!filter.empty() && str::lower(name).find(str::lower(filter)) == std::string::npos) continue;
        Json outcome = testOnce(tool, t.get("args").isNull() ? Json::object() : t.get("args"), t.get("setup"), actor);
        const Json& expect = t.get("expect");
        Json failures = Json::array();
        const std::string& wantError = expect.get("error").asString();
        bool wantOk = expect.get("ok").asBool(wantError.empty());
        bool ok = outcome.get("ok").asBool();
        if (ok != wantOk) {
            failures.push(wantOk ? "expected success, got [" + outcome.get("error").asString() + "] " + outcome.get("message").asString()
                                 : "expected an error, but the call succeeded");
        } else if (!wantError.empty() && outcome.get("error").asString() != wantError) {
            failures.push("expected error " + wantError + ", got " + outcome.get("error").asString() + ": " + outcome.get("message").asString());
        }
        if (ok && expect.contains("result")) {
            std::string why;
            if (!matches(expect.get("result"), outcome.get("result"), why)) failures.push(why);
        }
        if (const Json* c = expect.find("contains")) {
            std::string text = outcome.dump();
            if (text.find(c->asString()) == std::string::npos) failures.push("expected the output to contain \"" + c->asString() + "\"");
        }
        if (const Json* m = expect.find("max_ms"); m && outcome.get("ms").asNumber() > m->asNumber()) {
            failures.push("took " + std::to_string(static_cast<int>(outcome.get("ms").asNumber())) + " ms, more than max_ms " + m->dump());
        }
        bool pass = failures.size() == 0;
        (pass ? passed : failed) += 1;
        Json entry = Json::object({{"name", name}, {"passed", pass}, {"ms", outcome.get("ms")}});
        if (!pass) {
            entry["failures"] = failures;
            entry["outcome"] = outcome;
        }
        tests.push(entry);
    }
    audit(Json::object({{"event", "test"}, {"tool", tool->def.name}, {"actor", actor}, {"passed", passed}, {"failed", failed}}));
    return Json::object({{"tool", tool->def.name}, {"passed", passed}, {"failed", failed}, {"total", passed + failed}, {"tests", tests}});
}

// ---------------------------------------------------------------------------
// Wander builtins available inside tools
// ---------------------------------------------------------------------------

namespace {

Invocation& invocation(CallContext& c) {
    auto* inv = c.service<Invocation>();
    if (!inv) c.fail(c.def().name + "() is only available inside custom tools");
    return *inv;
}

CustomTools::Impl& manager(CallContext& c) {
    auto* impl = c.service<CustomTools::Impl>();
    if (!impl) c.fail(c.def().name + "() is only available inside custom tools");
    return *impl;
}

Json argsMap(CallContext& c, int i) {
    if (c.argc() <= i || c.arg(i).isNone()) return Json::object();
    Json a = toToolJson(c.arg(i));
    if (!a.isObject()) c.fail(c.def().name + "(): the arguments must be a map, e.g. {tag: \"enemy\"}");
    return a;
}

Value callTool(CallContext& c) {
    Invocation& inv = invocation(c);
    CustomTools::Impl& impl = manager(c);
    c.charge(1000);
    std::string name = c.string(0);
    ToolResult r = impl.callNested(inv, name, argsMap(c, 1));
    if (r.isError) {
        Error e = errorOf(r);
        c.fail("call_tool(\"" + name + "\") failed: [" + e.code + "] " + e.message + (e.hint.empty() ? "" : " (hint: " + e.hint + ")"));
    }
    return wander::fromJson(payload(r));
}

Value tryTool(CallContext& c) {
    Invocation& inv = invocation(c);
    CustomTools::Impl& impl = manager(c);
    c.charge(1000);
    ToolResult r = impl.callNested(inv, c.string(0), argsMap(c, 1));
    Json out = Json::object({{"ok", !r.isError}});
    if (r.isError) {
        Error e = errorOf(r);
        out["error"] = e.code;
        out["message"] = e.message;
        if (!e.hint.empty()) out["hint"] = e.hint;
    } else {
        out["result"] = payload(r);
    }
    return wander::fromJson(out);
}

Value toolWarn(CallContext& c) {
    Invocation& inv = invocation(c);
    if (inv.warnings.size() < 50) inv.warnings.push_back(c.display(c.arg(0)));
    return {};
}

Value toolFail(CallContext& c) {
    Invocation& inv = invocation(c);
    std::string message = c.display(c.arg(0));
    inv.failure = Error::make("tool_failed", message, c.argc() > 1 ? c.display(c.arg(1)) : std::string());
    c.fail(message);
}

Value readFile(CallContext& c) {
    Invocation& inv = invocation(c);
    auto path = manager(c).checkPath(inv, c.string(0), false);
    if (!path) c.fail(path.error().message);
    std::error_code ec;
    auto size = fs::file_size(*path, ec);
    if (ec) c.fail("no file " + c.string(0) + " in the project");
    if (static_cast<int64_t>(size) > kMaxReadBytes) c.fail(c.string(0) + " is larger than 4 MB");
    std::ifstream f(*path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    c.charge(static_cast<int64_t>(size / 64));
    return Value::string(ss.str());
}

Value writeFile(CallContext& c) {
    Invocation& inv = invocation(c);
    auto path = manager(c).checkPath(inv, c.string(0), true);
    if (!path) c.fail(path.error().message);
    std::string text = c.display(c.arg(1));
    if (static_cast<int64_t>(text.size()) > kMaxReadBytes) c.fail("write_file(): more than 4 MB");
    if (inv.dryRun) {
        if (inv.warnings.size() < 50) inv.warnings.push_back("dry run: " + c.string(0) + " was not written");
        return Value::boolean(true);
    }
    std::error_code ec;
    fs::create_directories(fs::path(*path).parent_path(), ec);
    std::ofstream f(*path, std::ios::binary | std::ios::trunc);
    f << text;
    if (!f) c.fail("cannot write " + c.string(0));
    return Value::boolean(true);
}

Value listFiles(CallContext& c) {
    Invocation& inv = invocation(c);
    std::string folder = c.string(0);
    if (!folder.empty() && folder.back() != '/') folder += "/";
    auto path = manager(c).checkPath(inv, folder, false);
    if (!path) c.fail(path.error().message);
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(*path, ec)) {
        if (names.size() >= 1000) break;
        names.push_back(folder + e.path().filename().string() + (e.is_directory(ec) ? "/" : ""));
    }
    std::sort(names.begin(), names.end());
    std::vector<Value> out;
    for (auto& n : names) out.push_back(Value::string(std::move(n)));
    c.charge(static_cast<int64_t>(out.size()));
    return Value::list(std::move(out));
}

Value fileExists(CallContext& c) {
    Invocation& inv = invocation(c);
    auto path = manager(c).checkPath(inv, c.string(0), false);
    if (!path) c.fail(path.error().message);
    std::error_code ec;
    return Value::boolean(fs::exists(*path, ec));
}

Value jsonParse(CallContext& c) {
    auto j = Json::parse(c.string(0));
    if (!j) c.fail("json_parse(): " + j.error().message);
    c.charge(static_cast<int64_t>(c.string(0).size() / 64));
    return wander::fromJson(j.value());
}

Value jsonText(CallContext& c) {
    int indent = c.argc() > 1 ? static_cast<int>(c.number(1)) : -1;
    return Value::string(toToolJson(c.arg(0)).dump(indent));
}

Value toolActor(CallContext& c) { return Value::string(invocation(c).actor); }

}  // namespace

void CustomTools::Impl::registerBuiltins() {
    builtins = std::make_unique<wander::BuiltinRegistry>(&engine.builtins());
    CustomTools::registerBuiltins(*builtins);
}

void CustomTools::registerBuiltins(wander::BuiltinRegistry& registry) {
    using namespace wander;
    BuiltinRegistry* builtins = &registry;
    auto add = [&](const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* doc, const char* example,
                   BuiltinImpl fn, bool pure = false) {
        BuiltinDef d;
        d.name = name;
        d.params = std::move(params);
        d.returns = returns;
        d.category = "tools";
        d.doc = doc;
        d.example = example;
        d.fn = fn;
        d.pure = pure;
        d.owner = "tools";
        builtins->add(std::move(d));
    };
    add("call_tool", {{"name", kTString}, {"args", kTMap | kTNone, true}}, kTAny,
        "Calls an engine tool (one listed in the tool's capabilities.calls) and returns its structured result; fails the "
        "tool if the call fails. Entities in arguments become their ids.",
        "let found = call_tool(\"scene_query\", {tag: \"enemy\"})", callTool);
    add("try_tool", {{"name", kTString}, {"args", kTMap | kTNone, true}}, kTMap,
        "Like call_tool, but never fails: returns {ok, result} or {ok: false, error, message, hint}.",
        "let r = try_tool(\"entity_get\", {entity: \"Boss\"})", tryTool);
    add("tool_warn", {{"message", kTAny}}, kTNone, "Adds a warning to the tool's result (shown to the calling agent).",
        "tool_warn(\"3 pickups had no rarity; treated as common\")", toolWarn);
    add("tool_fail", {{"message", kTAny}, {"hint", kTAny, true}}, kTNone,
        "Stops the tool with an error (code tool_failed) and an optional hint for the caller. Edits are rolled back.",
        "if radius <= 0 then tool_fail(\"radius must be positive\", \"try 10\") end", toolFail);
    add("read_file", {{"path", kTString}}, kTString, "Reads a project file the tool may read (capabilities.files.read).",
        "let waves = json_parse(read_file(\"data/waves.json\"))", readFile);
    add("write_file", {{"path", kTString}, {"text", kTAny}}, kTBool,
        "Writes a project file the tool may write (capabilities.files.write). Skipped in dry runs (tests).",
        "write_file(\"exports/report.json\", json_text(report, 2))", writeFile);
    add("list_files", {{"folder", kTString}}, kTList, "Project-relative paths in a folder the tool may read (folders end with /).",
        "for f in list_files(\"levels\") log f end", listFiles);
    add("file_exists", {{"path", kTString}}, kTBool, "Whether a project file the tool may read exists.",
        "if file_exists(\"data/waves.json\") then log \"found\" end", fileExists);
    add("json_parse", {{"text", kTString}}, kTAny,
        "Parses JSON text into maps, lists, numbers, strings and bools ([x, y, z] becomes a vector).",
        "let data = json_parse(read_file(\"data/loot.json\"))", jsonParse, true);
    add("json_text", {{"value", kTAny}, {"indent", kTNumber, true}}, kTString,
        "JSON text of a value (entities become their ids); indent pretty-prints.",
        "write_file(\"exports/state.json\", json_text(state, 2))", jsonText, true);
    add("tool_actor", {}, kTString, "Who called the tool (\"agent:mira\", \"mcp:claude-code\", \"user\").",
        "log \"called by {tool_actor()}\"", toolActor);
}

}  // namespace sky
