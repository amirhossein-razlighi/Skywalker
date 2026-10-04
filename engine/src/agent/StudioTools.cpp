// Studio tools: a multi-agent game studio (roster, board, feedback, director decisions,
// loops, messages) and playtests that actually play. See docs/STUDIO.md.
//
// Identity: every call carries the caller's actor ("agent:mira", "mcp:claude-code",
// "mcp:claude-code/mira"). External agents that join the roster either connect with a
// client name ending in "/<agent id>" or pass `as: "<agent id>"` on studio calls.

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/studio/Catalog.h"
#include "skywalker/studio/Playtest.h"
#include "skywalker/studio/Runner.h"
#include "skywalker/studio/Studio.h"

namespace sky::tools {

namespace {

using namespace schema;
using studio::Studio;

Json strings(std::string description) { return array(Json::object({{"type", "string"}}), std::move(description)); }

Json objectArg(std::string description) { return Json::object({{"type", "object"}, {"description", std::move(description)}}); }

Json asArg() {
    return string("Act as this roster member (agent id). For external agents whose MCP client name isn't "
                  "\"<client>/<agent id>\" (e.g. Claude Code subagents playing a role). Ignored when you already are one.");
}

Studio& S(Engine& e) {
    Studio& s = e.studio();
    s.syncFromDisk();
    return s;
}

/// The effective actor: ctx.actor, or "<ctx.actor>/<as>" for unidentified callers playing a role.
Result<std::string> actorFor(Engine& e, const Json& a, const ToolContext& ctx) {
    std::string as = a.get("as").asString();
    if (as.empty()) return ctx.actor;
    Studio& s = S(e);
    const studio::AgentProfile* p = s.agent(as);
    if (!p) return s.unknownAgent(as);
    std::string current = s.memberForActor(ctx.actor);
    if (!current.empty()) {
        if (current == p->id) return ctx.actor;
        return Error::make("permission_denied", "you are @" + current + "; you cannot act as @" + p->id);
    }
    std::string base = ctx.actor;
    if (size_t slash = base.find('/'); slash != std::string::npos) base = base.substr(0, slash);
    return base + "/" + p->id;
}

/// The roster member a call is about: explicit `agent`, else the caller.
Result<std::string> memberFor(Engine& e, const Json& a, const std::string& actor, bool required) {
    Studio& s = S(e);
    std::string ref = a.get("agent").asString();
    if (!ref.empty()) {
        const studio::AgentProfile* p = s.agent(ref);
        if (!p) return s.unknownAgent(ref);
        return p->id;
    }
    std::string m = s.memberForActor(actor);
    if (m.empty() && required) {
        return Error::make("invalid_arguments", "who? this call needs a roster identity",
                           "pass agent: \"<id>\", or as: \"<your agent id>\" (studio_agent_list shows ids)");
    }
    return m;
}

ToolResult err(const Error& e) { return ToolResult::error(e); }

std::string shortText(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "…"; }

Json feedbackJson(Studio& s, const studio::Feedback& f, bool withDecision) {
    Json j = f.toJson();
    j.erase("history");
    if (withDecision && !f.decision.empty()) {
        if (const studio::Decision* d = s.decision(f.decision)) {
            j["verdict"] = Json::object({{"decision", d->id},
                                         {"verdict", d->verdict},
                                         {"rationale", d->rationale},
                                         {"by", d->by},
                                         {"effect", d->effect}});
        }
    }
    return j;
}

std::string feedbackLine(Studio& s, const studio::Feedback& f) {
    std::string line = f.id + " [" + f.status + "] " + f.category + "/" + f.severity + ": " + f.summary;
    if (f.occurrences > 1) line += " ×" + std::to_string(f.occurrences);
    if (const studio::Decision* d = f.decision.empty() ? nullptr : s.decision(f.decision)) {
        line += "\n    → " + d->verdict + " by " + d->by + ": " + shortText(d->rationale, 140);
        std::string eff = d->effect.get("status").asString();
        if (!eff.empty() && eff != "n/a") line += " · effect: " + eff;
    }
    return line;
}

std::string loopText(const Json& st) {
    std::string out = "loop " + st.get("loop").asString() + ": " + st.get("status").asString();
    if (!st.get("iteration").isNull()) {
        out += " · iteration " + st.get("iteration").dump() + "/" + st.get("max_iterations").dump();
    }
    if (!st.get("stage").isNull()) out += " · stage " + st.get("stage").asString();
    if (st.contains("stop_reason")) out += " · " + st.get("stop_reason").asString();
    for (const auto& a : st.get("assignments").elements()) {
        out += "\n  → @" + a.get("agent").asString() + " (" + a.get("stage").asString() + ")";
    }
    if (st.contains("next")) out += "\nnext: " + st.get("next").asString();
    return out;
}

}  // namespace

void addStudioTools(Engine& engine, ToolRegistry& reg) {
    // ---------------------------------------------------------------- overview
    reg.add({"studio_overview", "Studio overview",
             "Start here when working with the studio (a team of AI agents making this game). Returns the roster with "
             "live status, the task board counts, feedback waiting for a decision, recent director decisions with "
             "measured effects, loops and their state, the latest playtest metrics, and token/cost usage. "
             "include_catalog=true also lists roles, disciplines, team templates and loop templates.",
             "studio", object({{"include_catalog", boolean("Also return roles, team templates and loop templates")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Studio& s = S(engine);
                 Json o = s.overview();
                 if (a.get("include_catalog").asBool()) o["catalog"] = studio::catalogJson();
                 std::string text = "studio: " + std::to_string(s.agents().size()) + " agents · board " +
                                    o.get("board").dump() + " · feedback " + o.get("feedback").dump();
                 if (o.get("needs_decision").size()) {
                     text += "\nneeds a decision:";
                     for (const auto& n : o.get("needs_decision").elements()) text += "\n  " + n.asString();
                 }
                 for (const auto& l : o.get("loops").elements()) {
                     text += "\nloop " + l.get("name").asString() + ": " + l.get("status").asString();
                 }
                 if (s.agents().empty()) text += "\nno agents yet — spawn a team with studio_team_template";
                 return ToolResult::json(o, text);
             }});

    // ---------------------------------------------------------------- roster
    reg.add({"studio_agent_define", "Define agent",
             "Create a studio agent or update one (by id or name; only the fields you pass change). An agent is a "
             "specialist: a role (e.g. level_designer, gameplay_programmer, lighting_artist, playtester, critic — see "
             "studio_overview include_catalog), a focus (what exactly it owns: \"enemy AI\", \"onboarding\"), a persona, "
             "standing instructions, a model, an autonomy level (observe = read-only, ask = changes need human approval, "
             "autonomous), per-tool-category permissions (allow|ask|off), and who it reports to. Saved as "
             "agents/<id>.agent.json (shareable, git-friendly). Playtesters take persona knobs in playtest "
             "{policy, reaction_time, skill, curiosity, patience}. Example: {name:\"Mira\", role:\"level_designer\", "
             "focus:\"secret areas\", focus_tags:[\"secrets\"], reports_to:\"nimbus\"}.",
             "studio",
             object({{"id", string("Agent id to update (default: derived from name)")},
                     {"name", string("Display name")},
                     {"role", enumeration(studio::roleIds(), "Role")},
                     {"discipline", enumeration(studio::disciplines(), "Discipline (default: from the role)")},
                     {"focus", string("What this agent specializes in")},
                     {"focus_tags", strings("Short focus tags")},
                     {"persona", string("Personality / voice")},
                     {"mission", string("Replaces the role's built-in mission")},
                     {"instructions", string("Standing instructions: style guides, constraints, conventions")},
                     {"provider", string("anthropic | openai | mock | an editor provider name")},
                     {"model", string("Model id (empty = provider default, e.g. claude-opus-5-5)")},
                     {"autonomy", enumeration({"observe", "ask", "autonomous"}, "How freely it may change the game")},
                     {"permissions", objectArg("Per tool category: {\"network\":\"off\",\"render\":\"ask\"}")},
                     {"reports_to", string("Agent id of its lead")},
                     {"color", string("Avatar color #rrggbb")},
                     {"face", enumeration({"happy", "focused", "dreamy", "wink", "determined", "curious"}, "Avatar face")},
                     {"max_rounds", integer("Max tool rounds per task (default 40)")},
                     {"memory", strings("Replace its long-term memory notes")},
                     {"playtest", objectArg("Playtester persona: {policy, reaction_time, skill, curiosity, patience}")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto r = S(engine).defineAgent(a, ctx.actor);
                 if (!r) return err(r.error());
                 return ToolResult::json(r->toJson(), "defined @" + r->id + " — " + r->name + " (" + r->role + ")");
             }});

    reg.add({"studio_agent_list", "List agents",
             "The studio roster: each agent's id (use it for assignees and @mentions), name, role, discipline, focus, "
             "live status, open tasks and usage. Filter by discipline or role; include_profiles returns full profiles "
             "(persona, instructions, permissions, memory).",
             "studio",
             object({{"discipline", string("Only this discipline")},
                     {"role", string("Only this role")},
                     {"include_profiles", boolean("Full profiles")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Studio& s = S(engine);
                 Json out = Json::array();
                 std::string text;
                 for (const auto& p : s.agents()) {
                     if (a.contains("discipline") && p.discipline != a.get("discipline").asString()) continue;
                     if (a.contains("role") && p.role != a.get("role").asString()) continue;
                     Json j = a.get("include_profiles").asBool() ? p.toJson()
                                                                 : Json::object({{"id", p.id},
                                                                                 {"name", p.name},
                                                                                 {"role", p.role},
                                                                                 {"discipline", p.discipline},
                                                                                 {"focus", p.focus},
                                                                                 {"autonomy", studio::toString(p.autonomy)},
                                                                                 {"provider", p.provider},
                                                                                 {"model", p.model},
                                                                                 {"reports_to", p.reportsTo},
                                                                                 {"color", p.color},
                                                                                 {"face", p.face}});
                     j["presence"] = s.presence(p.id);
                     auto u = s.usage().find(p.id);
                     if (u != s.usage().end()) j["usage"] = u->second.toJson();
                     int64_t open = std::count_if(s.tasks().begin(), s.tasks().end(), [&](const studio::Task& t) {
                         return t.assignee == p.id && (t.status == "todo" || t.status == "doing" || t.status == "review");
                     });
                     j["open_tasks"] = open;
                     out.push(j);
                     text += "@" + p.id + " " + p.name + " — " + p.role + (p.focus.empty() ? "" : " (" + p.focus + ")") + " · " +
                             s.presence(p.id).get("status").asString() + (open ? " · " + std::to_string(open) + " open" : "") + "\n";
                 }
                 if (text.empty()) text = "no agents — spawn a team with studio_team_template";
                 return ToolResult::json(Json::object({{"agents", out}}), text);
             }});

    reg.add({"studio_agent_brief", "Agent brief",
             "Everything needed to *be* a roster member: its system prompt (role mission, focus, persona, standing "
             "instructions, memory, the team, studio etiquette) and the engine tools it may use with their access "
             "(allow | ask). Use it to run a studio agent in any harness: e.g. give a Claude Code / Codex subagent this "
             "system prompt, let it call only the listed tools, and connect it as \"<client>/<agent id>\" (or pass "
             "as: \"<agent id>\") so its work is attributed. loop_member=true withholds loop and roster control tools.",
             "studio",
             object({{"agent", string("Agent id or name")}, {"loop_member", boolean("Running inside a loop stage")}}, {"agent"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Studio& s = S(engine);
                 const studio::AgentProfile* p = s.agent(a.get("agent").asString());
                 if (!p) return err(s.unknownAgent(a.get("agent").asString()));
                 auto tools = studio::AgentRunner::toolsFor(*p, engine.tools(), a.get("loop_member").asBool());
                 Json list = Json::array();
                 int asks = 0;
                 for (const auto& t : tools) {
                     list.push(Json::object({{"name", t.spec.name}, {"category", t.category}, {"access", studio::toString(t.access)}}));
                     asks += t.access == studio::Access::Ask;
                 }
                 std::string prompt = studio::AgentRunner::systemPrompt(*p, s.agents());
                 Json out = Json::object({{"agent", p->id},
                                          {"actor", p->actor()},
                                          {"model", p->model},
                                          {"provider", p->provider},
                                          {"max_rounds", p->maxRounds},
                                          {"system_prompt", prompt},
                                          {"tools", list}});
                 return ToolResult::json(out, "@" + p->id + ": " + std::to_string(tools.size()) + " tools (" + std::to_string(asks) +
                                                  " need approval)\n\n" + prompt);
             }});

    reg.add({"studio_agent_remove", "Remove agent",
             "Remove an agent from the studio (deletes agents/<id>.agent.json; its open tasks become unassigned).",
             "studio", object({{"agent", string("Agent id or name")}}, {"agent"}), true, true,
             [&engine](const Json& a, ToolContext& ctx) {
                 Studio& s = S(engine);
                 std::string ref = a.get("agent").asString();
                 if (Status st = s.removeAgent(ref, ctx.actor); !st) return fail(st);
                 return ToolResult::text("removed @" + studio::slugify(ref));
             }});

    reg.add({"studio_team_template", "Spawn a team",
             "Spawn a whole team of specialists from a template (agents that already exist by id are updated): "
             "starter_crew (5 generalists), indie_trio (3), aaa_strike_team (22: director, producer, five designers, four "
             "programmers, five artists, audio, writing, QA, two playtester personas, critic), narrative_team, qa_squad "
             "(QA lead, three playtester personas, critic, fixer), art_team, audio_team. Override provider/model/autonomy "
             "for everyone with `defaults`.",
             "studio",
             object({{"template", enumeration(studio::teamTemplateNames(), "Team template")},
                     {"defaults", objectArg("Fields applied to every member, e.g. {\"model\":\"claude-opus-5-5\"}")}},
                    {"template"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Studio& s = S(engine);
                 std::string name = a.get("template").asString();
                 Json members = studio::teamTemplate(name);
                 Json out = Json::array();
                 std::string text = "spawned " + name + ":";
                 for (const auto& m : members.elements()) {
                     Json spec = m;
                     for (const auto& [k, v] : a.get("defaults").members()) spec[k] = v;
                     auto r = s.defineAgent(spec, ctx.actor);
                     if (!r) return err(r.error());
                     out.push(Json::object({{"id", r->id}, {"name", r->name}, {"role", r->role}, {"focus", r->focus}}));
                     text += "\n  @" + r->id + " — " + r->role + (r->focus.empty() ? "" : " (" + r->focus + ")");
                 }
                 return ToolResult::json(Json::object({{"template", name}, {"agents", out}}), text);
             }});

    reg.add({"studio_memory", "Agent memory",
             "Your long-term memory as a studio agent (kept in your profile and shown at the start of every task): note "
             "a fact worth remembering (conventions, decisions, the human's preferences, unfinished work), forget an "
             "outdated note by number, or list notes.",
             "studio",
             object({{"action", enumeration({"note", "forget", "list"}, "What to do")},
                     {"text", string("The note (for note)")},
                     {"number", integer("Note number, 1-based (for forget)")},
                     {"agent", string("Whose memory (default: you)")},
                     {"as", asArg()}},
                    {"action"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto id = memberFor(engine, a, *actor, true);
                 if (!id) return err(id.error());
                 Studio& s = S(engine);
                 std::string action = a.get("action").asString();
                 if (action == "note") {
                     if (Status st = s.addMemory(*id, a.get("text").asString()); !st) return fail(st);
                 } else if (action == "forget") {
                     if (Status st = s.forgetMemory(*id, static_cast<int>(a.get("number").asInt())); !st) return fail(st);
                 }
                 Json notes = Json::array();
                 std::string text;
                 int i = 1;
                 for (const auto& n : s.agent(*id)->memory) {
                     notes.push(n);
                     text += std::to_string(i++) + ". " + n + "\n";
                 }
                 return ToolResult::json(Json::object({{"agent", *id}, {"memory", notes}}), text.empty() ? "(no notes)" : text);
             }});

    reg.add({"studio_usage_report", "Report usage",
             "Record tokens an agent spent with its own model, for studio budgets and cost tracking (runners that call "
             "models outside the engine — the editor crew, custom scripts — report each request here). Tool calls are "
             "counted automatically.",
             "studio",
             object({{"agent", string("Agent id (default: you)")},
                     {"model", string("Model id (prices the tokens)")},
                     {"input_tokens", integer("Uncached input tokens")},
                     {"output_tokens", integer("Output tokens")},
                     {"cache_read_tokens", integer("Cached input tokens read")},
                     {"cache_write_tokens", integer("Input tokens written to the cache")},
                     {"as", asArg()}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto id = memberFor(engine, a, *actor, true);
                 if (!id) return err(id.error());
                 Studio& s = S(engine);
                 s.recordUsage(*id, a.get("model").asString(), std::max<int64_t>(0, a.get("input_tokens").asInt()),
                               std::max<int64_t>(0, a.get("output_tokens").asInt()),
                               std::max<int64_t>(0, a.get("cache_read_tokens").asInt()),
                               std::max<int64_t>(0, a.get("cache_write_tokens").asInt()));
                 return ToolResult::json(s.usage().at(*id).toJson(), "recorded usage for @" + *id);
             }});

    reg.add({"studio_presence", "Set presence",
             "Show what a roster member is doing right now in the editor's Studio panel (live status and activity next to "
             "its avatar): working with a one-line activity when you start something, idle when you finish. External "
             "harnesses (the Python agent layer, scripts, Claude Code subagents) call it so their agents appear live like "
             "the editor's crew. Example: {status: \"working\", activity: \"T-4 widen the lava bridge\", as: \"mira\"}.",
             "studio",
             object({{"status", enumeration({"working", "idle", "waiting", "blocked"}, "Status")},
                     {"activity", string("One line: what you are doing (empty when idle)")},
                     {"agent", string("Whose presence (default: you)")},
                     {"as", asArg()}},
                    {"status"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto id = memberFor(engine, a, *actor, true);
                 if (!id) return err(id.error());
                 Studio& s = S(engine);
                 std::string status = a.get("status").asString();
                 s.setPresence(*id, status, status == "idle" ? "" : shortText(a.get("activity").asString(), 160));
                 return ToolResult::json(Json::object({{"agent", *id}, {"presence", s.presence(*id)}}), "@" + *id + " is " + status);
             }});

    // ---------------------------------------------------------------- board
    reg.add({"studio_task_create", "Create task",
             "Put a task on the studio board. Good tasks are small and checkable: a clear title, acceptance criteria "
             "(how we know it's done), a discipline, and an assignee (agent id, or \"@role\" to pick the least busy "
             "agent with that role). Link the feedback it addresses so the effect can be measured. Example: "
             "{title:\"Widen the lava bridge\", acceptance:[\"bridge 3 m wide\",\"deaths ≤ 1 per run\"], "
             "assignee:\"@level_designer\", feedback:[\"F-2\"]}.",
             "studio",
             object({{"title", string("Short imperative title")},
                     {"description", string("Context and details")},
                     {"acceptance", strings("Acceptance criteria, one per item")},
                     {"discipline", enumeration(studio::disciplines(), "Discipline")},
                     {"assignee", string("Agent id or @role")},
                     {"priority", enumeration(studio::priorities(), "Priority (default normal)")},
                     {"status", enumeration(studio::taskStatuses(), "Initial status (default todo)")},
                     {"depends_on", strings("Task ids that must be done first")},
                     {"feedback", strings("Feedback ids this task addresses")},
                     {"links", objectArg("Other links: {entities:[\"#12\"], assets:[...], captures:[...]}")},
                     {"as", asArg()}},
                    {"title"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto t = S(engine).createTask(a, *actor);
                 if (!t) return err(t.error());
                 return ToolResult::json(t->toJson(), "created " + t->id + " " + t->title +
                                                          (t->assignee.empty() ? "" : " → @" + t->assignee));
             }});

    reg.add({"studio_task_update", "Update task",
             "Move a task across the board (backlog → todo → doing → review → done, or dropped), reassign it, edit it, "
             "or add a comment. Marking the last task of a feedback item done moves that feedback to \"fixed\" (the next "
             "verification playtest then measures the effect). Moving to doing/done checks dependencies.",
             "studio",
             object({{"task", string("Task id, e.g. T-4")},
                     {"status", enumeration(studio::taskStatuses(), "New status")},
                     {"assignee", string("Agent id (empty string unassigns)")},
                     {"priority", enumeration(studio::priorities(), "Priority")},
                     {"title", string("Title")},
                     {"description", string("Description")},
                     {"acceptance", strings("Acceptance criteria (replaces the list)")},
                     {"discipline", string("Discipline")},
                     {"depends_on", strings("Replace dependencies")},
                     {"links", objectArg("Links to add")},
                     {"comment", string("Comment for the thread (what changed, what's left)")},
                     {"as", asArg()}},
                    {"task"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Json patch = a;
                 patch.erase("task");
                 patch.erase("as");
                 auto t = S(engine).updateTask(a.get("task").asString(), patch, *actor);
                 if (!t) return err(t.error());
                 return ToolResult::json(t->toJson(), t->id + " " + t->title + " · " + t->status);
             }});

    reg.add({"studio_task_list", "Task board",
             "The studio board. Filter by status (or \"open\" = backlog/todo/doing/review), assignee, discipline, or "
             "mine=true for your own tasks. Returns tasks with acceptance criteria, links and comment counts; the text "
             "is a compact kanban.",
             "studio",
             object({{"status", string("A status, or \"open\" / \"all\" (default all)")},
                     {"assignee", string("Agent id")},
                     {"discipline", string("Discipline")},
                     {"mine", boolean("Only tasks assigned to you")},
                     {"limit", integer("Max tasks (default 100)")},
                     {"as", asArg()}}),
             false, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Studio& s = S(engine);
                 std::string status = a.get("status").asString("all");
                 std::string assignee = a.get("assignee").asString();
                 if (a.get("mine").asBool()) {
                     auto m = memberFor(engine, Json::object(), *actor, true);
                     if (!m) return err(m.error());
                     assignee = *m;
                 }
                 size_t limit = static_cast<size_t>(std::clamp<int64_t>(a.get("limit").asInt(100), 1, 1000));
                 Json out = Json::array();
                 std::map<std::string, std::string> columns;
                 for (const auto& t : s.tasks()) {
                     bool open = t.status == "backlog" || t.status == "todo" || t.status == "doing" || t.status == "review";
                     if (status == "open" ? !open : (status != "all" && t.status != status)) continue;
                     if (!assignee.empty() && t.assignee != studio::slugify(assignee)) continue;
                     if (a.contains("discipline") && t.discipline != a.get("discipline").asString()) continue;
                     if (out.size() >= limit) break;
                     Json j = t.toJson();
                     j["comment_count"] = static_cast<int64_t>(t.comments.size());
                     if (t.comments.size() > 3) {
                         Json last = Json::array();
                         for (size_t i = t.comments.size() - 3; i < t.comments.size(); ++i) last.push(j.get("comments")[i]);
                         j["comments"] = last;
                     }
                     out.push(j);
                     columns[t.status] += "  " + t.id + " " + t.title + (t.assignee.empty() ? "" : " @" + t.assignee) + "\n";
                 }
                 std::string text;
                 for (const auto& st : studio::taskStatuses()) {
                     if (columns.count(st)) text += st + ":\n" + columns[st];
                 }
                 return ToolResult::json(Json::object({{"tasks", out}}), text.empty() ? "(no tasks)" : text);
             }});

    reg.add({"studio_task_claim", "Claim task",
             "Claim a task and start it (assignee = you, status = doing). Without `task`, claims your highest-priority "
             "todo task, or else an unassigned todo task in your discipline. External agents identify with `as` or a "
             "client name ending in /<agent id>.",
             "studio",
             object({{"task", string("Task id (default: pick the next one for you)")},
                     {"agent", string("Claim for this agent (default: you)")},
                     {"as", asArg()}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto who = memberFor(engine, a, *actor, true);
                 if (!who) return err(who.error());
                 Studio& s = S(engine);
                 std::string id = a.get("task").asString();
                 if (id.empty()) {
                     const studio::AgentProfile* p = s.agent(*who);
                     static const std::vector<std::string> order = {"critical", "high", "normal", "low"};
                     auto rank = [&](const studio::Task& t) { return std::find(order.begin(), order.end(), t.priority) - order.begin(); };
                     const studio::Task* best = nullptr;
                     for (int pass = 0; pass < 2 && !best; ++pass) {
                         for (const auto& t : s.tasks()) {
                             if (t.status != "todo") continue;
                             bool fits = pass == 0 ? t.assignee == *who
                                                   : t.assignee.empty() && (t.discipline.empty() || t.discipline == p->discipline);
                             if (fits && (!best || rank(t) < rank(*best))) best = &t;
                         }
                     }
                     if (!best) return ToolResult::json(Json::object({{"task", nullptr}}), "nothing to claim for @" + *who);
                     id = best->id;
                 }
                 auto t = s.claimTask(id, *who, *actor);
                 if (!t) return err(t.error());
                 return ToolResult::json(t->toJson(), "@" + *who + " claimed " + t->id + " " + t->title);
             }});

    // ---------------------------------------------------------------- feedback
    reg.add({"studio_feedback_submit", "Submit feedback",
             "File feedback about the game as a playtester, critic, agent or human: category (fun, difficulty, clarity, "
             "visuals, audio, performance, bug, narrative, accessibility), severity, a one-line summary, details, what "
             "it's about (target), and evidence {playtest: \"P-3\", captures: [...], metrics: {...}, positions: [[x,y,z]], "
             "repro: [steps]}. The director triages it with studio_decide. Pass a fingerprint to deduplicate recurring "
             "findings (a repeat bumps occurrences instead of filing a duplicate).",
             "studio",
             object({{"category", enumeration(studio::feedbackCategories(), "Category")},
                     {"severity", enumeration(studio::severities(), "Severity (default medium)")},
                     {"summary", string("One line: what's wrong or great")},
                     {"details", string("What happened, where, why it matters")},
                     {"target", string("Entity, area, system or asset it is about")},
                     {"evidence", objectArg("{playtest, captures, metrics, positions, repro, trace}")},
                     {"fingerprint", string("Dedup key for recurring issues")},
                     {"as", asArg()}},
                    {"category", "summary"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Json spec = a;
                 spec.erase("as");
                 bool merged = false;
                 auto f = S(engine).submitFeedback(spec, *actor, &merged);
                 if (!f) return err(f.error());
                 return ToolResult::json(f->toJson(), (merged ? "seen again: " : "filed ") + f->id + " " + f->summary);
             }});

    reg.add({"studio_feedback_list", "Feedback",
             "Feedback with each item's director verdict (act / drop / defer / merge_into), rationale and measured "
             "effect (metrics before/after). status: open, needs_decision (open + regressed), active (accepted, "
             "in_progress, fixed), a single status, or all (default).",
             "studio",
             object({{"status", string("Filter (default all)")},
                     {"category", enumeration(studio::feedbackCategories(), "Category")},
                     {"limit", integer("Max items, newest first (default 50)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Studio& s = S(engine);
                 std::string status = a.get("status").asString("all");
                 size_t limit = static_cast<size_t>(std::clamp<int64_t>(a.get("limit").asInt(50), 1, 500));
                 Json out = Json::array();
                 std::string text;
                 const auto& all = s.feedback();
                 for (auto it = all.rbegin(); it != all.rend() && out.size() < limit; ++it) {
                     const auto& f = *it;
                     bool keep = status == "all" || f.status == status ||
                                 (status == "needs_decision" && (f.status == "open" || f.status == "regressed")) ||
                                 (status == "active" && (f.status == "accepted" || f.status == "in_progress" || f.status == "fixed"));
                     if (!keep) continue;
                     if (a.contains("category") && f.category != a.get("category").asString()) continue;
                     out.push(feedbackJson(s, f, true));
                     text += feedbackLine(s, f) + "\n";
                 }
                 return ToolResult::json(Json::object({{"feedback", out}}), text.empty() ? "(no feedback)" : text);
             }});

    reg.add({"studio_feedback_update", "Update feedback",
             "Change a feedback item: status (e.g. verified after a critic's re-review, regressed to reopen its tasks), "
             "severity, category, summary, target, more evidence, or a comment for its history. Decisions themselves go "
             "through studio_decide.",
             "studio",
             object({{"feedback", string("Feedback id, e.g. F-3")},
                     {"status", enumeration(studio::feedbackStatuses(), "New status")},
                     {"severity", enumeration(studio::severities(), "Severity")},
                     {"category", enumeration(studio::feedbackCategories(), "Category")},
                     {"summary", string("Summary")},
                     {"details", string("Details")},
                     {"target", string("Target")},
                     {"evidence", objectArg("Evidence to merge in")},
                     {"comment", string("Note for the history")},
                     {"as", asArg()}},
                    {"feedback"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Json patch = a;
                 patch.erase("feedback");
                 patch.erase("as");
                 auto f = S(engine).updateFeedback(a.get("feedback").asString(), patch, *actor);
                 if (!f) return err(f.error());
                 return ToolResult::json(f->toJson(), f->id + " · " + f->status);
             }});

    reg.add({"studio_decide", "Decide on feedback",
             "The director's (or producer's) verdict on a feedback item, visible to everyone with its rationale: act "
             "(creates tasks — give each a title, acceptance criteria and an assignee — or links existing task_ids), "
             "drop (won't do, say why), defer (later), or merge_into a duplicate. The current playtest metrics are "
             "stored as the baseline, so the next verification playtest measures the effect (optionally against "
             "explicit targets like {\"deaths\":{\"max\":1}}); a regression reopens the tasks. Only direction/production "
             "agents (or humans) may decide. Example: {feedback:\"F-2\", verdict:\"act\", rationale:\"Unfair spike\", "
             "tasks:[{title:\"Telegraph the lava\", assignee:\"@lighting_artist\", acceptance:[\"glow visible from 10 m\"]}]}.",
             "studio",
             object({{"feedback", string("Feedback id")},
                     {"verdict", enumeration({"act", "drop", "defer", "merge_into"}, "Verdict")},
                     {"rationale", string("Why — shown next to the feedback")},
                     {"tasks", array(objectArg("Task spec (as studio_task_create)"), "Tasks to create (act)")},
                     {"task_ids", strings("Existing tasks that address it (act)")},
                     {"merge_into", string("Feedback id it duplicates (merge_into)")},
                     {"targets", objectArg("What success means: {metric: {min, max}}")},
                     {"metrics_before", objectArg("Baseline metrics (default: latest playtest)")},
                     {"redecide", boolean("Override an earlier decision")},
                     {"as", asArg()}},
                    {"feedback", "verdict", "rationale"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto d = S(engine).decide(a, *actor);
                 if (!d) return err(d.error());
                 std::string text = d->id + ": " + d->feedback + " → " + d->verdict;
                 for (const auto& t : d->tasks) text += " " + t;
                 return ToolResult::json(d->toJson(), text);
             }});

    // ---------------------------------------------------------------- loops
    reg.add({"studio_loop_define", "Define loop",
             "Define a studio loop: a repeatable cycle of stages with stop conditions, stored in "
             "studio/loops/<name>.loop.json. Start from a template (playtest_fix_verify, art_pass_with_critic, "
             "balance_tuning, vertical_slice_sprint, bug_bash) and override goal/stop, or write stages yourself. A stage "
             "is either kind \"playtest\" (the engine plays the game with bots: playtest {policy, runs, seconds}, "
             "file_feedback, verify_fixed) or kind \"agents\" (assignees: [\"@role\", \"@discipline\", \"agent-id\", "
             "\"@a|@b\" fallback], instruction template with {{goal}} {{inputs}} {{open_feedback}} {{my_tasks}} "
             "{{metrics}} {{roster}} {{iteration}}, parallel, only_with_tasks, gate {skip_if: no_open_feedback | "
             "no_todo_tasks | not_first_iteration | ..., approval: \"human\"}). stop: {max_iterations, metric_targets "
             "{completion_rate:{min:0.8}}, director_signoff, token_budget, time_budget_minutes}. delete=true removes it.",
             "studio",
             object({{"name", string("Loop name (default: the template's)")},
                     {"template", enumeration(studio::loopTemplateNames(), "Start from this template")},
                     {"goal", string("What the loop is trying to achieve")},
                     {"description", string("Description")},
                     {"stages", array(objectArg("Stage"), "Stages (replace the template's)")},
                     {"stop", objectArg("Stop conditions (merged over the template's)")},
                     {"delete", boolean("Remove the loop")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Studio& s = S(engine);
                 if (a.get("delete").asBool()) {
                     if (Status st = s.removeLoop(a.get("name").asString(), ctx.actor); !st) return fail(st);
                     return ToolResult::text("removed loop " + a.get("name").asString());
                 }
                 Json spec = a;
                 spec.erase("delete");
                 auto l = s.defineLoop(spec, ctx.actor);
                 if (!l) return err(l.error());
                 std::string text = "loop " + l->name + ": " + l->goal;
                 for (const auto& st : l->stages) {
                     text += "\n  " + st.id + " (" + st.kind + ")";
                     if (st.kind == "agents") {
                         std::string who;
                         for (const auto& x : st.assignees) who += (who.empty() ? "" : ", ") + x;
                         text += " → " + who;
                     }
                 }
                 return ToolResult::json(l->toJson(), text);
             }});

    reg.add({"studio_loop_start", "Start loop",
             "Start a run of a loop. The engine runs playtest stages itself and returns the first agent stage's "
             "assignments: [{agent, stage, prompt, tasks}]. Execute each assignment as that agent (in-editor crew, the "
             "headless runner `skywalker studio run`, or your own subagents with actor \"<client>/<agent id>\"; "
             "parallel=true means they may run at the same time), then report back with studio_loop_advance. Override "
             "goal or max_iterations for this run.",
             "studio",
             object({{"loop", string("Loop name")},
                     {"goal", string("Goal for this run")},
                     {"max_iterations", integer("Iteration limit for this run")}},
                    {"loop"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto r = S(engine).startLoop(a.get("loop").asString(), a, ctx.actor);
                 if (!r) return err(r.error());
                 return ToolResult::json(*r, loopText(*r));
             }});

    reg.add({"studio_loop_advance", "Advance loop",
             "Report the results of a loop stage's assignments and move the loop on: reports [{agent, report, signoff?, "
             "usage?}]. When every pending assignment has reported (or complete_stage=true), the loop runs the following "
             "stages — playtests, verification, effect measurement — until it needs agents again, waits for approval, "
             "or ends on its stop conditions. Returns the new status with the next assignments. Directors sign off with "
             "signoff=true (or a report starting with SIGNOFF). approve=true/false answers a human approval gate.",
             "studio",
             object({{"loop", string("Loop name")},
                     {"reports", array(objectArg("{agent, report, signoff?, usage?: {input_tokens, output_tokens, model}}"),
                                       "Reports from the stage's agents")},
                     {"approve", boolean("Answer an approval gate")},
                     {"signoff", boolean("Director/producer sign-off for this iteration")},
                     {"complete_stage", boolean("Finish the stage even if some agents did not report")},
                     {"as", asArg()}},
                    {"loop"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto r = S(engine).advanceLoop(a.get("loop").asString(), a, *actor);
                 if (!r) return err(r.error());
                 return ToolResult::json(*r, loopText(*r));
             }});

    reg.add({"studio_loop_status", "Loop status",
             "A loop's state: status, iteration, current stage, pending assignments (with prompts), per-iteration "
             "history (stage reports, playtests, filed feedback, verified effects) and metric trends. Without a loop "
             "name, lists every loop and the available templates.",
             "studio",
             object({{"loop", string("Loop name (omit to list)")}, {"history", boolean("Include iteration history (default true)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Studio& s = S(engine);
                 std::string name = a.get("loop").asString();
                 if (name.empty()) {
                     Json loops = Json::array();
                     std::string text;
                     for (const auto& l : s.loops()) {
                         Json st = s.loopStatus(l, false);
                         st.erase("assignments");
                         loops.push(st);
                         text += loopText(st) + "\n";
                     }
                     Json tmpl = Json::array();
                     for (const auto& t : studio::loopTemplateNames()) {
                         tmpl.push(Json::object({{"name", t}, {"description", studio::loopTemplateDescription(t)}}));
                     }
                     if (text.empty()) text = "no loops — studio_loop_define {template: \"playtest_fix_verify\"}";
                     return ToolResult::json(Json::object({{"loops", loops}, {"templates", tmpl}}), text);
                 }
                 studio::Loop* l = s.loop(name);
                 if (!l) return err(Error::make("not_found", "no loop '" + name + "'", "studio_loop_status lists loops"));
                 Json st = s.loopStatus(*l, a.get("history").asBool(true));
                 return ToolResult::json(st, loopText(st));
             }});

    reg.add({"studio_loop_stop", "Stop loop", "Stop a running loop (pending assignments are dropped).", "studio",
             object({{"loop", string("Loop name")}, {"reason", string("Why")}}, {"loop"}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto r = S(engine).stopLoop(a.get("loop").asString(), a.get("reason").asString(), ctx.actor);
                 if (!r) return err(r.error());
                 return ToolResult::json(*r, loopText(*r));
             }});

    // ---------------------------------------------------------------- messages
    reg.add({"studio_message_send", "Send message",
             "Talk to the team so the back-and-forth is visible: post in a channel (#general by default; any name "
             "creates a channel), address agents with `to` or @mentions in the text (@id, @role, @discipline, @all), "
             "reply in a thread with reply_to, and reference a task/feedback. Example: {text:\"@lighting_artist the "
             "lava needs a stronger glow, see F-2\", feedback:\"F-2\"}.",
             "studio",
             object({{"text", string("Message (supports @mentions)")},
                     {"to", strings("Recipients: agent ids or @roles")},
                     {"channel", string("Channel (default general)")},
                     {"reply_to", string("Message id to reply to (threads)")},
                     {"task", string("Related task id")},
                     {"feedback", string("Related feedback id")},
                     {"kind", string("Structured message kind for agent harnesses (default chat): request, inform, "
                                     "handoff, result, approval_request, approval_answer, ...")},
                     {"data", objectArg("Structured payload for agent harnesses (the text stays the human-readable summary)")},
                     {"as", asArg()}},
                    {"text"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Json spec = a;
                 spec.erase("as");
                 auto m = S(engine).sendMessage(spec, *actor);
                 if (!m) return err(m.error());
                 return ToolResult::json(m->toJson(), "sent " + m->id + " in #" + m->channel);
             }});

    reg.add({"studio_inbox", "Inbox",
             "Messages for you (addressed to you, mentioning you, or broadcast in #general), oldest first; unread only by "
             "default, and reading marks them read. Check it when you start a task. `channel` reads a whole channel; "
             "agent reads someone else's inbox without marking.",
             "studio",
             object({{"agent", string("Whose inbox (default: you)")},
                     {"unread_only", boolean("Only unread (default true)")},
                     {"channel", string("Read this channel instead (\"*\" = every channel)")},
                     {"thread", string("Read a whole thread instead (root message id); never marks read")},
                     {"after", string("With channel or thread: only messages newer than this id")},
                     {"limit", integer("Max messages (default 30)")},
                     {"as", asArg()}}),
             false, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Studio& s = S(engine);
                 size_t limit = static_cast<size_t>(std::clamp<int64_t>(a.get("limit").asInt(30), 1, 500));
                 Json out = Json::array();
                 std::string text;
                 auto line = [](const studio::Message& m) {
                     return m.id + " " + m.at.substr(11, 5) + " #" + m.channel + " @" + m.from + ": " + m.text + "\n";
                 };
                 int after = studio::idNumber(a.get("after").asString());
                 if (a.contains("thread")) {
                     std::string root = a.get("thread").asString();
                     std::vector<const studio::Message*> msgs;
                     for (const auto& m : s.messages()) {
                         if ((m.id == root || m.thread == root) && studio::idNumber(m.id) > after) msgs.push_back(&m);
                     }
                     size_t start = msgs.size() > limit ? msgs.size() - limit : 0;
                     for (size_t i = start; i < msgs.size(); ++i) {
                         out.push(msgs[i]->toJson());
                         text += line(*msgs[i]);
                     }
                     return ToolResult::json(Json::object({{"thread", root}, {"messages", out}}), text.empty() ? "(empty)" : text);
                 }
                 if (a.contains("channel")) {
                     std::string ch = a.get("channel").asString();
                     if (!ch.empty() && ch[0] == '#') ch = ch.substr(1);
                     std::vector<const studio::Message*> msgs;
                     for (const auto& m : s.messages()) {
                         if ((ch == "*" || m.channel == studio::slugify(ch)) && studio::idNumber(m.id) > after) msgs.push_back(&m);
                     }
                     size_t start = msgs.size() > limit ? msgs.size() - limit : 0;
                     for (size_t i = start; i < msgs.size(); ++i) {
                         out.push(msgs[i]->toJson());
                         text += line(*msgs[i]);
                     }
                     return ToolResult::json(Json::object({{"channel", ch}, {"messages", out}}), text.empty() ? "(empty)" : text);
                 }
                 bool explicitAgent = a.contains("agent");
                 auto who = memberFor(engine, a, *actor, false);
                 if (!who) return err(who.error());
                 bool unread = a.get("unread_only").asBool(true);
                 for (const auto* m : s.inbox(*who, unread, limit, !explicitAgent && !who->empty())) {
                     out.push(m->toJson());
                     text += line(*m);
                 }
                 return ToolResult::json(Json::object({{"agent", *who}, {"messages", out}}), text.empty() ? "(no new messages)" : text);
             }});

    // ---------------------------------------------------------------- playtests
    reg.add({"playtest_run", "Playtest",
             "Play the game with a bot in a sandbox copy of the scene (the scene you edit is untouched) and get a "
             "report: completion rate, time-to-goal, deaths and their causes, fails, damage, stuck periods, objectives, "
             "coverage, frame cost, screenshots at notable moments and a top-down heatmap (returned as an image). "
             "Policies: goal_seeker (walks to entities tagged \"goal\"/\"objective\", avoids \"hazard\"/\"enemy\"), "
             "explorer (covers unexplored space), random, scripted (script: [{t: seconds, hold|release|press|click|event: "
             "...}] — the same arguments as sim_input). persona {reaction_time, skill, curiosity, patience} shapes the "
             "bot; playtester agents use their own persona by default. runs>1 aggregates several seeds. Games should "
             "tag the player \"player\" and emit \"death\" / \"fail\" / \"damage\" / \"objective\" events. Saved under "
             "studio/playtests/<id>/; findings are returned for filing as feedback.",
             "studio",
             object({{"policy", enumeration({"goal_seeker", "explorer", "random", "scripted"}, "Bot policy (default goal_seeker)")},
                     {"seconds", number("Simulated seconds per run (default 30, max 600)")},
                     {"runs", integer("Runs with consecutive seeds (default 1, max 20)")},
                     {"seed", integer("First seed (default 1)")},
                     {"persona", objectArg("{reaction_time: s, skill: 0..1, curiosity: 0..1, patience: s}")},
                     {"controls", objectArg("Key mapping {up, down, left, right, jump} (default w/s/a/d/space)")},
                     {"script", array(objectArg("{t, ...sim_input arguments}"), "Input timeline (scripted policy)")},
                     {"player", schema::entity("Player entity (default: tagged \"player\")")},
                     {"goal_radius", number("Meters from a goal that count as reaching it (default 1)")},
                     {"screenshots", boolean("Capture screenshots (default true)")},
                     {"screenshot_interval", number("Also capture every N seconds")},
                     {"max_screenshots", integer("Screenshot cap (default 8)")},
                     {"label", string("Label for the report")},
                     {"agent", string("Use this playtester's persona")},
                     {"include_heatmap", boolean("Return the heatmap image (default true)")},
                     {"as", asArg()}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 auto r = studio::runAndRecordPlaytest(engine, a, *actor);
                 if (!r) return err(r.error());
                 Json brief = Json::object({{"id", r->get("id")},
                                            {"summary", r->get("summary")},
                                            {"metrics", r->get("metrics")},
                                            {"death_causes", r->get("death_causes")},
                                            {"findings", r->get("findings")},
                                            {"screenshots", r->get("screenshots")},
                                            {"files", r->get("files")},
                                            {"warnings", r->get("warnings")}});
                 Json runs = Json::array();
                 for (const auto& run : r->get("runs").elements()) {
                     Json x = run;
                     x.erase("trajectory");
                     x.erase("events");
                     runs.push(x);
                 }
                 brief["runs"] = runs;
                 std::string text = r->get("id").asString() + ": " + r->get("summary").asString();
                 for (const auto& f : r->get("findings").elements()) text += "\n  finding: " + f.get("summary").asString();
                 for (const auto& w : r->get("warnings").elements()) text += "\n  warning: " + w.asString();
                 ToolResult res = ToolResult::json(brief, text);
                 if (a.get("include_heatmap").asBool(true)) {
                     std::string path = engine.resolvePath("studio/playtests/" + r->get("id").asString() + "/heatmap.png");
                     std::ifstream f(path, std::ios::binary);
                     std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                     if (!bytes.empty()) res.image(str::base64Encode(bytes.data(), bytes.size()));
                 }
                 return res;
             }});

    reg.add({"playtest_compare", "Compare playtests",
             "Before/after comparison of two playtests (ids like P-3): every metric with its delta and whether it got "
             "better or worse (direction-aware: fewer deaths is better, higher completion is better). Pass feedback to "
             "record the result as that feedback's measured effect: improved → verified, regressed → regressed (its "
             "tasks reopen). Timing metrics (*_ms, est_fps) depend on the machine's load, so they only decide the "
             "verdict for performance feedback or when a target names them; they are always listed.",
             "studio",
             object({{"before", string("Earlier playtest id")},
                     {"after", string("Later playtest id")},
                     {"feedback", string("Record the effect for this feedback item")},
                     {"as", asArg()}},
                    {"before", "after"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto actor = actorFor(engine, a, ctx);
                 if (!actor) return err(actor.error());
                 Studio& s = S(engine);
                 auto before = s.loadPlaytestReport(a.get("before").asString());
                 if (!before) return err(before.error());
                 auto after = s.loadPlaytestReport(a.get("after").asString());
                 if (!after) return err(after.error());
                 Json cmp = studio::compareMetrics(before->get("metrics"), after->get("metrics"));
                 Json out = Json::object({{"before", before->get("id")}, {"after", after->get("id")}, {"metrics", cmp},
                                          {"verdict", studio::effectVerdict(cmp, {}, Json::object())}});
                 std::string text = before->get("id").asString() + " → " + after->get("id").asString() + ": " +
                                    out.get("verdict").asString();
                 for (const auto& [k, row] : cmp.members()) {
                     if (studio::metricDirection(k) == 0) continue;
                     text += "\n  " + k + " " + row.get("before").dump() + " → " + row.get("after").dump() + " (" +
                             row.get("verdict").asString() + ")";
                 }
                 if (a.contains("feedback")) {
                     auto eff = s.recordEffect(a.get("feedback").asString(), before->get("metrics"), after->get("metrics"),
                                               before->get("id").asString(), after->get("id").asString(), *actor);
                     if (!eff) return err(eff.error());
                     out["effect"] = *eff;
                     text += "\nrecorded effect for " + a.get("feedback").asString() + ": " + eff->get("status").asString();
                 }
                 return ToolResult::json(out, text);
             }});
}

}  // namespace sky::tools
