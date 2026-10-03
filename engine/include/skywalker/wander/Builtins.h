#pragma once
// The Wander builtin registry: one place where every function scripts can call is
// declared — name, arity, parameter/return types, documentation, category and the
// implementation. The compiler checks calls against it (arity, types, did-you-mean),
// `wander_reference` and docs are generated from it, and the VM and AOT-compiled code
// dispatch through it.
//
// Adding builtins from a subsystem (physics, audio, animation, UI, input, ...):
//
//   // engine/src/physics/PhysicsBuiltins.cpp
//   void registerPhysicsBuiltins(wander::BuiltinRegistry& reg) {
//       // Designated initializers must follow the field order of BuiltinDef.
//       reg.add({.name = "raycast",
//                .params = {{"from", wander::kTPoint}, {"direction", wander::kTVec}, {"max", wander::kTNumber, true}},
//                .returns = wander::kTMap | wander::kTNone,
//                .category = "physics",
//                .doc = "First hit along a ray: {entity, point, normal, distance} or none.",
//                .example = "let hit = raycast(self, forward(self), 20)",
//                .fn = [](wander::CallContext& c) -> wander::Value {
//                    Engine* engine = c.service<Engine>();   // provided by the Engine
//                    ...
//                }});
//   }
//
// then add one line `registerPhysicsBuiltins(reg);` to `sky::registerEngineBuiltins`
// (engine/src/engine/EngineBuiltins.cpp). Engine state is reached through services the
// embedding provides (`c.service<Engine>()`), never through globals, so several engines
// (tests, sandboxes) can coexist. Builtins must be deterministic: use `c.rng()` for
// randomness and never read wall-clock time.
//
// Subsystems can also register trigger words (`on contact`): an event named like the
// trigger, delivered with Runtime::emit(name, target, payload, other).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include "skywalker/core/Random.h"
#include "skywalker/input/InputState.h"
#include "skywalker/wander/Ast.h"
#include "skywalker/wander/Value.h"

namespace sky {
class Scene;
}

namespace sky::wander {

class Runtime;
struct ExecState;

/// Raw devices plus the evaluated input actions (see skywalker/input/InputState.h).
using InputState = input::InputState;

struct BuiltinDef;

/// What a builtin implementation sees: its arguments and the calling script's context.
class CallContext {
public:
    CallContext(ExecState& state, const BuiltinDef& def, Value* args, int argc, SourceLoc loc)
        : state_(state), def_(def), args_(args), argc_(argc), loc_(loc) {}

    // --- arguments -----------------------------------------------------------------
    int argc() const { return argc_; }
    const Value* args() const { return args_; }
    const Value& arg(int i) const { return args_[i]; }
    Value& mutArg(int i) { return args_[i]; }  // methods: args 0 is the receiver
    double number(int i) const;
    bool boolean(int i) const;  // truthiness
    const std::string& string(int i) const;
    Vec3 vec(int i) const;      // a vector
    Vec3 point(int i) const;    // a vector, or an entity's world position
    Vec4 color(int i) const;
    EntityRef entity(int i) const;  // must be an existing entity
    const std::vector<Value>& list(int i) const;
    const MapObj& map(int i) const;

    // --- context -------------------------------------------------------------------
    Runtime& runtime() const;
    Scene& scene() const;
    EntityRef self() const;
    EntityRef other() const;
    double time() const;
    float dt() const;
    uint64_t frame() const;
    Random& rng() const;
    const InputState& input() const;
    SourceLoc loc() const { return loc_; }
    const BuiltinDef& def() const { return def_; }
    /// Services provided by the embedding (e.g. the Engine); null when unavailable
    /// (bare runtimes in tests, sandboxes). Builtins should degrade gracefully.
    template <typename T>
    T* service() const;

    /// Text form of a value with entity names resolved ("Player", not "#12").
    std::string display(const Value& v) const;
    bool truthy(const Value& v) const;
    /// Adds work to the handler's execution budget (e.g. per element for sort/scan).
    void charge(int64_t units) const;
    /// Aborts the calling handler with a runtime error at the call site.
    [[noreturn]] void fail(std::string message) const;

private:
    friend ExecState& CallContextAccess(CallContext& c);
    void* serviceById(std::type_index t) const;

    ExecState& state_;
    const BuiltinDef& def_;
    Value* args_;
    int argc_;
    SourceLoc loc_;
};

template <typename T>
T* CallContext::service() const {
    return static_cast<T*>(serviceById(std::type_index(typeid(T))));
}

using BuiltinImpl = Value (*)(CallContext& ctx);

struct BuiltinParam {
    std::string name;
    TypeSet type = kTAny;
    bool optional = false;
};

struct BuiltinDef {
    std::string name;
    /// Methods (`list.push(x)`): the receiver types. 0 = a free function.
    TypeSet receiver = 0;
    std::vector<BuiltinParam> params;  // arity: required params .. all params (excluding the receiver)
    bool variadic = false;             // accepts any number of trailing args of the last param's type
    TypeSet returns = kTAny;
    std::string category;  // math, vector, entity, scene, random, input, string, list, map, effects, ...
    std::string doc;
    std::string example;
    BuiltinImpl fn = nullptr;
    void* user = nullptr;  // context for implementations registered at runtime (native modules)
    bool pure = false;     // no side effects and deterministic in its arguments (constant-foldable)
    bool mutates = false;  // method changes its receiver (written back to the variable/property)
    bool hidden = false;   // statement helpers (not listed in references)
    std::string owner;     // who registered it ("core", "engine", "native:<module>")
    std::shared_ptr<void> keepAlive;  // owns what `user` points to (lives as long as the definition)

    int minArgs() const;
    int maxArgs() const;  // -1 = variadic
    /// "distance(a: vector|entity, b: vector|entity) -> number"
    std::string signature() const;
    Json toJson() const;
};

struct TriggerDef {
    std::string name;     // `on <name>`
    std::string doc;
    std::string payload;  // what `data` holds, e.g. "{point: vec, normal: vec, impulse: number}"
    std::string category;
};

class BuiltinRegistry {
public:
    /// A registry can chain to a parent (e.g. a runtime's native-module builtins on top of
    /// the global registry); lookups fall back to the parent.
    explicit BuiltinRegistry(const BuiltinRegistry* parent = nullptr) : parent_(parent) {}
    BuiltinRegistry(const BuiltinRegistry&) = delete;
    BuiltinRegistry& operator=(const BuiltinRegistry&) = delete;

    /// Core builtins plus everything the engine registered (registerEngineBuiltins).
    static BuiltinRegistry& global();

    /// Adds or replaces (same name and receiver) a builtin.
    void add(BuiltinDef def);
    bool remove(std::string_view name, TypeSet receiver = 0);
    /// Removes every builtin registered by `owner` (e.g. when a native module unloads).
    void removeOwner(std::string_view owner);
    void addTrigger(TriggerDef t);

    std::shared_ptr<const BuiltinDef> find(std::string_view name) const;
    std::shared_ptr<const BuiltinDef> findMethod(std::string_view name, VType receiver) const;
    /// Any method with this name, whatever the receiver (for diagnostics).
    bool hasMethodNamed(std::string_view name) const;
    const TriggerDef* trigger(std::string_view name) const;

    /// Visible builtins (free functions and methods), sorted by category then name.
    std::vector<std::shared_ptr<const BuiltinDef>> all(bool includeHidden = false) const;
    std::vector<TriggerDef> triggers() const;
    std::vector<std::string> functionNames() const;
    std::vector<std::string> methodNames(VType receiver) const;

    /// Changes whenever this registry or a parent changes (programs compiled against an
    /// older generation are recompiled).
    uint64_t generation() const;

private:
    struct Key {
        std::string name;
        TypeSet receiver;
        bool operator<(const Key& o) const { return name != o.name ? name < o.name : receiver < o.receiver; }
    };
    mutable std::recursive_mutex mutex_;
    const BuiltinRegistry* parent_ = nullptr;
    std::map<Key, std::shared_ptr<const BuiltinDef>> defs_;
    std::map<std::string, TriggerDef, std::less<>> triggers_;
    uint64_t generation_ = 1;
};

/// Registers the core library (math, vectors, entities, lists, maps, strings, input, ...).
/// Called once by BuiltinRegistry::global().
void registerCoreBuiltins(BuiltinRegistry& reg);
/// Audio, input actions, physics, characters and navigation (through Runtime hooks).
void registerSystemBuiltins(BuiltinRegistry& reg);

/// Interned names (vars, properties, events). Ids are process-wide and stable.
uint32_t intern(std::string_view name);
const std::string& symbolName(uint32_t id);

}  // namespace sky::wander
