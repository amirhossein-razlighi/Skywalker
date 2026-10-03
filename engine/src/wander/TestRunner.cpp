// Sandbox runner for Wander `test` blocks.

#include "skywalker/wander/Testing.h"

#include "skywalker/core/Strings.h"

namespace sky::wander {

namespace {
constexpr float kDt = 1.f / 60.f;
}

Json TestCaseResult::toJson() const {
    Json fails = Json::array();
    for (const auto& f : failures) {
        Json j = Json::object({{"line", f.line}, {"message", f.message}});
        if (!f.detail.empty()) j["detail"] = f.detail;
        fails.push(j);
    }
    Json logsJ = Json::array();
    for (const auto& l : logs) logsJ.push(l);
    Json j = Json::object({{"name", name},
                           {"passed", passed},
                           {"ticks", ticks},
                           {"seconds", ticks * static_cast<double>(kDt)},
                           {"expectations", expectations}});
    if (!behavior.empty()) j["behavior"] = behavior;
    if (fails.size()) j["failures"] = fails;
    if (logsJ.size()) j["logs"] = logsJ;
    return j;
}

Json TestReport::toJson() const {
    Json list = Json::array();
    for (const auto& t : tests) list.push(t.toJson());
    Json diags = Json::array();
    for (const auto& d : diagnostics) diags.push(diagnosticToJson(d));
    return Json::object({{"compiled", compiled},
                         {"passed", passed},
                         {"failed", failed},
                         {"total", static_cast<int>(tests.size())},
                         {"tests", list},
                         {"diagnostics", diags}});
}

namespace {

struct Pending {
    std::string behavior;  // "" for file-level tests
    std::string name;
};

// Finds the test proto in the sandbox's own copy of the program.
int findTest(const Program& p, const Pending& t) {
    for (const auto& b : p.behaviors) {
        if (b.name != t.behavior) continue;
        for (const auto& ti : b.tests) {
            if (ti.name == t.name) return ti.proto;
        }
    }
    for (const auto& ti : p.fileTests) {
        if (t.behavior.empty() && ti.name == t.name) return ti.proto;
    }
    return -1;
}

TestCaseResult runOne(std::string_view source, const TestOptions& o, const Pending& t) {
    TestCaseResult result;
    result.name = t.name;
    result.behavior = t.behavior;
    auto fail = [&](int line, std::string msg, std::string detail = {}) {
        result.failures.push_back({line, std::move(msg), std::move(detail)});
    };

    Scene sandbox;
    EntityId self = kNoEntity;
    if (o.scene) {
        if (Status s = sandbox.loadJson(o.scene->toJson()); !s) {
            fail(0, "cannot copy the scene: " + s.error().message);
            return result;
        }
        self = o.entityInScene;
        if (!sandbox.exists(self)) {
            fail(0, "the entity under test is not in the scene");
            return result;
        }
    } else {
        self = sandbox.create(o.entity.isObject() ? o.entity.get("name").asString("Subject") : "Subject");
        bool hasComponents = false;
        if (o.entity.isObject()) {
            Json doc = Json::object();
            for (const char* k : {"tags", "vars", "enabled"}) {
                if (o.entity.contains(k)) doc[k] = o.entity.get(k);
            }
            if (const Json* c = o.entity.find("components"); c && c->isObject()) {
                doc["components"] = *c;
                hasComponents = c->size() > 0;
            }
            if (Status s = sandbox.applyEntityJson(self, doc); !s) fail(0, "cannot copy the entity: " + s.error().message);
        }
        if (!hasComponents) (void)sandbox.patchComponent(self, "mesh", Json::object());
    }
    // Attach the behavior under test (replacing a behavior of the same name).
    Json list = sandbox.entityToJson(self).get("behaviors");
    if (!list.isArray()) list = Json::array();
    Json kept = Json::array();
    for (const auto& b : list.elements()) {
        if (b.get("name").asString() != o.behaviorName) kept.push(b);
    }
    kept.push(Json::object({{"name", o.behaviorName}, {"source", std::string(source)}}));
    (void)sandbox.setBehaviors(self, kept);

    Runtime rt(sandbox, o.registry);
    if (o.configure) o.configure(rt, sandbox);
    rt.reset();
    InputState input;
    TestHooks hooks;
    hooks.press = [&](const std::string& k) { input.pressed.insert(k); };
    hooks.hold = [&](const std::string& k) { input.held.insert(k); };
    hooks.release = [&](const std::string& k) { input.held.erase(k); };
    hooks.click = [&](EntityId e) { input.clicked.push_back(e); };
    hooks.fail = [&](SourceLoc loc, const std::string& message, const std::string& detail) { fail(loc.line, message, detail); };
    auto collect = [&] {
        for (const auto& m : rt.drainMessages()) {
            if (m.kind == RuntimeMessage::Kind::Log) {
                result.logs.push_back(m.text);
            } else if (m.script != "test") {
                fail(m.line, std::string(m.kind == RuntimeMessage::Kind::Compile ? "compile error" : "runtime error") +
                                 " in " + m.script + ": " + m.text);
            }
        }
    };
    auto tick = [&] {
        rt.tick(kDt, input);
        input.pressed.clear();
        input.clicked.clear();
        ++result.ticks;
    };
    tick();  // `on start` and initial states run first
    std::shared_ptr<const Program> program;
    if (const Behavior* b = sandbox.get<Behavior>(self)) {
        for (const auto& s : b->scripts) {
            if (s.name == o.behaviorName) program = s.program;
        }
    }
    int proto = program ? findTest(*program, t) : -1;
    if (proto < 0) {
        collect();
        fail(0, "the behavior did not compile in the sandbox");
        return result;
    }
    auto driver = rt.startTest(program, proto, self, &hooks);
    collect();
    while (!rt.testFinished(*driver)) {
        if (result.ticks > o.maxTicks) {
            const Proto& P = program->protos[proto];
            int line = driver->pc > 0 && driver->pc <= P.locs.size() ? P.locs[driver->pc - 1].line : 0;
            fail(line, "the test timed out after " + std::to_string(o.maxTicks) + " ticks",
                 "a wait never finished (wait until a condition that never became true?)");
            break;
        }
        tick();
        rt.stepTest(*driver, kDt);
        collect();
    }
    result.expectations = hooks.expectations;
    result.passed = result.failures.empty();
    return result;
}

}  // namespace

TestReport runTests(std::string_view source, const TestOptions& o) {
    TestReport report;
    Scene probe;
    Runtime probeRt(probe, o.registry);
    if (o.configure) o.configure(probeRt, probe);
    CompileResult cr = compile(source, probeRt.compileOptions());
    report.diagnostics = cr.diagnostics;
    report.compiled = cr.ok();
    if (!cr.ok()) return report;
    std::vector<Pending> tests;
    for (const auto& b : cr.program->behaviors) {
        for (const auto& t : b.tests) tests.push_back({b.name, t.name});
    }
    for (const auto& t : cr.program->fileTests) tests.push_back({"", t.name});
    for (const auto& t : tests) {
        if (!o.filter.empty() && str::lower(t.name).find(str::lower(o.filter)) == std::string::npos) continue;
        TestCaseResult r = runOne(source, o, t);
        (r.passed ? report.passed : report.failed) += 1;
        report.tests.push_back(std::move(r));
    }
    return report;
}

}  // namespace sky::wander
