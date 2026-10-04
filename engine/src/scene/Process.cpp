// The `process` component and the per-tick process gate (skywalker/scene/Process.h).

#include "skywalker/scene/Process.h"

#include <vector>

namespace sky {

const TypeInfo& Process::type() {
    static const TypeInfo info{
        "process",
        "How this entity and its children run while the game is paused (pause_game) or slowed down (time_scale), the "
        "order their behaviors run in, and whether their motion is smoothed between 60 Hz ticks on fast displays. "
        "A pause menu canvas runs `always` (UI canvases do by default); enemies stay `pausable`.",
        {
            SKY_FIELD_ENUM(Process, mode,
                           "inherit (from the parent; root default pausable) | pausable (stops while the game is paused) | "
                           "when_paused (runs only while paused: pause menu logic) | always (never stops: UI, music "
                           "controllers) | disabled (never runs, still drawn)",
                           "inherit", "pausable", "when_paused", "always", "disabled"),
            SKY_FIELD_RANGE(Process, priority, Int,
                            "Behavior run order: lower runs first (ties keep scene order). Not inherited", -1000000.f, 1000000.f),
            SKY_FIELD_ENUM(Process, clock,
                           "inherit | game (dt follows time_scale: slow motion) | real (ignores time_scale: menus, HUD)",
                           "inherit", "game", "real"),
            SKY_FIELD_ENUM(Process, interpolation,
                           "inherit | on (motion is smoothed between ticks on screen) | off (shown exactly at tick "
                           "positions: snapping things, pixel-art)",
                           "inherit", "on", "off"),
        }};
    return info;
}

const char* toString(ProcessMode m) {
    switch (m) {
        case ProcessMode::Inherit: return "inherit";
        case ProcessMode::Pausable: return "pausable";
        case ProcessMode::WhenPaused: return "when_paused";
        case ProcessMode::Always: return "always";
        case ProcessMode::Disabled: return "disabled";
    }
    return "inherit";
}

std::optional<ProcessMode> processModeFromName(std::string_view name) {
    if (name == "inherit" || name.empty()) return ProcessMode::Inherit;
    if (name == "pausable") return ProcessMode::Pausable;
    if (name == "when_paused") return ProcessMode::WhenPaused;
    if (name == "always") return ProcessMode::Always;
    if (name == "disabled") return ProcessMode::Disabled;
    return std::nullopt;
}

bool processRuns(ProcessMode mode, bool gamePaused) {
    switch (mode) {
        case ProcessMode::Inherit:
        case ProcessMode::Pausable: return !gamePaused;
        case ProcessMode::WhenPaused: return gamePaused;
        case ProcessMode::Always: return true;
        case ProcessMode::Disabled: return false;
    }
    return !gamePaused;
}

namespace {

/// What one entity says about each inherited setting (nullopt = inherit).
struct Local {
    std::optional<ProcessMode> mode;
    std::optional<bool> realClock;
    std::optional<bool> interpolate;
    int priority = 0;
};

Local localSettings(const Scene& scene, EntityId e) {
    Local l;
    if (const Process* p = scene.get<Process>(e)) {
        if (auto m = processModeFromName(p->mode); m && *m != ProcessMode::Inherit) l.mode = *m;
        if (p->clock == "game") l.realClock = false;
        else if (p->clock == "real") l.realClock = true;
        if (p->interpolation == "on") l.interpolate = true;
        else if (p->interpolation == "off") l.interpolate = false;
        l.priority = p->priority;
    }
    // A UI canvas runs always on the real clock unless its own process component says otherwise.
    if (scene.get<UICanvas>(e)) {
        if (!l.mode) l.mode = ProcessMode::Always;
        if (!l.realClock) l.realClock = true;
    }
    return l;
}

/// Fills the settings of `e` that are still open from `l` (nearest setting wins).
void inherit(ResolvedProcess& out, bool& haveMode, bool& haveClock, bool& haveInterp, const Local& l, EntityId from) {
    if (!haveMode && l.mode) {
        out.mode = *l.mode;
        out.modeFrom = from;
        haveMode = true;
    }
    if (!haveClock && l.realClock) {
        out.realClock = *l.realClock;
        haveClock = true;
    }
    if (!haveInterp && l.interpolate) {
        out.interpolate = *l.interpolate;
        haveInterp = true;
    }
}

}  // namespace

ResolvedProcess resolveProcess(const Scene& scene, EntityId e) {
    ResolvedProcess out;
    bool haveMode = false, haveClock = false, haveInterp = false;
    int guard = 0;
    for (EntityId cur = e; cur && scene.exists(cur) && guard < 4096; ++guard) {
        Local l = localSettings(scene, cur);
        if (cur == e) out.priority = l.priority;
        inherit(out, haveMode, haveClock, haveInterp, l, cur);
        if (haveMode && haveClock && haveInterp) break;
        cur = scene.record(cur)->parent;
    }
    return out;
}

void ProcessGate::update(const Scene& scene, bool gamePaused, float timeScale) {
    scene_ = &scene;
    paused_ = gamePaused;
    timeScale_ = timeScale;
    cache_.clear();
    // Fast path: without process components or canvases every entity has the default settings.
    auto& reg = const_cast<Scene&>(scene).registry();
    uniform_ = reg.count<Process>() == 0 && reg.count<UICanvas>() == 0;
    ordered_ = false;
    if (!uniform_ && reg.count<Process>() > 0) {
        for (EntityId e : scene.entities()) {
            const Process* p = scene.get<Process>(e);
            if (p && p->priority != 0) {
                ordered_ = true;
                break;
            }
        }
    }
}

void ProcessGate::reset() {
    scene_ = nullptr;
    paused_ = false;
    timeScale_ = 1.f;
    uniform_ = true;
    ordered_ = false;
    cache_.clear();
}

const ResolvedProcess& ProcessGate::resolved(EntityId e) const {
    if (uniform_ || !scene_) return default_;
    auto it = cache_.find(e);
    if (it != cache_.end()) return it->second;
    // Walk up until an ancestor already resolved (memoized), then fill the chain top-down.
    std::vector<EntityId> chain;
    EntityId cur = e;
    const ResolvedProcess* base = nullptr;
    while (cur && scene_->exists(cur) && chain.size() < 4096) {
        auto hit = cache_.find(cur);
        if (hit != cache_.end()) {
            base = &hit->second;
            break;
        }
        chain.push_back(cur);
        cur = scene_->record(cur)->parent;
    }
    ResolvedProcess parent = base ? *base : ResolvedProcess{};  // root default: pausable, game clock, interpolated
    for (auto it2 = chain.rbegin(); it2 != chain.rend(); ++it2) {
        Local l = localSettings(*scene_, *it2);
        ResolvedProcess r = parent;  // inherited (modeFrom too)
        r.priority = l.priority;     // not inherited
        if (l.mode) {
            r.mode = *l.mode;
            r.modeFrom = *it2;
        }
        if (l.realClock) r.realClock = *l.realClock;
        if (l.interpolate) r.interpolate = *l.interpolate;
        parent = cache_[*it2] = r;
    }
    auto found = cache_.find(e);
    return found != cache_.end() ? found->second : default_;
}

bool ProcessGate::runs(EntityId e) const {
    if (uniform_) return !paused_;
    return processRuns(resolved(e).mode, paused_);
}

float ProcessGate::scale(EntityId e) const {
    if (uniform_) return paused_ ? 0.f : timeScale_;
    const ResolvedProcess& r = resolved(e);
    if (!processRuns(r.mode, paused_)) return 0.f;
    return r.realClock ? 1.f : timeScale_;
}

bool ProcessGate::interpolates(EntityId e) const { return uniform_ ? true : resolved(e).interpolate; }

int ProcessGate::priority(EntityId e) const { return uniform_ ? 0 : resolved(e).priority; }

}  // namespace sky
