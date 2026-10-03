#pragma once
// Deterministic Wander interpreter.
//
// The runtime walks the compiled AST. It is single-threaded and fully deterministic:
// entities are processed in Scene order, events are delivered on the next tick in
// emission order, and randomness comes from a seeded PCG generator. Each handler
// invocation has an execution budget so a buggy (or adversarial) script can never
// hang the engine; scripts that keep failing are disabled and reported.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Random.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Ast.h"

namespace sky::wander {

struct InputState {
    std::set<std::string> held;     // keys currently held ("w", "space", "left", ...)
    std::set<std::string> pressed;  // keys pressed since last tick
    std::vector<EntityId> clicked;  // entities clicked since last tick
};

struct RuntimeMessage {
    enum class Kind { Log, Error, Compile } kind = Kind::Log;
    EntityId entity = kNoEntity;
    std::string script;
    int line = 0;
    std::string text;

    Json toJson() const;
};

class Runtime {
public:
    explicit Runtime(Scene& scene);

    /// Forget all per-instance state (call when play starts/stops). Events queued from
    /// outside (e.g. agent input injected before pressing play) can be kept.
    void reset(bool keepQueuedEvents = false);

    /// Compiles any script whose source changed. Compile errors become messages.
    void compileScripts();

    /// Advances one fixed step.
    void tick(float dt, const InputState& input);

    /// Queue an event for delivery on the next tick (from the editor or an agent).
    void emit(std::string name, EntityId target = kNoEntity);

    std::vector<RuntimeMessage> drainMessages();

    double time() const { return time_; }
    uint64_t frame() const { return frame_; }
    Random& rng() { return rng_; }

    /// Instantiates "prefab:path" for spawn(); set by the engine (asset system).
    std::function<Result<EntityId>(const std::string& ref, Vec3 position, const std::string& name)> spawnPrefab;

    /// Maximum AST nodes evaluated per handler invocation.
    static constexpr int kBudget = 200000;

private:
    friend class Exec;

    struct Event {
        std::string name;
        EntityId target = kNoEntity;  // kNoEntity = broadcast
    };
    struct Instance {
        // State resets when the script's program changes. Holding the shared_ptr keeps the old
        // program alive, so a newly compiled program can never reuse its address (ABA).
        std::shared_ptr<const Program> program;
        bool started = false;
        std::unordered_map<int, double> timers;
        std::set<int> fired;
    };

    Scene& scene_;
    Random rng_;
    double time_ = 0;
    uint64_t frame_ = 0;
    std::vector<Event> pending_;
    std::vector<Event> nextPending_;
    std::map<std::pair<EntityId, size_t>, Instance> instances_;
    std::vector<RuntimeMessage> messages_;
    std::vector<EntityId> toDestroy_;
    int spawnedThisTick_ = 0;
};

}  // namespace sky::wander
