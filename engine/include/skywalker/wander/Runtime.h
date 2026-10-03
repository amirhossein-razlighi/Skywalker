#pragma once
// Deterministic Wander runtime: schedules behaviors and runs their bytecode.
//
// Each entity/script pair gets an *instance* (state machines, timers, waiting
// coroutines). Every fixed tick the runtime visits active entities in scene order and,
// per instance: starts it (var initializers, `on start`, initial state), resumes due
// coroutines, delivers last tick's events, key presses and clicks, then runs `on tick`
// (behavior-level, then the current state's). Randomness is seeded, so runs replay
// exactly; every handler run has an execution budget, so scripts can never hang the
// engine; scripts that keep failing are disabled and reported.
//
// Entity vars live in per-entity tables during play (fast slots for declared vars) and
// are mirrored into EntityRecord::vars after every tick, so tools and agents always see
// current values; edits made between ticks (by tools) are picked up on the next tick.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Random.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Builtins.h"
#include "skywalker/wander/Bytecode.h"
#include "skywalker/wander/Compiler.h"

namespace sky::wander {

struct RuntimeMessage {
    enum class Kind { Log, Error, Compile } kind = Kind::Log;
    EntityId entity = kNoEntity;
    std::string script;
    int line = 0;
    std::string text;
    std::string file;  // module path when the error is inside a `use`d module

    Json toJson() const;
};

/// Native (AOT-compiled) code for one program; see wander/Aot.h.
struct NativeProgram;

/// Hooks used by the test runner (wander_test) to drive a sandbox.
struct TestHooks {
    std::function<void(const std::string& key)> press;
    std::function<void(const std::string& key)> hold;
    std::function<void(const std::string& key)> release;
    std::function<void(EntityId)> click;
    /// A failed `expect` ("expected hp == 2", detail: the compared values) or a runtime error.
    std::function<void(SourceLoc loc, const std::string& message, const std::string& detail)> fail;
    int expectations = 0;
};

class Runtime {
public:
    explicit Runtime(Scene& scene, const BuiltinRegistry* registry = nullptr);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Forget all per-instance state (call when play starts/stops). Events queued from
    /// outside (e.g. agent input injected before pressing play) can be kept.
    void reset(bool keepQueuedEvents = false);

    /// Compiles any script whose source (or used modules) changed. Errors become messages.
    void compileScripts();

    /// Advances one fixed step.
    void tick(float dt, const InputState& input);

    /// Queue an event for delivery on the next tick. `other` is the counterpart entity
    /// (`other` in the handler: the sender, or the body we collided with), `payload` is
    /// `data` in the handler.
    void emit(std::string name, EntityId target = kNoEntity, Value payload = {}, EntityId other = kNoEntity);
    void emitJson(std::string name, EntityId target, const Json& payload, EntityId other = kNoEntity);

    std::vector<RuntimeMessage> drainMessages();

    // --- Access for native code and tools ----------------------------------------------
    /// Adds a log line (shows in the editor console and `logs`).
    void log(EntityId entity, std::string text, std::string script = "native");
    /// Entity var by name, as scripts see it during play (none if unset).
    Value getVar(EntityId entity, std::string_view name);
    void setVar(EntityId entity, std::string_view name, Value value);
    /// Destroys an entity: at the end of the tick while scripts run, otherwise now.
    void destroyEntity(EntityId entity);
    bool ticking() const { return ticking_; }

    double time() const { return time_; }
    uint64_t frame() const { return frame_; }
    Random& rng() { return rng_; }
    Scene& scene() { return scene_; }
    const BuiltinRegistry& registry() const { return *registry_; }

    /// Instantiates "prefab:path" for spawn(); set by the engine (asset system).
    std::function<Result<EntityId>(const std::string& ref, Vec3 position, const std::string& name)> spawnPrefab;

    // --- Services for builtins (CallContext::service<T>()) ---------------------------
    template <typename T>
    void provide(T* service) {
        services_[std::type_index(typeid(T))] = service;
    }
    void* serviceById(std::type_index t) const {
        auto it = services_.find(t);
        return it == services_.end() ? nullptr : it->second;
    }

    // --- Modules ---------------------------------------------------------------------
    /// Enables `use "path"` (files under the project folder).
    void setProjectDir(std::string dir);
    const std::string& projectDir() const { return projectDir_; }
    /// Re-reads module files that changed on disk; scripts using them recompile. Returns
    /// true if anything changed.
    bool refreshModules();
    /// Options the runtime compiles with (components, registry, module loader) — use them
    /// for wander_check so checks match what runs.
    CompileOptions compileOptions() const;

    // --- Native code (AOT) ------------------------------------------------------------
    /// Attaches native code for programs with this hash (handlers then run natively).
    void attachNative(uint64_t programHash, std::shared_ptr<const NativeProgram> native);
    void detachNative(uint64_t programHash);
    void clearNative();
    /// Programs currently running natively.
    size_t nativeProgramCount() const { return native_.size(); }

    // --- Introspection ----------------------------------------------------------------
    /// Runtime state of an entity's behaviors (states, waits, timers, vars) for tools.
    Json inspect(EntityId e) const;
    /// Current state name of the first behavior with states ("" if none).
    std::string currentState(EntityId e) const;

    // --- Test driving (wander_test) -------------------------------------------------------
    /// A test proto running as a coroutine with `self` = the entity under test.
    struct TestDriver {
        std::shared_ptr<const Program> program;
        int proto = -1;
        EntityId self = kNoEntity;
        TestHooks* hooks = nullptr;
        std::vector<Value> regs;
        size_t pc = 0;
        bool finished = false;
        bool byFrames = false;
        double remaining = 0;
    };
    /// Starts a test: runs it until its first `wait` (or the end).
    std::unique_ptr<TestDriver> startTest(std::shared_ptr<const Program> program, int proto, EntityId self, TestHooks* hooks);
    /// Call once per tick: resumes the test when its wait has elapsed. Returns true when
    /// the test has finished.
    bool stepTest(TestDriver& driver, float dt);
    bool testFinished(const TestDriver& driver) const;

    /// Execution budget per handler run, in instructions (loops charge their length per
    /// iteration, calls the callee's length).
    static constexpr int64_t kBudget = 1'000'000;
    /// Maximum fn call depth (recursion).
    static constexpr int kMaxDepth = 200;
    /// Maximum coroutines (waiting handler runs) per instance.
    static constexpr size_t kMaxCoroutines = 64;

    struct Impl;

private:
    friend class CallContext;
    friend struct ExecState;
    friend struct Vm;
    friend struct NativeBridge;

    Scene& scene_;
    const BuiltinRegistry* registry_;
    std::unique_ptr<Impl> impl_;
    Random rng_;
    double time_ = 0;
    uint64_t frame_ = 0;
    bool ticking_ = false;
    std::unordered_map<std::type_index, void*> services_;
    std::string projectDir_;
    std::unordered_map<uint64_t, std::shared_ptr<const NativeProgram>> native_;
};

}  // namespace sky::wander
