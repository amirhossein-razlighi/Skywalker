#pragma once
// Process modes resolved through the hierarchy, and the per-tick gate every simulated system
// asks "does this entity run this tick, and how fast?" (the `process` component,
// ecs/ProcessComponent.h; docs/ARCHITECTURE.md "Game pause, process modes and time scale").
//
//   mode         runs while the game plays   runs while the game is paused
//   pausable     yes                         no      (default)
//   when_paused  no                          yes     (pause menu logic)
//   always       yes                         yes     (UI canvases by default)
//   disabled     no                          no      (frozen, still drawn)
//
// The gate is rebuilt at the start of every tick (Runtime::prepareTick) and resolves entities
// lazily, so systems pay only for the entities they ask about. A scene without `process`
// components or UI canvases takes a fast path (no lookups at all).

#include <optional>
#include <string_view>
#include <unordered_map>

#include "skywalker/scene/Scene.h"

namespace sky {

enum class ProcessMode : uint8_t { Inherit, Pausable, WhenPaused, Always, Disabled };
const char* toString(ProcessMode m);
std::optional<ProcessMode> processModeFromName(std::string_view name);

/// Effective (inherited) process settings of one entity.
struct ResolvedProcess {
    ProcessMode mode = ProcessMode::Pausable;  // never Inherit
    bool realClock = false;                    // ignores the game's time scale
    bool interpolate = true;                   // smoothed between ticks on screen
    int priority = 0;                          // the entity's own (not inherited)
    float speed = 1.f;                         // product of process.timeScale along the hierarchy
    EntityId modeFrom = kNoEntity;             // entity whose `process` (or UI canvas) decided the mode; 0 = default
};

/// Walks up the hierarchy (no caching): for tools and tests. Systems use ProcessGate.
ResolvedProcess resolveProcess(const Scene& scene, EntityId e);
/// Whether an entity with this effective mode runs while the game is (not) paused.
bool processRuns(ProcessMode mode, bool gamePaused);

class ProcessGate {
public:
    /// Starts a tick: the pause state and time scale for it. Forgets resolved entities.
    void update(const Scene& scene, bool gamePaused, float timeScale);
    void reset();

    /// Does `e` run this tick?
    bool runs(EntityId e) const;
    /// Factor applied to the fixed dt for `e`: the time scale (1 on the real clock), 0 when it does not run.
    float scale(EntityId e) const;
    /// Is `e` smoothed between ticks on screen (`interpolation`)?
    bool interpolates(EntityId e) const;
    int priority(EntityId e) const;
    const ResolvedProcess& resolved(EntityId e) const;

    bool paused() const { return paused_; }
    float timeScale() const { return timeScale_; }
    /// Any entity has a non-zero priority (Wander then sorts its run order).
    bool ordered() const { return ordered_; }
    /// No `process` components and no UI canvases: every entity has the default settings.
    bool uniform() const { return uniform_; }

private:
    const Scene* scene_ = nullptr;
    bool paused_ = false;
    float timeScale_ = 1.f;
    bool uniform_ = true;
    bool ordered_ = false;
    mutable std::unordered_map<EntityId, ResolvedProcess> cache_;
    ResolvedProcess default_;
};

}  // namespace sky
