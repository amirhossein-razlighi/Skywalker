#pragma once
// Running Wander `test` blocks in a sandbox (the `wander_test` tool).
//
// Each test gets a fresh sandbox scene with the entity under test (a plain entity, a
// copy of a real entity's components/tags/vars, or a copy of the whole scene) carrying
// the behavior. The sandbox ticks once (so `on start` and initial states have run), then
// the test body runs as a coroutine with `self` = that entity: `wait` advances the
// simulation (fixed 1/60 s ticks), `emit`/`press`/`hold`/`release`/`click` inject input,
// and `expect` records failures with the compared values. Nothing touches the real scene.

#include <functional>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/Runtime.h"

namespace sky::wander {

struct TestOptions {
    std::string behaviorName = "Behavior";  // name of the script in the sandbox
    std::string filter;                     // only tests whose name contains this
    Json entity;                            // entity document to copy (components, tags, vars, name)
    const Scene* scene = nullptr;           // run inside a copy of this scene...
    EntityId entityInScene = kNoEntity;     // ...on this entity (its behavior with behaviorName is replaced)
    int maxTicks = 36000;                   // per test (10 simulated minutes)
    const BuiltinRegistry* registry = nullptr;  // builtins (null: the global registry)
    /// Called on each sandbox runtime before it runs (modules, prefab spawning, services).
    std::function<void(Runtime&, Scene&)> configure;
};

struct TestFailure {
    int line = 0;
    std::string message;
    std::string detail;
};

struct TestCaseResult {
    std::string name;
    std::string behavior;
    bool passed = false;
    int ticks = 0;
    int expectations = 0;
    std::vector<TestFailure> failures;
    std::vector<std::string> logs;
    Json toJson() const;
};

struct TestReport {
    bool compiled = false;
    std::vector<Diagnostic> diagnostics;
    std::vector<TestCaseResult> tests;
    int passed = 0;
    int failed = 0;
    Json toJson() const;
};

TestReport runTests(std::string_view source, const TestOptions& options);

}  // namespace sky::wander
