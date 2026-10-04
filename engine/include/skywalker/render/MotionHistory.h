#pragma once
// Previous-frame transforms for per-object motion vectors (the velocity buffer).
//
// Renderer backends need, for every drawn object, the model matrix it had in the previously
// rendered frame: the vertex shader projects both and the difference becomes the object's
// motion, which TAA, MetalFX temporal upscaling and object motion blur consume. Camera motion
// is reprojected from depth separately, so a static object has no object motion at all.
//
// Usage, once per rendered frame:
//   history.begin(resetHistory);
//   for (draw : frame.draws) prev = history.previous(draw.entity, draw.mesh, draw.model);
//   history.end();
//
// Deterministic and renderer-agnostic (CPU only), so it is unit tested without a GPU.

#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "skywalker/math/Math.h"

namespace sky {

using EntityId = uint64_t;

class MotionHistory {
public:
    struct Stats {
        size_t tracked = 0;        // objects drawn this frame
        size_t moving = 0;         // ... whose transform changed since the previous frame
        size_t teleported = 0;     // ... that jumped farther than teleportDistance (no motion)
        float maxDistance = 0.f;   // largest translation since the previous frame (m)
    };

    /// Starts a frame. `reset` (camera cut, history reset) makes every object start static.
    void begin(bool reset);
    /// The model matrix the object (entity + mesh) had in the previous frame; `model` itself
    /// when it was not drawn then (new, re-shown, after a reset) or jumped beyond
    /// teleportDistance. Idempotent within a frame (shadow and sub-sample passes may ask again).
    Mat4 previous(EntityId entity, std::string_view mesh, const Mat4& model);
    /// Ends the frame: forgets objects that were not drawn in it.
    void end();
    void clear();

    const Stats& stats() const { return stats_; }
    uint64_t frame() const { return frame_; }

    /// A translation larger than this in one frame is a teleport, not motion (meters).
    float teleportDistance = 25.f;

private:
    struct Entry {
        Mat4 model;     // this frame's (after previous() was called this frame)
        Mat4 previous;  // the previous frame's, as reported this frame
        uint64_t frame = 0;
    };
    static uint64_t key(EntityId entity, std::string_view mesh);

    std::unordered_map<uint64_t, Entry> entries_;
    uint64_t frame_ = 0;
    bool reset_ = false;
    bool open_ = false;
    Stats stats_;
};

/// True when two transforms differ (object motion), with a small tolerance.
bool transformChanged(const Mat4& a, const Mat4& b, float epsilon = 1e-6f);

}  // namespace sky
