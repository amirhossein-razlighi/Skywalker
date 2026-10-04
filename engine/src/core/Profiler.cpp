#include "skywalker/core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sky::prof {

double roundMs(double ms) { return std::round(ms * 1000.0) / 1000.0; }

// ---------------------------------------------------------------------------
// RollingStat
// ---------------------------------------------------------------------------

void RollingStat::add(double v) {
    if (!std::isfinite(v)) return;
    samples_[next_] = v;
    next_ = (next_ + 1) % kWindow;
    count_ = std::min(count_ + 1, kWindow);
    ++total_;
    last_ = v;
}

void RollingStat::reset() { *this = RollingStat{}; }

double RollingStat::average() const {
    if (count_ == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < count_; ++i) sum += samples_[i];
    return sum / static_cast<double>(count_);
}

double RollingStat::min() const {
    if (count_ == 0) return 0;
    return *std::min_element(samples_, samples_ + count_);
}

double RollingStat::max() const {
    if (count_ == 0) return 0;
    return *std::max_element(samples_, samples_ + count_);
}

// ---------------------------------------------------------------------------
// Pass aggregation
// ---------------------------------------------------------------------------

std::vector<PassTotal> aggregatePasses(const std::vector<PassSample>& samples) {
    std::vector<PassTotal> out;
    for (const PassSample& s : samples) {
        if (!(s.endMs >= s.startMs) || !std::isfinite(s.startMs) || !std::isfinite(s.endMs)) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const PassTotal& t) { return t.label == s.label; });
        if (it == out.end()) {
            out.push_back({s.label, s.group, 0.0, -1.0, 0});
            it = out.end() - 1;
        }
        it->ms += s.endMs - s.startMs;
        if (s.vertexMs >= 0) it->vertexMs = std::max(it->vertexMs, 0.0) + s.vertexMs;
        ++it->count;
    }
    return out;
}

double passSpanMs(const std::vector<PassSample>& samples) {
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const PassSample& s : samples) {
        if (!(s.endMs >= s.startMs)) continue;
        lo = std::min(lo, s.startMs);
        hi = std::max(hi, s.endMs);
    }
    return hi >= lo ? hi - lo : 0.0;
}

// ---------------------------------------------------------------------------
// PassTimeline
// ---------------------------------------------------------------------------

void PassTimeline::addFrame(const std::vector<PassSample>& samples) {
    std::vector<PassTotal> totals = aggregatePasses(samples);
    if (totals.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
    span_.add(passSpanMs(samples));
    for (size_t i = 0; i < totals.size(); ++i) {
        const PassTotal& t = totals[i];
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.label == t.label; });
        if (it == entries_.end()) {
            entries_.push_back(Entry{t.label, t.group, {}, {}, 0, 0, 0});
            it = entries_.end() - 1;
        }
        it->group = t.group;
        it->ms.add(t.ms);
        if (t.vertexMs >= 0) it->vertexMs.add(t.vertexMs);
        it->count = t.count;
        it->lastFrame = frames_;
        it->order = i;
    }
    // Passes that stopped running (an effect was turned off) fall out after a second of frames.
    std::erase_if(entries_, [&](const Entry& e) { return frames_ - e.lastFrame > RollingStat::kWindow; });
}

void PassTimeline::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    span_.reset();
    frames_ = 0;
}

uint64_t PassTimeline::frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
}

Json PassTimeline::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Entry*> sorted;
    for (const Entry& e : entries_) sorted.push_back(&e);
    // Encode order of the latest frame; passes missing from it go after, by their last order.
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
        if (a->lastFrame != b->lastFrame) return a->lastFrame > b->lastFrame;
        return a->order < b->order;
    });
    Json passes = Json::array();
    Json groups = Json::object();
    double sum = 0;
    const double window = static_cast<double>(std::min<uint64_t>(frames_, RollingStat::kWindow));
    for (const Entry* e : sorted) {
        const double avg = e->ms.average();
        // Cost per frame: a pass that ran in only some frames of the window counts by that share.
        const double perFrame = window > 0 ? avg * std::min(1.0, static_cast<double>(e->ms.count()) / window) : avg;
        Json p = Json::object({{"pass", e->label},
                               {"group", e->group},
                               {"ms", roundMs(e->ms.last())},
                               {"avgMs", roundMs(avg)},
                               {"minMs", roundMs(e->ms.min())},
                               {"maxMs", roundMs(e->ms.max())},
                               {"count", e->count},
                               {"seen", static_cast<int64_t>(e->ms.count())}});
        if (e->vertexMs.count() > 0) p["vertexMs"] = roundMs(e->vertexMs.average());
        passes.push(std::move(p));
        groups[e->group] = roundMs(groups.get(e->group).asNumber(0.0) + perFrame);
        sum += perFrame;
    }
    return Json::object({{"frames", static_cast<int64_t>(frames_)},
                         {"window", static_cast<int64_t>(window)},
                         {"spanMs", roundMs(span_.average())},
                         {"sumMs", roundMs(sum)},
                         {"passes", passes},
                         {"groups", groups}});
}

// ---------------------------------------------------------------------------
// CPU scopes
// ---------------------------------------------------------------------------

CpuProfiler& CpuProfiler::instance() {
    static CpuProfiler p;
    return p;
}

void CpuProfiler::record(const char* name, double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == name; });
    if (it == entries_.end()) {
        entries_.push_back(Entry{name, {}});
        it = entries_.end() - 1;
    }
    it->ms.add(ms);
}

void CpuProfiler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

Json CpuProfiler::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::array();
    for (const Entry& e : entries_) {
        out.push(Json::object({{"scope", e.name},
                               {"avgMs", roundMs(e.ms.average())},
                               {"minMs", roundMs(e.ms.min())},
                               {"maxMs", roundMs(e.ms.max())},
                               {"lastMs", roundMs(e.ms.last())},
                               {"calls", static_cast<int64_t>(e.ms.total())}}));
    }
    return out;
}

Scope::~Scope() {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    CpuProfiler::instance().record(name_, ms);
}

}  // namespace sky::prof
