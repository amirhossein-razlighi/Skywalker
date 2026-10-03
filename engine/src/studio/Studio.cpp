#include "skywalker/studio/Studio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <type_traits>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/studio/Catalog.h"

namespace sky::studio {

namespace fs = std::filesystem;

// Loop state is edited through references into nested Json; Json must move without
// throwing so that growing an outer object never invalidates inner array storage.
static_assert(std::is_nothrow_move_constructible_v<Json>);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

Json strArray(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push(s);
    return a;
}

std::vector<std::string> toStrings(const Json& j) {
    std::vector<std::string> out;
    if (j.isString()) {
        if (!j.asString().empty()) out.push_back(j.asString());
        return out;
    }
    for (const auto& e : j.elements()) {
        if (e.isString()) out.push_back(e.asString());
        else if (e.isNumber()) out.push_back(e.dump());
    }
    return out;
}

/// Acceptance criteria may come as an array or as a multi-line string.
std::vector<std::string> toLines(const Json& j) {
    if (!j.isString()) return toStrings(j);
    std::vector<std::string> out;
    for (auto& line : str::split(j.asString(), '\n')) {
        std::string t = str::trim(line);
        while (!t.empty() && (t[0] == '-' || t[0] == '*')) t = str::trim(t.substr(1));
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

Json commentsJson(const std::vector<Comment>& cs) {
    Json a = Json::array();
    for (const auto& c : cs) a.push(Json::object({{"by", c.by}, {"at", c.at}, {"text", c.text}}));
    return a;
}

std::vector<Comment> commentsFrom(const Json& j) {
    std::vector<Comment> out;
    for (const auto& c : j.elements()) out.push_back({c.get("by").asString(), c.get("at").asString(), c.get("text").asString()});
    return out;
}

bool contains(const std::vector<std::string>& v, std::string_view s) { return std::find(v.begin(), v.end(), s) != v.end(); }

void addUnique(std::vector<std::string>& v, const std::string& s) {
    if (!s.empty() && !contains(v, s)) v.push_back(s);
}

Status enumCheck(const std::string& field, const std::string& value, const std::vector<std::string>& allowed) {
    if (contains(allowed, value)) return {};
    std::string guess = str::closest(value, allowed, 3);
    std::string list;
    for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
    return Error::make("invalid_arguments", field + " '" + value + "' is not one of: " + list,
                       guess.empty() ? "" : "did you mean '" + guess + "'?");
}

std::string readText(const std::string& path, bool* ok = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (ok) *ok = static_cast<bool>(f);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) return -1;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

std::string shortText(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // keep UTF-8 intact
    return s.substr(0, cut) + "…";
}

const std::vector<std::string> kPalette = {"#8b73fa", "#5c8fed", "#54ccad", "#ffb873", "#ed6b7a",
                                           "#6aa5ff", "#f2c14e", "#b48cff", "#5ad1c9", "#c9935b"};
const std::vector<std::string> kFaces = {"happy", "focused", "dreamy", "wink", "determined", "curious"};

bool validColor(const std::string& c) {
    if (c.size() != 7 || c[0] != '#') return false;
    return std::all_of(c.begin() + 1, c.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); });
}

/// Open = still waiting for a decision.
bool needsDecision(const Feedback& f) { return f.status == "open" || f.status == "regressed"; }

bool taskOpen(const Task& t) { return t.status == "todo" || t.status == "doing" || t.status == "review" || t.status == "backlog"; }

std::string fmtNumber(double v) {
    char buf[64];
    if (std::fabs(v - std::round(v)) < 1e-9) std::snprintf(buf, sizeof(buf), "%.0f", v);
    else std::snprintf(buf, sizeof(buf), "%.3g", v);
    return buf;
}

}  // namespace

std::string slugify(std::string_view s) {
    std::string out;
    bool dash = false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            dash = false;
        } else if (c == '_') {
            out.push_back('_');
            dash = false;
        } else if (!out.empty() && !dash) {
            out.push_back('-');
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

int idNumber(std::string_view id) {
    size_t dash = id.rfind('-');
    std::string_view digits = dash == std::string_view::npos ? id : id.substr(dash + 1);
    int n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
        if (n > 100000000) return 0;
    }
    return n;
}

const char* toString(Autonomy a) {
    switch (a) {
        case Autonomy::Observe: return "observe";
        case Autonomy::Ask: return "ask";
        case Autonomy::Autonomous: return "autonomous";
    }
    return "autonomous";
}

const char* toString(Access a) {
    switch (a) {
        case Access::Inherit: return "inherit";
        case Access::Allow: return "allow";
        case Access::Ask: return "ask";
        case Access::Off: return "off";
    }
    return "inherit";
}

std::optional<Autonomy> parseAutonomy(std::string_view s) {
    if (s == "observe") return Autonomy::Observe;
    if (s == "ask") return Autonomy::Ask;
    if (s == "autonomous" || s == "auto") return Autonomy::Autonomous;
    return std::nullopt;
}

std::optional<Access> parseAccess(std::string_view s) {
    if (s == "inherit" || s == "default") return Access::Inherit;
    if (s == "allow") return Access::Allow;
    if (s == "ask") return Access::Ask;
    if (s == "off") return Access::Off;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Records <-> JSON
// ---------------------------------------------------------------------------

Access AgentProfile::access(const std::string& category, bool readOnly, bool openWorld) const {
    auto it = permissions.find(category);
    Access explicitAccess = it == permissions.end() ? Access::Inherit : it->second;
    if (explicitAccess == Access::Off) return Access::Off;
    if (category == "studio") return explicitAccess == Access::Ask ? Access::Ask : Access::Allow;
    if (readOnly && !openWorld) return Access::Allow;
    if (explicitAccess != Access::Inherit) return explicitAccess;
    // Reaching outside the project (downloads) needs the human's OK unless explicitly allowed.
    if (openWorld || category == "network") return autonomy == Autonomy::Observe ? Access::Off : Access::Ask;
    switch (autonomy) {
        case Autonomy::Observe: return Access::Off;
        case Autonomy::Ask: return Access::Ask;
        case Autonomy::Autonomous: return Access::Allow;
    }
    return Access::Off;
}

Json AgentProfile::toJson() const {
    Json perms = Json::object();
    for (const auto& [k, v] : permissions) perms[k] = toString(v);
    Json j = Json::object({{"format", "skywalker.agent"},
                           {"version", 2},
                           {"id", id},
                           {"name", name},
                           {"discipline", discipline},
                           {"role", role},
                           {"focus", focus},
                           {"focus_tags", strArray(focusTags)},
                           {"persona", persona},
                           {"mission", mission},
                           {"instructions", instructions},
                           {"provider", provider},
                           {"model", model},
                           {"autonomy", toString(autonomy)},
                           {"permissions", perms},
                           {"reports_to", reportsTo},
                           {"color", color},
                           {"face", face},
                           {"max_rounds", maxRounds},
                           {"memory", strArray(memory)}});
    if (playtest.isObject() && !playtest.members().empty()) j["playtest"] = playtest;
    return j;
}

Result<AgentProfile> AgentProfile::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_agent", "an agent must be a JSON object");
    AgentProfile a;
    a.name = str::trim(j.get("name").asString());
    a.id = slugify(j.get("id").asString());
    if (a.id.empty()) a.id = slugify(a.name);
    if (a.name.empty()) a.name = a.id;
    if (a.id.empty()) return Error::make("invalid_agent", "an agent needs a name", "pass {\"name\": \"Mira\", \"role\": ...}");
    a.role = j.get("role").asString();
    a.discipline = j.get("discipline").asString();
    if (a.discipline.empty()) {
        if (const RoleInfo* r = findRole(a.role)) a.discipline = r->discipline;
    }
    a.focus = j.get("focus").asString();
    a.focusTags = toStrings(j.get("focus_tags"));
    a.persona = j.get("persona").asString(j.get("personality").asString());
    a.mission = j.get("mission").asString();
    a.instructions = j.get("instructions").asString();
    a.provider = j.get("provider").asString("anthropic");
    if (a.provider.empty()) a.provider = "anthropic";
    a.model = j.get("model").asString();
    if (j.contains("autonomy")) {
        auto au = parseAutonomy(j.get("autonomy").asString());
        if (!au) return Error::make("invalid_agent", "autonomy must be observe, ask or autonomous");
        a.autonomy = *au;
    }
    for (const auto& [k, v] : j.get("permissions").members()) {
        auto acc = parseAccess(v.asString());
        if (!acc) return Error::make("invalid_agent", "permission '" + k + "' must be inherit, allow, ask or off");
        if (*acc != Access::Inherit) a.permissions[k] = *acc;
    }
    a.reportsTo = slugify(j.get("reports_to").asString());
    a.color = j.get("color").asString(a.color);
    a.face = j.get("face").asString(a.face);
    a.maxRounds = static_cast<int>(std::clamp<int64_t>(j.get("max_rounds").asInt(40), 1, 200));
    a.memory = toStrings(j.get("memory"));
    if (j.get("playtest").isObject()) a.playtest = j.get("playtest");
    return a;
}

Json Task::toJson() const {
    Json l = Json::object();
    for (const auto& [k, v] : links) {
        if (!v.empty()) l[k] = strArray(v);
    }
    return Json::object({{"id", id},
                         {"title", title},
                         {"description", description},
                         {"acceptance", strArray(acceptance)},
                         {"discipline", discipline},
                         {"assignee", assignee},
                         {"status", status},
                         {"priority", priority},
                         {"depends_on", strArray(dependsOn)},
                         {"links", l},
                         {"comments", commentsJson(comments)},
                         {"created_by", createdBy},
                         {"created_at", createdAt},
                         {"updated_at", updatedAt}});
}

Task Task::fromJson(const Json& j) {
    Task t;
    t.id = j.get("id").asString();
    t.title = j.get("title").asString();
    t.description = j.get("description").asString();
    t.acceptance = toLines(j.get("acceptance"));
    t.discipline = j.get("discipline").asString();
    t.assignee = j.get("assignee").asString();
    t.status = j.get("status").asString("todo");
    t.priority = j.get("priority").asString("normal");
    t.dependsOn = toStrings(j.get("depends_on"));
    for (const auto& [k, v] : j.get("links").members()) t.links[k] = toStrings(v);
    t.comments = commentsFrom(j.get("comments"));
    t.createdBy = j.get("created_by").asString();
    t.createdAt = j.get("created_at").asString();
    t.updatedAt = j.get("updated_at").asString();
    return t;
}

Json Feedback::toJson() const {
    Json j = Json::object({{"id", id},
                           {"by", by},
                           {"category", category},
                           {"severity", severity},
                           {"summary", summary},
                           {"details", details},
                           {"target", target},
                           {"status", status},
                           {"evidence", evidence},
                           {"occurrences", occurrences},
                           {"decision", decision},
                           {"tasks", strArray(tasks)},
                           {"created_at", createdAt},
                           {"updated_at", updatedAt},
                           {"history", commentsJson(history)}});
    if (!fingerprint.empty()) j["fingerprint"] = fingerprint;
    if (!mergedInto.empty()) j["merged_into"] = mergedInto;
    return j;
}

Feedback Feedback::fromJson(const Json& j) {
    Feedback f;
    f.id = j.get("id").asString();
    f.by = j.get("by").asString();
    f.category = j.get("category").asString();
    f.severity = j.get("severity").asString("medium");
    f.summary = j.get("summary").asString();
    f.details = j.get("details").asString();
    f.target = j.get("target").asString();
    f.status = j.get("status").asString("open");
    if (j.get("evidence").isObject()) f.evidence = j.get("evidence");
    f.fingerprint = j.get("fingerprint").asString();
    f.occurrences = static_cast<int>(j.get("occurrences").asInt(1));
    f.decision = j.get("decision").asString();
    f.tasks = toStrings(j.get("tasks"));
    f.mergedInto = j.get("merged_into").asString();
    f.createdAt = j.get("created_at").asString();
    f.updatedAt = j.get("updated_at").asString();
    f.history = commentsFrom(j.get("history"));
    return f;
}

Json Decision::toJson() const {
    Json j = Json::object({{"id", id},
                           {"feedback", feedback},
                           {"verdict", verdict},
                           {"rationale", rationale},
                           {"by", by},
                           {"at", at},
                           {"tasks", strArray(tasks)},
                           {"metrics_before", metricsBefore},
                           {"targets", targets},
                           {"effect", effect}});
    if (!mergeInto.empty()) j["merge_into"] = mergeInto;
    if (!playtestBefore.empty()) j["playtest_before"] = playtestBefore;
    return j;
}

Decision Decision::fromJson(const Json& j) {
    Decision d;
    d.id = j.get("id").asString();
    d.feedback = j.get("feedback").asString();
    d.verdict = j.get("verdict").asString();
    d.rationale = j.get("rationale").asString();
    d.by = j.get("by").asString();
    d.at = j.get("at").asString();
    d.tasks = toStrings(j.get("tasks"));
    d.mergeInto = j.get("merge_into").asString();
    if (j.get("metrics_before").isObject()) d.metricsBefore = j.get("metrics_before");
    d.playtestBefore = j.get("playtest_before").asString();
    if (j.get("targets").isObject()) d.targets = j.get("targets");
    if (j.get("effect").isObject()) d.effect = j.get("effect");
    return d;
}

Json LoopStage::toJson() const {
    Json j = Json::object({{"id", id}, {"title", title}, {"kind", kind}});
    if (kind == "agents") {
        j["assignees"] = strArray(assignees);
        j["instruction"] = instruction;
        j["parallel"] = parallel;
        if (onlyWithTasks) j["only_with_tasks"] = true;
    } else {
        j["playtest"] = playtest;
        j["file_feedback"] = fileFeedback;
        j["verify_fixed"] = verifyFixed;
    }
    if (!inputs.empty()) j["inputs"] = strArray(inputs);
    if (!gate.members().empty()) j["gate"] = gate;
    return j;
}

Result<LoopStage> LoopStage::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_loop", "each stage must be an object");
    LoopStage s;
    s.id = slugify(j.get("id").asString());
    s.title = j.get("title").asString();
    if (s.id.empty()) s.id = slugify(s.title);
    if (s.id.empty()) return Error::make("invalid_loop", "a stage needs an id or a title");
    if (s.title.empty()) s.title = s.id;
    s.kind = j.get("kind").asString("agents");
    if (s.kind != "agents" && s.kind != "playtest") {
        return Error::make("invalid_loop", "stage '" + s.id + "': kind must be \"agents\" or \"playtest\"");
    }
    s.assignees = toStrings(j.get("assignees"));
    if (s.assignees.empty()) s.assignees = toStrings(j.get("agents"));
    s.instruction = j.get("instruction").asString();
    s.inputs = toStrings(j.get("inputs"));
    s.parallel = j.get("parallel").asBool(true);
    s.onlyWithTasks = j.get("only_with_tasks").asBool(false);
    if (j.get("gate").isObject()) s.gate = j.get("gate");
    if (j.get("playtest").isObject()) s.playtest = j.get("playtest");
    s.fileFeedback = j.get("file_feedback").asBool(true);
    s.verifyFixed = j.get("verify_fixed").asBool(false);
    if (s.kind == "agents" && s.assignees.empty()) {
        return Error::make("invalid_loop", "stage '" + s.id + "' needs assignees",
                           "e.g. [\"@creative_director\"], [\"@engineering\"], [\"mira\"] or \"@a|@b\" for a fallback");
    }
    if (s.kind == "agents" && s.instruction.empty()) {
        return Error::make("invalid_loop", "stage '" + s.id + "' needs an instruction");
    }
    return s;
}

Json LoopStop::toJson() const {
    return Json::object({{"max_iterations", maxIterations},
                         {"metric_targets", metricTargets},
                         {"director_signoff", directorSignoff},
                         {"token_budget", tokenBudget},
                         {"time_budget_minutes", timeBudgetMinutes}});
}

LoopStop LoopStop::fromJson(const Json& j) {
    LoopStop s;
    s.maxIterations = static_cast<int>(std::clamp<int64_t>(j.get("max_iterations").asInt(3), 1, 100));
    if (j.get("metric_targets").isObject()) s.metricTargets = j.get("metric_targets");
    s.directorSignoff = j.get("director_signoff").asBool(false);
    s.tokenBudget = std::max<int64_t>(0, j.get("token_budget").asInt(0));
    s.timeBudgetMinutes = std::max(0.0, j.get("time_budget_minutes").asNumber(0));
    return s;
}

Json Loop::toJson() const {
    Json st = Json::array();
    for (const auto& s : stages) st.push(s.toJson());
    Json j = Json::object({{"format", "skywalker.loop"},
                           {"version", 1},
                           {"name", name},
                           {"goal", goal},
                           {"description", description}});
    if (!templateName.empty()) j["template"] = templateName;
    j["stages"] = st;
    j["stop"] = stop.toJson();
    j["state"] = state;
    return j;
}

Result<Loop> Loop::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_loop", "a loop must be a JSON object");
    Loop l;
    l.name = slugify(j.get("name").asString());
    if (l.name.empty()) return Error::make("invalid_loop", "a loop needs a name");
    std::replace(l.name.begin(), l.name.end(), '-', '_');
    l.goal = j.get("goal").asString();
    l.description = j.get("description").asString();
    l.templateName = j.get("template").asString();
    std::set<std::string> ids;
    for (const auto& sj : j.get("stages").elements()) {
        auto s = LoopStage::fromJson(sj);
        if (!s) return s.error();
        if (!ids.insert(s->id).second) return Error::make("invalid_loop", "duplicate stage id '" + s->id + "'");
        for (const auto& in : s->inputs) {
            if (!ids.count(in) || in == s->id) {
                return Error::make("invalid_loop", "stage '" + s->id + "' takes input from '" + in +
                                                       "', which is not an earlier stage");
            }
        }
        l.stages.push_back(std::move(*s));
    }
    if (l.stages.empty()) return Error::make("invalid_loop", "a loop needs at least one stage");
    l.stop = LoopStop::fromJson(j.get("stop"));
    if (j.get("state").isObject()) l.state = j.get("state");
    return l;
}

Json Message::toJson() const {
    Json j = Json::object({{"id", id}, {"at", at}, {"from", from}, {"channel", channel}});
    if (!to.empty()) j["to"] = strArray(to);
    if (!mentions.empty()) j["mentions"] = strArray(mentions);
    if (!thread.empty()) j["thread"] = thread;
    j["text"] = text;
    if (!refs.members().empty()) j["refs"] = refs;
    return j;
}

Message Message::fromJson(const Json& j) {
    Message m;
    m.id = j.get("id").asString();
    m.at = j.get("at").asString();
    m.from = j.get("from").asString();
    m.channel = j.get("channel").asString("general");
    m.to = toStrings(j.get("to"));
    m.mentions = toStrings(j.get("mentions"));
    m.thread = j.get("thread").asString();
    m.text = j.get("text").asString();
    if (j.get("refs").isObject()) m.refs = j.get("refs");
    return m;
}

Json Usage::toJson() const {
    return Json::object({{"input_tokens", inputTokens},
                         {"output_tokens", outputTokens},
                         {"cache_read_tokens", cacheReadTokens},
                         {"cache_write_tokens", cacheWriteTokens},
                         {"requests", requests},
                         {"tool_calls", toolCalls},
                         {"tool_errors", toolErrors},
                         {"cost_usd", std::round(costUsd * 10000) / 10000},
                         {"last_active", lastActive}});
}

Usage Usage::fromJson(const Json& j) {
    Usage u;
    u.inputTokens = j.get("input_tokens").asInt();
    u.outputTokens = j.get("output_tokens").asInt();
    u.cacheReadTokens = j.get("cache_read_tokens").asInt();
    u.cacheWriteTokens = j.get("cache_write_tokens").asInt();
    u.requests = j.get("requests").asInt();
    u.toolCalls = j.get("tool_calls").asInt();
    u.toolErrors = j.get("tool_errors").asInt();
    u.costUsd = j.get("cost_usd").asNumber();
    u.lastActive = j.get("last_active").asString();
    return u;
}

Json Assignment::toJson() const {
    return Json::object({{"agent", agent}, {"stage", stage}, {"tasks", strArray(tasks)}, {"prompt", prompt}});
}

Price priceFor(std::string_view model) {
    // USD per million tokens (Claude API list prices). Cache writes (5-minute TTL) cost
    // 1.25x input. Unknown models report 0 and are flagged "unpriced".
    struct Row {
        const char* prefix;
        double in, out, read;
    };
    static const Row rows[] = {
        {"claude-fable-5-1", 10, 50, 0.25}, {"claude-fable-5", 10, 50, 1.0},   {"claude-mythos-5", 10, 50, 1.0},
        {"claude-opus-5-5", 4, 20, 0.20},   {"claude-opus-5", 5, 25, 0.50},    {"claude-opus-4", 5, 25, 0.50},
        {"claude-sonnet-5", 2, 10, 0.20},   {"claude-sonnet-4", 3, 15, 0.30},  {"claude-haiku-4", 1, 5, 0.10},
    };
    for (const auto& r : rows) {
        if (str::startsWith(model, r.prefix)) return {r.in, r.out, r.read, r.in * 1.25, true};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

int metricDirection(std::string_view m) {
    static const char* higher[] = {"completion_rate", "objectives", "objectives_avg", "coverage", "est_fps", "goals_reached"};
    static const char* lower[] = {"deaths",        "fails",       "damage",      "time_to_goal", "time_to_first_goal",
                                  "stuck_seconds", "avg_tick_ms", "p95_tick_ms", "max_tick_ms",  "script_errors",
                                  "quit_rate",     "render_ms"};
    for (const char* h : higher) {
        if (m == h) return 1;
    }
    for (const char* l : lower) {
        if (m == l) return -1;
    }
    return 0;
}

Json compareMetrics(const Json& before, const Json& after) {
    Json out = Json::object();
    for (const auto& [k, a] : after.members()) {
        const Json* bp = before.find(k);
        if (!bp) continue;
        const Json& b = *bp;
        if (!(a.isNumber() || a.isNull()) || !(b.isNumber() || b.isNull())) continue;
        if (a.isNull() && b.isNull()) continue;
        int dir = metricDirection(k);
        Json row = Json::object({{"before", b}, {"after", a}});
        std::string verdict;
        if (a.isNull() || b.isNull()) {
            // null = "never happened" (e.g. time_to_goal when nobody finished): worst for
            // lower-is-better metrics.
            row["delta"] = nullptr;
            bool improved = b.isNull();  // went from never to a value
            verdict = dir == 0 ? "changed" : ((dir < 0) == improved ? "better" : "worse");
        } else {
            double bv = b.asNumber(), av = a.asNumber(), delta = av - bv;
            double tol = std::max(1e-9, std::fabs(bv) * 0.05);
            row["delta"] = std::round(delta * 1000) / 1000;
            row["change"] = bv != 0 ? std::round(delta / std::fabs(bv) * 1000) / 1000 : (delta == 0 ? 0.0 : (delta > 0 ? 1.0 : -1.0));
            if (std::fabs(delta) <= tol) verdict = "same";
            else if (dir == 0) verdict = "changed";
            else verdict = (delta * dir > 0) ? "better" : "worse";
        }
        row["verdict"] = verdict;
        out[k] = row;
    }
    return out;
}

bool targetsMet(const Json& metrics, const Json& targets, std::string* unmet) {
    for (const auto& [k, t] : targets.members()) {
        const Json& v = metrics.get(k);
        auto fail = [&](const std::string& why) {
            if (unmet) *unmet = k + " " + why;
            return false;
        };
        if (!v.isNumber()) return fail("not measured");
        double x = v.asNumber();
        if (t.isNumber()) {
            int dir = metricDirection(k);
            if (dir >= 0 && x < t.asNumber()) return fail("= " + fmtNumber(x) + " (target >= " + fmtNumber(t.asNumber()) + ")");
            if (dir < 0 && x > t.asNumber()) return fail("= " + fmtNumber(x) + " (target <= " + fmtNumber(t.asNumber()) + ")");
            continue;
        }
        if (t.contains("min") && x < t.get("min").asNumber()) {
            return fail("= " + fmtNumber(x) + " (target >= " + fmtNumber(t.get("min").asNumber()) + ")");
        }
        if (t.contains("max") && x > t.get("max").asNumber()) {
            return fail("= " + fmtNumber(x) + " (target <= " + fmtNumber(t.get("max").asNumber()) + ")");
        }
    }
    return true;
}

std::string effectVerdict(const Json& comparison, const std::vector<std::string>& metrics, const Json& targets) {
    int better = 0, worse = 0, considered = 0;
    Json after = Json::object();
    for (const auto& [k, row] : comparison.members()) {
        after[k] = row.get("after");
        bool relevant = metrics.empty() ? metricDirection(k) != 0 : contains(metrics, k) || targets.contains(k);
        if (!relevant) continue;
        ++considered;
        const std::string& v = row.get("verdict").asString();
        if (v == "better") ++better;
        if (v == "worse") ++worse;
    }
    if (considered == 0) return "unmeasured";
    if (!targets.members().empty()) {
        if (targetsMet(after, targets)) return worse ? "mixed" : "improved";
        if (worse && !better) return "regressed";
        return better ? "mixed" : (worse ? "regressed" : "unchanged");
    }
    if (better && !worse) return "improved";
    if (worse && !better) return "regressed";
    if (better && worse) return "mixed";
    return "unchanged";
}

std::vector<std::string> metricsForCategory(std::string_view c) {
    if (c == "difficulty") return {"deaths", "completion_rate", "time_to_goal", "fails", "damage"};
    if (c == "clarity") return {"stuck_seconds", "completion_rate", "time_to_goal"};
    if (c == "performance") return {"est_fps", "avg_tick_ms", "p95_tick_ms"};
    if (c == "bug") return {"script_errors", "fails", "stuck_seconds"};
    if (c == "fun") return {"completion_rate", "quit_rate", "coverage"};
    if (c == "accessibility") return {"completion_rate", "stuck_seconds", "time_to_goal"};
    return {};
}

// ---------------------------------------------------------------------------
// Studio: lifetime, persistence
// ---------------------------------------------------------------------------

Studio::Studio(std::string projectDir, EventSink sink)
    : projectDir_(std::move(projectDir)), sink_(std::move(sink)) {
    loadAll();
}

Studio::~Studio() {
    if (usageDirty_) saveUsage();  // tool-call counters are batched
}

double Studio::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

std::string Studio::timestamp() const {
    double t = now();
    auto secs = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string Studio::studioDir() const { return (fs::path(projectDir_) / "studio").string(); }

void Studio::emit(const std::string& kind, const std::string& action, const std::string& id, const std::string& actor,
                  const std::string& summary, Json extra) {
    if (!sink_) return;
    Json e = Json::object({{"type", "studio"}, {"kind", kind}, {"action", action}, {"id", id}, {"actor", actor}, {"summary", summary}});
    for (const auto& [k, v] : extra.members()) e[k] = v;
    sink_(std::move(e));
}

void Studio::writeFile(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::error("studio", "cannot write " + path);
            return;
        }
        f << text;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        log::error("studio", "cannot write " + path + ": " + ec.message());
        fs::remove(tmp, ec);
        return;
    }
    noteFile(path);
}

void Studio::noteFile(const std::string& path) { mtimes_[path] = mtimeOf(path); }

bool Studio::changedOnDisk(const std::string& path) const {
    auto it = mtimes_.find(path);
    int64_t m = mtimeOf(path);
    if (it == mtimes_.end()) return m != -1;
    return it->second != m;
}

void Studio::loadAll() {
    loadAgents();
    loadBoard();
    loadFeedback();
    loadDecisions();
    loadLoops();
    loadMessages();
    loadUsage();
}

void Studio::loadAgents() {
    agents_.clear();
    fs::path dir = fs::path(projectDir_) / "agents";
    std::error_code ec;
    std::vector<std::string> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.size() > 11 && name.substr(name.size() - 11) == ".agent.json") files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
    for (auto it = mtimes_.begin(); it != mtimes_.end();) {  // forget agent files that vanished
        if (it->first.find("/agents/") != std::string::npos && !contains(files, it->first)) it = mtimes_.erase(it);
        else ++it;
    }
    for (const auto& f : files) {
        noteFile(f);
        auto doc = Json::parse(readText(f));
        if (!doc) {
            log::warn("studio", "skipping " + f + ": " + doc.error().message);
            continue;
        }
        auto a = AgentProfile::fromJson(*doc);
        if (!a) {
            log::warn("studio", "skipping " + f + ": " + a.error().message);
            continue;
        }
        // The file name is the id: keep them in sync even if the file was renamed by hand.
        std::string stem = fs::path(f).filename().string();
        a->id = stem.substr(0, stem.size() - 11);
        agents_.push_back(std::move(*a));
    }
    // Order: reporting lines first (directors before their reports), otherwise by file.
    std::stable_sort(agents_.begin(), agents_.end(), [](const AgentProfile& a, const AgentProfile& b) {
        auto rank = [](const AgentProfile& p) {
            const auto& d = disciplines();
            auto it = std::find(d.begin(), d.end(), p.discipline);
            return it == d.end() ? d.size() : static_cast<size_t>(it - d.begin());
        };
        return rank(a) < rank(b);
    });
}

void Studio::loadBoard() {
    tasks_.clear();
    std::string path = studioDir() + "/board.json";
    noteFile(path);
    auto doc = Json::parse(readText(path));
    if (!doc) return;
    for (const auto& t : doc->get("tasks").elements()) {
        tasks_.push_back(Task::fromJson(t));
        nextTask_ = std::max(nextTask_, idNumber(tasks_.back().id) + 1);
    }
}

void Studio::loadFeedback() {
    feedback_.clear();
    std::string path = studioDir() + "/feedback.json";
    noteFile(path);
    auto doc = Json::parse(readText(path));
    if (!doc) return;
    for (const auto& f : doc->get("items").elements()) {
        feedback_.push_back(Feedback::fromJson(f));
        nextFeedback_ = std::max(nextFeedback_, idNumber(feedback_.back().id) + 1);
    }
}

void Studio::loadDecisions() {
    decisions_.clear();
    std::string path = studioDir() + "/decisions.json";
    noteFile(path);
    auto doc = Json::parse(readText(path));
    if (!doc) return;
    for (const auto& d : doc->get("items").elements()) {
        decisions_.push_back(Decision::fromJson(d));
        nextDecision_ = std::max(nextDecision_, idNumber(decisions_.back().id) + 1);
    }
}

void Studio::loadLoops() {
    loops_.clear();
    fs::path dir = fs::path(studioDir()) / "loops";
    std::error_code ec;
    std::vector<std::string> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.size() > 10 && name.substr(name.size() - 10) == ".loop.json") files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
    for (auto it = mtimes_.begin(); it != mtimes_.end();) {
        if (it->first.find("/studio/loops/") != std::string::npos && !contains(files, it->first)) it = mtimes_.erase(it);
        else ++it;
    }
    for (const auto& f : files) {
        noteFile(f);
        auto doc = Json::parse(readText(f));
        if (!doc) continue;
        auto l = Loop::fromJson(*doc);
        if (!l) {
            log::warn("studio", "skipping " + f + ": " + l.error().message);
            continue;
        }
        loops_.push_back(std::move(*l));
    }
}

void Studio::loadMessages() {
    messages_.clear();
    std::string path = studioDir() + "/messages.jsonl";
    noteFile(path);
    std::string text = readText(path);
    for (const auto& line : str::split(text, '\n')) {
        if (str::trim(line).empty()) continue;
        auto j = Json::parse(line);
        if (!j) continue;
        messages_.push_back(Message::fromJson(*j));
        nextMessage_ = std::max(nextMessage_, idNumber(messages_.back().id) + 1);
    }
}

void Studio::loadUsage() {
    usage_.clear();
    std::string path = studioDir() + "/usage.json";
    noteFile(path);
    if (auto doc = Json::parse(readText(path))) {
        for (const auto& [k, v] : doc->get("agents").members()) usage_[k] = Usage::fromJson(v);
        for (const auto& [k, v] : doc->get("read_cursors").members()) readCursor_[k] = v.asString();
        latestPlaytest_ = doc->get("latest_playtest").asString();
        if (doc->get("latest_metrics").isObject()) latestMetrics_ = doc->get("latest_metrics");
    }
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::path(studioDir()) / "playtests", ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        nextPlaytest_ = std::max(nextPlaytest_, idNumber(it->path().filename().string()) + 1);
    }
}

void Studio::syncFromDisk() {
    // Agents: any file added, removed or changed.
    bool agentsChanged = false;
    {
        fs::path dir = fs::path(projectDir_) / "agents";
        std::error_code ec;
        size_t seen = 0;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.size() <= 11 || name.substr(name.size() - 11) != ".agent.json") continue;
            ++seen;
            if (changedOnDisk(it->path().string())) agentsChanged = true;
        }
        size_t known = 0;
        for (const auto& [k, v] : mtimes_) known += k.find("/agents/") != std::string::npos;
        if (seen != known) agentsChanged = true;
    }
    if (agentsChanged) loadAgents();
    if (changedOnDisk(studioDir() + "/board.json")) loadBoard();
    if (changedOnDisk(studioDir() + "/feedback.json")) loadFeedback();
    if (changedOnDisk(studioDir() + "/decisions.json")) loadDecisions();
    if (changedOnDisk(studioDir() + "/messages.jsonl")) loadMessages();
    if (changedOnDisk(studioDir() + "/usage.json")) loadUsage();
    bool loopsChanged = false;
    {
        std::error_code ec;
        size_t seen = 0;
        for (auto it = fs::directory_iterator(fs::path(studioDir()) / "loops", ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            ++seen;
            if (changedOnDisk(it->path().string())) loopsChanged = true;
        }
        size_t known = 0;
        for (const auto& [k, v] : mtimes_) known += k.find("/studio/loops/") != std::string::npos;
        if (seen != known) loopsChanged = true;
    }
    if (loopsChanged) loadLoops();
}

void Studio::saveAgent(const AgentProfile& a) {
    writeFile((fs::path(projectDir_) / "agents" / (a.id + ".agent.json")).string(), a.toJson().dump(2) + "\n");
}

void Studio::saveBoard() {
    Json arr = Json::array();
    for (const auto& t : tasks_) arr.push(t.toJson());
    writeFile(studioDir() + "/board.json",
              Json::object({{"format", "skywalker.board"}, {"version", 1}, {"tasks", arr}}).dump(2) + "\n");
}

void Studio::saveFeedback() {
    Json arr = Json::array();
    for (const auto& f : feedback_) arr.push(f.toJson());
    writeFile(studioDir() + "/feedback.json",
              Json::object({{"format", "skywalker.feedback"}, {"version", 1}, {"items", arr}}).dump(2) + "\n");
}

void Studio::saveDecisions() {
    Json arr = Json::array();
    for (const auto& d : decisions_) arr.push(d.toJson());
    writeFile(studioDir() + "/decisions.json",
              Json::object({{"format", "skywalker.decisions"}, {"version", 1}, {"items", arr}}).dump(2) + "\n");
}

void Studio::saveLoop(const Loop& l) {
    writeFile(studioDir() + "/loops/" + l.name + ".loop.json", l.toJson().dump(2) + "\n");
}

void Studio::appendMessage(const Message& m) {
    std::string path = studioDir() + "/messages.jsonl";
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f << m.toJson().dump() << "\n";
    }
    noteFile(path);
}

void Studio::saveUsage() {
    usageDirty_ = false;
    lastUsageSave_ = now();
    Json agents = Json::object();
    for (const auto& [k, u] : usage_) agents[k] = u.toJson();
    Json cursors = Json::object();
    for (const auto& [k, v] : readCursor_) cursors[k] = v;
    writeFile(studioDir() + "/usage.json", Json::object({{"format", "skywalker.usage"},
                                                         {"version", 1},
                                                         {"agents", agents},
                                                         {"read_cursors", cursors},
                                                         {"latest_playtest", latestPlaytest_},
                                                         {"latest_metrics", latestMetrics_}})
                                                   .dump(2) +
                                               "\n");
}

std::string Studio::nextId(const char* prefix, int& counter) { return std::string(prefix) + "-" + std::to_string(counter++); }

// ---------------------------------------------------------------------------
// Roster
// ---------------------------------------------------------------------------

const AgentProfile* Studio::agent(std::string_view ref) const {
    std::string_view r = ref;
    if (!r.empty() && r[0] == '@') r.remove_prefix(1);
    if (r.empty()) return nullptr;
    std::string slug = slugify(r);
    for (const auto& a : agents_) {
        if (a.id == r || a.id == slug) return &a;
    }
    std::string lower = str::lower(r);
    for (const auto& a : agents_) {
        if (str::lower(a.name) == lower) return &a;
    }
    return nullptr;
}

Error Studio::unknownAgent(std::string_view ref) const {
    std::vector<std::string> names;
    for (const auto& a : agents_) {
        names.push_back(a.id);
        names.push_back(a.name);
    }
    std::string guess = str::closest(std::string(ref), names, 3);
    return Error::make("not_found", "no agent '" + std::string(ref) + "' on the roster",
                       guess.empty() ? "studio_agent_list shows the roster; studio_team_template spawns a team"
                                     : "did you mean '" + guess + "'?");
}

std::string Studio::memberForActor(std::string_view actor) const {
    std::string_view a = actor;
    for (std::string_view prefix : {"agent:", "mcp:", "editor:", "cli:"}) {
        if (str::startsWith(a, prefix)) {
            a.remove_prefix(prefix.size());
            break;
        }
    }
    size_t slash = a.rfind('/');
    if (slash != std::string_view::npos) a = a.substr(slash + 1);
    if (a.empty() || actor == "user" || actor == "editor") return {};
    const AgentProfile* p = agent(a);
    return p ? p->id : std::string();
}

std::vector<std::string> Studio::resolveAssignees(const std::vector<std::string>& refs) const {
    std::vector<std::string> out;
    for (const auto& ref : refs) {
        for (const auto& alt : str::split(ref, '|')) {
            std::string r = str::trim(alt);
            std::vector<std::string> found;
            if (r == "@all" || r == "*") {
                for (const auto& a : agents_) found.push_back(a.id);
            } else if (const AgentProfile* p = agent(r)) {
                found.push_back(p->id);
            } else {
                std::string key = r.size() > 1 && r[0] == '@' ? r.substr(1) : r;
                for (const auto& a : agents_) {
                    if (a.role == key || a.discipline == key) found.push_back(a.id);
                }
            }
            if (!found.empty()) {
                for (auto& f : found) addUnique(out, f);
                break;  // first alternative that matches wins
            }
        }
    }
    return out;
}

Result<AgentProfile> Studio::defineAgent(const Json& spec, const std::string& actor) {
    if (!spec.isObject()) return Error::make("invalid_arguments", "agent spec must be an object");
    std::string ref = spec.get("id").asString();
    if (ref.empty()) ref = spec.get("name").asString();
    if (ref.empty()) return Error::make("invalid_arguments", "an agent needs a name (or the id of an agent to update)");
    const AgentProfile* existing = agent(ref);
    Json doc;
    bool created = !existing;
    if (existing) {
        doc = existing->toJson();
        Json patch = spec;
        patch.erase("id");  // the id is the file name: stable across renames
        doc.mergePatch(patch);
        // Lists replace rather than merge.
        for (const char* k : {"focus_tags", "memory"}) {
            if (spec.contains(k)) doc[k] = spec.get(k);
        }
    } else {
        doc = spec;
        if (!doc.contains("role")) {
            return Error::make("invalid_arguments", "a new agent needs a role",
                               "studio_overview {include_catalog:true} lists roles, e.g. level_designer");
        }
    }
    // Validate role / discipline with did-you-mean hints.
    std::string role = doc.get("role").asString();
    const RoleInfo* info = findRole(role);
    if (!info) {
        std::string guess = str::closest(role, roleIds(), 4);
        return Error::make("invalid_arguments", "unknown role '" + role + "'",
                           guess.empty() ? "see studio_overview {include_catalog:true} for roles" : "did you mean '" + guess + "'?");
    }
    if (doc.get("discipline").asString().empty() || (spec.contains("role") && !spec.contains("discipline"))) {
        doc["discipline"] = info->discipline;
    }
    if (Status s = enumCheck("discipline", doc.get("discipline").asString(), disciplines()); !s) return s.error();
    if (created) {
        if (!doc.contains("autonomy")) doc["autonomy"] = info->autonomy;
        if (!doc.contains("color")) {
            size_t h = std::hash<std::string>{}(slugify(doc.get("name").asString(ref)));
            doc["color"] = kPalette[h % kPalette.size()];
        }
        if (!doc.contains("face")) doc["face"] = kFaces[agents_.size() % kFaces.size()];
    }
    if (!validColor(doc.get("color").asString("#7fb3ff"))) {
        return Error::make("invalid_arguments", "color must be \"#rrggbb\"");
    }
    if (doc.contains("face")) {
        if (Status s = enumCheck("face", doc.get("face").asString(), kFaces); !s) return s.error();
    }
    for (const auto& [k, v] : doc.get("permissions").members()) {
        if (v.isNull()) continue;
        if (!parseAccess(v.asString())) {
            return Error::make("invalid_arguments", "permission '" + k + "' must be inherit, allow, ask or off");
        }
    }
    auto profile = AgentProfile::fromJson(doc);
    if (!profile) return profile.error();
    if (existing) profile->id = existing->id;
    if (!profile->reportsTo.empty() && profile->reportsTo == profile->id) {
        return Error::make("invalid_arguments", "an agent cannot report to itself");
    }
    if (created && agent(profile->id)) return Error::make("conflict", "an agent with id '" + profile->id + "' exists");
    if (existing) {
        for (auto& a : agents_) {
            if (a.id == profile->id) a = *profile;
        }
    } else {
        agents_.push_back(*profile);
    }
    saveAgent(*profile);
    emit("agent", created ? "created" : "updated", profile->id, actor,
         (created ? "joined: " : "updated: ") + profile->name + " (" + profile->role + ")");
    return *profile;
}

Status Studio::removeAgent(std::string_view ref, const std::string& actor) {
    const AgentProfile* p = agent(ref);
    if (!p) return unknownAgent(ref);
    std::string id = p->id, name = p->name;
    std::error_code ec;
    std::string path = (fs::path(projectDir_) / "agents" / (id + ".agent.json")).string();
    fs::remove(path, ec);
    mtimes_.erase(path);
    std::erase_if(agents_, [&](const AgentProfile& a) { return a.id == id; });
    bool boardChanged = false;
    for (auto& t : tasks_) {
        if (t.assignee != id) continue;
        t.assignee.clear();
        if (t.status == "doing") t.status = "todo";
        t.comments.push_back({actor, timestamp(), "unassigned: " + name + " left the studio"});
        boardChanged = true;
    }
    if (boardChanged) saveBoard();
    emit("agent", "removed", id, actor, name + " left the studio");
    return {};
}

Status Studio::addMemory(const std::string& agentId, const std::string& note) {
    for (auto& a : agents_) {
        if (a.id != agentId) continue;
        std::string n = shortText(str::trim(note), 500);
        if (n.empty()) return Error::make("invalid_arguments", "empty note");
        a.memory.push_back(n);
        if (a.memory.size() > 50) a.memory.erase(a.memory.begin());
        saveAgent(a);
        return {};
    }
    return unknownAgent(agentId);
}

Status Studio::forgetMemory(const std::string& agentId, int number) {
    for (auto& a : agents_) {
        if (a.id != agentId) continue;
        if (number < 1 || number > static_cast<int>(a.memory.size())) {
            return Error::make("invalid_arguments", "no note number " + std::to_string(number),
                               "notes are numbered from 1 as listed in your instructions");
        }
        a.memory.erase(a.memory.begin() + (number - 1));
        saveAgent(a);
        return {};
    }
    return unknownAgent(agentId);
}

bool Studio::isDecider(const std::string& actor) const {
    std::string m = memberForActor(actor);
    if (m.empty()) return true;  // humans and unidentified external agents act for the human
    const AgentProfile* p = agent(m);
    return p && (p->discipline == "direction" || p->discipline == "production");
}

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

Task* Studio::task(std::string_view id) {
    std::string up = str::lower(id);
    for (auto& t : tasks_) {
        if (str::lower(t.id) == up) return &t;
    }
    if (int n = idNumber(id); n > 0) {  // "12" or "#12"
        for (auto& t : tasks_) {
            if (idNumber(t.id) == n) return &t;
        }
    }
    return nullptr;
}

Result<Task> Studio::createTask(const Json& spec, const std::string& actor) {
    Task t;
    t.title = str::trim(spec.get("title").asString());
    if (t.title.empty()) return Error::make("invalid_arguments", "a task needs a title");
    t.description = spec.get("description").asString();
    t.acceptance = toLines(spec.get("acceptance"));
    t.discipline = spec.get("discipline").asString();
    if (!t.discipline.empty()) {
        if (Status s = enumCheck("discipline", t.discipline, disciplines()); !s) return s.error();
    }
    t.status = spec.get("status").asString("todo");
    if (Status s = enumCheck("status", t.status, taskStatuses()); !s) return s.error();
    t.priority = spec.get("priority").asString("normal");
    if (Status s = enumCheck("priority", t.priority, priorities()); !s) return s.error();
    std::string assignee = spec.get("assignee").asString();
    if (!assignee.empty()) {
        if (const AgentProfile* p = agent(assignee)) {
            t.assignee = p->id;
        } else {
            // "@role": the matching agent with the fewest open tasks.
            auto candidates = resolveAssignees({assignee[0] == '@' ? assignee : "@" + assignee});
            if (candidates.empty()) return unknownAgent(assignee);
            auto load = [&](const std::string& id) {
                return std::count_if(tasks_.begin(), tasks_.end(), [&](const Task& x) { return x.assignee == id && taskOpen(x); });
            };
            t.assignee = *std::min_element(candidates.begin(), candidates.end(),
                                           [&](const std::string& a, const std::string& b) { return load(a) < load(b); });
        }
        if (t.discipline.empty()) t.discipline = agent(t.assignee)->discipline;
    }
    for (const auto& d : toStrings(spec.get("depends_on"))) {
        Task* dep = task(d);
        if (!dep) return Error::make("not_found", "dependency " + d + " is not on the board");
        t.dependsOn.push_back(dep->id);
    }
    for (const auto& [k, v] : spec.get("links").members()) {
        for (const auto& s : toStrings(v)) addUnique(t.links[k], s);
    }
    for (const auto& fid : toStrings(spec.get("feedback"))) {
        Feedback* f = feedbackItem(fid);
        if (!f) return Error::make("not_found", "no feedback " + fid);
        addUnique(t.links["feedback"], f->id);
    }
    t.id = nextId("T", nextTask_);
    t.createdBy = memberForActor(actor).empty() ? actor : memberForActor(actor);
    t.createdAt = t.updatedAt = timestamp();
    for (const auto& fid : t.links["feedback"]) {
        if (Feedback* f = feedbackItem(fid)) {
            addUnique(f->tasks, t.id);
            f->updatedAt = t.createdAt;
        }
    }
    tasks_.push_back(t);
    saveBoard();
    if (!t.links["feedback"].empty()) saveFeedback();
    emit("task", "created", t.id, actor, t.title + (t.assignee.empty() ? "" : " → @" + t.assignee),
         Json::object({{"status", t.status}}));
    return t;
}

void Studio::syncFeedbackFromTasks(const Task& t, const std::string& actor) {
    auto it = t.links.find("feedback");
    if (it == t.links.end()) return;
    bool changed = false;
    for (const auto& fid : it->second) {
        Feedback* f = feedbackItem(fid);
        if (!f) continue;
        if (t.status == "doing" && f->status == "accepted") {
            f->status = "in_progress";
            f->history.push_back({actor, timestamp(), "in progress: " + t.id + " started"});
            changed = true;
        }
        if (t.status == "done" && (f->status == "accepted" || f->status == "in_progress" || f->status == "regressed")) {
            bool allDone = true, anyDone = false;
            for (const auto& tid : f->tasks) {
                Task* x = task(tid);
                if (!x) continue;
                if (x->status == "done") anyDone = true;
                else if (x->status != "dropped") allDone = false;
            }
            if (allDone && anyDone) {
                f->status = "fixed";
                f->history.push_back({actor, timestamp(), "fixed: all tasks done; awaiting verification"});
                f->updatedAt = timestamp();
                changed = true;
                emit("feedback", "fixed", f->id, actor, f->summary);
            }
        }
    }
    if (changed) saveFeedback();
}

Result<Task> Studio::updateTask(std::string_view id, const Json& patch, const std::string& actor) {
    Task* t = task(id);
    if (!t) return Error::make("not_found", "no task " + std::string(id), "studio_task_list shows the board");
    Task next = *t;
    if (patch.contains("title")) next.title = patch.get("title").asString();
    if (patch.contains("description")) next.description = patch.get("description").asString();
    if (patch.contains("acceptance")) next.acceptance = toLines(patch.get("acceptance"));
    if (patch.contains("discipline")) {
        next.discipline = patch.get("discipline").asString();
        if (!next.discipline.empty()) {
            if (Status s = enumCheck("discipline", next.discipline, disciplines()); !s) return s.error();
        }
    }
    if (patch.contains("priority")) {
        next.priority = patch.get("priority").asString();
        if (Status s = enumCheck("priority", next.priority, priorities()); !s) return s.error();
    }
    if (patch.contains("assignee")) {
        std::string a = patch.get("assignee").asString();
        if (a.empty()) {
            next.assignee.clear();
        } else {
            const AgentProfile* p = agent(a);
            if (!p) return unknownAgent(a);
            next.assignee = p->id;
        }
    }
    if (patch.contains("depends_on")) {
        next.dependsOn.clear();
        for (const auto& d : toStrings(patch.get("depends_on"))) {
            Task* dep = task(d);
            if (!dep) return Error::make("not_found", "dependency " + d + " is not on the board");
            if (dep->id == next.id) return Error::make("invalid_arguments", "a task cannot depend on itself");
            next.dependsOn.push_back(dep->id);
        }
    }
    for (const auto& [k, v] : patch.get("links").members()) {
        for (const auto& s : toStrings(v)) addUnique(next.links[k], s);
    }
    std::string oldStatus = t->status;
    if (patch.contains("status")) {
        next.status = patch.get("status").asString();
        if (Status s = enumCheck("status", next.status, taskStatuses()); !s) return s.error();
        if ((next.status == "doing" || next.status == "done") && next.status != oldStatus) {
            for (const auto& d : next.dependsOn) {
                Task* dep = task(d);
                if (dep && dep->status != "done" && dep->status != "dropped") {
                    return Error::make("blocked", next.id + " is blocked by " + dep->id + " (" + dep->status + ": " + dep->title + ")",
                                       "finish or drop the dependency first, or message its assignee");
                }
            }
        }
    }
    std::string comment = patch.get("comment").asString();
    std::string who = memberForActor(actor).empty() ? actor : memberForActor(actor);
    if (!comment.empty()) next.comments.push_back({who, timestamp(), comment});
    if (next.status != oldStatus) next.comments.push_back({who, timestamp(), oldStatus + " → " + next.status});
    next.updatedAt = timestamp();
    *t = next;
    saveBoard();
    if (next.status != oldStatus) {
        emit("task", "status", next.id, actor, next.title + ": " + oldStatus + " → " + next.status,
             Json::object({{"status", next.status}}));
        syncFeedbackFromTasks(next, actor);
    } else {
        emit("task", "updated", next.id, actor, next.title + (comment.empty() ? "" : ": " + shortText(comment, 80)));
    }
    return *task(next.id);
}

Result<Task> Studio::claimTask(std::string_view id, const std::string& agentId, const std::string& actor) {
    Task* t = task(id);
    if (!t) return Error::make("not_found", "no task " + std::string(id), "studio_task_list shows the board");
    const AgentProfile* p = agent(agentId);
    if (!p) {
        return Error::make("invalid_arguments", "claiming needs a roster identity",
                           "pass agent (your roster id), connect as \"<client>/<agent id>\", or define yourself with "
                           "studio_agent_define");
    }
    if (t->status == "done" || t->status == "dropped") {
        return Error::make("invalid_state", t->id + " is already " + t->status);
    }
    if (!t->assignee.empty() && t->assignee != p->id && t->status == "doing") {
        return Error::make("conflict", t->id + " is being worked on by @" + t->assignee,
                           "message them with studio_message_send, or pick another task");
    }
    Json patch = Json::object({{"assignee", p->id}, {"status", "doing"}, {"comment", "claimed by @" + p->id}});
    auto r = updateTask(t->id, patch, actor);
    if (r) setPresence(p->id, "working", t->id + " " + t->title);
    return r;
}

// ---------------------------------------------------------------------------
// Feedback & decisions
// ---------------------------------------------------------------------------

Feedback* Studio::feedbackItem(std::string_view id) {
    std::string low = str::lower(id);
    for (auto& f : feedback_) {
        if (str::lower(f.id) == low) return &f;
    }
    if (int n = idNumber(id); n > 0) {
        for (auto& f : feedback_) {
            if (idNumber(f.id) == n) return &f;
        }
    }
    return nullptr;
}

Decision* Studio::decision(std::string_view id) {
    for (auto& d : decisions_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

Result<Feedback> Studio::submitFeedback(const Json& spec, const std::string& actor, bool* merged) {
    if (merged) *merged = false;
    Feedback f;
    f.summary = str::trim(spec.get("summary").asString());
    if (f.summary.empty()) return Error::make("invalid_arguments", "feedback needs a summary");
    f.category = spec.get("category").asString();
    if (Status s = enumCheck("category", f.category, feedbackCategories()); !s) return s.error();
    f.severity = spec.get("severity").asString("medium");
    if (Status s = enumCheck("severity", f.severity, severities()); !s) return s.error();
    f.details = spec.get("details").asString();
    f.target = spec.get("target").isString() ? spec.get("target").asString() : (spec.get("target").isNull() ? "" : spec.get("target").dump());
    if (spec.get("evidence").isObject()) f.evidence = spec.get("evidence");
    f.fingerprint = spec.get("fingerprint").asString();
    f.by = spec.get("by").asString();
    if (f.by.empty()) f.by = memberForActor(actor).empty() ? actor : memberForActor(actor);
    std::string ts = timestamp();

    if (!f.fingerprint.empty()) {
        static const std::vector<std::string> sevOrder = {"low", "medium", "high", "critical"};
        auto rank = [&](const std::string& s) { return std::find(sevOrder.begin(), sevOrder.end(), s) - sevOrder.begin(); };
        for (auto& existing : feedback_) {
            if (existing.fingerprint != f.fingerprint || existing.status == "verified") continue;
            existing.occurrences++;
            existing.evidence = f.evidence;
            existing.updatedAt = ts;
            if ((existing.status == "dropped" || existing.status == "deferred") && rank(f.severity) > rank(existing.severity)) {
                existing.status = "open";
                existing.history.push_back({actor, ts, "reopened: seen again and worse (" + f.severity + ")"});
            }
            if (rank(f.severity) > rank(existing.severity)) existing.severity = f.severity;
            if (existing.status == "open" || existing.status == "regressed") existing.summary = f.summary;
            saveFeedback();
            if (merged) *merged = true;
            emit("feedback", "seen_again", existing.id, actor, existing.summary,
                 Json::object({{"occurrences", existing.occurrences}, {"status", existing.status}}));
            return existing;
        }
    }
    f.id = nextId("F", nextFeedback_);
    f.createdAt = f.updatedAt = ts;
    f.history.push_back({actor, ts, "filed"});
    feedback_.push_back(f);
    saveFeedback();
    emit("feedback", "submitted", f.id, actor, "[" + f.category + "/" + f.severity + "] " + f.summary,
         Json::object({{"status", f.status}}));
    return f;
}

Result<Feedback> Studio::updateFeedback(std::string_view id, const Json& patch, const std::string& actor) {
    Feedback* f = feedbackItem(id);
    if (!f) return Error::make("not_found", "no feedback " + std::string(id), "studio_feedback_list shows feedback");
    Feedback next = *f;
    if (patch.contains("category")) {
        next.category = patch.get("category").asString();
        if (Status s = enumCheck("category", next.category, feedbackCategories()); !s) return s.error();
    }
    if (patch.contains("severity")) {
        next.severity = patch.get("severity").asString();
        if (Status s = enumCheck("severity", next.severity, severities()); !s) return s.error();
    }
    if (patch.contains("summary")) next.summary = patch.get("summary").asString();
    if (patch.contains("details")) next.details = patch.get("details").asString();
    if (patch.contains("target")) next.target = patch.get("target").asString();
    if (patch.get("evidence").isObject()) next.evidence.mergePatch(patch.get("evidence"));
    std::string note = patch.get("comment").asString();
    if (patch.contains("status")) {
        next.status = patch.get("status").asString();
        if (Status s = enumCheck("status", next.status, feedbackStatuses()); !s) return s.error();
        if (next.status != f->status) {
            next.history.push_back({actor, timestamp(), f->status + " → " + next.status + (note.empty() ? "" : ": " + note)});
            note.clear();
        }
    }
    if (!note.empty()) next.history.push_back({actor, timestamp(), note});
    next.updatedAt = timestamp();
    bool reopened = next.status == "regressed" && f->status != "regressed";
    *f = next;
    if (reopened) reopenTasksFor(*f, actor, "regressed");
    saveFeedback();
    emit("feedback", "updated", f->id, actor, f->summary, Json::object({{"status", f->status}}));
    return *f;
}

void Studio::reopenTasksFor(Feedback& f, const std::string& actor, const std::string& why) {
    bool changed = false;
    for (const auto& tid : f.tasks) {
        Task* t = task(tid);
        if (!t || t->status != "done") continue;
        t->status = "todo";
        t->comments.push_back({actor, timestamp(), "reopened: " + why});
        t->updatedAt = timestamp();
        changed = true;
        emit("task", "status", t->id, actor, t->title + ": done → todo (" + why + ")", Json::object({{"status", "todo"}}));
    }
    if (changed) saveBoard();
}

Result<Decision> Studio::decide(const Json& spec, const std::string& actor) {
    if (!isDecider(actor)) {
        const AgentProfile* p = agent(memberForActor(actor));
        return Error::make("permission_denied",
                           "only direction or production decides on feedback; @" + (p ? p->id : actor) + " is " +
                               (p ? p->discipline : "not a decider"),
                           "submit feedback or message @creative_director / @producer with studio_message_send");
    }
    std::string fid = spec.get("feedback").asString();
    Feedback* f = feedbackItem(fid);
    if (!f) return Error::make("not_found", "no feedback " + fid, "studio_feedback_list {status:\"open\"} shows what needs a decision");
    std::string verdict = spec.get("verdict").asString();
    if (Status s = enumCheck("verdict", verdict, {"act", "drop", "defer", "merge_into"}); !s) return s.error();
    std::string rationale = str::trim(spec.get("rationale").asString());
    if (rationale.empty()) {
        return Error::make("invalid_arguments", "a decision needs a rationale",
                           "say why in one or two sentences — it is shown next to the feedback for everyone");
    }
    bool redecide = spec.get("redecide").asBool(false);
    if (!needsDecision(*f) && f->status != "deferred" && !redecide) {
        return Error::make("invalid_state", f->id + " was already decided (" + f->status + (f->decision.empty() ? "" : ", " + f->decision) + ")",
                           "pass redecide: true to override, or studio_feedback_update to change its status");
    }
    Decision d;
    d.feedback = f->id;
    d.verdict = verdict;
    d.rationale = rationale;
    d.by = memberForActor(actor).empty() ? actor : memberForActor(actor);
    d.at = timestamp();
    d.metricsBefore = spec.get("metrics_before").isObject() ? spec.get("metrics_before") : latestMetrics_;
    d.playtestBefore = spec.get("playtest_before").asString(latestPlaytest_);
    if (spec.get("targets").isObject()) d.targets = spec.get("targets");

    std::vector<Task> created;
    if (verdict == "act") {
        const Json& specs = spec.get("tasks");
        std::vector<std::string> existing = toStrings(spec.get("task_ids"));
        if (specs.size() == 0 && existing.empty()) {
            return Error::make("invalid_arguments", "act needs tasks",
                               "pass tasks: [{title, acceptance, assignee, discipline}] or task_ids of existing tasks");
        }
        for (const auto& tid : existing) {
            if (!task(tid)) return Error::make("not_found", "no task " + tid);
        }
        // Validate every task before creating any (all-or-nothing).
        for (const auto& ts : specs.elements()) {
            if (str::trim(ts.get("title").asString()).empty()) return Error::make("invalid_arguments", "every task needs a title");
            std::string a = ts.get("assignee").asString();
            if (!a.empty() && !agent(a) && resolveAssignees({a[0] == '@' ? a : "@" + a}).empty()) return unknownAgent(a);
        }
        for (const auto& ts : specs.elements()) {
            Json t = ts;
            t["feedback"] = Json::array({f->id});
            if (!t.contains("priority")) {
                const std::string& sev = f->severity;
                t["priority"] = sev == "critical" ? "critical" : sev == "high" ? "high" : sev == "low" ? "low" : "normal";
            }
            if (!t.contains("description")) t["description"] = f->summary + (f->details.empty() ? "" : "\n\n" + f->details);
            auto r = createTask(t, actor);
            if (!r) return r.error();
            created.push_back(*r);
            d.tasks.push_back(r->id);
        }
        for (const auto& tid : existing) {
            Task* t = task(tid);
            addUnique(t->links["feedback"], f->id);
            addUnique(f->tasks, t->id);
            d.tasks.push_back(t->id);
        }
        if (!existing.empty()) saveBoard();
        f->status = "accepted";
        d.effect = Json::object({{"status", "pending"}});
    } else if (verdict == "drop") {
        f->status = "dropped";
        d.effect = Json::object({{"status", "n/a"}});
    } else if (verdict == "defer") {
        f->status = "deferred";
        d.effect = Json::object({{"status", "n/a"}});
    } else {  // merge_into
        std::string into = spec.get("merge_into").asString();
        Feedback* target = feedbackItem(into);
        if (!target) return Error::make("not_found", "merge_into: no feedback " + into);
        if (target->id == f->id) return Error::make("invalid_arguments", "cannot merge feedback into itself");
        d.mergeInto = target->id;
        f->status = "dropped";
        f->mergedInto = target->id;
        target->occurrences += f->occurrences;
        target->history.push_back({actor, d.at, "merged " + f->id + ": " + f->summary});
        d.effect = Json::object({{"status", "n/a"}});
    }
    d.id = nextId("D", nextDecision_);
    f->decision = d.id;
    for (const auto& t : d.tasks) addUnique(f->tasks, t);
    f->history.push_back({actor, d.at, verdict + ": " + rationale});
    f->updatedAt = d.at;
    decisions_.push_back(d);
    saveDecisions();
    saveFeedback();
    emit("decision", verdict, d.id, actor, f->id + " " + verdict + ": " + shortText(rationale, 120),
         Json::object({{"feedback", f->id}, {"verdict", verdict}, {"tasks", strArray(d.tasks)}}));
    return d;
}

Result<Json> Studio::recordEffect(std::string_view feedbackId, const Json& before, const Json& after,
                                  const std::string& playtestBefore, const std::string& playtestAfter,
                                  const std::string& actor) {
    Feedback* f = feedbackItem(feedbackId);
    if (!f) return Error::make("not_found", "no feedback " + std::string(feedbackId));
    Decision* d = decision(f->decision);
    Json comparison = compareMetrics(before, after);
    Json targets = d ? d->targets : Json::object();
    std::vector<std::string> metrics = metricsForCategory(f->category);
    // Categories without metrics (visuals, audio, narrative) need a human or critic to verify.
    std::string verdict = metrics.empty() && targets.members().empty() ? "unmeasured" : effectVerdict(comparison, metrics, targets);
    Json relevant = Json::object();
    for (const auto& [k, row] : comparison.members()) {
        if (contains(metrics, k) || targets.contains(k)) relevant[k] = row;
    }
    Json effect = Json::object({{"status", verdict},
                                {"metrics", relevant},
                                {"playtest_before", playtestBefore},
                                {"playtest_after", playtestAfter},
                                {"measured_at", timestamp()},
                                {"measured_by", actor}});
    if (!targets.members().empty()) {
        std::string unmet;
        effect["targets_met"] = targetsMet(after, targets, &unmet);
        if (!unmet.empty()) effect["unmet"] = unmet;
    }
    if (d) d->effect = effect;
    std::string old = f->status;
    if (verdict == "improved") {
        f->status = "verified";
    } else if (verdict == "regressed") {
        f->status = "regressed";
        reopenTasksFor(*f, actor, "the fix made " + f->category + " metrics worse");
    }
    std::string summary;
    for (const auto& [k, row] : relevant.members()) {
        summary += (summary.empty() ? "" : ", ") + k + " " + row.get("before").dump() + "→" + row.get("after").dump();
    }
    f->history.push_back({actor, timestamp(), "effect " + verdict + (summary.empty() ? "" : ": " + summary)});
    f->updatedAt = timestamp();
    saveFeedback();
    if (d) saveDecisions();
    emit("feedback", "effect", f->id, actor, f->summary + " — " + verdict + (summary.empty() ? "" : " (" + summary + ")"),
         Json::object({{"status", f->status}, {"effect", verdict}, {"previous_status", old}}));
    return effect;
}

// ---------------------------------------------------------------------------
// Playtests
// ---------------------------------------------------------------------------

std::string Studio::nextPlaytestId() {
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::path(studioDir()) / "playtests", ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        nextPlaytest_ = std::max(nextPlaytest_, idNumber(it->path().filename().string()) + 1);
    }
    return nextId("P", nextPlaytest_);
}

std::string Studio::playtestDir(std::string_view id) const {
    return (fs::path(studioDir()) / "playtests" / std::string(id)).string();
}

void Studio::notePlaytest(const std::string& id, const Json& metrics, const std::string& actor, const std::string& summary) {
    latestPlaytest_ = id;
    latestMetrics_ = metrics;
    saveUsage();
    emit("playtest", "finished", id, actor, summary, Json::object({{"metrics", metrics}}));
}

Result<Json> Studio::loadPlaytestReport(std::string_view id) const {
    std::string ref(id);
    if (int n = idNumber(ref); n > 0) ref = "P-" + std::to_string(n);
    bool ok = false;
    std::string text = readText(playtestDir(ref) + "/report.json", &ok);
    if (!ok) return Error::make("not_found", "no playtest " + std::string(id), "playtest_run returns the id of each run");
    return Json::parse(text);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

Result<Message> Studio::sendMessage(const Json& spec, const std::string& actor) {
    Message m;
    m.text = str::trim(spec.get("text").asString());
    if (m.text.empty()) return Error::make("invalid_arguments", "a message needs text");
    m.channel = spec.get("channel").asString("general");
    if (!m.channel.empty() && m.channel[0] == '#') m.channel = m.channel.substr(1);
    m.channel = slugify(m.channel);
    if (m.channel.empty()) m.channel = "general";
    for (const auto& ref : toStrings(spec.get("to"))) {
        auto ids = resolveAssignees({ref});
        if (ids.empty()) return unknownAgent(ref);
        for (const auto& id : ids) addUnique(m.to, id);
    }
    // @mentions in the text: @id, @role, @discipline, @all.
    for (size_t i = 0; i < m.text.size(); ++i) {
        if (m.text[i] != '@' || (i > 0 && (std::isalnum(static_cast<unsigned char>(m.text[i - 1])) || m.text[i - 1] == '_'))) continue;
        size_t j = i + 1;
        while (j < m.text.size() && (std::isalnum(static_cast<unsigned char>(m.text[j])) || m.text[j] == '_' || m.text[j] == '-')) ++j;
        if (j == i + 1) continue;
        for (const auto& id : resolveAssignees({m.text.substr(i, j - i)})) addUnique(m.mentions, id);
    }
    std::string replyTo = spec.get("reply_to").asString();
    if (!replyTo.empty()) {
        const Message* parent = nullptr;
        for (const auto& x : messages_) {
            if (x.id == replyTo) parent = &x;
        }
        if (!parent) return Error::make("not_found", "no message " + replyTo);
        m.thread = parent->thread.empty() ? parent->id : parent->thread;
        if (!spec.contains("channel")) m.channel = parent->channel;
        addUnique(m.to, parent->from);
    }
    for (const char* k : {"task", "feedback", "decision", "loop"}) {
        if (spec.contains(k)) m.refs[k] = spec.get(k);
    }
    m.from = spec.get("from").asString();
    if (m.from.empty()) m.from = memberForActor(actor).empty() ? actor : memberForActor(actor);
    std::erase(m.to, m.from);
    m.id = nextId("M", nextMessage_);
    m.at = timestamp();
    messages_.push_back(m);
    appendMessage(m);
    std::string who = m.to.empty() ? "#" + m.channel : "@" + m.to.front() + (m.to.size() > 1 ? " +" + std::to_string(m.to.size() - 1) : "");
    emit("message", "sent", m.id, actor, m.from + " → " + who + ": " + shortText(m.text, 100),
         Json::object({{"channel", m.channel}, {"to", strArray(m.to)}, {"mentions", strArray(m.mentions)}}));
    return m;
}

std::vector<const Message*> Studio::inbox(const std::string& agentId, bool unreadOnly, size_t limit, bool markRead) {
    std::vector<const Message*> out;
    int cursor = unreadOnly ? idNumber(readCursor_[agentId]) : 0;
    for (const auto& m : messages_) {
        if (idNumber(m.id) <= cursor || m.from == agentId) continue;
        bool forMe = agentId.empty() || contains(m.to, agentId) || contains(m.mentions, agentId) ||
                     (m.to.empty() && m.mentions.empty() && m.channel == "general");
        if (forMe) out.push_back(&m);
    }
    if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
    if (markRead && !agentId.empty() && !out.empty()) {
        readCursor_[agentId] = out.back()->id;
        saveUsage();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Usage & presence
// ---------------------------------------------------------------------------

void Studio::recordUsage(const std::string& agentId, const std::string& model, int64_t input, int64_t output,
                         int64_t cacheRead, int64_t cacheWrite) {
    Usage& u = usage_[agentId];
    u.inputTokens += input;
    u.outputTokens += output;
    u.cacheReadTokens += cacheRead;
    u.cacheWriteTokens += cacheWrite;
    u.requests += 1;
    Price p = priceFor(model);
    u.costUsd += (static_cast<double>(input) * p.input + static_cast<double>(output) * p.output +
                  static_cast<double>(cacheRead) * p.cacheRead + static_cast<double>(cacheWrite) * p.cacheWrite) /
                 1e6;
    u.lastActive = timestamp();
    saveUsage();
    emit("usage", "recorded", agentId, "agent:" + agentId, std::to_string(input + output + cacheRead + cacheWrite) + " tokens",
         Json::object({{"usage", u.toJson()}}));
}

void Studio::noteToolCall(std::string_view actor, std::string_view tool, bool ok) {
    (void)tool;
    if (actor == "user" || actor == "editor" || actor.empty()) return;
    std::string member = memberForActor(actor);
    Usage& u = usage_[member.empty() ? std::string(actor) : member];
    u.toolCalls += 1;
    if (!ok) u.toolErrors += 1;
    u.lastActive = timestamp();
    // Tool calls are frequent: batched, persisted at most every few seconds.
    usageDirty_ = true;
    if (now() - lastUsageSave_ > 5.0) saveUsage();
}

int64_t Studio::totalTokens() const {
    int64_t t = 0;
    for (const auto& [k, u] : usage_) t += u.inputTokens + u.outputTokens + u.cacheReadTokens + u.cacheWriteTokens;
    return t;
}

void Studio::setPresence(const std::string& agentId, const std::string& status, const std::string& activity) {
    presence_[agentId] = Json::object({{"status", status}, {"activity", activity}, {"since", timestamp()}});
    emit("agent", "presence", agentId, "agent:" + agentId, status + (activity.empty() ? "" : ": " + activity),
         Json::object({{"status", status}, {"activity", activity}}));
}

Json Studio::presence(const std::string& agentId) const {
    auto it = presence_.find(agentId);
    if (it != presence_.end() && it->second.get("status").asString() != "idle") return it->second;
    Json p = Json::object({{"status", "idle"}});
    auto u = usage_.find(agentId);
    if (u != usage_.end() && !u->second.lastActive.empty()) p["last_active"] = u->second.lastActive;
    return p;
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------

Json Studio::overview() const {
    Json roster = Json::array();
    for (const auto& a : agents_) {
        int64_t open = std::count_if(tasks_.begin(), tasks_.end(), [&](const Task& t) { return t.assignee == a.id && taskOpen(t); });
        Json p = presence(a.id);
        roster.push(Json::object({{"id", a.id},
                                  {"name", a.name},
                                  {"role", a.role},
                                  {"discipline", a.discipline},
                                  {"focus", a.focus},
                                  {"status", p.get("status")},
                                  {"activity", p.get("activity")},
                                  {"open_tasks", open}}));
    }
    Json board = Json::object();
    for (const auto& s : taskStatuses()) {
        board[s] = static_cast<int64_t>(std::count_if(tasks_.begin(), tasks_.end(), [&](const Task& t) { return t.status == s; }));
    }
    Json fb = Json::object();
    for (const auto& s : feedbackStatuses()) {
        fb[s] = static_cast<int64_t>(std::count_if(feedback_.begin(), feedback_.end(), [&](const Feedback& f) { return f.status == s; }));
    }
    Json needs = Json::array();
    for (const auto& f : feedback_) {
        if (needsDecision(f)) needs.push(f.id + " [" + f.category + "/" + f.severity + "] " + shortText(f.summary, 100));
    }
    Json loops = Json::array();
    for (const auto& l : loops_) {
        loops.push(Json::object({{"name", l.name},
                                 {"status", l.state.get("status").asString("idle")},
                                 {"iteration", l.state.get("iteration")},
                                 {"stage", l.state.get("stage")}}));
    }
    Usage total;
    double cost = 0;
    for (const auto& [k, u] : usage_) {
        total.inputTokens += u.inputTokens;
        total.outputTokens += u.outputTokens;
        total.cacheReadTokens += u.cacheReadTokens;
        total.cacheWriteTokens += u.cacheWriteTokens;
        total.requests += u.requests;
        total.toolCalls += u.toolCalls;
        cost += u.costUsd;
    }
    total.costUsd = cost;
    Json recent = Json::array();
    for (size_t i = decisions_.size() > 5 ? decisions_.size() - 5 : 0; i < decisions_.size(); ++i) {
        const auto& d = decisions_[i];
        recent.push(Json::object({{"id", d.id},
                                  {"feedback", d.feedback},
                                  {"verdict", d.verdict},
                                  {"rationale", shortText(d.rationale, 120)},
                                  {"effect", d.effect.get("status")}}));
    }
    return Json::object({{"project", projectDir_},
                         {"roster", roster},
                         {"board", board},
                         {"feedback", fb},
                         {"needs_decision", needs},
                         {"recent_decisions", recent},
                         {"loops", loops},
                         {"latest_playtest", latestPlaytest_},
                         {"latest_metrics", latestMetrics_},
                         {"messages", static_cast<int64_t>(messages_.size())},
                         {"usage", total.toJson()}});
}

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

Loop* Studio::loop(std::string_view name) {
    std::string n = slugify(name);
    std::replace(n.begin(), n.end(), '-', '_');
    for (auto& l : loops_) {
        if (l.name == n) return &l;
    }
    return nullptr;
}

Result<Loop> Studio::defineLoop(const Json& specIn, const std::string& actor) {
    Json spec = specIn;
    std::string tmpl = spec.get("template").asString();
    Json doc = Json::object();
    if (!tmpl.empty()) {
        doc = loopTemplate(tmpl);
        if (!doc.isObject()) {
            std::string guess = str::closest(tmpl, loopTemplateNames(), 4);
            return Error::make("not_found", "no loop template '" + tmpl + "'",
                               guess.empty() ? "templates: playtest_fix_verify, art_pass_with_critic, balance_tuning, "
                                               "vertical_slice_sprint, bug_bash"
                                             : "did you mean '" + guess + "'?");
        }
        if (!spec.contains("name")) spec["name"] = tmpl;
    } else if (const Loop* existing = loop(spec.get("name").asString())) {
        doc = existing->toJson();  // partial update of an existing loop
        doc.erase("state");
    }
    // Stages replace wholesale; stop conditions merge.
    Json stop = doc.get("stop");
    if (spec.get("stop").isObject()) {
        if (!stop.isObject()) stop = Json::object();
        stop.mergePatch(spec.get("stop"));
    }
    for (const auto& [k, v] : spec.members()) {
        if (k != "stop") doc[k] = v;
    }
    if (stop.isObject()) doc["stop"] = stop;
    doc.erase("state");
    auto l = Loop::fromJson(doc);
    if (!l) return l.error();
    Loop* existing = loop(l->name);
    if (existing) {
        std::string st = existing->state.get("status").asString("idle");
        if (st == "running" || st == "awaiting_approval") {
            return Error::make("invalid_state", "loop '" + l->name + "' is " + st, "stop it with studio_loop_stop first");
        }
        l->state = existing->state;
        *existing = *l;
    } else {
        l->state = Json::object({{"status", "idle"}, {"runs", 0}});
        loops_.push_back(*l);
    }
    saveLoop(*l);
    emit("loop", existing ? "updated" : "defined", l->name, actor, l->name + ": " + std::to_string(l->stages.size()) + " stages");
    return *l;
}

Status Studio::removeLoop(std::string_view name, const std::string& actor) {
    Loop* l = loop(name);
    if (!l) return Error::make("not_found", "no loop " + std::string(name));
    std::string n = l->name;
    std::error_code ec;
    std::string path = studioDir() + "/loops/" + n + ".loop.json";
    fs::remove(path, ec);
    mtimes_.erase(path);
    std::erase_if(loops_, [&](const Loop& x) { return x.name == n; });
    emit("loop", "removed", n, actor, n + " removed");
    return {};
}

Json& Studio::currentIteration(Loop& L) {
    Json& its = L.state["iterations"];
    if (!its.isArray()) its = Json::array();
    if (its.size() == 0) its.push(Json::object());
    return its.elements().back();
}

Json Studio::loopStatus(const Loop& L, bool withHistory) const {
    const Json& st = L.state;
    Json out = Json::object({{"loop", L.name},
                             {"goal", st.get("goal").asString(L.goal)},
                             {"status", st.get("status").asString("idle")},
                             {"iteration", st.get("iteration")},
                             {"max_iterations", st.get("max_iterations").isNull() ? Json(L.stop.maxIterations) : st.get("max_iterations")},
                             {"stage", st.get("stage")},
                             {"stage_index", st.get("stage_index")}});
    std::string status = st.get("status").asString("idle");
    if (status == "running") {
        const Json& pending = st.get("pending");
        out["assignments"] = pending;
        int idx = static_cast<int>(st.get("stage_index").asInt());
        if (idx >= 0 && idx < static_cast<int>(L.stages.size())) out["parallel"] = L.stages[static_cast<size_t>(idx)].parallel;
        Json who = Json::array();
        for (const auto& a : pending.elements()) who.push(a.get("agent"));
        out["next"] = pending.size() ? "run each assignment's prompt as that agent (actor agent:<id> or "
                                       "mcp:<client>/<id>), then call studio_loop_advance with their reports"
                                     : "call studio_loop_advance";
        out["waiting_for"] = who;
    } else if (status == "awaiting_approval") {
        out["next"] = "a human must approve the next stage: studio_loop_advance {approve: true}";
    }
    if (!st.get("stop_reason").isNull()) out["stop_reason"] = st.get("stop_reason");
    if (st.get("trends").isObject()) out["trends"] = st.get("trends");
    if (st.get("metrics").isObject()) out["metrics"] = st.get("metrics");
    out["tokens_used"] = std::max<int64_t>(0, totalTokens() - st.get("tokens_at_start").asInt(totalTokens()));
    if (withHistory && st.get("iterations").isArray()) out["iterations"] = st.get("iterations");
    Json stages = Json::array();
    for (const auto& s : L.stages) {
        stages.push(Json::object({{"id", s.id}, {"title", s.title}, {"kind", s.kind}, {"parallel", s.parallel}}));
    }
    out["stages"] = stages;
    return out;
}

bool Studio::budgetExhausted(const Loop& L, std::string& why) const {
    const Json& st = L.state;
    if (L.stop.tokenBudget > 0) {
        int64_t used = totalTokens() - st.get("tokens_at_start").asInt();
        if (used >= L.stop.tokenBudget) {
            why = "token budget exhausted (" + std::to_string(used) + " / " + std::to_string(L.stop.tokenBudget) + ")";
            return true;
        }
    }
    if (L.stop.timeBudgetMinutes > 0) {
        double elapsed = now() - st.get("started_epoch").asNumber(now());
        if (elapsed >= L.stop.timeBudgetMinutes * 60) {
            why = "time budget exhausted (" + fmtNumber(elapsed / 60) + " min)";
            return true;
        }
    }
    return false;
}

Result<Json> Studio::startLoop(std::string_view name, const Json& overrides, const std::string& actor) {
    Loop* L = loop(name);
    if (!L) {
        std::vector<std::string> names;
        for (const auto& l : loops_) names.push_back(l.name);
        std::string guess = str::closest(std::string(name), names, 4);
        bool isTemplate = contains(loopTemplateNames(), std::string(name));
        return Error::make("not_found", "no loop '" + std::string(name) + "'",
                           !guess.empty() ? "did you mean '" + guess + "'?"
                           : isTemplate   ? "define it first: studio_loop_define {template: \"" + std::string(name) + "\"}"
                                          : "studio_loop_define creates loops (from templates or from scratch)");
    }
    std::string status = L->state.get("status").asString("idle");
    if (status == "running" || status == "awaiting_approval") {
        return Error::make("invalid_state", "loop '" + L->name + "' is already " + status,
                           "continue with studio_loop_advance, check studio_loop_status, or studio_loop_stop it");
    }
    if (agents_.empty()) {
        bool anyAgents = std::any_of(L->stages.begin(), L->stages.end(), [](const LoopStage& s) { return s.kind == "agents"; });
        if (anyAgents) {
            return Error::make("invalid_state", "the studio has no agents",
                               "spawn a team with studio_team_template or define agents with studio_agent_define");
        }
    }
    Json prev = L->state;
    int runs = static_cast<int>(prev.get("runs").asInt()) + 1;
    Json previousRuns = prev.get("previous_runs").isArray() ? prev.get("previous_runs") : Json::array();
    if (prev.contains("iterations")) {
        previousRuns.push(Json::object({{"run", prev.get("run")},
                                        {"ended_at", prev.get("ended_at")},
                                        {"status", prev.get("status")},
                                        {"stop_reason", prev.get("stop_reason")},
                                        {"iterations", prev.get("iteration")},
                                        {"metrics", prev.get("metrics")}}));
        while (previousRuns.size() > 10) previousRuns.elements().erase(previousRuns.elements().begin());
    }
    int maxIt = static_cast<int>(std::clamp<int64_t>(overrides.get("max_iterations").asInt(L->stop.maxIterations), 1, 100));
    L->state = Json::object({{"status", "running"},
                             {"run", runs},
                             {"runs", runs},
                             {"goal", overrides.get("goal").asString(L->goal)},
                             {"started_at", timestamp()},
                             {"started_epoch", now()},
                             {"started_by", actor},
                             {"iteration", 1},
                             {"max_iterations", maxIt},
                             {"stage_index", 0},
                             {"stage", L->stages.front().id},
                             {"pending", Json::array()},
                             {"tokens_at_start", totalTokens()},
                             {"trends", Json::object()},
                             {"iterations", Json::array({Json::object({{"n", 1}, {"started_at", timestamp()}, {"stages", Json::array()},
                                                                       {"signoffs", Json::array()}})})},
                             {"previous_runs", previousRuns}});
    emit("loop", "started", L->name, actor, L->name + " run " + std::to_string(runs) + ": " + L->state.get("goal").asString(),
         Json::object({{"status", "running"}, {"iteration", 1}}));
    return enterStages(*L, actor);
}

bool Studio::shouldSkip(const Loop& L, const LoopStage& stage, std::string& why) const {
    std::vector<std::string> conds = toStrings(stage.gate.get("skip_if"));
    int iteration = static_cast<int>(L.state.get("iteration").asInt(1));
    for (const auto& c : conds) {
        if (c == "first_iteration" && iteration == 1) {
            why = "first iteration";
            return true;
        }
        if (c == "not_first_iteration" && iteration > 1) {
            why = "only runs in the first iteration";
            return true;
        }
        if (c == "no_open_feedback" && std::none_of(feedback_.begin(), feedback_.end(), needsDecision)) {
            why = "no feedback waits for a decision";
            return true;
        }
        if (c == "no_todo_tasks" &&
            std::none_of(tasks_.begin(), tasks_.end(), [](const Task& t) { return t.status == "todo" || t.status == "doing"; })) {
            why = "no open tasks";
            return true;
        }
        if (c == "no_fixed_feedback" &&
            std::none_of(feedback_.begin(), feedback_.end(), [](const Feedback& f) { return f.status == "fixed"; })) {
            why = "nothing fixed to verify";
            return true;
        }
    }
    return false;
}

std::vector<Assignment> Studio::buildAssignments(Loop& L, const LoopStage& stage, const std::string& actor) {
    std::vector<std::string> ids = resolveAssignees(stage.assignees);
    std::map<std::string, std::vector<std::string>> taskMap;
    if (stage.onlyWithTasks) {
        // Hand unassigned work to the matching candidate with the least open work.
        bool boardChanged = false;
        auto load = [&](const std::string& id) {
            return std::count_if(tasks_.begin(), tasks_.end(), [&](const Task& t) { return t.assignee == id && taskOpen(t); });
        };
        for (auto& t : tasks_) {
            if (!t.assignee.empty() || t.status != "todo") continue;
            std::vector<std::string> fit;
            for (const auto& id : ids) {
                const AgentProfile* p = agent(id);
                if (p && (t.discipline.empty() || p->discipline == t.discipline)) fit.push_back(id);
            }
            if (fit.empty()) continue;
            std::string best = *std::min_element(fit.begin(), fit.end(),
                                                 [&](const std::string& a, const std::string& b) { return load(a) < load(b); });
            t.assignee = best;
            t.comments.push_back({actor, timestamp(), "auto-assigned to @" + best + " by loop " + L.name});
            boardChanged = true;
        }
        if (boardChanged) saveBoard();
        std::vector<std::string> withTasks;
        for (const auto& id : ids) {
            for (const auto& t : tasks_) {
                if (t.assignee == id && (t.status == "todo" || t.status == "doing")) taskMap[id].push_back(t.id);
            }
            if (!taskMap[id].empty()) withTasks.push_back(id);
        }
        ids = withTasks;
    } else {
        for (const auto& id : ids) {
            for (const auto& t : tasks_) {
                if (t.assignee == id && (t.status == "todo" || t.status == "doing")) taskMap[id].push_back(t.id);
            }
        }
    }
    std::vector<Assignment> out;
    for (const auto& id : ids) {
        const AgentProfile* p = agent(id);
        if (!p) continue;
        Assignment a;
        a.agent = id;
        a.stage = stage.id;
        a.tasks = taskMap[id];
        a.prompt = renderPrompt(L, stage, *p, a.tasks);
        out.push_back(std::move(a));
    }
    return out;
}

std::string Studio::renderPrompt(const Loop& L, const LoopStage& stage, const AgentProfile& agentProfile,
                                 const std::vector<std::string>& myTasks) const {
    const Json& st = L.state;
    int iteration = static_cast<int>(st.get("iteration").asInt(1));
    const Json& iters = st.get("iterations");
    const Json& cur = iters.size() ? iters[iters.size() - 1] : Json::null();

    auto stageReports = [&](const Json& iter, const std::vector<std::string>& only) {
        std::string s;
        for (const auto& rec : iter.get("stages").elements()) {
            const std::string& sid = rec.get("id").asString();
            if (sid == stage.id) continue;
            if (!only.empty() && !contains(only, sid)) continue;
            if (rec.get("status").asString() == "skipped") continue;
            if (rec.contains("summary")) s += "- [" + rec.get("title").asString(sid) + "] " + rec.get("summary").asString() + "\n";
            for (const auto& r : rec.get("reports").elements()) {
                s += "- [" + rec.get("title").asString(sid) + "] @" + r.get("agent").asString() + ": " +
                     shortText(r.get("report").asString(), 1500) + "\n";
            }
        }
        return s;
    };
    auto feedbackList = [&](auto pred) {
        std::string s;
        for (const auto& f : feedback_) {
            if (!pred(f)) continue;
            s += "- " + f.id + " [" + f.category + "/" + f.severity + "] " + f.summary;
            if (!f.target.empty()) s += " (target: " + f.target + ")";
            if (f.occurrences > 1) s += " ×" + std::to_string(f.occurrences);
            if (f.status == "regressed") s += " — REGRESSED after " + f.decision;
            if (f.evidence.contains("playtest")) s += " — evidence: " + f.evidence.get("playtest").asString();
            s += " — by " + f.by + "\n";
        }
        return s.empty() ? std::string("(none)\n") : s;
    };
    auto taskLine = [&](const Task& t) {
        std::string s = "- " + t.id + " [" + t.priority + ", " + t.status + "] " + t.title;
        if (!t.acceptance.empty()) {
            s += "\n  acceptance:";
            for (const auto& a : t.acceptance) s += "\n    • " + a;
        }
        if (!t.description.empty()) s += "\n  details: " + shortText(t.description, 400);
        auto fl = t.links.find("feedback");
        if (fl != t.links.end() && !fl->second.empty()) {
            s += "\n  feedback:";
            for (const auto& f : fl->second) s += " " + f;
        }
        return s + "\n";
    };

    std::map<std::string, std::string> vars;
    vars["goal"] = st.get("goal").asString(L.goal);
    vars["loop"] = L.name;
    vars["iteration"] = std::to_string(iteration);
    vars["max_iterations"] = std::to_string(st.get("max_iterations").asInt(L.stop.maxIterations));
    vars["stage"] = stage.title;
    vars["agent"] = agentProfile.name;
    vars["role"] = agentProfile.role;
    vars["focus"] = agentProfile.focus.empty() ? "(general)" : agentProfile.focus;
    std::string inputs = stageReports(cur, stage.inputs);
    vars["inputs"] = inputs.empty() ? "(none yet)" : inputs;
    std::string previous = iters.size() > 1 ? stageReports(iters[iters.size() - 2], {}) : "";
    vars["previous"] = previous.empty() ? "(first iteration)" : previous;
    vars["open_feedback"] = feedbackList([](const Feedback& f) { return needsDecision(f); });
    vars["fixed_feedback"] = feedbackList([](const Feedback& f) { return f.status == "fixed"; });
    std::string mine;
    for (const auto& id : myTasks) {
        for (const auto& t : tasks_) {
            if (t.id == id) mine += taskLine(t);
        }
    }
    vars["my_tasks"] = mine.empty() ? "(none)" : mine;
    std::string todo;
    for (const auto& t : tasks_) {
        if (t.status == "todo" || t.status == "doing") todo += taskLine(t);
    }
    vars["todo_tasks"] = todo.empty() ? "(none)" : todo;
    vars["metrics"] = latestMetrics_.members().empty() ? "(no playtest yet)" : latestMetrics_.dump();
    vars["trends"] = st.get("trends").dump();
    std::string roster;
    for (const auto& a : agents_) {
        roster += "- " + a.id + " — " + a.name + " (" + a.role + (a.focus.empty() ? "" : "; " + a.focus) + ")\n";
    }
    vars["roster"] = roster;
    for (auto& [k, v] : vars) {
        while (!v.empty() && v.back() == '\n') v.pop_back();  // templates add their own spacing
    }

    std::string body;
    const std::string& tpl = stage.instruction;
    for (size_t i = 0; i < tpl.size();) {
        if (tpl.compare(i, 2, "{{") == 0) {
            size_t end = tpl.find("}}", i + 2);
            if (end != std::string::npos) {
                std::string key = str::trim(tpl.substr(i + 2, end - i - 2));
                auto it = vars.find(key);
                if (it != vars.end()) {
                    body += it->second;
                    i = end + 2;
                    continue;
                }
            }
        }
        body += tpl[i++];
    }
    std::string header = "Studio loop \"" + L.name + "\" — iteration " + vars["iteration"] + " of " + vars["max_iterations"] +
                         ", stage \"" + stage.title + "\".\nGoal: " + vars["goal"] + "\nYou are @" + agentProfile.id + " (" +
                         agentProfile.role + ", focus: " + vars["focus"] + ").\n\n";
    std::string footer =
        "\n\nWhen you are done, reply with a short report: what you did, what you found, what is still open. It is passed "
        "to the next stage.";
    if (agentProfile.discipline == "direction" || agentProfile.discipline == "production") {
        if (L.stop.directorSignoff) {
            footer += " If the goal is fully met, start your report with SIGNOFF.";
        }
    }
    return header + body + footer;
}

Json Studio::runPlaytestStage(Loop& L, const LoopStage& stage, const std::string& actor) {
    Json rec = Json::object();
    if (!playtest_) {
        rec["status"] = "failed";
        rec["summary"] = "no playtest runner is available in this client";
        return rec;
    }
    Json args = stage.playtest.isObject() ? stage.playtest : Json::object();
    args["label"] = L.name + " · iteration " + std::to_string(L.state.get("iteration").asInt(1)) + " · " + stage.title;
    std::string loopActor = "studio:loop/" + L.name;
    auto r = playtest_(args, loopActor);
    if (!r) {
        rec["status"] = "failed";
        rec["summary"] = "playtest failed: " + r.error().message;
        return rec;
    }
    const Json& report = *r;
    std::string pid = report.get("id").asString();
    rec["playtest"] = pid;
    rec["metrics"] = report.get("metrics");
    std::string summary = report.get("summary").asString();
    Json verified = Json::array();
    if (stage.verifyFixed) verifyFixedFeedback(pid, report.get("metrics"), loopActor, verified);
    if (verified.size()) rec["verified"] = verified;
    Json filed = Json::array();
    if (stage.fileFeedback) {
        for (const auto& finding : report.get("findings").elements()) {
            Json spec = finding;
            spec["by"] = "bot:" + report.get("config").get("policy").asString("bot");
            bool merged = false;
            auto f = submitFeedback(spec, loopActor, &merged);
            if (f) filed.push(Json::object({{"id", f->id}, {"merged", merged}, {"summary", f->summary}}));
        }
    }
    if (filed.size()) rec["filed"] = filed;
    std::string findings;
    for (const auto& f : filed.elements()) findings += "\n  • " + f.get("id").asString() + " " + f.get("summary").asString();
    std::string verdicts;
    for (const auto& v : verified.elements()) {
        verdicts += "\n  • " + v.get("feedback").asString() + " effect: " + v.get("effect").asString();
    }
    rec["summary"] = pid + ": " + summary + (findings.empty() ? "" : "\n  findings:" + findings) +
                     (verdicts.empty() ? "" : "\n  verification:" + verdicts);
    rec["status"] = "done";
    (void)actor;
    return rec;
}

void Studio::verifyFixedFeedback(const std::string& playtestId, const Json& metrics, const std::string& actor, Json& out) {
    std::vector<std::string> ids;
    for (const auto& f : feedback_) {
        if (f.status == "fixed") ids.push_back(f.id);
    }
    for (const auto& id : ids) {
        Feedback* f = feedbackItem(id);
        Decision* d = f ? decision(f->decision) : nullptr;
        Json before = d ? d->metricsBefore : Json::object();
        if (before.members().empty() && f) before = f->evidence.get("metrics");
        auto effect = recordEffect(id, before, metrics, d ? d->playtestBefore : "", playtestId, actor);
        if (effect) out.push(Json::object({{"feedback", id}, {"effect", effect->get("status")}}));
    }
}

Json Studio::finishIteration(Loop& L, const std::string& actor, bool& finished) {
    Json& st = L.state;
    Json& iter = currentIteration(L);
    iter["ended_at"] = timestamp();
    // The iteration's metrics: from its last playtest stage.
    Json metrics = Json::object();
    for (const auto& rec : iter.get("stages").elements()) {
        if (rec.get("metrics").isObject()) metrics = rec.get("metrics");
    }
    if (!metrics.members().empty()) {
        iter["metrics"] = metrics;
        st["metrics"] = metrics;
        Json& trends = st["trends"];
        if (!trends.isObject()) trends = Json::object();
        for (const auto& [k, v] : metrics.members()) {
            if (!v.isNumber() && !v.isNull()) continue;
            Json& series = trends[k];
            if (!series.isArray()) series = Json::array();
            series.push(v);
        }
    }
    int iteration = static_cast<int>(st.get("iteration").asInt(1));
    int maxIt = static_cast<int>(st.get("max_iterations").asInt(L.stop.maxIterations));
    bool hasTargets = !L.stop.metricTargets.members().empty();
    std::string unmet;
    bool targetsOk = hasTargets && !metrics.members().empty() && targetsMet(metrics, L.stop.metricTargets, &unmet);
    bool signedOff = iter.get("signoffs").size() > 0;
    bool success = (hasTargets || L.stop.directorSignoff) && (!hasTargets || targetsOk) && (!L.stop.directorSignoff || signedOff);
    std::string reason;
    if (success) {
        reason = hasTargets && L.stop.directorSignoff ? "metric targets met and director signed off"
                 : hasTargets                         ? "metric targets met"
                                                      : "director signed off";
        st["status"] = "done";
    } else if (iteration >= maxIt) {
        reason = "max iterations reached (" + std::to_string(maxIt) + ")";
        if (hasTargets && !targetsOk) reason += "; targets not met: " + (unmet.empty() ? "no metrics" : unmet);
        if (L.stop.directorSignoff && !signedOff) reason += "; no director sign-off";
        st["status"] = "done";
    }
    emit("loop", "iteration_finished", L.name, actor, L.name + " iteration " + std::to_string(iteration) + " finished",
         Json::object({{"iteration", iteration}, {"metrics", metrics}}));
    if (!reason.empty()) {
        finished = true;
        st["stop_reason"] = reason;
        st["ended_at"] = timestamp();
        st["pending"] = Json::array();
        st["stage"] = nullptr;
        emit("loop", "finished", L.name, actor, L.name + ": " + reason, Json::object({{"status", "done"}}));
        return st;
    }
    st["iteration"] = iteration + 1;
    st["stage_index"] = 0;
    st["stage"] = L.stages.front().id;
    st["iterations"].push(Json::object({{"n", iteration + 1}, {"started_at", timestamp()}, {"stages", Json::array()},
                                        {"signoffs", Json::array()}}));
    emit("loop", "iteration_started", L.name, actor, L.name + " iteration " + std::to_string(iteration + 1),
         Json::object({{"iteration", iteration + 1}}));
    return st;
}

Json Studio::enterStages(Loop& L, const std::string& actor) {
    Json& st = L.state;
    for (int guard = 0; guard < 10000; ++guard) {
        if (st.get("status").asString() != "running") break;
        std::string why;
        if (budgetExhausted(L, why)) {
            st["status"] = "stopped";
            st["stop_reason"] = why;
            st["ended_at"] = timestamp();
            st["pending"] = Json::array();
            emit("loop", "finished", L.name, actor, L.name + ": " + why, Json::object({{"status", "stopped"}}));
            break;
        }
        size_t idx = static_cast<size_t>(st.get("stage_index").asInt());
        if (idx >= L.stages.size()) {
            bool finished = false;
            finishIteration(L, actor, finished);
            if (finished) break;
            continue;
        }
        const LoopStage& stage = L.stages[idx];
        st["stage"] = stage.id;
        Json& iter = currentIteration(L);
        Json& stages = iter["stages"];
        if (!stages.isArray()) stages = Json::array();
        // Find or create this stage's record (an approval round-trip re-enters the same stage).
        Json* rec = nullptr;
        for (auto& r : stages.elements()) {
            if (r.get("id").asString() == stage.id) rec = &r;
        }
        if (!rec) {
            stages.push(Json::object({{"id", stage.id}, {"title", stage.title}, {"kind", stage.kind}, {"started_at", timestamp()}}));
            rec = &stages.elements().back();
        }
        std::string skipWhy;
        if (shouldSkip(L, stage, skipWhy)) {
            (*rec)["status"] = "skipped";
            (*rec)["summary"] = "skipped: " + skipWhy;
            emit("loop", "stage_skipped", L.name, actor, L.name + " · " + stage.title + " — skipped: " + skipWhy,
                 Json::object({{"stage", stage.id}}));
            st["stage_index"] = static_cast<int64_t>(idx + 1);
            continue;
        }
        if (stage.gate.get("approval").asString() == "human" && !rec->get("approved").asBool()) {
            (*rec)["status"] = "awaiting_approval";
            st["status"] = "awaiting_approval";
            emit("loop", "awaiting_approval", L.name, actor, L.name + ": approve stage \"" + stage.title + "\"",
                 Json::object({{"stage", stage.id}}));
            break;
        }
        if (stage.kind == "playtest") {
            (*rec)["status"] = "running";
            emit("loop", "stage_started", L.name, actor, L.name + " · " + stage.title, Json::object({{"stage", stage.id}}));
            Json r = runPlaytestStage(L, stage, actor);
            // runPlaytestStage may append feedback/tasks but never touches loop state: `rec` is still valid.
            for (const auto& [k, v] : r.members()) (*rec)[k] = v;
            (*rec)["ended_at"] = timestamp();
            emit("loop", "stage_finished", L.name, actor, L.name + " · " + stage.title + ": " + r.get("summary").asString(),
                 Json::object({{"stage", stage.id}}));
            st["stage_index"] = static_cast<int64_t>(idx + 1);
            continue;
        }
        std::vector<Assignment> work = buildAssignments(L, stage, actor);
        if (work.empty()) {
            std::string refs;
            for (const auto& a : stage.assignees) refs += (refs.empty() ? "" : ", ") + a;
            (*rec)["status"] = "skipped";
            (*rec)["summary"] = stage.onlyWithTasks ? "skipped: no " + refs + " agent holds open tasks"
                                                    : "skipped: no roster member matches " + refs;
            emit("loop", "stage_skipped", L.name, actor, L.name + " · " + stage.title + " — " + rec->get("summary").asString(),
                 Json::object({{"stage", stage.id}}));
            st["stage_index"] = static_cast<int64_t>(idx + 1);
            continue;
        }
        Json pending = Json::array();
        Json who = Json::array();
        for (const auto& a : work) {
            pending.push(a.toJson());
            who.push(a.agent);
        }
        (*rec)["status"] = "running";
        (*rec)["assignments"] = who;
        (*rec)["reports"] = Json::array();
        st["pending"] = pending;
        emit("loop", "stage_started", L.name, actor, L.name + " · " + stage.title, Json::object({{"stage", stage.id}, {"agents", who}}));
        break;
    }
    saveLoop(L);
    return loopStatus(L, false);
}

Result<Json> Studio::advanceLoop(std::string_view name, const Json& args, const std::string& actor) {
    Loop* L = loop(name);
    if (!L) return Error::make("not_found", "no loop '" + std::string(name) + "'", "studio_loop_status lists loops");
    Json& st = L->state;
    std::string status = st.get("status").asString("idle");
    if (status == "awaiting_approval") {
        if (!args.contains("approve")) {
            return Error::make("invalid_state", "loop '" + L->name + "' waits for a human approval",
                               "studio_loop_advance {loop, approve: true} (or false to stop)");
        }
        Json& iter = currentIteration(*L);
        for (auto& r : iter["stages"].elements()) {
            if (r.get("id").asString() == st.get("stage").asString()) r["approved"] = args.get("approve").asBool();
        }
        if (!args.get("approve").asBool()) {
            st["status"] = "stopped";
            st["stop_reason"] = "approval declined by " + actor;
            st["ended_at"] = timestamp();
            saveLoop(*L);
            emit("loop", "finished", L->name, actor, L->name + ": approval declined", Json::object({{"status", "stopped"}}));
            return loopStatus(*L, false);
        }
        st["status"] = "running";
        return enterStages(*L, actor);
    }
    if (status != "running") {
        return Error::make("invalid_state", "loop '" + L->name + "' is " + status,
                           status == "idle" ? "start it with studio_loop_start" : "start a new run with studio_loop_start");
    }
    // Make sure every key we hold a reference to exists before taking references.
    if (!st.get("pending").isArray()) st["pending"] = Json::array();
    Json& pending = st["pending"];
    Json& iter = currentIteration(*L);
    if (!iter.get("signoffs").isArray()) iter["signoffs"] = Json::array();
    if (!iter.get("stages").isArray()) iter["stages"] = Json::array();
    Json* rec = nullptr;
    for (auto& r : iter["stages"].elements()) {
        if (r.get("id").asString() == st.get("stage").asString()) rec = &r;
    }
    if (rec && !rec->get("reports").isArray()) (*rec)["reports"] = Json::array();
    auto canSignOff = [&](const std::string& who) {
        const AgentProfile* p = agent(who);
        return !p || p->discipline == "direction" || p->discipline == "production";
    };
    for (const auto& r : args.get("reports").elements()) {
        std::string who = r.get("agent").asString();
        if (who.empty()) who = memberForActor(actor);
        const AgentProfile* p = agent(who);
        std::string id = p ? p->id : who;
        auto& items = pending.elements();
        auto it = std::find_if(items.begin(), items.end(), [&](const Json& a) { return a.get("agent").asString() == id; });
        if (it == items.end()) {
            std::string waiting;
            for (const auto& a : items) waiting += (waiting.empty() ? "" : ", ") + a.get("agent").asString();
            return Error::make("invalid_arguments", "no pending assignment for '" + who + "' in this stage",
                               waiting.empty() ? "nothing is pending; call studio_loop_status" : "waiting for: " + waiting);
        }
        items.erase(it);
        std::string text = r.get("report").asString();
        bool signoff = r.get("signoff").asBool(false) || str::startsWith(str::trim(text), "SIGNOFF");
        Json row = Json::object({{"agent", id}, {"report", shortText(text, 8000)}});
        if (signoff && canSignOff(id)) {
            row["signoff"] = true;
            iter["signoffs"].push(id);
        }
        if (rec) (*rec)["reports"].push(row);
        const Json& u = r.get("usage");
        if (u.isObject() && p) {
            recordUsage(p->id, u.get("model").asString(p->model), u.get("input_tokens").asInt(), u.get("output_tokens").asInt(),
                        u.get("cache_read_tokens").asInt(), u.get("cache_write_tokens").asInt());
        }
        emit("loop", "report", L->name, actor, "@" + id + ": " + shortText(text, 140), Json::object({{"stage", st.get("stage")}}));
    }
    if (args.get("signoff").asBool(false)) {
        std::string who = memberForActor(actor);
        if (!canSignOff(who)) {
            return Error::make("permission_denied", "only direction/production (or the human) can sign off");
        }
        iter["signoffs"].push(who.empty() ? actor : who);
    }
    bool complete = pending.size() == 0 || args.get("complete_stage").asBool(false);
    if (!complete) {
        saveLoop(*L);
        return loopStatus(*L, false);
    }
    if (rec) {
        if (pending.size()) {
            Json missing = Json::array();
            for (const auto& a : pending.elements()) missing.push(a.get("agent"));
            (*rec)["missing"] = missing;
        }
        (*rec)["status"] = "done";
        (*rec)["ended_at"] = timestamp();
    }
    pending = Json::array();
    st["stage_index"] = st.get("stage_index").asInt() + 1;
    emit("loop", "stage_finished", L->name, actor, L->name + " · " + (rec ? rec->get("title").asString() : std::string()),
         Json::object({{"stage", st.get("stage")}}));
    return enterStages(*L, actor);
}

Result<Json> Studio::stopLoop(std::string_view name, const std::string& reason, const std::string& actor) {
    Loop* L = loop(name);
    if (!L) return Error::make("not_found", "no loop '" + std::string(name) + "'");
    std::string status = L->state.get("status").asString("idle");
    if (status != "running" && status != "awaiting_approval") {
        return Error::make("invalid_state", "loop '" + L->name + "' is not running (" + status + ")");
    }
    L->state["status"] = "stopped";
    L->state["stop_reason"] = reason.empty() ? "stopped by " + actor : reason;
    L->state["ended_at"] = timestamp();
    L->state["pending"] = Json::array();
    saveLoop(*L);
    emit("loop", "finished", L->name, actor, L->name + ": " + L->state.get("stop_reason").asString(),
         Json::object({{"status", "stopped"}}));
    return loopStatus(*L, false);
}

}  // namespace sky::studio
