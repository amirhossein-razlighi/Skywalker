// Render interpolation: transform history, scoped in-between transforms, display history and
// frame pacing (skywalker/engine/Interpolation.h).

#include "skywalker/engine/Interpolation.h"

#include <algorithm>
#include <cmath>

#include "skywalker/anim/AnimMath.h"
#include "skywalker/scene/Process.h"

namespace sky {

Transform interpolateTransform(const Transform& a, const Transform& b, float alpha, float teleportDistance) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    if (alpha >= 1.f || distance(a.position, b.position) > teleportDistance) return b;
    Transform out = b;
    out.position = lerp(a.position, b.position, alpha);
    out.scale = lerp(a.scale, b.scale, alpha);
    if (a.rotation.x != b.rotation.x || a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z) {
        out.rotation = anim::eulerDegFromQuat(anim::slerp(anim::Quat::fromEulerDeg(a.rotation), anim::Quat::fromEulerDeg(b.rotation), alpha));
    }
    return out;
}

// ---------------------------------------------------------------------------
// TransformHistory
// ---------------------------------------------------------------------------

void TransformHistory::capture(const Scene& scene) {
    entries_.clear();
    index_.clear();
    const auto& order = scene.entities();
    entries_.reserve(order.size());
    for (EntityId e : order) {
        if (const Transform* t = scene.get<Transform>(e)) {
            index_[e] = entries_.size();
            entries_.push_back({e, *t, false});
        }
    }
    valid_ = true;
    ++captures_;
}

void TransformHistory::clear() {
    entries_.clear();
    index_.clear();
    valid_ = false;
    captures_ = 0;
}

void TransformHistory::teleport(EntityId e) {
    auto it = index_.find(e);
    if (it != index_.end()) entries_[it->second].teleported = true;
}

const Transform* TransformHistory::previous(EntityId e) const {
    auto it = index_.find(e);
    if (it == index_.end() || entries_[it->second].teleported) return nullptr;
    return &entries_[it->second].t;
}

// ---------------------------------------------------------------------------
// ScopedInterpolation
// ---------------------------------------------------------------------------

ScopedInterpolation::ScopedInterpolation(Scene& scene, const TransformHistory& history, float alpha, const ProcessGate* gate,
                                         float teleportDistance)
    : scene_(scene) {
    if (!history.valid() || alpha >= 1.f) return;
    alpha = std::max(alpha, 0.f);
    for (const auto& entry : history.entries_) {
        if (entry.teleported) continue;
        Transform* t = scene.get<Transform>(entry.id);
        if (!t) continue;
        const Transform& a = entry.t;
        if (a.position == t->position && a.rotation == t->rotation && a.scale == t->scale) continue;  // at rest
        if (gate && !gate->interpolates(entry.id)) continue;
        restore_.emplace_back(entry.id, *t);
        *t = interpolateTransform(a, *t, alpha, teleportDistance);
    }
}

ScopedInterpolation::~ScopedInterpolation() {
    for (auto it = restore_.rbegin(); it != restore_.rend(); ++it) {
        if (Transform* t = scene_.get<Transform>(it->first)) *t = it->second;
    }
}

// ---------------------------------------------------------------------------
// DisplayHistory
// ---------------------------------------------------------------------------

void DisplayHistory::endFrame() {
    prev_.swap(next_);
    next_.clear();
}

void DisplayHistory::clear() {
    prev_.clear();
    next_.clear();
}

const Mat4* DisplayHistory::previous(EntityId e) const {
    auto it = prev_.find(e);
    return it == prev_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------

namespace {
double round3(double v) { return std::round(v * 1000.0) / 1000.0; }
constexpr double kTickSeconds = 1.0 / 60.0;
constexpr uint64_t kWindow = 240;  // frames per RMS window (2 s at 120 Hz)
}  // namespace

Json PacingStats::toJson() const {
    return Json::object({{"frames", frames},
                         {"alpha", round3(alpha)},
                         {"displayDtMs", round3(displayDt * 1000.0)},
                         {"ticksLastFrame", ticksLastFrame},
                         {"jitterMs", round3(jitterMs)},
                         {"jitterMsWithoutInterpolation", round3(jitterMsRaw)},
                         {"maxErrorMs", round3(maxErrorMs)},
                         {"windowFrames", windowFrames}});
}

void PacingMeter::frame(double realDt, double tickTime, float alpha, int ticks, bool interpolated) {
    const double a = interpolated ? std::clamp(static_cast<double>(alpha), 0.0, 1.0) : 1.0;
    const double shown = tickTime - (1.0 - a) * kTickSeconds;
    ++stats_.frames;
    stats_.alpha = static_cast<float>(a);
    stats_.displayDt = realDt;
    stats_.ticksLastFrame = ticks;
    if (havePrev_ && realDt > 0.0 && realDt < 0.25) {  // stalls (and the spiral-of-death clamp) are not jitter
        const double err = (shown - prevShown_) - realDt;
        const double errRaw = (tickTime - prevRaw_) - realDt;
        if (n_ >= kWindow) {  // start a new window, keeping the last one's averages as a prior
            sumSq_ = stats_.jitterMs * stats_.jitterMs * 1e-6;
            sumSqRaw_ = stats_.jitterMsRaw * stats_.jitterMsRaw * 1e-6;
            maxErr_ = 0;
            n_ = 1;
        }
        sumSq_ += err * err;
        sumSqRaw_ += errRaw * errRaw;
        maxErr_ = std::max(maxErr_, std::fabs(err));
        ++n_;
        stats_.jitterMs = std::sqrt(sumSq_ / static_cast<double>(n_)) * 1000.0;
        stats_.jitterMsRaw = std::sqrt(sumSqRaw_ / static_cast<double>(n_)) * 1000.0;
        stats_.maxErrorMs = maxErr_ * 1000.0;
        stats_.windowFrames = n_;
    }
    prevShown_ = shown;
    prevRaw_ = tickTime;
    havePrev_ = true;
}

void PacingMeter::reset() {
    stats_ = {};
    havePrev_ = false;
    prevShown_ = prevRaw_ = 0;
    sumSq_ = sumSqRaw_ = maxErr_ = 0;
    n_ = 0;
}

}  // namespace sky
