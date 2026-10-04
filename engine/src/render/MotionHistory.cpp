#include "skywalker/render/MotionHistory.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace sky {

bool transformChanged(const Mat4& a, const Mat4& b, float epsilon) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > epsilon) return true;
    }
    return false;
}

uint64_t MotionHistory::key(EntityId entity, std::string_view mesh) {
    // Mesh changes (swapped models, skinned instance keys) start a new history.
    return entity * 0x9E3779B97F4A7C15ull ^ std::hash<std::string_view>{}(mesh);
}

void MotionHistory::begin(bool reset) {
    ++frame_;
    reset_ = reset;
    open_ = true;
    stats_ = {};
}

Mat4 MotionHistory::previous(EntityId entity, std::string_view mesh, const Mat4& model) {
    Entry& e = entries_[key(entity, mesh)];
    if (e.frame == frame_) return e.previous;  // already asked this frame
    const bool known = e.frame != 0 && e.frame + 1 == frame_ && !reset_;
    Mat4 prev = known ? e.model : model;
    ++stats_.tracked;
    if (known && transformChanged(prev, model)) {
        const float moved = distance(prev.translation(), model.translation());
        if (moved > teleportDistance) {
            ++stats_.teleported;
            prev = model;
        } else {
            ++stats_.moving;
            stats_.maxDistance = std::max(stats_.maxDistance, moved);
        }
    }
    e.model = model;
    e.previous = prev;
    e.frame = frame_;
    return prev;
}

void MotionHistory::end() {
    if (!open_) return;
    open_ = false;
    std::erase_if(entries_, [&](const auto& kv) { return kv.second.frame != frame_; });
}

void MotionHistory::clear() {
    entries_.clear();
    stats_ = {};
}

}  // namespace sky
