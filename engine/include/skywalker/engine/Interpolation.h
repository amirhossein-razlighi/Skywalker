#pragma once
// Render interpolation (docs/ARCHITECTURE.md "Render interpolation").
//
// The simulation runs at a fixed 60 Hz. A display refreshing at 120 Hz (ProMotion) or 144 Hz
// would show each tick for two frames or an uneven number of frames, so motion stutters.
// Real-time frames therefore show the world *between* the last two ticks:
//
//   alpha  = accumulator / fixed dt                 (0..1, how far real time is into the next tick)
//   shown  = lerp/slerp(previous tick, last tick, alpha)  per entity local transform
//
// i.e. the picture lags the simulation by up to one tick, like every engine with fixed-step
// physics (the usual "physics interpolation" technique). The engine keeps the
// local transforms of the tick before the last one (TransformHistory, filled at the start of each
// tick); a ScopedInterpolation writes the in-between transforms into the scene for the time it
// takes to build one frame and restores them afterwards. Everything that reads world matrices
// (meshes, cameras, lights, sprites, text, UI in world space, particle emitters, hair, bone
// attachments) is smoothed that way; skinned poses blend joint matrices (AnimationSystem) and
// CPU particles move along their velocity. Simulation, tools and captures never see these
// values: captures stay tick-exact unless they ask for an alpha.
//
// The movie renderer (Movie.cpp) uses the same history and scope for its sub-frames.
//
// For renderer features that need the previous frame (velocity buffers, motion blur): what
// was displayed last frame per entity is in DisplayHistory (Engine::displayHistory()); it is
// filled after every real-time frame from the draws' model matrices.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "skywalker/scene/Scene.h"

namespace sky {

class ProcessGate;

/// lerp / slerp between two local transforms. A move longer than `teleportDistance` in one
/// tick is a jump, not motion: the result is `b`.
Transform interpolateTransform(const Transform& a, const Transform& b, float alpha, float teleportDistance = 25.f);

/// Local transforms as of the previous tick.
class TransformHistory {
public:
    /// Remembers every entity's local transform (call right before a tick runs).
    void capture(const Scene& scene);
    void clear();
    /// The next frames show `e` exactly where it is (no smear after a jump). Until the next capture.
    void teleport(EntityId e);
    bool valid() const { return valid_; }
    size_t size() const { return entries_.size(); }
    /// The transform `e` had before the last tick (null: new, teleported, or no history).
    const Transform* previous(EntityId e) const;
    /// Ticks captured since play started (stats).
    uint64_t captures() const { return captures_; }

private:
    friend class ScopedInterpolation;
    struct Entry {
        EntityId id = kNoEntity;
        Transform t;
        bool teleported = false;
    };
    std::vector<Entry> entries_;  // scene order
    std::unordered_map<EntityId, size_t> index_;
    bool valid_ = false;
    uint64_t captures_ = 0;
};

/// Writes interpolated transforms into the scene for its lifetime; restores the tick state when
/// destroyed. Entities the gate says not to interpolate (`process.interpolation: off`) stay put.
class ScopedInterpolation {
public:
    ScopedInterpolation(Scene& scene, const TransformHistory& history, float alpha, const ProcessGate* gate = nullptr,
                        float teleportDistance = 25.f);
    ~ScopedInterpolation();
    ScopedInterpolation(const ScopedInterpolation&) = delete;
    ScopedInterpolation& operator=(const ScopedInterpolation&) = delete;
    /// Entities shown between ticks this frame.
    size_t moved() const { return restore_.size(); }

private:
    Scene& scene_;
    std::vector<std::pair<EntityId, Transform>> restore_;
};

/// What the last real-time frame displayed, per entity (model matrices of its draws), for
/// features that need "previous frame" data: velocity buffers, per-object motion blur, jitter stats.
class DisplayHistory {
public:
    void record(EntityId e, const Mat4& model) { next_[e] = model; }
    /// Ends a frame: what was recorded becomes "previous".
    void endFrame();
    void clear();
    /// The model matrix `e` was drawn with on the previous frame (null if it was not drawn).
    const Mat4* previous(EntityId e) const;

private:
    std::unordered_map<EntityId, Mat4> prev_, next_;
};

/// Frame pacing as an agent can check it: how evenly the displayed simulation time advances
/// compared to real time (0 = perfectly smooth).
struct PacingStats {
    uint64_t frames = 0;
    float alpha = 1.f;             // of the last frame
    double displayDt = 0;          // real seconds between the last two frames
    int ticksLastFrame = 0;        // fixed ticks run before the last frame
    double jitterMs = 0;           // RMS of (displayed time step - real time step), with interpolation as configured
    double jitterMsRaw = 0;        // the same if frames showed whole ticks (no interpolation): the stutter avoided
    double maxErrorMs = 0;         // worst single-frame error in the window
    uint64_t windowFrames = 0;     // frames in the current RMS window
    Json toJson() const;
};

/// Accumulates PacingStats from presented frames.
class PacingMeter {
public:
    /// One presented frame: `realDt` seconds since the previous one, `tickTime` the simulation
    /// time of the last tick (unscaled), `alpha` the interpolation used (1 = none).
    void frame(double realDt, double tickTime, float alpha, int ticks, bool interpolated);
    void reset();
    const PacingStats& stats() const { return stats_; }

private:
    PacingStats stats_;
    bool havePrev_ = false;
    double prevShown_ = 0, prevRaw_ = 0;
    double sumSq_ = 0, sumSqRaw_ = 0;
    double maxErr_ = 0;
    uint64_t n_ = 0;
};

}  // namespace sky
