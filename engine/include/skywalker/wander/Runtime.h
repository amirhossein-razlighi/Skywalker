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
#include "skywalker/input/InputState.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Ast.h"
#include "skywalker/wander/PhysicsHooks.h"

namespace sky::wander {

/// Raw devices plus the evaluated input actions (see skywalker/input/InputState.h).
using InputState = input::InputState;

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

    /// Particle bursts for burst(); set by the engine (effects system).
    std::function<void(EntityId emitter, int count)> burst;
    /// Water surface height for water_height(); set by the engine (ocean simulation).
    std::function<float(float x, float z)> waterHeight;
    // Studio builtins/hooks: observes every emitted event (scripts and external emit())
    // so playtest bots can record deaths, objectives and damage. Optional.
    std::function<void(const std::string& name, EntityId target, EntityId source)> onEmit;

    // audio builtins: set by the engine (audio system). Each returns an error message, "" on success.
    std::function<std::string(EntityId entity)> playAudio;                                    // play(e)
    std::function<void(EntityId entity)> stopAudio;                                           // stop_sound(e)
    std::function<std::string(const std::string& clip, float volume, EntityId at)> playSound;  // play_sound(path, volume?) at an entity
    std::function<std::string(const std::string& clip, float fadeSeconds)> playMusic;         // music(path, fade?)
    std::function<void(const std::string& bus, float volume)> setBusVolume;                   // set_volume(bus, v)

    // --- physics builtins ---------------------------------------------------------------
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
    void queueContact(const Contact& contact) { nextContacts_.push_back(contact); }

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
    bool matchesContactFilter(EntityId other, const std::string& filter) const;  // physics builtins
    std::vector<Contact> contacts_;      // physics builtins: delivered this tick
    std::vector<Contact> nextContacts_;  // physics builtins: queued for the next tick
    std::map<std::pair<EntityId, size_t>, Instance> instances_;
    std::vector<RuntimeMessage> messages_;
    std::vector<EntityId> toDestroy_;
    int spawnedThisTick_ = 0;
};

}  // namespace sky::wander
