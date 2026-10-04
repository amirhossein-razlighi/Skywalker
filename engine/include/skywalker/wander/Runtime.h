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
#include <optional>
#include <set>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Random.h"
#include "skywalker/scene/Process.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Builtins.h"
#include "skywalker/wander/Bytecode.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/PhysicsHooks.h"

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
class Debugger;

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

    /// Game time in seconds: advances by dt * time_scale per tick and stops while the game is paused.
    double time() const { return time_; }
    /// Ticks run since play started (also while paused).
    uint64_t frame() const { return frame_; }
    /// Real (unscaled) seconds since play started: ticks * the fixed dt, paused or not.
    double unscaledTime() const { return realTime_; }
    Random& rng() { return rng_; }
    Scene& scene() { return scene_; }
    const BuiltinRegistry& registry() const { return *registry_; }

    /// Instantiates "prefab:path" for spawn(); set by the engine (asset system).
    std::function<Result<EntityId>(const std::string& ref, Vec3 position, const std::string& name)> spawnPrefab;

    // --- Engine hooks used by builtins and tools ------------------------------------------
    /// Studio: observes every emitted event (scripts and external emit()) so playtest bots
    /// can record deaths, objectives and damage. Optional.
    std::function<void(const std::string& name, EntityId target, EntityId source)> onEmit;
    /// Audio (set by the engine's audio system). Each returns an error message, "" on success.
    std::function<std::string(EntityId entity)> playAudio;                                    // play(e)
    std::function<void(EntityId entity)> stopAudio;                                           // stop_sound(e)
    std::function<std::string(const std::string& clip, float volume, EntityId at)> playSound;  // play_sound(path, volume?)
    std::function<std::string(const std::string& clip, float fadeSeconds)> playMusic;         // music(path, fade?)
    std::function<void(const std::string& bus, float volume)> setBusVolume;                   // set_volume(bus, v)
    /// Physics, character and navigation services behind push(), raycast(), walk(),
    /// navigate()...; installed by the engine (null = those builtins report an error).
    PhysicsHooks* physics = nullptr;
    /// A contact from the physics step. Delivered on the next tick to `self`'s
    /// `on collide` / `on trigger_enter` / `on trigger_exit` handlers whose filter matches
    /// `other` (name or tag); `other`, `contact_point`, `contact_normal` and `impact` describe it.
    struct Contact {
        Trigger trigger = Trigger::Collide;
        EntityId self = kNoEntity;
        EntityId other = kNoEntity;
        Vec3 point;
        Vec3 normal;      // pointing away from `other`, toward `self`
        float speed = 0;  // approach speed along the normal (m/s)
    };
    void queueContact(const Contact& contact);
    // animation builtins: set_param, trigger, play_animation, anim_state, play_sequence.
    // Set by the engine (AnimationSystem); args exclude the entity. Returns the value.
    std::function<Result<Json>(const std::string& fn, EntityId entity, const std::vector<Json>& args)> animation;

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

    // --- Game pause and time scale (scene/Process.h; docs/WANDER.md "Pause and slow motion") ---
    /// Requests from scripts (pause_game, time_scale) and tools. They apply at the start of the next
    /// tick, so a whole tick always runs under one pause state; `pause` / `resume` events go out then.
    void requestPause(bool paused) { requestedPause_ = paused; }
    void requestTimeScale(double scale);
    /// The state of the current tick.
    bool gamePaused() const { return paused_; }
    double timeScale() const { return timeScale_; }
    /// The latest request (what the next tick will use).
    bool pauseRequested() const { return requestedPause_.value_or(paused_); }
    double timeScaleRequested() const { return requestedScale_.value_or(timeScale_); }
    static constexpr double kMaxTimeScale = 10.0;
    /// Applies pending requests and refreshes the process gate. The engine calls it at the start of
    /// every tick, before any system runs; tick() calls it itself when the embedding did not.
    void prepareTick();
    /// Which entities run this tick and how fast (valid after prepareTick).
    const ProcessGate& processGate() const { return gate_; }

    // --- Cosmetic `on frame` handlers (display rate; docs/WANDER.md "on frame") ---------------
    struct FrameInfo {
        float dt = 0;      // real seconds since the last displayed frame
        float alpha = 1;   // where the frame sits between the last two ticks (render interpolation)
        double time = 0;   // the displayed game time (interpolated)
        const InputState* input = nullptr;  // devices and actions as of the last tick (null: none)
    };
    /// Runs every `on frame` handler of running instances. Their writes (transform, mesh, light, camera,
    /// sprite, text, ui fields) are recorded and undone by revertFrame(), so they never reach the
    /// simulation. Returns the number of handler runs.
    size_t runFrameHandlers(const FrameInfo& info);
    /// Restores every field written by the last runFrameHandlers().
    void revertFrame();
    /// Any compiled script has an `on frame` handler.
    bool hasFrameHandlers() const;
    // --- Calling functions directly (tools defined in Wander, docs/CUSTOM_TOOLS.md) -------------
    struct FunctionCall {
        int64_t budget = 1'000'000;  // instructions (same accounting as handler runs)
        EntityId self = kNoEntity;   // `self` inside the function
        std::string scriptName = "tool";  // shown with log lines
    };
    struct FunctionResult {
        bool ok = false;
        Value value;          // the return value
        std::string error;    // runtime error message
        SourceLoc loc;        // where it failed
        std::string file;     // module file when the error is inside a `use`d module
        int64_t instructions = 0;  // budget used
    };
    /// Runs a file-level `fn` of a compiled program outside the tick loop, against this runtime's
    /// scene. Entity var writes and deferred destroys are applied to the scene before it returns.
    /// Errors (unknown function, wrong argument count, runtime errors, budget) are reported in the
    /// result, never thrown.
    FunctionResult callFunction(const std::shared_ptr<const Program>& program, std::string_view name,
                                std::vector<Value> args, const FunctionCall& options);
    /// While not empty, assigning entity properties, vars or component fields raises this message
    /// (read-only tools). Builtins that edit the scene are not covered: callers check those separately.
    void setSceneWriteGuard(std::string message) { sceneWriteGuard_ = std::move(message); }
    const std::string& sceneWriteGuard() const { return sceneWriteGuard_; }
    /// When on, those assignments first notify the scene's ChangeObserver (the undo History), so a
    /// tool's direct edits are recorded without snapshotting every entity up front.
    void setRecordSceneWrites(bool on) { recordSceneWrites_ = on; }
    bool recordSceneWrites() const { return recordSceneWrites_; }

    // --- Save games (RuntimeState.cpp; docs/SAVE_GAMES.md) ------------------------------------
    /// The exact play state between two ticks: clocks, the random generator, queued events and, for every
    /// instance whose entity `include` accepts, its state machines, timers and waiting handlers. Values are
    /// type-tagged, so a list of three numbers stays a list and replays exactly.
    Json saveState(const std::function<bool(EntityId)>& include) const;
    /// Restores saveState() output into the running game (call between ticks, after the scene is restored).
    /// Instances of the saved entities are replaced; an instance whose script changed since the save starts
    /// fresh (reported in `warnings`), so a game update never resumes old bytecode.
    Status loadState(const Json& state, std::vector<std::string>& warnings);
    // --- Runtime scene changes (RuntimeScenes.cpp; docs/SCENE_FLOW.md) ------------------------
    /// Drops the instances, vars, queued events and contacts of entities that no longer exist (a scene
    /// change or an additive unload removed them between ticks). Entities that still exist keep running.
    void forgetMissingEntities();
    // --- Debugging (Debugger.h; docs/WANDER.md "Debugging") -------------------------------------
    /// Breakpoints, stepping and inspection of this runtime's scripts. Costs nothing while idle.
    Debugger& debugger() { return *debugger_; }
    const Debugger& debugger() const { return *debugger_; }

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
    friend class Debugger;
    friend struct ExecState;
    friend struct Vm;
    friend struct NativeBridge;

    Scene& scene_;
    const BuiltinRegistry* registry_;
    std::unique_ptr<Impl> impl_;
    Random rng_;
    double time_ = 0;
    uint64_t frame_ = 0;
    double realTime_ = 0;
    // game clock (prepareTick)
    bool paused_ = false;
    double timeScale_ = 1.0;
    std::optional<bool> requestedPause_;
    std::optional<double> requestedScale_;
    uint64_t preparedFrame_ = ~0ull;
    ProcessGate gate_;
    bool ticking_ = false;
    std::unordered_map<std::type_index, void*> services_;
    std::string projectDir_;
    std::unordered_map<uint64_t, std::shared_ptr<const NativeProgram>> native_;
    std::string sceneWriteGuard_;
    bool recordSceneWrites_ = false;
    std::unique_ptr<Debugger> debugger_;
};

}  // namespace sky::wander
