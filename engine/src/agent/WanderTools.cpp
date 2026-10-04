// Wander 2 tools: the language guide, checking, behaviors with intent + spec + code,
// in-language tests in a sandbox, the node-graph view, runtime inspection and AOT.

#include <algorithm>
#include <set>

#include "ToolHelpers.h"
#include "skywalker/assets/Prefab.h"
#include "skywalker/core/Strings.h"
#include "skywalker/native/NativeModules.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/Graph.h"
#include "skywalker/wander/Parser.h"
#include "skywalker/wander/Testing.h"

namespace sky::tools {

namespace {

using namespace schema;

Json compileReport(Engine& engine, const std::string& source) {
    return wander::compile(source, engine.runtime().compileOptions()).toJson();
}

// The behaviors array of an entity and the entry named `name` (null if absent).
struct BehaviorRef {
    Json list;
    int index = -1;
    const Json* get() const { return index >= 0 ? &list.elements()[static_cast<size_t>(index)] : nullptr; }
};

BehaviorRef findBehavior(Engine& engine, EntityId id, const std::string& name) {
    BehaviorRef r;
    r.list = engine.scene().entityToJson(id).get("behaviors");
    if (!r.list.isArray()) r.list = Json::array();
    for (size_t i = 0; i < r.list.size(); ++i) {
        if (r.list[i].get("name").asString() == name) r.index = static_cast<int>(i);
    }
    return r;
}

ToolResult noBehavior(const BehaviorRef& r, const std::string& name) {
    std::vector<std::string> names;
    std::string all;
    for (const auto& b : r.list.elements()) {
        names.push_back(b.get("name").asString());
        all += (all.empty() ? "" : ", ") + names.back();
    }
    std::string guess = str::closest(name, names, 3);
    std::string hint = !guess.empty() ? "did you mean \"" + guess + "\"?"
                       : names.empty() ? "the entity has no behaviors (behavior_set adds one)"
                                       : "behaviors: " + all;
    return ToolResult::error(Error::make("not_found", "no behavior \"" + name + "\" on this entity", hint));
}

// Behavior source from {entity, name} or {source}.
Result<std::string> sourceArg(Engine& engine, const Json& a, EntityId* entityOut, std::string* nameOut) {
    if (a.contains("source")) return a.get("source").asString();
    if (!a.contains("entity") || !a.contains("name")) {
        return Error::make("invalid_argument", "pass `source`, or `entity` and `name` of a behavior");
    }
    auto id = resolve(engine, a.get("entity"));
    if (!id) return id.error();
    std::string name = a.get("name").asString();
    BehaviorRef r = findBehavior(engine, *id, name);
    if (!r.get()) {
        ToolResult err = noBehavior(r, name);
        return Error::make("not_found", err.content.empty() ? "no such behavior" : err.content.front().text);
    }
    if (entityOut) *entityOut = *id;
    if (nameOut) *nameOut = name;
    return r.get()->get("source").asString();
}

// Saves a behavior (create or replace), compiling first. Returns the compile report.
ToolResult saveBehavior(Engine& engine, ToolContext& ctx, EntityId id, const std::string& name, const Json& fields,
                        bool allowErrors, const std::string& verb) {
    Scene& s = engine.scene();
    BehaviorRef r = findBehavior(engine, id, name);
    if (r.index < 0) {
        r.list.push(Json::object({{"name", name}, {"intent", ""}, {"source", ""}, {"enabled", true}}));
        r.index = static_cast<int>(r.list.size() - 1);
    }
    Json& b = r.list.elements()[static_cast<size_t>(r.index)];
    for (const auto& [k, v] : fields.members()) b[k] = v;
    Json report = compileReport(engine, b.get("source").asString());
    if (!report.get("ok").asBool() && !allowErrors) {
        ToolResult res = ToolResult::json(report, "rejected: the Wander source has errors (nothing changed)");
        res.isError = true;
        return res;
    }
    Status st = engine.edit(ctx.actor, "Behavior " + name, [&] { return s.setBehaviors(id, r.list); });
    if (!st) return fail(st);
    return ToolResult::json(report, "behavior \"" + name + "\" " + verb + " on " + s.record(id)->name);
}

Json specView(Engine& engine, EntityId id, const Json& behavior) {
    std::string source = behavior.get("source").asString();
    wander::CompileResult cr = wander::compile(source, engine.runtime().compileOptions());
    const Json& spec = behavior.get("spec");
    Json out = Json::object({{"entity", id},
                             {"behavior", behavior.get("name")},
                             {"intent", behavior.get("intent")},
                             {"spec", spec.isNull() ? Json::object() : spec},
                             {"compiles", cr.ok()}});
    std::set<std::string> testNames;
    if (cr.ok()) {
        Json triggers = Json::array(), states = Json::array(), params = Json::array(), vars = Json::array(),
             tests = Json::array(), fns = Json::array();
        const EntityRecord* rec = engine.scene().record(id);
        for (const auto& b : cr.program->behaviors) {
            for (const auto& h : b.handlers) {
                std::string t = h.custom ? h.argument : wander::toString(h.trigger);
                if (!h.custom && !h.argument.empty()) t += " \"" + h.argument + "\"";
                if (h.state >= 0) t = b.states[h.state].name + ": " + t;
                triggers.push(t);
            }
            for (const auto& st : b.states) states.push(st.name);
            for (const auto& v : b.vars) {
                Json j = Json::object({{"name", v.name}, {"type", wander::typeSetName(v.type)}});
                if (v.isParam) {
                    j["default"] = v.defaultValue;
                    if (v.hasRange) {
                        j["min"] = v.min;
                        j["max"] = v.max;
                    }
                    if (!v.doc.empty()) j["doc"] = v.doc;
                    const Json* cur = rec ? rec->vars.find(v.name) : nullptr;
                    j["value"] = cur ? *cur : v.defaultValue;
                    params.push(j);
                } else {
                    vars.push(j);
                }
            }
            for (const auto& t : b.tests) {
                tests.push(t.name);
                testNames.insert(t.name);
            }
        }
        for (const auto& t : cr.program->fileTests) {
            tests.push(t.name);
            testNames.insert(t.name);
        }
        for (const auto& f : cr.program->functions) fns.push(f.name);
        out["derived"] = Json::object({{"triggers", triggers}, {"states", states}, {"params", params}, {"vars", vars},
                                       {"functions", fns}, {"tests", tests}});
    } else {
        out["diagnostics"] = cr.toJson().get("diagnostics");
    }
    // Coverage: every rule should name at least one existing test.
    Json uncovered = Json::array(), dangling = Json::array();
    std::set<std::string> referenced;
    int rules = 0;
    for (const auto& r : spec.get("rules").elements()) {
        ++rules;
        std::string text = r.isString() ? r.asString() : r.get("text").asString();
        bool covered = false;
        for (const auto& t : r.get("tests").elements()) {
            referenced.insert(t.asString());
            if (testNames.count(t.asString())) covered = true;
            else dangling.push(Json::object({{"rule", text}, {"test", t}}));
        }
        if (!covered) uncovered.push(text);
    }
    Json unlinked = Json::array();
    for (const auto& t : testNames) {
        if (!referenced.count(t)) unlinked.push(t);
    }
    out["coverage"] = Json::object({{"rules", rules},
                                    {"covered", rules - static_cast<int>(uncovered.size())},
                                    {"uncovered_rules", uncovered},
                                    {"missing_tests", dangling},
                                    {"tests_without_rule", unlinked}});
    std::string next;
    if (rules == 0) next = "derive rules from the intent (behavior_set spec={rules:[{text, tests:[...]}]})";
    else if (uncovered.size()) next = "write a `test` block for each uncovered rule, then run wander_test";
    else next = "run wander_test to verify every rule";
    out["next_step"] = next;
    return out;
}

}  // namespace

void addWanderTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"wander_reference", "Wander language reference",
             "The Wander 2 guide for writing behaviors: syntax, triggers, statements, idioms, and every builtin function "
             "with its signature (generated from the builtin registry, including subsystem and native-module builtins). "
             "Read it once before writing behaviors. topic=\"<category or function>\" returns detailed entries with "
             "parameter types and examples, e.g. topic=\"physics\", topic=\"list\", topic=\"raycast\".",
             "wander", object({{"topic", string("Optional: a builtin category (math, vector, scene, list, physics...) or a function name")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 std::string topic = a.get("topic").asString();
                 if (topic.empty()) return ToolResult::text(wander::referenceText(engine.builtins()));
                 Json entries = wander::builtinReference(engine.builtins(), topic);
                 if (entries.size() == 0) {
                     std::vector<std::string> names = engine.builtins().functionNames();
                     for (const auto& d : engine.builtins().all()) names.push_back(d->category);
                     std::string guess = str::closest(topic, names, 3);
                     return ToolResult::error(Error::make("not_found", "no builtin or category \"" + topic + "\"",
                                                          guess.empty() ? "call wander_reference without a topic for the list"
                                                                        : "did you mean \"" + guess + "\"?"));
                 }
                 return ToolResult::json(Json::object({{"entries", entries}}), std::to_string(entries.size()) + " entries");
             }});

    reg.add({"wander_check", "Check Wander code",
             "Compile Wander source without attaching it, exactly as the engine would run it (components, builtins, "
             "`use`d modules from the project). Returns diagnostics (line, column, stable code, message, did-you-mean "
             "hint, file for module errors), warnings, and a summary of the behaviors (vars, params, handlers, states, "
             "tests). format=true also returns the canonical formatting; disassemble=true the bytecode listing.",
             "wander",
             object({{"source", string("Wander source code")},
                     {"format", boolean("Also return the canonically formatted source")},
                     {"disassemble", boolean("Also return the bytecode listing (performance work)")}},
                    {"source"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto r = wander::compile(a.get("source").asString(), engine.runtime().compileOptions());
                 Json j = r.toJson();
                 if (a.get("format").asBool() && r.module && r.errorCount() == 0) j["formatted"] = wander::format(*r.module);
                 if (a.get("disassemble").asBool() && r.program) j["bytecode"] = r.program->disassemble();
                 return ToolResult::json(j, r.ok() ? "compiles cleanly" : std::to_string(r.errorCount()) + " error(s)");
             }});

    reg.add({"behavior_set", "Set behavior",
             "Create or replace a named behavior on an entity: its natural-language `intent` (the human source of truth), "
             "an optional structured `spec` derived from the intent ({summary, rules: [{text, tests: [test names]}]}), and "
             "the Wander `source`. The source is compiled; by default code with errors is rejected and the diagnostics "
             "returned so you can fix and retry. Workflow: intent -> spec rules -> code with a `test` block per rule -> "
             "wander_test -> behavior_spec to check coverage.",
             "wander",
             object({{"entity", schema::entity()},
                     {"name", string("Behavior name, e.g. \"Patrol\"")},
                     {"intent", string("What the behavior should do, in plain language")},
                     {"spec", Json::object({{"type", "object"},
                                            {"description", "Structured spec: {summary, rules: [{text, tests: [...]}], notes}"}})},
                     {"source", string("Wander source code")},
                     {"enabled", boolean("Enabled (default true)")},
                     {"allow_errors", boolean("Save even if the code does not compile (default false)")}},
                    {"entity", "name"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Json fields = Json::object();
                 for (const char* k : {"intent", "source", "enabled", "spec"}) {
                     if (a.contains(k)) fields[k] = a.get(k);
                 }
                 return saveBehavior(engine, ctx, *id, a.get("name").asString(), fields, a.get("allow_errors").asBool(), "saved");
             }});

    reg.add({"behavior_remove", "Remove behavior", "Remove a named behavior from an entity.", "wander",
             object({{"entity", schema::entity()}, {"name", string("Behavior name")}}, {"entity", "name"}), true, true,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 BehaviorRef r = findBehavior(engine, *id, a.get("name").asString());
                 if (!r.get()) return noBehavior(r, a.get("name").asString());
                 Json kept = Json::array();
                 for (size_t i = 0; i < r.list.size(); ++i) {
                     if (static_cast<int>(i) != r.index) kept.push(r.list[i]);
                 }
                 Status st = engine.edit(ctx.actor, "Remove behavior", [&] { return engine.scene().setBehaviors(*id, kept); });
                 if (!st) return fail(st);
                 return ToolResult::text("removed");
             }});

    reg.add({"behavior_spec", "Behavior spec",
             "The intent -> spec -> code view of a behavior: its intent, the stored spec (rules an agent derived from the "
             "intent, each naming the tests that verify it), what the code declares (triggers, states, tunable params with "
             "ranges and current per-entity values, vars, functions, tests), and coverage (rules without tests, tests "
             "without rules). Use it to keep intent, spec and code in sync.",
             "wander", object({{"entity", schema::entity()}, {"name", string("Behavior name")}}, {"entity", "name"}), false,
             false, [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 BehaviorRef r = findBehavior(engine, *id, a.get("name").asString());
                 if (!r.get()) return noBehavior(r, a.get("name").asString());
                 Json v = specView(engine, *id, *r.get());
                 const Json& cov = v.get("coverage");
                 return ToolResult::json(v, std::to_string(cov.get("covered").asInt()) + "/" +
                                                std::to_string(cov.get("rules").asInt()) + " rules covered by tests");
             }});

    reg.add({"wander_test", "Run behavior tests",
             "Run the `test \"...\"` blocks of a behavior in a sandbox simulation (the real scene is never touched): each "
             "test gets a fresh world, ticks once (on start ran), then runs with self = the entity under test; `wait` "
             "advances the simulation at 1/60 s, `emit`/`press`/`hold`/`release`/`click` inject input, and `expect` "
             "reports failures with the compared values. Pass entity+name (mode \"entity\" copies that entity's components, "
             "tags and vars; \"scene\" runs inside a copy of the whole scene; \"isolated\" uses a plain entity) or raw "
             "`source`. Returns pass/fail per test, failures with line numbers, logs and simulated time.",
             "wander",
             object({{"entity", schema::entity("Entity whose behavior to test")},
                     {"name", string("Behavior name")},
                     {"source", string("Test this source instead of a saved behavior")},
                     {"filter", string("Only tests whose name contains this")},
                     {"mode", enumeration({"entity", "scene", "isolated"}, "Sandbox contents (default: entity)")},
                     {"max_seconds", number("Simulated time limit per test (default 600)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 EntityId id = kNoEntity;
                 std::string name;
                 auto src = sourceArg(engine, a, &id, &name);
                 if (!src) return ToolResult::error(src.error());
                 if (!id && a.contains("entity")) {
                     auto r = resolve(engine, a.get("entity"));
                     if (!r) return ToolResult::error(r.error());
                     id = *r;
                 }
                 wander::TestOptions o;
                 o.behaviorName = name.empty() ? (a.contains("name") ? a.get("name").asString() : "Behavior") : name;
                 o.filter = a.get("filter").asString();
                 o.registry = &engine.builtins();
                 o.maxTicks = static_cast<int>(std::clamp(a.get("max_seconds").asNumber(600), 1.0, 3600.0) * 60);
                 std::string mode = a.get("mode").asString(id ? "entity" : "isolated");
                 if (mode == "scene" && id) {
                     o.scene = &engine.scene();
                     o.entityInScene = id;
                 } else if (mode == "entity" && id) {
                     o.entity = engine.scene().entityToJson(id);
                 }
                 std::string project = engine.config().projectDir;
                 o.configure = [&engine, project](wander::Runtime& rt, Scene& sandbox) {
                     rt.setProjectDir(project);
                     rt.spawnPrefab = [&engine, &sandbox](const std::string& ref, Vec3 pos, const std::string& n) -> Result<EntityId> {
                         auto prefab = engine.loadPrefabAsset(ref);
                         if (!prefab) return prefab.error();
                         PrefabPlacement p;
                         p.hasPosition = true;
                         p.position = pos;
                         p.name = n;
                         return instantiatePrefab(sandbox, prefab.value(), p);
                     };
                 };
                 wander::TestReport rep = wander::runTests(src.value(), o);
                 Json j = rep.toJson();
                 if (!rep.compiled) {
                     ToolResult r = ToolResult::json(j, "the behavior does not compile");
                     r.isError = true;
                     return r;
                 }
                 if (rep.tests.empty()) {
                     return ToolResult::json(j, "no tests: add `test \"name\" ... expect ... end` blocks to the behavior");
                 }
                 return ToolResult::json(j, std::to_string(rep.passed) + " passed, " + std::to_string(rep.failed) + " failed");
             }});

    reg.add({"behavior_graph", "Behavior as a node graph",
             "Visual node graph of a behavior (or raw `source`): bodies (handlers, fns, tests, state handlers) "
             "with an entry node, exec-flow statement nodes wired by exec pins (if: then/elif/else; loops: body), "
             "expression nodes wired into data pins, and literals/names inline on pins (`value`). Positions come from the "
             "saved layout or an automatic layout. Edit the JSON and send it back with behavior_from_graph. "
             "palette=true adds every node type with its pins.",
             "wander",
             object({{"entity", schema::entity()},
                     {"name", string("Behavior name")},
                     {"source", string("Graph this source instead")},
                     {"palette", boolean("Include the node palette")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 EntityId id = kNoEntity;
                 std::string name;
                 auto src = sourceArg(engine, a, &id, &name);
                 if (!src) return ToolResult::error(src.error());
                 wander::ParseResult pr = wander::parse(src.value());
                 if (!pr.ok()) {
                     Json diags = Json::array();
                     for (const auto& d : pr.diagnostics) diags.push(wander::diagnosticToJson(d));
                     ToolResult r = ToolResult::json(Json::object({{"diagnostics", diags}}),
                                                     "the source does not parse; fix it before viewing it as a graph");
                     r.isError = true;
                     return r;
                 }
                 Json layout;
                 if (id) {
                     BehaviorRef r = findBehavior(engine, id, name);
                     if (r.get()) layout = r.get()->get("graph");
                 }
                 Json g = wander::toGraph(*pr.module, layout);
                 if (a.get("palette").asBool()) g["palette"] = wander::graphPalette();
                 size_t nodes = wander::graphLayout(g).size();
                 return ToolResult::json(g, std::to_string(nodes) + " nodes");
             }});

    reg.add({"behavior_from_graph", "Behavior from a node graph",
             "Turn a node graph (from behavior_graph, possibly edited) back into Wander source. With entity+name the code "
             "is compiled and saved as that behavior (rejected on errors unless allow_errors) and the node positions are "
             "stored, so the editor reopens the same layout. Returns the generated source and diagnostics.",
             "wander",
             object({{"graph", Json::object({{"type", "object"}, {"description", "Graph JSON (format wander-graph)"}})},
                     {"entity", schema::entity()},
                     {"name", string("Behavior name to save into")},
                     {"allow_errors", boolean("Save even if the code does not compile")}},
                    {"graph"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto code = wander::fromGraph(a.get("graph"));
                 if (!code) return ToolResult::error(code.error());
                 if (!a.contains("entity")) {
                     Json report = compileReport(engine, code.value());
                     report["source"] = code.value();
                     return ToolResult::json(report, report.get("ok").asBool() ? "graph -> code (compiles)" : "graph -> code (has errors)");
                 }
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::string name = a.get("name").asString("Behavior");
                 Json fields = Json::object({{"source", code.value()}, {"graph", wander::graphLayout(a.get("graph"))}});
                 ToolResult r = saveBehavior(engine, ctx, *id, name, fields, a.get("allow_errors").asBool(), "saved from graph");
                 r.structured["source"] = code.value();
                 return r;
             }});

    reg.add({"wander_inspect", "Inspect running behaviors",
             "While playing: the runtime state of an entity's behaviors — current state of each state machine and how long "
             "it has been active, handlers waiting in `wait` (and for how long), plus the entity's vars. Use it to debug "
             "why a behavior is not doing what its intent says.",
             "wander", object({{"entity", schema::entity()}}, {"entity"}), false, false, [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Json j = Json::object({{"entity", *id},
                                        {"playing", engine.playState() != PlayState::Editing},
                                        {"scripts", engine.runtime().inspect(*id)},
                                        {"vars", engine.scene().record(*id)->vars}});
                 return ToolResult::json(j);
             }});

    reg.add({"wander_compile_native", "Compile behaviors to native code",
             "AOT-compile Wander behaviors to C++ and load them as native code (system clang++, cached by program hash, "
             "so unchanged behaviors load instantly). Native code has identical semantics (same results, errors and step "
             "budget) and runs hot loops several times faster; anything that fails to build keeps running in the VM. "
             "Pass entity to compile its behaviors, or nothing for every behavior in the scene. auto=true compiles every "
             "behavior automatically whenever play starts. Edited behaviors fall back to the VM until compiled again.",
             "code",
             object({{"entity", schema::entity("Only this entity's behaviors")},
                     {"force", boolean("Rebuild even if a cached library exists")},
                     {"auto", boolean("Compile all behaviors natively at every play start")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 std::vector<EntityId> ids;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     ids.push_back(*id);
                 }
                 if (a.contains("auto")) engine.native().setAutoCompile(a.get("auto").asBool());
                 auto r = engine.native().compileBehaviors(ids, a.get("force").asBool());
                 if (!r) return ToolResult::error(r.error());
                 Json j = r.value();
                 j["auto"] = engine.native().autoCompile();
                 return ToolResult::json(j, std::to_string(j.get("compiled").asInt()) + " program(s) native, " +
                                                std::to_string(j.get("failed").asInt()) + " failed");
             }});
}

}  // namespace sky::tools
