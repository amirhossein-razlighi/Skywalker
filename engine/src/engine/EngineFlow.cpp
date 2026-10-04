// The engine's frame flow: game pause and time scale (process modes), real-time accumulation,
// render interpolation and cosmetic `on frame` handlers, frame pacing stats, and the Wander
// builtins for them (pause_game, resume_game, is_paused, time_scale, teleport, ...).
// See docs/ARCHITECTURE.md "Game pause, process modes and time scale" and "Render interpolation".

#include <algorithm>
#include <cmath>
#include <optional>

#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

int Engine::advance(double seconds) {
    if (playState_ != PlayState::Playing || seconds <= 0.0) return 0;
    realSinceFrame_ += seconds;
    accumulator_ += std::min(seconds, 0.25);  // avoid spiral of death after stalls
    int ticks = 0;
    while (accumulator_ >= kFixedDt) {
        accumulator_ -= kFixedDt;
        ++ticks;
    }
    if (ticks) step(ticks);
    ticksSinceFrame_ += ticks;
    return ticks;
}

void Engine::setGamePaused(bool paused) { runtime_->requestPause(paused); }
bool Engine::gamePaused() const { return runtime_->pauseRequested(); }
void Engine::setTimeScale(double scale) { runtime_->requestTimeScale(scale); }
double Engine::timeScale() const { return runtime_->timeScaleRequested(); }

float Engine::interpolationAlpha() const {
    if (playState_ != PlayState::Playing) return 1.f;
    return static_cast<float>(std::clamp(accumulator_ / static_cast<double>(kFixedDt), 0.0, 1.0));
}

void Engine::stepPhysics() {
    const ProcessGate& gate = runtime_->processGate();
    // The physics world follows the process mode of the entity holding `physics_world` (if any):
    // `always` keeps bodies moving under a pause menu; by default physics holds while paused.
    float scale = gate.paused() ? 0.f : gate.timeScale();
    if (!gate.uniform() && scene_->registry().count<PhysicsSettings>() > 0) {
        for (EntityId e : scene_->entities()) {
            if (scene_->get<PhysicsSettings>(e)) {
                scale = gate.scale(e);
                break;
            }
        }
    }
    if (scale <= 0.f) return;  // paused or stopped time: bodies hold
    // Fast forward splits the step so bodies never take steps longer than a tick.
    const int substeps = scale > 1.f ? static_cast<int>(std::ceil(scale)) : 1;
    const float h = kFixedDt * scale / static_cast<float>(substeps);
    for (int i = 0; i < substeps; ++i) physics_->step(h, *runtime_);
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

void Engine::resetFrameFlow() {
    transformHistory_.clear();
    displayHistory_.clear();
    pacing_.reset();
    realSinceFrame_ = 0;
    ticksSinceFrame_ = 0;
    flowStats_ = {};
    animation_->setDisplayAlpha(1.f);
}

FrameData Engine::frame(const CaptureOptions& opts) {
    const bool live = playState_ == PlayState::Playing;
    const float alpha = live && interpolate_ ? std::clamp(opts.interpolationAlpha, 0.f, 1.f) : 1.f;
    const ProcessGate& gate = runtime_->processGate();
    const float gameScale = gate.paused() ? 0.f : gate.timeScale();
    const double displayTime = runtime_->time() - (1.0 - alpha) * static_cast<double>(kFixedDt) * gameScale;

    // Everything below is undone when the frame is built, in reverse order: the cosmetic writes of
    // `on frame` handlers first, then the in-between transforms (declared first, destroyed last).
    std::optional<ScopedInterpolation> between;
    struct Restore {
        Engine& e;
        bool effects = false, particles = false;
        ~Restore() {
            e.runtime_->revertFrame();
            e.animation_->setDisplayAlpha(1.f);
            if (effects) e.effectsTimeOverride_.reset();
            if (particles) e.particles_.setRenderTimeOffset(0.f);
        }
    } restore{*this};

    flowStats_.interpolated = 0;
    if (alpha < 1.f && transformHistory_.valid()) {
        between.emplace(*scene_, transformHistory_, alpha, &gate);
        flowStats_.interpolated = between->moved();
        animation_->setDisplayAlpha(alpha);  // skinned poses blend joints
        if (!effectsTimeOverride_) {  // water, sky, GPU effects at the displayed time (movie renders set their own)
            effectsTimeOverride_ = displayTime;
            restore.effects = true;
            particles_.setRenderTimeOffset(-(1.f - alpha) * kFixedDt * gameScale);  // CPU particles along their velocity
            restore.particles = true;
        }
    }
    flowStats_.frameHandlerRuns = 0;
    if (opts.frameHandlers && live) {
        wander::Runtime::FrameInfo info;
        info.dt = std::max(0.f, opts.frameDt);
        info.alpha = alpha;
        info.time = effectsTimeOverride_ ? *effectsTimeOverride_ : displayTime;  // movie sub-frames: exactly tau
        info.input = &input_;
        flowStats_.frameHandlerRuns = runtime_->runFrameHandlers(info);
    }
    return buildFrameData(opts);
}

void Engine::notePresentedFrame(float alpha) {
    pacing_.frame(realSinceFrame_, runtime_->unscaledTime(), alpha, ticksSinceFrame_, interpolate_);
    realSinceFrame_ = 0;
    ticksSinceFrame_ = 0;
}

void Engine::presented(const FrameData& f, float alpha) {
    if (trackDisplayHistory_) {
        for (const DrawItem& d : f.draws) {
            if (d.entity) displayHistory_.record(d.entity, d.model);
        }
        displayHistory_.endFrame();
    }
    notePresentedFrame(alpha);
}

// ---------------------------------------------------------------------------
// Wander builtins
// ---------------------------------------------------------------------------

namespace {

using namespace wander;

void def(BuiltinRegistry& reg, const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* category,
         const char* doc, const char* example, BuiltinImpl fn) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = returns;
    d.category = category;
    d.doc = doc;
    d.example = example;
    d.owner = "engine";
    d.fn = fn;
    reg.add(std::move(d));
}

}  // namespace

void registerFlowBuiltins(wander::BuiltinRegistry& reg) {
    using namespace wander;
    def(reg, "pause_game", {{"paused", kTBool, true}}, kTNone, "time",
        "Pauses the game (a pause menu) from the next tick: `pausable` entities (the default) stop — behaviors, physics, "
        "animation, particles, sounds — while UI canvases and entities with process mode `always` or `when_paused` keep "
        "running. Sends `on pause` to every behavior. pause_game(false) = resume_game().",
        "on action \"pause\"\n  if is_paused() then resume_game() else pause_game() end\nend",
        [](CallContext& c) -> Value {
            c.runtime().requestPause(c.argc() == 0 || c.boolean(0));
            return Value();
        });
    def(reg, "resume_game", {}, kTNone, "time", "Resumes a paused game from the next tick and sends `on resume`.",
        "on ui \"Resume\"\n  resume_game()\nend", [](CallContext& c) -> Value {
            c.runtime().requestPause(false);
            return Value();
        });
    def(reg, "is_paused", {}, kTBool, "time",
        "True while the game is paused (or will be from the next tick, after pause_game()).", "if is_paused() then ... end",
        [](CallContext& c) -> Value { return Value::boolean(c.runtime().pauseRequested()); });
    def(reg, "time_scale", {{"scale", kTNumber, true}}, kTNumber, "time",
        "Slow motion / fast forward: time_scale(0.3) makes the game clock run at 30% from the next tick (dt, timers, "
        "waits, physics, animation, particles; 0..10). Entities on the `real` clock (UI, process clock real) ignore it. "
        "Returns the scale (the requested one).",
        "time_scale(0.25)  -- bullet time\nafter 2 do time_scale(1) end",
        [](CallContext& c) -> Value {
            if (c.argc() > 0) c.runtime().requestTimeScale(c.number(0));
            return Value::number(c.runtime().timeScaleRequested());
        });
    def(reg, "unscaled_dt", {}, kTNumber, "time",
        "Real seconds per tick (1/60), whatever the time scale or pause: timers that must ignore slow motion.",
        "menu_timer += unscaled_dt()", [](CallContext&) -> Value { return Value::number(static_cast<double>(Engine::kFixedDt)); });
    def(reg, "unscaled_time", {}, kTNumber, "time",
        "Real seconds since play started (keeps counting while the game is paused or slowed; `time` does not).",
        "let pulse = sin(unscaled_time() * 4)", [](CallContext& c) -> Value { return Value::number(c.runtime().unscaledTime()); });
    def(reg, "teleport", {{"entity", kTEntity}, {"position", kTVec | kTEntity, true}}, kTNone, "time",
        "Moves an entity instantly (when given a position) and tells render interpolation not to smear it across the "
        "screen: respawns, portals, checkpoints. Without a position it only resets the smoothing for this frame.",
        "teleport(self, find(\"Spawn\"))", [](CallContext& c) -> Value {
            EntityRef e = c.entity(0);
            if (c.argc() > 1) {
                Vec3 p = c.point(1);
                Scene& s = c.scene();
                Transform& t = s.add<Transform>(e);
                const EntityRecord* rec = s.record(e);
                t.position = rec && rec->parent ? s.worldMatrix(rec->parent).inverse().transformPoint(p) : p;
                s.markDirty();
            }
            if (Engine* engine = c.service<Engine>()) engine->teleport(e);
            return Value();
        });
    reg.addTrigger({"pause", "The game was paused (pause_game): delivered to every behavior, also those the pause stops.", "",
                    "time"});
    reg.addTrigger({"resume", "The game resumed (resume_game).", "", "time"});
}

}  // namespace sky
