// Custom tools: definitions, validation, persistence (tools/*.tool.json), the approval policy and
// external hosts. Execution lives in CustomToolRun.cpp, the tool_* tools in CustomToolTools.cpp.

#include "skywalker/agent/CustomTools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <sstream>

#include "CustomToolsInternal.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/studio/Studio.h"
#include "skywalker/wander/Compiler.h"

namespace sky {

namespace fs = std::filesystem;
using namespace customtools;

namespace {

constexpr const char* kFormat = "skywalker.tool/1";
constexpr const char* kApprovalsFile = "tools/approvals.json";
constexpr size_t kMaxAuditBytes = 4 * 1024 * 1024;

/// Paths custom tools may never write (they hold the tools, their approvals and the policy, or
/// agent profiles and permissions).
constexpr const char* kProtectedWrites[] = {"tools/", "game.json", ".skywalker/", "agents/", "studio/"};

std::optional<std::string> readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Status writeAtomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return Error::make("io_error", "cannot write " + p.string());
        f << text;
        if (!f) return Error::make("io_error", "cannot write " + p.string());
    }
    fs::rename(tmp, p, ec);
    if (ec) return Error::make("io_error", "cannot write " + p.string() + ": " + ec.message());
    return {};
}

std::string shortName(const std::string& name) {
    size_t us = name.find('_');
    return us == std::string::npos ? name : name.substr(us + 1);
}

std::string titleFromName(const std::string& name) {
    std::string t = shortName(name);
    std::replace(t.begin(), t.end(), '_', ' ');
    if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

Error unknownKey(const std::string& where, const std::string& key, const std::vector<std::string>& known) {
    std::string guess = str::closest(key, known, 3);
    std::string all;
    for (const auto& k : known) all += (all.empty() ? "" : ", ") + k;
    return Error::make("invalid_definition", where + " has no field \"" + key + "\"",
                       guess.empty() ? "known fields: " + all : "did you mean \"" + guess + "\"?");
}

Status checkKeys(const Json& obj, const std::string& where, const std::vector<std::string>& known) {
    for (const auto& [k, v] : obj.members()) {
        if (std::find(known.begin(), known.end(), k) == known.end()) return unknownKey(where, k, known);
    }
    return {};
}

Result<std::vector<std::string>> stringList(const Json& j, const std::string& where) {
    std::vector<std::string> out;
    if (j.isNull()) return out;
    if (!j.isArray()) return Error::make("invalid_definition", where + " must be an array of strings");
    for (const auto& e : j.elements()) {
        if (!e.isString() || e.asString().empty()) return Error::make("invalid_definition", where + " must be an array of non-empty strings");
        out.push_back(e.asString());
    }
    return out;
}

/// A project-relative path pattern for capabilities.files: no absolute paths, no "..".
Status checkPathPattern(const std::string& p, bool write, const std::string& where) {
    if (p.empty() || p[0] == '/' || p[0] == '~' || p.find("..") != std::string::npos || p.find('\\') != std::string::npos) {
        return Error::make("invalid_definition", where + ": \"" + p + "\" must be a path inside the project (relative, no \"..\")",
                           "e.g. \"data/\" (a folder) or \"levels/*.json\"");
    }
    if (write) {
        for (const char* prot : kProtectedWrites) {
            std::string_view pv(prot);
            if (str::startsWith(p, pv) || str::startsWith(pv, p) || (p.find('*') != std::string::npos && str::globMatch(p, std::string(pv) + "x"))) {
                return Error::make("invalid_definition", where + ": tools may not write \"" + p + "\"",
                                   "tools/, game.json, .skywalker/, agents/ and studio/ hold tools, approvals, the policy and agent "
                                   "permissions; pick another folder such as \"exports/\"");
            }
        }
    }
    return {};
}

Result<ToolCapabilities> parseCaps(const Json& j, const std::string& kind) {
    ToolCapabilities c;
    if (kind == "external") c.readScene = false;
    if (j.isNull()) return c;
    if (!j.isObject()) return Error::make("invalid_definition", "capabilities must be an object");
    if (Status s = checkKeys(j, "capabilities", {"read_scene", "mutate", "calls", "files", "network"}); !s) return s.error();
    if (const Json* v = j.find("read_scene")) c.readScene = v->asBool(c.readScene);
    if (const Json* v = j.find("mutate")) c.mutate = v->asBool(false);
    if (const Json* v = j.find("network")) c.network = v->asBool(false);
    auto calls = stringList(j.get("calls"), "capabilities.calls");
    if (!calls) return calls.error();
    c.calls = std::move(calls.value());
    if (const Json* f = j.find("files")) {
        if (!f->isObject()) return Error::make("invalid_definition", "capabilities.files must be {\"read\": [...], \"write\": [...]}");
        if (Status s = checkKeys(*f, "capabilities.files", {"read", "write"}); !s) return s.error();
        auto r = stringList(f->get("read"), "capabilities.files.read");
        if (!r) return r.error();
        auto w = stringList(f->get("write"), "capabilities.files.write");
        if (!w) return w.error();
        c.readPaths = std::move(r.value());
        c.writePaths = std::move(w.value());
        for (const auto& p : c.readPaths) {
            if (Status s = checkPathPattern(p, false, "capabilities.files.read"); !s) return s.error();
        }
        for (const auto& p : c.writePaths) {
            if (Status s = checkPathPattern(p, true, "capabilities.files.write"); !s) return s.error();
        }
    }
    if (kind != "external" && c.network) {
        return Error::make("invalid_definition", "capabilities.network is only available to external tools",
                           "Wander and composite tools run inside the engine without network access; host the tool in a "
                           "connected client (skywalker/tools/register) to reach the network");
    }
    if (kind == "external" && (!c.readPaths.empty() || !c.writePaths.empty())) {
        return Error::make("invalid_definition", "capabilities.files does not apply to external tools",
                           "an external tool runs in its own process; declare the engine tools it calls back into in capabilities.calls");
    }
    return c;
}

Result<ToolLimits> parseLimits(const Json& j, const std::string& kind) {
    ToolLimits l;
    if (kind == "external") l.timeoutMs = 30'000;
    if (j.isNull()) return l;
    if (!j.isObject()) return Error::make("invalid_definition", "limits must be an object");
    if (Status s = checkKeys(j, "limits", {"instructions", "timeout_ms", "max_output_bytes", "max_calls"}); !s) return s.error();
    auto ranged = [&](const char* key, int64_t lo, int64_t hi, int64_t& out) -> Status {
        const Json* v = j.find(key);
        if (!v) return {};
        if (!v->isNumber() || v->asInt() < lo || v->asInt() > hi) {
            return Error::make("invalid_definition", std::string("limits.") + key + " must be a number from " + std::to_string(lo) +
                                                         " to " + std::to_string(hi));
        }
        out = v->asInt();
        return {};
    };
    int64_t timeout = l.timeoutMs, calls = l.maxCalls;
    if (Status s = ranged("instructions", 1000, CustomTools::Impl::kMaxInstructions, l.instructions); !s) return s.error();
    if (Status s = ranged("timeout_ms", 10, CustomTools::Impl::kMaxTimeoutMs, timeout); !s) return s.error();
    if (Status s = ranged("max_output_bytes", 256, CustomTools::Impl::kMaxOutputBytes, l.maxOutputBytes); !s) return s.error();
    if (Status s = ranged("max_calls", 0, CustomTools::Impl::kMaxCallsLimit, calls); !s) return s.error();
    l.timeoutMs = static_cast<int>(timeout);
    l.maxCalls = static_cast<int>(calls);
    return l;
}

const std::vector<std::string>& definitionKeys() {
    static const std::vector<std::string> keys = {
        "format", "name", "title", "description", "category", "kind", "input_schema", "output_schema", "capabilities",
        "limits", "wander", "source", "steps", "result", "tests", "enabled", "version", "author", "provenance", "persist"};
    return keys;
}

/// Steps referenced by {{steps.<id>...}} templates in a composite step's fields.
void collectStepRefs(const Json& j, std::set<std::string>& out) {
    if (j.isString()) {
        const std::string& s = j.asString();
        size_t at = 0;
        while ((at = s.find("{{", at)) != std::string::npos) {
            size_t end = s.find("}}", at);
            if (end == std::string::npos) break;
            std::string expr = str::trim(s.substr(at + 2, end - at - 2));
            if (str::startsWith(expr, "steps.")) {
                std::string rest = expr.substr(6);
                size_t stop = rest.find_first_of(".[ ?");
                out.insert(rest.substr(0, stop));
            }
            at = end + 2;
        }
    } else if (j.isArray()) {
        for (const auto& e : j.elements()) collectStepRefs(e, out);
    } else if (j.isObject()) {
        for (const auto& [k, v] : j.members()) collectStepRefs(v, out);
    }
}

bool hasTemplate(const Json& j) {
    if (j.isString()) return j.asString().find("{{") != std::string::npos;
    if (j.isArray()) {
        for (const auto& e : j.elements()) {
            if (hasTemplate(e)) return true;
        }
    }
    if (j.isObject()) {
        for (const auto& [k, v] : j.members()) {
            if (hasTemplate(v)) return true;
        }
    }
    return false;
}

int strictness(ToolPolicy p) {
    switch (p) {
        case ToolPolicy::Off: return 3;
        case ToolPolicy::Ask: return 2;
        case ToolPolicy::Auto: return 1;
        case ToolPolicy::Trust: return 0;
    }
    return 3;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace customtools {

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Result<std::string> normalizeName(std::string_view raw, std::string_view ns) {
    std::string name = str::trim(raw);
    for (char& c : name) {
        if (c == '.' || c == ':' || c == '-' || c == ' ' || c == '/') c = '_';
    }
    if (name.empty()) return Error::make("invalid_definition", "the tool needs a name", "e.g. \"enemies_near\" (becomes user_enemies_near)");
    std::string prefix = std::string(ns) + "_";
    if (!str::startsWith(name, prefix)) name = prefix + name;
    bool ok = std::islower(static_cast<unsigned char>(name[0])) && name.size() <= 64;
    for (char c : name) ok = ok && (std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_');
    if (!ok || name.size() == prefix.size()) {
        return Error::make("invalid_definition", "invalid tool name \"" + std::string(raw) + "\"",
                           "use lowercase letters, digits and underscores, at most 64 characters, e.g. " + prefix + "enemies_near "
                           "(model APIs reject '.' and ':' in tool names)");
    }
    return name;
}

Status checkSchema(const Json& schema, const std::string& where) {
    static const std::vector<std::string> types = {"object", "array", "string", "number", "integer", "boolean", "null"};
    if (!schema.isObject()) return Error::make("invalid_schema", where + " must be a JSON schema object");
    auto checkType = [&](const Json& t) -> Status {
        if (t.isString()) {
            if (std::find(types.begin(), types.end(), t.asString()) == types.end()) {
                std::string guess = str::closest(t.asString(), types, 3);
                return Error::make("invalid_schema", where + ".type \"" + t.asString() + "\" is not a JSON schema type",
                                   guess.empty() ? "use object, array, string, number, integer, boolean or null" : "did you mean \"" + guess + "\"?");
            }
            return {};
        }
        if (t.isArray()) {
            for (const auto& e : t.elements()) {
                if (!e.isString() || std::find(types.begin(), types.end(), e.asString()) == types.end()) {
                    return Error::make("invalid_schema", where + ".type must list JSON schema types");
                }
            }
            return {};
        }
        return Error::make("invalid_schema", where + ".type must be a string or an array of strings");
    };
    if (const Json* t = schema.find("type"); t) {
        if (Status s = checkType(*t); !s) return s;
    }
    if (const Json* e = schema.find("enum"); e && (!e->isArray() || e->size() == 0)) {
        return Error::make("invalid_schema", where + ".enum must be a non-empty array");
    }
    if (const Json* props = schema.find("properties")) {
        if (!props->isObject()) return Error::make("invalid_schema", where + ".properties must be an object");
        for (const auto& [k, v] : props->members()) {
            if (Status s = checkSchema(v, where + ".properties." + k); !s) return s;
        }
    }
    if (const Json* req = schema.find("required")) {
        if (!req->isArray()) return Error::make("invalid_schema", where + ".required must be an array of property names");
        for (const auto& r : req->elements()) {
            if (!r.isString() || !schema.get("properties").contains(r.asString())) {
                return Error::make("invalid_schema", where + ".required lists \"" + r.asString() + "\", which is not in properties");
            }
        }
    }
    if (const Json* items = schema.find("items")) {
        if (Status s = checkSchema(*items, where + ".items"); !s) return s;
    }
    return {};
}

Json withDefaults(const Json& schema, const Json& args) {
    Json out = args.isObject() ? args : Json::object();
    for (const auto& [k, prop] : schema.get("properties").members()) {
        if (!out.contains(k) && prop.contains("default")) out[k] = prop.get("default");
    }
    return out;
}

bool allows(const std::vector<std::string>& patterns, std::string_view name) {
    for (const auto& p : patterns) {
        if (p == name || (p.find_first_of("*?") != std::string::npos && str::globMatch(p, name))) return true;
    }
    return false;
}

}  // namespace customtools

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

const char* toString(ToolPolicy p) {
    switch (p) {
        case ToolPolicy::Off: return "off";
        case ToolPolicy::Ask: return "ask";
        case ToolPolicy::Auto: return "auto";
        case ToolPolicy::Trust: return "trust";
    }
    return "auto";
}

std::optional<ToolPolicy> parseToolPolicy(std::string_view s) {
    if (s == "off") return ToolPolicy::Off;
    if (s == "ask") return ToolPolicy::Ask;
    if (s == "auto") return ToolPolicy::Auto;
    if (s == "trust") return ToolPolicy::Trust;
    return std::nullopt;
}

Json ToolCapabilities::toJson() const {
    Json c = Json::object({{"read_scene", readScene}, {"mutate", mutate}});
    Json callsJ = Json::array();
    for (const auto& n : calls) callsJ.push(n);
    c["calls"] = callsJ;
    if (!readPaths.empty() || !writePaths.empty()) {
        Json r = Json::array(), w = Json::array();
        for (const auto& p : readPaths) r.push(p);
        for (const auto& p : writePaths) w.push(p);
        c["files"] = Json::object({{"read", r}, {"write", w}});
    }
    c["network"] = network;
    return c;
}

Json ToolLimits::toJson() const {
    return Json::object({{"instructions", instructions},
                         {"timeout_ms", timeoutMs},
                         {"max_output_bytes", maxOutputBytes},
                         {"max_calls", maxCalls}});
}

Json CustomToolDef::toJson() const {
    Json j = Json::object({{"format", kFormat},
                           {"name", name},
                           {"title", title},
                           {"description", description},
                           {"category", category},
                           {"kind", kind},
                           {"input_schema", inputSchema}});
    if (!outputSchema.isNull()) j["output_schema"] = outputSchema;
    j["capabilities"] = caps.toJson();
    j["limits"] = limits.toJson();
    if (kind == "wander") {
        if (sourceFile.empty()) {
            j["wander"] = source;
        } else {
            j["source"] = sourceFile;
        }
    }
    if (kind == "composite") {
        j["steps"] = steps;
        if (!result.isNull()) j["result"] = result;
    }
    if (kind == "external") j["persist"] = persist;
    j["tests"] = tests.isArray() ? tests : Json::array();
    j["enabled"] = enabled;
    j["version"] = version;
    j["author"] = author;
    j["provenance"] = provenance;
    return j;
}

bool CustomToolDef::privileged() const { return caps.mutate || !caps.writePaths.empty() || caps.network; }

std::string CustomToolDef::hash() const {
    Json core = Json::object({{"name", name},
                              {"kind", kind},
                              {"description", description},
                              {"category", category},
                              {"input_schema", inputSchema},
                              {"output_schema", outputSchema},
                              {"capabilities", caps.toJson()},
                              {"limits", limits.toJson()},
                              {"source", source},
                              {"steps", steps},
                              {"result", result}});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(wander::fnv1a(core.dump())));
    return buf;
}

// ---------------------------------------------------------------------------
// Impl: definitions and persistence
// ---------------------------------------------------------------------------

CustomTools::Impl::Impl(Engine& e) : engine(e) {}

std::string CustomTools::Impl::projectDir() const { return engine.config().projectDir; }

std::shared_ptr<ToolEntry> CustomTools::Impl::find(const std::string& name) const {
    auto it = entries.find(name);
    if (it != entries.end()) return it->second;
    // Accept the forms agents tend to write: "enemies_near", "user.enemies_near".
    if (auto n = normalizeName(name, "user")) {
        it = entries.find(*n);
        if (it != entries.end()) return it->second;
    }
    return nullptr;
}

Error CustomTools::Impl::unknownTool(const std::string& name) const {
    std::vector<std::string> names;
    for (const auto& [n, e] : entries) names.push_back(n);
    std::string guess = str::closest(name, names, 4);
    return Error::make("not_found", "no custom tool named '" + name + "'",
                       guess.empty() ? "tool_list_custom lists them" : "did you mean '" + guess + "'?");
}

ToolPolicy CustomTools::Impl::policy() const {
    fs::path file = fs::path(projectDir()) / "game.json";
    std::error_code ec;
    auto t = fs::last_write_time(file, ec);
    int64_t mtime = ec ? -1 : static_cast<int64_t>(t.time_since_epoch().count());
    if (mtime == gameJsonTime) return cachedPolicy;
    gameJsonTime = mtime;
    cachedPolicy = ToolPolicy::Auto;
    if (auto text = readText(file)) {
        if (auto doc = Json::parse(*text)) {
            const std::string& p = doc->get("customTools").get("policy").asString();
            if (auto parsed = parseToolPolicy(p)) {
                cachedPolicy = *parsed;
            } else if (!p.empty()) {
                log::warn("tools", "game.json customTools.policy \"" + p + "\" is unknown; using \"auto\"");
            }
        }
    }
    return cachedPolicy;
}

bool CustomTools::Impl::needsApproval(const CustomToolDef& def) const {
    switch (policy()) {
        case ToolPolicy::Trust: return false;
        case ToolPolicy::Auto: return def.privileged();
        case ToolPolicy::Ask:
        case ToolPolicy::Off: return true;
    }
    return true;
}

bool CustomTools::Impl::approved(const ToolEntry& entry) const {
    const Json& a = approvals.get(entry.def.name);
    return a.get("hash").asString() == entry.hash && a.get("verdict").asString() == "approved";
}

Result<std::shared_ptr<const wander::Program>> CustomTools::Impl::compile(const CustomToolDef& def, std::vector<std::string>& warnings) {
    wander::CompileOptions options = engine.runtime().compileOptions();
    options.registry = builtins.get();
    wander::CompileResult r = wander::compile(def.source, options);
    if (!r.ok()) {
        std::string first;
        std::string hint;
        for (const auto& d : r.diagnostics) {
            if (d.severity != wander::Severity::Error) continue;
            first = "line " + std::to_string(d.loc.line) + ": " + d.message;
            hint = d.hint;
            break;
        }
        return Error::make("compile_error", "the Wander code does not compile: " + first,
                           hint.empty() ? "check it with wander_check; the tool body is a file of fn/const/use declarations with "
                                          "`fn run(args) ... end`"
                                        : hint);
    }
    for (const auto& d : r.diagnostics) {
        if (d.severity == wander::Severity::Warning) warnings.push_back("line " + std::to_string(d.loc.line) + ": " + d.message);
    }
    const wander::FnInfo* run = nullptr;
    bool behaviorRun = false;
    for (const auto& f : r.program->functions) {
        if (f.name != "run") continue;
        if (f.behavior < 0) {
            run = &f;
        } else {
            behaviorRun = true;
        }
    }
    if (!run) {
        return Error::make("invalid_definition",
                           behaviorRun ? "fn run is inside a behavior; a tool's code holds only fn, const and use declarations"
                                       : "the Wander code has no `fn run(args)`",
                           "write `fn run(args) ... return {…} end` at the top level; `args` is a map of the tool's arguments");
    }
    if (run->params.size() > 1) {
        return Error::make("invalid_definition", "fn run takes " + std::to_string(run->params.size()) + " parameters; it must take one (args) or none");
    }
    if (!r.program->behaviors.empty()) warnings.push_back("behaviors in a tool's code never run; only fn run(args) does");
    if (!r.program->fileTests.empty()) warnings.push_back("Wander test blocks are not run for tools; put tests in the definition's \"tests\"");
    return r.program;
}

bool CustomTools::Impl::callsSceneBuiltins(const wander::Program& program) {
    static const std::set<std::string> safeCategories = {"math", "vector", "color", "random", "text", "string", "list", "map", "tools", "input"};
    static const std::set<std::string> safeNames = {"find", "find_all", "nearest", "count", "tagged", "exists", "children", "has",
                                                    "__log", "__emit", "__emit_to"};
    auto safe = [&](const wander::BuiltinDef& d) {
        return d.pure || safeCategories.count(d.category) || safeNames.count(d.name);
    };
    for (const auto& b : program.builtins) {
        if (b && !safe(*b)) return true;
    }
    for (const auto& perReceiver : program.methods) {
        for (const auto& m : perReceiver) {
            if (m && !safe(*m)) return true;
        }
    }
    return false;
}

Status CustomTools::Impl::validate(CustomToolDef& def, std::vector<std::string>& warnings, std::shared_ptr<const wander::Program>* program) {
    if (def.kind != "external" && def.description.size() < 20) {
        return Error::make("invalid_definition", "the description is too short",
                           "write it for language models: what the tool does, when to use it, key arguments and an example call");
    }
    if (def.title.empty()) def.title = titleFromName(def.name);
    bool categoryOk = !def.category.empty() && def.category.size() <= 32;
    for (char c : def.category) categoryOk = categoryOk && (std::islower(static_cast<unsigned char>(c)) || c == '_');
    if (!categoryOk) return Error::make("invalid_definition", "category must be a lowercase word, e.g. \"scene\" or \"custom\"");
    if (def.inputSchema.isNull()) def.inputSchema = Json::object({{"type", "object"}, {"properties", Json::object()}});
    if (Status s = checkSchema(def.inputSchema, "input_schema"); !s) return s;
    if (def.inputSchema.get("type").asString() != "object") {
        return Error::make("invalid_schema", "input_schema must have \"type\": \"object\" (tool arguments are a JSON object)");
    }
    if (!def.inputSchema.contains("properties")) def.inputSchema["properties"] = Json::object();
    // Closed schemas give agents did-you-mean errors for misspelled arguments (hosts validate their own).
    if (def.kind != "external" && !def.inputSchema.contains("additionalProperties")) def.inputSchema["additionalProperties"] = false;
    if (!def.outputSchema.isNull()) {
        if (Status s = checkSchema(def.outputSchema, "output_schema"); !s) return s;
    }
    if (const ToolDef* builtin = engine.tools().find(def.name); builtin && builtin->origin.empty()) {
        return Error::make("name_taken", def.name + " is one of the engine's own tools", "pick another name");
    }
    // Calls: every named tool must exist; what they do must fit the declared capabilities.
    for (const auto& pattern : def.caps.calls) {
        bool glob = pattern.find_first_of("*?") != std::string::npos;
        std::vector<const ToolDef*> matched;
        for (const auto& n : engine.tools().names()) {
            if (n == pattern || (glob && str::globMatch(pattern, n))) {
                if (const ToolDef* t = engine.tools().find(n)) matched.push_back(t);
            }
        }
        bool customTarget = !glob && entries.count(pattern) && pattern != def.name;
        if (matched.empty() && !customTarget) {
            if (glob) {
                warnings.push_back("capabilities.calls: \"" + pattern + "\" matches no tool");
                continue;
            }
            std::string guess = str::closest(pattern, engine.tools().names(), 4);
            return Error::make("invalid_definition", "capabilities.calls lists \"" + pattern + "\", which is not a tool",
                               guess.empty() ? "tools/list shows every tool" : "did you mean \"" + guess + "\"?");
        }
        for (const auto& t : matched) {
            if (t->name.rfind("tool_", 0) == 0 && t->origin.empty()) {
                return Error::make("invalid_definition", "custom tools may not call the tool_* lifecycle tools (" + t->name + ")",
                                   "tools cannot define, approve or remove tools");
            }
            if (t->mutates && !def.caps.mutate && !glob) {
                return Error::make("invalid_definition",
                                   "capabilities.calls lists " + t->name + ", which modifies the project; declare \"mutate\": true",
                                   "mutating tools need a human's approval under the default policy");
            }
            if (t->openWorld && !def.caps.network && !glob) {
                return Error::make("invalid_definition", "capabilities.calls lists " + t->name + ", which reaches outside the project",
                                   def.kind == "external" ? "declare \"network\": true" : "only external tools may reach the network");
            }
        }
        if (glob) {
            bool anyMutating = std::any_of(matched.begin(), matched.end(), [](const auto& t) { return t->mutates; });
            if (anyMutating && !def.caps.mutate) {
                warnings.push_back("capabilities.calls: \"" + pattern + "\" includes tools that modify the project; without "
                                   "\"mutate\": true those calls are refused");
            }
        }
    }
    if (def.kind == "wander") {
        if (def.source.empty()) {
            return Error::make("invalid_definition", "a wander tool needs its code in \"wander\"",
                               "e.g. \"fn run(args)\\n  return {count: count(\\\"enemy\\\")}\\nend\"");
        }
        auto program2 = compile(def, warnings);
        if (!program2) return program2.error();
        if (program) *program = program2.value();
    } else if (def.kind == "composite") {
        if (!def.steps.isArray() || def.steps.size() == 0 || def.steps.size() > 64) {
            return Error::make("invalid_definition", "a composite tool needs \"steps\": 1 to 64 tool calls",
                               "e.g. [{\"id\":\"find\",\"tool\":\"scene_query\",\"args\":{\"tag\":\"{{args.tag}}\"}}]");
        }
        std::vector<std::string> ids;
        bool autoCalls = def.caps.calls.empty();
        for (size_t i = 0; i < def.steps.size(); ++i) {
            const Json& step = def.steps[i];
            std::string where = "steps[" + std::to_string(i) + "]";
            if (!step.isObject()) return Error::make("invalid_definition", where + " must be an object {tool, args}");
            if (Status s = checkKeys(step, where, {"id", "tool", "args", "for_each", "as", "when", "continue_on_error", "note"}); !s) return s;
            const std::string& tool = step.get("tool").asString();
            if (tool.empty()) return Error::make("invalid_definition", where + " needs \"tool\"");
            if (step.contains("args") && !step.get("args").isObject()) return Error::make("invalid_definition", where + ".args must be an object");
            const ToolDef* target = engine.tools().find(tool);
            bool customTarget = entries.count(tool) && tool != def.name;
            if (!target && !customTarget) {
                std::string guess = str::closest(tool, engine.tools().names(), 4);
                return Error::make("invalid_definition", where + " calls \"" + tool + "\", which is not a tool",
                                   guess.empty() ? "tools/list shows every tool" : "did you mean \"" + guess + "\"?");
            }
            if (tool == def.name) return Error::make("invalid_definition", where + " calls the tool itself");
            if (target && target->origin.empty() && tool.rfind("tool_", 0) == 0) {
                return Error::make("invalid_definition", "custom tools may not call the tool_* lifecycle tools (" + tool + ")");
            }
            if (target && target->mutates && !def.caps.mutate) {
                return Error::make("invalid_definition", where + " calls " + tool + ", which modifies the project; declare capabilities.mutate: true",
                                   "mutating tools need a human's approval under the default policy");
            }
            if (target && target->openWorld) {
                return Error::make("invalid_definition", where + " calls " + tool + ", which reaches outside the project",
                                   "only external tools may reach the network");
            }
            if (autoCalls) {
                if (!allows(def.caps.calls, tool)) def.caps.calls.push_back(tool);
            } else if (!allows(def.caps.calls, tool)) {
                return Error::make("invalid_definition", where + " calls " + tool + ", which capabilities.calls does not allow",
                                   "add it to capabilities.calls (or leave calls empty to derive it from the steps)");
            }
            std::set<std::string> refs;
            collectStepRefs(step, refs);
            for (const auto& r : refs) {
                if (std::find(ids.begin(), ids.end(), r) == ids.end()) {
                    std::string guess = str::closest(r, ids, 3);
                    return Error::make("invalid_definition", where + " uses {{steps." + r + "}}, which is not an earlier step's id",
                                       guess.empty() ? "give the earlier step an \"id\"" : "did you mean \"" + guess + "\"?");
                }
            }
            if (target && step.contains("args") && !hasTemplate(step.get("args")) && !step.contains("for_each")) {
                if (Status s = validateSchema(target->inputSchema, step.get("args"), where + ".args"); !s) return s;
            }
            std::string id = step.get("id").asString();
            if (!id.empty()) {
                if (std::find(ids.begin(), ids.end(), id) != ids.end()) return Error::make("invalid_definition", where + ".id \"" + id + "\" is used twice");
                ids.push_back(id);
            }
        }
        if (autoCalls) warnings.push_back("capabilities.calls was derived from the steps");
    } else if (def.kind != "external") {
        return Error::make("invalid_definition", "unknown kind \"" + def.kind + "\"", "wander, composite (or external, registered by a connected client)");
    }
    if (!def.tests.isArray()) return Error::make("invalid_definition", "tests must be an array");
    for (size_t i = 0; i < def.tests.size(); ++i) {
        const Json& t = def.tests[i];
        std::string where = "tests[" + std::to_string(i) + "]";
        if (!t.isObject()) return Error::make("invalid_definition", where + " must be an object {name, args, expect}");
        if (Status s = checkKeys(t, where, {"name", "args", "expect", "setup"}); !s) return s;
        if (t.contains("args") && !t.get("args").isObject()) return Error::make("invalid_definition", where + ".args must be an object");
        if (t.contains("setup") && !t.get("setup").isArray()) return Error::make("invalid_definition", where + ".setup must be an array of {tool, args}");
        if (t.contains("expect")) {
            if (!t.get("expect").isObject()) return Error::make("invalid_definition", where + ".expect must be an object");
            if (Status s = checkKeys(t.get("expect"), where + ".expect", {"ok", "error", "result", "contains", "max_ms"}); !s) return s;
        }
        if (t.contains("args")) {
            if (Status s = validateSchema(def.inputSchema, withDefaults(def.inputSchema, t.get("args")), where + ".args"); !s) return s;
        }
    }
    return {};
}

Result<CustomToolDef> CustomTools::Impl::parse(const Json& specIn, const std::string& actor, const CustomToolDef* previous,
                                              std::vector<std::string>& warnings) {
    if (!specIn.isObject()) return Error::make("invalid_definition", "a tool definition is a JSON object");
    Json spec = specIn;
    if (previous) {
        Json base = previous->toJson();
        base.erase("source");
        base["wander"] = previous->source;
        if (spec.contains("wander") || spec.contains("source")) base.erase("wander");
        base.mergePatch(spec);
        spec = std::move(base);
    }
    if (Status s = checkKeys(spec, "the definition", definitionKeys()); !s) return s.error();
    CustomToolDef def;
    std::string ns = spec.get("kind").asString() == "external" ? "" : "user";
    if (ns.empty()) {
        def.name = spec.get("name").asString();  // already normalized by hostRequest
    } else {
        auto name = normalizeName(spec.get("name").asString(), ns);
        if (!name) return name.error();
        def.name = *name;
    }
    def.title = spec.get("title").asString();
    def.description = spec.get("description").asString();
    def.category = spec.get("category").asString("custom");
    def.kind = spec.get("kind").asString();
    if (def.kind.empty()) def.kind = spec.contains("steps") ? "composite" : "wander";
    def.inputSchema = spec.get("input_schema");
    def.outputSchema = spec.get("output_schema");
    auto caps = parseCaps(spec.get("capabilities"), def.kind);
    if (!caps) return caps.error();
    def.caps = std::move(caps.value());
    auto limits = parseLimits(spec.get("limits"), def.kind);
    if (!limits) return limits.error();
    def.limits = limits.value();
    if (def.kind == "wander") {
        if (const Json* w = spec.find("wander"); w && w->isString()) {
            def.source = w->asString();
        } else if (const Json* src = spec.find("source"); src && src->isString()) {
            def.sourceFile = src->asString();
            if (Status s = checkPathPattern(def.sourceFile, false, "source"); !s) return s.error();
            auto text = readText(fs::path(projectDir()) / def.sourceFile);
            if (!text) return Error::make("not_found", "source file " + def.sourceFile + " does not exist");
            def.source = *text;
        }
    }
    def.steps = spec.get("steps");
    def.result = spec.get("result");
    def.tests = spec.get("tests").isNull() ? Json::array() : spec.get("tests");
    def.enabled = spec.get("enabled").asBool(true);
    def.persist = spec.get("persist").asBool(false);
    def.version = static_cast<int>(spec.get("version").asInt(1));
    def.author = spec.get("author").asString();
    if (def.author.empty()) def.author = actor;
    def.provenance = spec.get("provenance").isObject() ? spec.get("provenance") : Json::object();
    if (Status s = validate(def, warnings, nullptr); !s) return s.error();
    return def;
}

Status CustomTools::Impl::save(ToolEntry& entry) {
    CustomToolDef& def = entry.def;
    hasToolFiles = true;
    fs::path root = projectDir();
    if (entry.file.empty()) entry.file = CustomTools::fileFor(def.name);
    if (def.kind == "wander") {
        // The code lives next to the definition, where people (and diffs) can read it.
        if (def.sourceFile.empty()) def.sourceFile = "tools/" + shortName(def.name) + ".wander";
        if (Status s = writeAtomic(root / def.sourceFile, def.source); !s) return s;
    }
    return writeAtomic(root / entry.file, def.toJson().dump(2) + "\n");
}

void CustomTools::Impl::loadApprovals() {
    approvals = Json::object();
    if (auto text = readText(fs::path(projectDir()) / kApprovalsFile)) {
        if (auto doc = Json::parse(*text); doc && doc->get("approvals").isObject()) approvals = doc->get("approvals");
    }
}

Status CustomTools::Impl::saveApprovals() {
    Json doc = Json::object({{"format", "skywalker.tool_approvals/1"}, {"approvals", approvals}});
    return writeAtomic(fs::path(projectDir()) / kApprovalsFile, doc.dump(2) + "\n");
}

Result<std::shared_ptr<ToolEntry>> CustomTools::Impl::loadFile(const std::string& rel) {
    auto text = readText(fs::path(projectDir()) / rel);
    auto entry = std::make_shared<ToolEntry>();
    entry->file = rel;
    std::string stem = fs::path(rel).filename().string();
    stem = stem.substr(0, stem.size() - std::string(".tool.json").size());
    entry->def.name = "user_" + stem;
    if (!text) return Error::make("io_error", "cannot read " + rel);
    auto doc = Json::parse(*text);
    if (!doc) {
        entry->loadError = rel + " is not valid JSON: " + doc.error().message;
        return entry;
    }
    if (doc->get("name").isString()) entry->def.name = doc->get("name").asString();
    std::vector<std::string> warnings;
    auto def = parse(doc.value(), doc->get("author").asString("project"), nullptr, warnings);
    if (!def) {
        entry->loadError = def.error().message + (def.error().hint.empty() ? "" : " (hint: " + def.error().hint + ")");
        return entry;
    }
    entry->def = std::move(def.value());
    entry->warnings = std::move(warnings);
    if (entry->def.kind == "wander") {
        std::vector<std::string> ignored;
        if (auto program = compile(entry->def, ignored)) entry->program = program.value();
    }
    entry->hash = entry->def.hash();
    return entry;
}

void CustomTools::Impl::loadAll() {
    loadApprovals();
    fs::path dir = fs::path(projectDir()) / "tools";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<std::string> files;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        std::string name = f.path().filename().string();
        if (name.size() > 10 && name.substr(name.size() - 10) == ".tool.json" && name.find(" 2.") == std::string::npos) {
            files.push_back("tools/" + name);
        }
    }
    std::sort(files.begin(), files.end());
    hasToolFiles = hasToolFiles || !files.empty();
    // Tools may call each other (a composite step, capabilities.calls): files that refer to a tool
    // defined in a later file are retried once it is loaded.
    std::vector<std::pair<std::string, std::string>> failed;
    for (int pass = 0; pass < 8 && !files.empty(); ++pass) {
        std::vector<std::string> retry;
        failed.clear();
        for (const auto& rel : files) {
            auto entry = loadFile(rel);
            if (!entry) continue;
            auto e = entry.value();
            if (e->def.kind == "external") e->def.persist = true;  // waits (offline) for its client to register it again
            entries[e->def.name] = e;
            refresh(*e);
            if (!e->loadError.empty()) {
                retry.push_back(rel);
                failed.emplace_back(rel, e->loadError);
            }
        }
        if (retry.size() == files.size()) break;  // no progress
        files = std::move(retry);
    }
    for (const auto& [rel, error] : failed) log::warn("tools", "custom tool " + rel + ": " + error);
}

void CustomTools::Impl::refresh(ToolEntry& entry) {
    std::string before = entry.status;
    const CustomToolDef& def = entry.def;
    entry.reason.clear();
    if (!entry.loadError.empty()) {
        entry.status = "invalid";
        entry.reason = entry.loadError;
    } else if (policy() == ToolPolicy::Off) {
        entry.status = "policy_off";
        entry.reason = "custom tools are turned off for this project (game.json customTools.policy = off)";
    } else if (!def.enabled) {
        entry.status = "disabled";
        entry.reason = "disabled (tool_enable turns it back on)";
    } else if (def.kind == "external" && !(entry.host && entry.host->connected())) {
        entry.status = "offline";
        entry.reason = "the client hosting it is not connected";
    } else if (def.kind == "wander" && !entry.program) {
        entry.status = "invalid";
        entry.reason = "the Wander code does not compile";
    } else if (needsApproval(def) && !approved(entry)) {
        const Json& a = approvals.get(def.name);
        bool rejected = a.get("hash").asString() == entry.hash && a.get("verdict").asString() == "rejected";
        entry.status = rejected ? "rejected" : "pending_approval";
        entry.reason = rejected ? "a human rejected this definition" + (a.get("note").asString().empty() ? std::string() : ": " + a.get("note").asString())
                                : std::string("needs a human's approval (") + (def.privileged() ? "it can modify the project or reach the network" : "the project policy is \"ask\"") + ")";
    } else {
        entry.status = "active";
    }
    if (entry.status == "active") {
        publish(entry);
    } else {
        unpublish(def.name);
    }
    if (before != entry.status && !before.empty()) {
        engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", "status"}, {"tool", def.name}, {"status", entry.status}}));
    }
}

void CustomTools::Impl::publish(const ToolEntry& entry) {
    const CustomToolDef& def = entry.def;
    ToolDef t;
    t.name = def.name;
    t.title = def.title;
    t.description = def.description;
    t.category = def.category;
    t.inputSchema = def.inputSchema;
    t.mutates = def.caps.mutate || !def.caps.writePaths.empty();
    t.destructive = false;
    t.openWorld = def.caps.network;
    t.origin = def.kind == "external" ? "external" : "custom";
    t.meta = Json::object({{"skywalker/kind", def.kind}, {"skywalker/version", def.version}, {"skywalker/author", def.author}});
    std::string name = def.name;
    t.handler = [this, name](const Json& args, ToolContext& ctx) { return run(name, args, ctx); };
    if (Status s = engine.tools().addDynamic(std::move(t)); !s) {
        log::warn("tools", "cannot register " + def.name + ": " + s.error().message);
    }
}

void CustomTools::Impl::unpublish(const std::string& name) {
    const ToolDef* t = engine.tools().find(name);
    if (t && !t->origin.empty()) engine.tools().removeDynamic(name);
}

void CustomTools::Impl::audit(Json event) {
    event["at"] = nowIso();
    std::error_code ec;
    fs::path root = projectDir();
    // Only real projects get a log (an engine started in some folder for a quick call does not).
    if (!hasToolFiles && !fs::exists(root / "game.json", ec) && !fs::is_directory(root / ".skywalker", ec)) return;
    fs::path dir = root / ".skywalker" / "logs";
    fs::create_directories(dir, ec);
    fs::path file = dir / "custom_tools.jsonl";
    if (fs::file_size(file, ec) > kMaxAuditBytes && !ec) {
        fs::rename(file, dir / "custom_tools.1.jsonl", ec);  // keep one previous file
    }
    std::ofstream f(file, std::ios::app | std::ios::binary);
    if (f) f << event.dump() << "\n";
}

// ---------------------------------------------------------------------------
// Stats and summaries
// ---------------------------------------------------------------------------

void ToolStats::record(const std::string& actor, bool ok, double ms, const std::string& error, const Json& nested) {
    ++calls;
    if (!ok) ++failures;
    totalMs += ms;
    lastMs = ms;
    maxMs = std::max(maxMs, ms);
    lastActor = actor;
    lastAt = nowIso();
    Json call = Json::object({{"at", lastAt}, {"actor", actor}, {"ok", ok}, {"ms", ms}});
    if (nested.isArray() && nested.size()) call["calls"] = nested;
    if (!ok) {
        call["error"] = error;
        recentErrors.push_back(Json::object({{"at", lastAt}, {"actor", actor}, {"error", error}}));
        while (recentErrors.size() > 5) recentErrors.pop_front();
    }
    recentCalls.push_back(std::move(call));
    while (recentCalls.size() > 10) recentCalls.pop_front();
}

Json ToolStats::toJson() const {
    Json errors = Json::array(), recent = Json::array();
    for (const auto& e : recentErrors) errors.push(e);
    for (const auto& c : recentCalls) recent.push(c);
    return Json::object({{"calls", calls},
                         {"failures", failures},
                         {"avg_ms", calls ? totalMs / static_cast<double>(calls) : 0.0},
                         {"last_ms", lastMs},
                         {"max_ms", maxMs},
                         {"last_actor", lastActor},
                         {"last_at", lastAt},
                         {"recent_errors", errors},
                         {"recent_calls", recent}});
}

Json ToolEntry::summary() const {
    Json j = Json::object({{"name", def.name},
                           {"title", def.title},
                           {"kind", def.kind},
                           {"status", status},
                           {"category", def.category},
                           {"version", def.version},
                           {"author", def.author},
                           {"mutates", def.caps.mutate || !def.caps.writePaths.empty()},
                           {"privileged", def.privileged()},
                           {"calls", stats.calls},
                           {"failures", stats.failures}});
    if (!reason.empty()) j["reason"] = reason;
    if (!file.empty()) j["file"] = file;
    if (host) j["host"] = host->actor();
    std::string d = def.description.substr(0, 140);
    j["description"] = d + (def.description.size() > 140 ? "…" : "");
    return j;
}

// ---------------------------------------------------------------------------
// CustomTools (public API)
// ---------------------------------------------------------------------------

CustomTools::CustomTools(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {
    impl_->sandbox = std::make_unique<Scene>();
    impl_->registerBuiltins();
    engine.tools().setGate([impl = impl_.get()](const ToolDef& tool, const Json& args, ToolContext& ctx) { return impl->gate(tool, args, ctx); });
    impl_->loadAll();
    engine.toolHost().setPublisher(hostedPublisher());  // py_* tools from tool_host_register take the same path
}

CustomTools::~CustomTools() {
    impl_->engine.tools().setGate(nullptr);
    impl_->engine.toolHost().setPublisher({});  // the tool host may outlive this manager
}

bool CustomTools::isHuman(std::string_view actor) { return actor == "user" || actor == "cli"; }

std::string CustomTools::fileFor(const std::string& name) { return "tools/" + shortName(name) + ".tool.json"; }

std::string CustomTools::libraryDir() {
    if (const char* env = std::getenv("SKYWALKER_TOOL_LIBRARY"); env && *env) return env;
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : "/tmp") / ".skywalker" / "tools").string();
}

ToolPolicy CustomTools::policy() const { return impl_->policy(); }

Result<Json> CustomTools::define(const Json& specIn, const std::string& actor) {
    Impl& I = *impl_;
    if (I.policy() == ToolPolicy::Off) {
        return Error::make("policy_off", "custom tools are turned off for this project",
                           "a human can enable them: game.json \"customTools\": {\"policy\": \"auto\"} (or tool_policy)");
    }
    Json spec = specIn;
    bool runTests = spec.get("run_tests").asBool(true);
    bool validateOnly = spec.get("validate_only").asBool(false);
    spec.erase("run_tests");
    spec.erase("validate_only");
    if (const Json* lib = spec.find("from_library")) {
        auto name = normalizeName(lib->asString(), "user");
        if (!name) return name.error();
        fs::path file = fs::path(libraryDir()) / (shortName(*name) + ".tool.json");
        auto text = readText(file);
        if (!text) return Error::make("not_found", "the tool library has no " + *name, "tool_list_custom {include_library: true} lists it");
        auto doc = Json::parse(*text);
        if (!doc) return Error::make("invalid_definition", file.string() + ": " + doc.error().message);
        Json base = doc.value();
        base.erase("provenance");
        base.erase("version");
        spec.erase("from_library");
        base.mergePatch(spec);
        spec = std::move(base);
        spec["provenance"] = Json::object({{"from_library", file.string()}});
    }
    if (spec.get("kind").asString() == "external") {
        return Error::make("invalid_definition", "external tools are registered by the client that hosts them",
                           "send skywalker/tools/register on its MCP connection (docs/CUSTOM_TOOLS.md, \"External tools\")");
    }
    auto name = normalizeName(spec.get("name").asString(), "user");
    if (!name) return name.error();
    spec["name"] = *name;
    std::shared_ptr<ToolEntry> previous = I.find(*name);
    if (previous && previous->def.kind == "external") {
        return Error::make("name_taken", *name + " is an external tool hosted by a client", "pick another name");
    }
    std::vector<std::string> warnings;
    auto def = I.parse(spec, actor, previous && previous->loadError.empty() ? &previous->def : nullptr, warnings);
    if (!def) return def.error();
    auto entry = std::make_shared<ToolEntry>();
    entry->def = std::move(def.value());
    entry->warnings = warnings;
    if (entry->def.kind == "wander") {
        std::vector<std::string> ignored;
        auto program = I.compile(entry->def, ignored);
        if (!program) return program.error();
        entry->program = program.value();
    }
    entry->hash = entry->def.hash();
    if (previous) {
        entry->file = previous->file;
        entry->stats = previous->stats;
        entry->def.version = previous->def.version + (previous->hash == entry->hash ? 0 : 1);
        entry->def.author = previous->def.author;
        Json prov = previous->def.provenance;
        prov["updated_by"] = actor;
        prov["updated_at"] = nowIso();
        entry->def.provenance = prov;
    } else {
        entry->def.version = 1;
        entry->def.author = actor;
        Json prov = entry->def.provenance.isObject() ? entry->def.provenance : Json::object();
        prov["created_by"] = actor;
        prov["created_at"] = nowIso();
        entry->def.provenance = prov;
    }
    Json report = Json::object();
    bool testsOk = true;
    if (runTests && entry->def.tests.size() > 0) {
        report = I.runTests(entry, actor, "");
        testsOk = report.get("failed").asInt() == 0;
    }
    bool approvalNeeded = I.needsApproval(entry->def);
    Json out = Json::object({{"name", entry->def.name},
                             {"kind", entry->def.kind},
                             {"version", entry->def.version},
                             {"hash", entry->hash},
                             {"capabilities", entry->def.caps.toJson()},
                             {"needs_approval", approvalNeeded}});
    if (report.isObject() && report.size()) out["tests"] = report;
    Json warn = Json::array();
    for (const auto& w : warnings) warn.push(w);
    if (warn.size()) out["warnings"] = warn;
    if (!testsOk) {
        out["status"] = "tests_failed";
        out["saved"] = false;
        I.audit(Json::object({{"event", "define_failed"}, {"tool", entry->def.name}, {"actor", actor}, {"reason", "tests failed"}}));
        return out;
    }
    if (validateOnly) {
        out["status"] = "valid";
        out["saved"] = false;
        return out;
    }
    if (Status s = I.save(*entry); !s) return s.error();
    I.entries[entry->def.name] = entry;
    I.refresh(*entry);
    out["status"] = entry->status;
    out["saved"] = true;
    out["file"] = entry->file;
    if (!entry->def.sourceFile.empty()) out["source_file"] = entry->def.sourceFile;
    if (entry->status == "pending_approval") {
        out["hint"] = "a human must approve it before it can run: the editor's Studio > Tools panel, or `skywalker call tool_approve "
                      "'{\"name\":\"" + entry->def.name + "\"}' --project <dir>`. Until then it is listed by tool_list_custom but not callable.";
    } else if (entry->status == "active") {
        out["hint"] = "call it like any tool: " + entry->def.name + " (MCP clients get a tools/list_changed notification)";
    }
    I.audit(Json::object({{"event", previous ? "update" : "define"},
                          {"tool", entry->def.name},
                          {"actor", actor},
                          {"version", entry->def.version},
                          {"hash", entry->hash},
                          {"status", entry->status}}));
    I.engine.emitEvent(Json::object({{"type", "custom_tool"},
                                     {"action", previous ? "updated" : "defined"},
                                     {"tool", entry->def.name},
                                     {"actor", actor},
                                     {"status", entry->status}}));
    return out;
}

Result<Json> CustomTools::test(const Json& request, const std::string& actor) {
    Impl& I = *impl_;
    std::shared_ptr<const ToolEntry> tool;
    if (const Json* spec = request.find("definition"); spec && spec->isObject()) {
        std::vector<std::string> warnings;
        Json s = *spec;
        auto name = normalizeName(s.get("name").asString("draft"), "user");
        if (!name) return name.error();
        s["name"] = *name;
        auto def = I.parse(s, actor, nullptr, warnings);
        if (!def) return def.error();
        auto entry = std::make_shared<ToolEntry>();
        entry->def = std::move(def.value());
        if (entry->def.kind == "wander") {
            auto program = I.compile(entry->def, warnings);
            if (!program) return program.error();
            entry->program = program.value();
        }
        entry->hash = entry->def.hash();
        tool = entry;
    } else {
        auto entry = I.find(request.get("name").asString());
        if (!entry) return I.unknownTool(request.get("name").asString());
        if (!entry->loadError.empty()) return Error::make("invalid_definition", entry->loadError);
        tool = entry;
    }
    if (request.contains("args")) {
        return I.testOnce(tool, request.get("args"), request.get("setup"), actor);
    }
    if (tool->def.tests.size() == 0) {
        return Error::make("no_tests", tool->def.name + " has no tests",
                           "add \"tests\": [{\"name\": \"...\", \"args\": {...}, \"expect\": {\"result\": {...}}}] with tool_define, "
                           "or pass \"args\" for a single dry run");
    }
    return I.runTests(tool, actor, request.get("filter").asString());
}

Json CustomTools::list(bool includeLibrary, bool reload) {
    Impl& I = *impl_;
    Json changes;
    if (reload) changes = this->reload();
    Json tools = Json::array();
    for (const auto& [name, e] : I.entries) tools.push(e->summary());
    Json out = Json::object({{"policy", toString(I.policy())}, {"count", static_cast<int64_t>(tools.size())}, {"tools", tools}});
    if (reload) out["reloaded"] = changes;
    if (includeLibrary) {
        Json lib = Json::array();
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(libraryDir(), ec)) {
            std::string fname = f.path().filename().string();
            if (fname.size() <= 10 || fname.substr(fname.size() - 10) != ".tool.json") continue;
            auto text = readText(f.path());
            auto doc = text ? Json::parse(*text) : Result<Json>(Error::make("io_error", "unreadable"));
            if (!doc) continue;
            lib.push(Json::object({{"name", doc->get("name")},
                                   {"kind", doc->get("kind")},
                                   {"description", doc->get("description")},
                                   {"file", f.path().string()}}));
        }
        out["library"] = lib;
        out["library_dir"] = libraryDir();
    }
    return out;
}

Result<Json> CustomTools::inspect(const std::string& name) const {
    const Impl& I = *impl_;
    auto e = I.find(name);
    if (!e) return I.unknownTool(name);
    Json out = e->summary();
    out["definition"] = e->def.toJson();
    if (e->def.kind == "wander") out["code"] = e->def.source;
    out["hash"] = e->hash;
    out["description"] = e->def.description;
    out["stats"] = e->stats.toJson();
    const Json& a = I.approvals.get(e->def.name);
    if (a.isObject()) {
        Json approval = a;
        approval["current"] = a.get("hash").asString() == e->hash;
        out["approval"] = approval;
    }
    out["needs_approval"] = I.needsApproval(e->def);
    if (!e->warnings.empty()) {
        Json w = Json::array();
        for (const auto& s : e->warnings) w.push(s);
        out["warnings"] = w;
    }
    return out;
}

Result<Json> CustomTools::remove(const std::string& name, bool deleteFiles, const std::string& actor) {
    Impl& I = *impl_;
    auto e = I.find(name);
    if (!e) return I.unknownTool(name);
    I.unpublish(e->def.name);
    Json removed = Json::array();
    if (deleteFiles && !e->file.empty()) {
        std::error_code ec;
        fs::path root = I.projectDir();
        if (fs::remove(root / e->file, ec)) removed.push(e->file);
        if (!e->def.sourceFile.empty() && str::startsWith(e->def.sourceFile, "tools/") && fs::remove(root / e->def.sourceFile, ec)) {
            removed.push(e->def.sourceFile);
        }
    }
    if (I.approvals.erase(e->def.name)) (void)I.saveApprovals();
    std::string full = e->def.name;
    I.entries.erase(full);
    I.audit(Json::object({{"event", "remove"}, {"tool", full}, {"actor", actor}}));
    I.engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", "removed"}, {"tool", full}, {"actor", actor}}));
    return Json::object({{"removed", full}, {"files_deleted", removed}});
}

Result<Json> CustomTools::approve(const std::string& name, bool approve, const std::string& note, const std::string& actor) {
    Impl& I = *impl_;
    if (!isHuman(actor)) {
        return Error::make("approval_requires_human", "only a human can approve or reject custom tools (you are " + actor + ")",
                           "ask the human to approve it in the editor's Studio > Tools panel, or to run `skywalker call tool_approve "
                           "'{\"name\":\"" + name + "\"}' --project <dir>`");
    }
    auto e = I.find(name);
    if (!e) return I.unknownTool(name);
    if (!e->loadError.empty()) return Error::make("invalid_definition", "cannot approve an invalid definition: " + e->loadError);
    I.approvals[e->def.name] = Json::object(
        {{"hash", e->hash}, {"verdict", approve ? "approved" : "rejected"}, {"by", actor}, {"at", nowIso()}, {"note", note}, {"version", e->def.version}});
    if (Status s = I.saveApprovals(); !s) return s.error();
    I.refresh(*e);
    I.audit(Json::object({{"event", approve ? "approve" : "reject"}, {"tool", e->def.name}, {"actor", actor}, {"hash", e->hash}, {"note", note}}));
    I.engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", approve ? "approved" : "rejected"}, {"tool", e->def.name}, {"actor", actor}}));
    return Json::object({{"name", e->def.name}, {"status", e->status}, {"hash", e->hash}, {"version", e->def.version}});
}

Result<Json> CustomTools::setEnabled(const std::string& name, bool enabled, const std::string& actor) {
    Impl& I = *impl_;
    auto e = I.find(name);
    if (!e) return I.unknownTool(name);
    if (!e->loadError.empty()) return Error::make("invalid_definition", e->loadError);
    e->def.enabled = enabled;
    if (!e->file.empty()) {
        if (Status s = I.save(*e); !s) return s.error();
    }
    I.refresh(*e);
    I.audit(Json::object({{"event", enabled ? "enable" : "disable"}, {"tool", e->def.name}, {"actor", actor}}));
    Json out = Json::object({{"name", e->def.name}, {"enabled", enabled}, {"status", e->status}});
    if (!e->reason.empty()) out["reason"] = e->reason;
    return out;
}

Result<Json> CustomTools::promote(const std::string& name, const std::string& actor) {
    Impl& I = *impl_;
    auto e = I.find(name);
    if (!e) return I.unknownTool(name);
    if (e->def.kind == "external") return Error::make("invalid_definition", "external tools live in their client; there is nothing to promote");
    if (!e->loadError.empty()) return Error::make("invalid_definition", e->loadError);
    Json doc = e->def.toJson();
    doc.erase("source");
    if (e->def.kind == "wander") doc["wander"] = e->def.source;  // self-contained
    doc["provenance"]["promoted_by"] = actor;
    doc["provenance"]["promoted_at"] = nowIso();
    doc["provenance"]["project"] = I.projectDir();
    fs::path file = fs::path(libraryDir()) / (shortName(e->def.name) + ".tool.json");
    if (Status s = writeAtomic(file, doc.dump(2) + "\n"); !s) return s.error();
    I.audit(Json::object({{"event", "promote"}, {"tool", e->def.name}, {"actor", actor}, {"file", file.string()}}));
    return Json::object({{"name", e->def.name},
                         {"library_file", file.string()},
                         {"hint", "other projects install it with tool_define {\"from_library\": \"" + e->def.name + "\"} (approval rules apply there)"}});
}

Result<Json> CustomTools::setPolicy(ToolPolicy p, const std::string& actor) {
    Impl& I = *impl_;
    ToolPolicy current = I.policy();
    if (strictness(p) < strictness(current) && !isHuman(actor)) {
        return Error::make("approval_requires_human", std::string("only a human can loosen the policy (\"") + toString(current) + "\" -> \"" + toString(p) + "\")",
                           "agents may tighten it; ask the human to change game.json \"customTools\"");
    }
    fs::path file = fs::path(I.projectDir()) / "game.json";
    Json doc = Json::object();
    if (auto text = readText(file)) {
        auto parsed = Json::parse(*text);
        if (!parsed) return Error::make("invalid_game_json", "game.json: " + parsed.error().message);
        doc = parsed.value();
    }
    doc["customTools"]["policy"] = toString(p);
    if (Status s = writeAtomic(file, doc.dump(2) + "\n"); !s) return s.error();
    I.gameJsonTime = -2;
    for (auto& [n, e] : I.entries) I.refresh(*e);
    I.audit(Json::object({{"event", "policy"}, {"actor", actor}, {"from", toString(current)}, {"to", toString(p)}}));
    return Json::object({{"policy", toString(p)}, {"previous", toString(current)}});
}

Json CustomTools::reload() {
    Impl& I = *impl_;
    I.gameJsonTime = -2;
    I.loadApprovals();
    Json added = Json::array(), updated = Json::array(), removed = Json::array(), invalid = Json::array();
    std::set<std::string> seen;
    fs::path dir = fs::path(I.projectDir()) / "tools";
    std::error_code ec;
    std::vector<std::string> files;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        std::string name = f.path().filename().string();
        if (name.size() > 10 && name.substr(name.size() - 10) == ".tool.json" && name.find(" 2.") == std::string::npos) files.push_back("tools/" + name);
    }
    std::sort(files.begin(), files.end());
    for (const auto& rel : files) {
        auto loaded = I.loadFile(rel);
        if (!loaded) continue;
        auto e = loaded.value();
        if (e->def.kind == "external") e->def.persist = true;
        seen.insert(e->def.name);
        auto it = I.entries.find(e->def.name);
        if (it == I.entries.end()) {
            added.push(e->def.name);
        } else {
            if (it->second->hash == e->hash && it->second->loadError == e->loadError && it->second->def.enabled == e->def.enabled) {
                I.refresh(*it->second);
                continue;
            }
            e->stats = it->second->stats;
            e->host = it->second->host;
            updated.push(e->def.name);
        }
        I.entries[e->def.name] = e;
        I.refresh(*e);
    }
    // Retry definitions that refer to tools loaded after them; whatever still fails is reported.
    for (int pass = 0; pass < 8; ++pass) {
        bool progress = false;
        for (auto& [name, entry] : I.entries) {
            if (entry->loadError.empty() || entry->file.empty()) continue;
            auto again = I.loadFile(entry->file);
            if (!again || !again.value()->loadError.empty()) continue;
            again.value()->stats = entry->stats;
            entry = again.value();
            I.refresh(*entry);
            progress = true;
        }
        if (!progress) break;
    }
    for (const auto& [name, entry] : I.entries) {
        if (!entry->loadError.empty()) invalid.push(Json::object({{"name", name}, {"error", entry->loadError}}));
    }
    for (auto it = I.entries.begin(); it != I.entries.end();) {
        bool live = it->second->def.kind == "external" && it->second->host && it->second->host->connected();
        if (!seen.count(it->first) && !it->second->file.empty() && !live) {
            I.unpublish(it->first);
            removed.push(it->first);
            it = I.entries.erase(it);
        } else {
            ++it;
        }
    }
    return Json::object({{"added", added}, {"updated", updated}, {"removed", removed}, {"invalid", invalid}});
}

// ---------------------------------------------------------------------------
// External hosts
// ---------------------------------------------------------------------------

Result<Json> CustomTools::hostRequest(const std::string& method, const Json& params, const std::shared_ptr<ExternalHost>& host) {
    Impl& I = *impl_;
    if (method == "skywalker/tools/list") {
        Json tools = Json::array();
        for (const auto& [n, e] : I.entries) {
            if (e->host && e->host->id() == host->id()) tools.push(e->summary());
        }
        return Json::object({{"tools", tools}, {"policy", toString(I.policy())}});
    }
    if (method == "skywalker/tools/unregister") {
        auto names = stringList(params.get("names"), "names");
        if (!names) return names.error();
        bool all = params.get("all").asBool(false);
        bool forget = params.get("forget").asBool(false);
        Json removed = Json::array();
        for (auto it = I.entries.begin(); it != I.entries.end();) {
            auto& e = it->second;
            bool mine = e->host && e->host->id() == host->id();
            bool named = all || std::find(names->begin(), names->end(), e->def.name) != names->end();
            if (!mine || !named) {
                ++it;
                continue;
            }
            removed.push(e->def.name);
            I.audit(Json::object({{"event", "unregister"}, {"tool", e->def.name}, {"actor", host->actor()}}));
            if (e->def.persist && !forget) {
                e->host = nullptr;
                I.refresh(*e);
                ++it;
                continue;
            }
            if (forget && !e->file.empty()) {
                std::error_code ec;
                fs::remove(fs::path(I.projectDir()) / e->file, ec);
            }
            I.unpublish(e->def.name);
            it = I.entries.erase(it);
        }
        return Json::object({{"unregistered", removed}});
    }
    if (method != "skywalker/tools/register") {
        return Error::make("unknown_method", "unknown method " + method,
                           "skywalker/tools/register, skywalker/tools/unregister and skywalker/tools/list");
    }
    if (I.policy() == ToolPolicy::Off) {
        return Error::make("policy_off", "custom and external tools are turned off for this project",
                           "a human can enable them: game.json \"customTools\": {\"policy\": \"auto\"}");
    }
    std::string ns = params.get("namespace").asString("ext");
    bool nsOk = ns.size() >= 2 && ns.size() <= 16 && std::islower(static_cast<unsigned char>(ns[0])) && ns != "user" && ns != "tool";
    for (char c : ns) nsOk = nsOk && (std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)));
    if (!nsOk) {
        return Error::make("invalid_namespace", "namespace \"" + ns + "\" is not usable",
                           "2-16 lowercase letters/digits, not \"user\" (project tools) or \"tool\"; e.g. \"py\" gives py_<name>");
    }
    if (!params.get("tools").isArray() || params.get("tools").size() == 0) {
        return Error::make("invalid_arguments", "params.tools must be a non-empty array of tool definitions");
    }
    bool persistAll = params.get("persist").asBool(false);
    Json registered = Json::array(), errors = Json::array();
    for (const auto& t : params.get("tools").elements()) {
        std::string rawName = t.get("name").asString();
        auto fail = [&](const Error& e) {
            Json err = Json::object({{"name", rawName}, {"error", e.code}, {"message", e.message}});
            if (!e.hint.empty()) err["hint"] = e.hint;
            errors.push(err);
        };
        if (!t.isObject()) {
            fail(Error::make("invalid_definition", "each tool is an object {name, description, inputSchema, capabilities}"));
            continue;
        }
        auto name = normalizeName(rawName, ns);
        if (!name) {
            fail(name.error());
            continue;
        }
        Json spec = Json::object({{"name", *name}, {"kind", "external"}});
        static const std::vector<std::string> keys = {"name", "title", "description", "category", "inputSchema", "input_schema",
                                                      "outputSchema", "output_schema", "capabilities", "limits", "persist", "annotations"};
        if (Status s = checkKeys(t, "tool " + rawName, keys); !s) {
            fail(s.error());
            continue;
        }
        for (const char* k : {"title", "description", "category", "capabilities", "limits"}) {
            if (t.contains(k)) spec[k] = t.get(k);
        }
        spec["input_schema"] = t.contains("inputSchema") ? t.get("inputSchema") : t.get("input_schema");
        if (t.contains("outputSchema") || t.contains("output_schema")) spec["output_schema"] = t.contains("outputSchema") ? t.get("outputSchema") : t.get("output_schema");
        spec["persist"] = t.get("persist").asBool(persistAll);
        if (spec["input_schema"].isNull()) spec.erase("input_schema");
        auto entry = I.adoptExternal(std::move(spec), host);
        if (!entry) {
            fail(entry.error());
            continue;
        }
        const ToolEntry& e = *entry.value();
        Json r = Json::object({{"name", e.def.name}, {"status", e.status}, {"hash", e.hash}});
        if (!e.reason.empty()) r["reason"] = e.reason;
        if (!e.warnings.empty()) {
            Json w = Json::array();
            for (const auto& x : e.warnings) w.push(x);
            r["warnings"] = w;
        }
        registered.push(r);
    }
    return Json::object({{"tools", registered},
                         {"errors", errors},
                         {"policy", toString(I.policy())},
                         {"call_method", "skywalker/tools/call"}});
}

Result<std::shared_ptr<ToolEntry>> CustomTools::Impl::adoptExternal(Json spec, const std::shared_ptr<ExternalHost>& host,
                                                                    std::shared_ptr<const ToolDef> hosted) {
    const std::string name = spec.get("name").asString();
    std::shared_ptr<ToolEntry> previous = find(name);
    if (previous && previous->def.kind != "external") {
        return Error::make("name_taken", name + " is a project tool (" + previous->def.kind + ")", "pick another name");
    }
    if (previous && previous->host && previous->host->connected() && previous->host->id() != host->id() &&
        !(previous->hosted && str::startsWith(host->id(), "poll:"))) {  // the tool host already settled poll takeovers
        return Error::make("name_taken", name + " is hosted by another connected client (" + previous->host->actor() + ")", "pick another name");
    }
    if (spec.get("description").asString().empty()) {
        spec["description"] = "Tool " + name + " served by " + host->actor() + ".";
    }
    std::vector<std::string> warnings;
    auto def = parse(spec, host->actor(), nullptr, warnings);
    if (!def) return def.error();
    auto entry = std::make_shared<ToolEntry>();
    entry->def = std::move(def.value());
    entry->def.author = host->actor();
    entry->def.provenance = Json::object({{"registered_by", host->actor()}, {"registered_at", nowIso()}, {"connection", host->id()}});
    entry->hash = entry->def.hash();
    entry->host = host;
    entry->hosted = std::move(hosted);
    entry->warnings = std::move(warnings);
    if (previous) {
        entry->stats = previous->stats;
        entry->file = previous->file;
        entry->def.version = previous->def.version + (previous->hash == entry->hash ? 0 : 1);
    }
    if (entry->def.persist) {
        if (Status st = save(*entry); !st) return st.error();
    }
    entries[entry->def.name] = entry;
    refresh(*entry);
    audit(Json::object({{"event", "register"}, {"tool", entry->def.name}, {"actor", host->actor()}, {"status", entry->status}, {"hash", entry->hash}}));
    engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", "registered"}, {"tool", entry->def.name}, {"actor", host->actor()}, {"status", entry->status}}));
    return entry;
}

namespace {

/// A host of the poll-based ToolHost seen as an ExternalHost: its calls go through the transport's own
/// handler (ToolEntry::hosted), so only identity and liveness matter here.
class HostedLink final : public ExternalHost {
public:
    HostedLink(std::string id, std::string actor) : id_(std::move(id)), actor_(std::move(actor)) {}
    const std::string& id() const override { return id_; }
    std::string actor() const override { return actor_; }
    void setActor(std::string) override {}
    bool connected() const override { return alive_.load(); }
    Result<Json> request(const std::string&, const Json&, std::chrono::milliseconds, const std::function<void()>&) override {
        return Error::make("unavailable", "hosted tools are called through their tool host");
    }
    void drop() { alive_ = false; }

private:
    std::string id_;
    std::string actor_;
    std::atomic<bool> alive_{true};
};

/// Hosted tools' links by tool name, shared with the withdraw callback (any thread).
struct HostedLinks {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<HostedLink>> byTool;
};

}  // namespace

void CustomTools::Impl::dropHosted(const std::string& name, const std::string& hostId) {
    auto it = entries.find(name);
    if (it == entries.end() || !it->second->hosted || !it->second->host || it->second->host->id() != hostId) return;
    audit(Json::object({{"event", "offline"}, {"tool", name}, {"actor", it->second->host->actor()}}));
    engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", "removed"}, {"tool", name}, {"actor", it->second->host->actor()}}));
    unpublish(name);
    entries.erase(it);
}

ToolHost::Publisher CustomTools::hostedPublisher() {
    ToolHost::Publisher p;
    auto links = std::make_shared<HostedLinks>();
    Impl* I = impl_.get();
    p.publish = [I, links](ToolDef def, const ToolHost::ToolSpec& spec, const std::string& hostId, const std::string& owner,
                           const std::string& label) -> Status {
        std::string linkId = "poll:" + hostId;
        std::shared_ptr<HostedLink> link;
        {
            std::lock_guard lock(links->mutex);
            for (const auto& [n, l] : links->byTool) {
                if (l->id() == linkId) link = l;
            }
            if (!link) link = std::make_shared<HostedLink>(linkId, owner.empty() ? std::string("mcp:tool-host") : owner);
        }
        Json spec2 = Json::object({{"name", def.name},
                                   {"kind", "external"},
                                   {"title", def.title},
                                   {"description", def.description},
                                   {"category", def.category.empty() ? std::string("python") : def.category},
                                   {"input_schema", def.inputSchema}});
        Json caps = spec.capabilities.isObject() ? spec.capabilities : Json::object();
        if (spec.mutates) caps["mutate"] = true;
        spec2["capabilities"] = caps;
        if (spec.limits.isObject()) spec2["limits"] = spec.limits;
        (void)label;
        auto entry = I->adoptExternal(std::move(spec2), link, std::make_shared<const ToolDef>(std::move(def)));
        if (!entry) return entry.error();
        std::lock_guard lock(links->mutex);
        links->byTool[entry.value()->def.name] = link;
        return {};
    };
    p.withdraw = [I, links](const std::string& name) {
        std::shared_ptr<HostedLink> link;
        {
            std::lock_guard lock(links->mutex);
            auto it = links->byTool.find(name);
            if (it == links->byTool.end()) return;
            link = it->second;
            links->byTool.erase(it);
            bool shared = false;
            for (const auto& [n, l] : links->byTool) shared = shared || l == link;
            if (!shared) link->drop();
        }
        // Gone for every client at once (the registry is thread-safe); the bookkeeping runs on the main thread.
        I->engine.tools().removeDynamic(name);
        std::string hostId = link->id();
        if (I->engine.onMainThread()) {
            I->dropHosted(name, hostId);
        } else {
            (void)I->engine.post([I, name, hostId]() -> Json {
                I->dropHosted(name, hostId);
                return Json();
            });
        }
    };
    return p;
}

Json CustomTools::statusOf(const Json& names) const {
    Json out = Json::object();
    for (const auto& n : names.elements()) {
        auto e = impl_->find(n.asString());
        if (!e) continue;
        Json s = Json::object({{"status", e->status}});
        if (!e->reason.empty()) s["reason"] = e->reason;
        out[e->def.name] = s;
    }
    return out;
}

void CustomTools::hostClosed(const std::shared_ptr<ExternalHost>& host) {
    Impl& I = *impl_;
    for (auto it = I.entries.begin(); it != I.entries.end();) {
        auto& e = it->second;
        if (!e->host || e->host->id() != host->id()) {
            ++it;
            continue;
        }
        I.audit(Json::object({{"event", "offline"}, {"tool", e->def.name}, {"actor", host->actor()}}));
        if (e->def.persist) {
            e->host = nullptr;
            I.refresh(*e);
            ++it;
        } else {
            I.unpublish(e->def.name);
            I.engine.emitEvent(Json::object({{"type", "custom_tool"}, {"action", "removed"}, {"tool", e->def.name}, {"actor", host->actor()}}));
            it = I.entries.erase(it);
        }
    }
}

HostHandlers CustomTools::hostHandlers() {
    HostHandlers h;
    Engine* engine = &impl_->engine;
    h.request = [this, engine](const std::string& method, const Json& params, const std::shared_ptr<ExternalHost>& host) -> Result<Json> {
        if (engine->onMainThread()) return hostRequest(method, params, host);
        auto out = std::make_shared<std::optional<Result<Json>>>();
        std::future<Json> done = engine->post([this, method, params, host, out]() -> Json {
            *out = hostRequest(method, params, host);
            return Json();
        });
        if (done.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
            return Error::make("timeout", "the engine did not respond in time");
        }
        Json status = done.get();
        if (!out->has_value()) return Error::make("unavailable", "the engine is not accepting requests");
        return std::move(**out);
    };
    h.closed = [this, engine](const std::shared_ptr<ExternalHost>& host) {
        if (engine->onMainThread()) {
            hostClosed(host);
            return;
        }
        (void)engine->post([this, host]() -> Json {
            hostClosed(host);
            return Json();
        });
    };
    return h;
}

}  // namespace sky
