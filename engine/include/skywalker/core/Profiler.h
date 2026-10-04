#pragma once
// Frame profiling: rolling timing statistics, CPU scopes and the per-pass GPU timeline.
//
// CPU scopes (`SKY_PROFILE_SCOPE("frame.build")`) time a block on the calling thread and feed a
// process-wide table of rolling statistics (the last 60 samples). The GPU backends feed the same
// `PassTimeline` type with per-pass timestamps; `perf_stats {passes: true}` merges both.
//
// Everything here is CPU-only and unit-tested; the Metal backend only produces raw samples.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"

namespace sky::prof {

/// Rolling statistics over the last `kWindow` samples (frames): average, min, max, last.
class RollingStat {
public:
    static constexpr size_t kWindow = 60;
    void add(double v);
    void reset();
    size_t count() const { return count_; }  // samples in the window (<= kWindow)
    uint64_t total() const { return total_; }  // samples ever added
    double last() const { return last_; }
    double average() const;
    double min() const;
    double max() const;

private:
    double samples_[kWindow] = {};
    size_t next_ = 0, count_ = 0;
    uint64_t total_ = 0;
    double last_ = 0;
};

/// One measured GPU pass of one frame: a label (the encoder label, e.g. "SSGI"), the group it
/// belongs to (e.g. "ssgi", "post"), and its start/end in milliseconds on any common clock.
struct PassSample {
    std::string label;
    std::string group;
    double startMs = 0;
    double endMs = 0;
    double vertexMs = -1;  // render passes: vertex-stage span (runs overlapped with earlier passes); < 0 = n/a
};

/// Per-frame totals of a list of samples: passes with the same label are summed (bloom levels,
/// still sub-samples), in first-seen order. Invalid samples (end < start) are skipped.
struct PassTotal {
    std::string label;
    std::string group;
    double ms = 0;
    double vertexMs = -1;  // summed vertex spans (< 0 when no sample had one)
    int count = 0;  // encoders merged into this label this frame
};
std::vector<PassTotal> aggregatePasses(const std::vector<PassSample>& samples);
/// Span of a frame's samples (last end - first start): the GPU time the measured passes cover.
double passSpanMs(const std::vector<PassSample>& samples);

/// Rolling per-pass timeline fed one frame at a time (thread-safe: the Metal backend adds frames
/// from command-buffer completion handlers).
class PassTimeline {
public:
    void addFrame(const std::vector<PassSample>& samples);
    void reset();
    uint64_t frames() const;
    /// {frames, window, spanMs, sumMs, passes: [{pass, group, ms, avgMs, minMs, maxMs, vertexMs?, count, seen}],
    /// groups: {group: ms per frame}}. `seen` = frames of the window the pass ran in; one-off passes
    /// (environment bakes) weigh into groups/sumMs by that share. Passes keep the order of the most
    /// recent frame; labels not seen for a window are dropped.
    Json toJson() const;

private:
    struct Entry {
        std::string label, group;
        RollingStat ms, vertexMs;
        int count = 0;
        uint64_t lastFrame = 0;
        size_t order = 0;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    RollingStat span_;
    uint64_t frames_ = 0;
};

/// Process-wide CPU scope table (thread-safe).
class CpuProfiler {
public:
    static CpuProfiler& instance();
    void record(const char* name, double ms);
    void reset();
    /// [{scope, avgMs, minMs, maxMs, lastMs, calls}] in first-recorded order.
    Json toJson() const;

private:
    struct Entry {
        std::string name;
        RollingStat ms;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

/// RAII timer for a CPU scope: records the elapsed milliseconds when it goes out of scope.
class Scope {
public:
    explicit Scope(const char* name) : name_(name), start_(std::chrono::steady_clock::now()) {}
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name_;
    std::chrono::steady_clock::time_point start_;
};

/// Rounds a millisecond value for JSON output (3 decimals).
double roundMs(double ms);

}  // namespace sky::prof

#define SKY_PROFILE_CONCAT_INNER(a, b) a##b
#define SKY_PROFILE_CONCAT(a, b) SKY_PROFILE_CONCAT_INNER(a, b)
/// Times the enclosing block as a named CPU scope (see perf_stats {passes: true}).
#define SKY_PROFILE_SCOPE(name) ::sky::prof::Scope SKY_PROFILE_CONCAT(skyProfileScope_, __LINE__)(name)
