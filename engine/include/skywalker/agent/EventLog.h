#pragma once
// EventLog: a bounded, sequence-numbered history of engine events that many readers can follow.
//
// Engine::drainEvents() hands each event to exactly one consumer (the editor's activity feed).
// External agents need to *follow* the same stream without stealing it, from any thread, and
// resume where they left off: every event gets a monotonically increasing `seq`, readers keep
// a cursor, and a reader can block until something newer arrives (long polling on an agent
// connection thread). Thread-safe; the engine appends on the main thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "skywalker/core/Json.h"

namespace sky {

class EventLog {
public:
    explicit EventLog(size_t capacity = 4096);

    /// Stamps the event with "seq" and "time" (seconds since the Unix epoch), stores it and
    /// wakes waiting readers. Returns the sequence number. The oldest events fall off the end.
    uint64_t append(Json event);

    struct Batch {
        std::vector<Json> events;  // seq > cursor, oldest first, at most `limit`
        uint64_t next = 0;         // the cursor to pass next time (last event scanned)
        bool truncated = false;    // events after the cursor were already dropped (cursor too old)
        bool more = false;         // the limit cut the batch short
        bool reset = false;        // the cursor came from an earlier engine session; reading restarted at 0
    };
    using Filter = std::function<bool(const Json&)>;
    /// Events after `cursor` that pass `filter` (all when empty).
    Batch since(uint64_t cursor, size_t limit, const Filter& filter = {}) const;
    /// since(), but if nothing matches yet, waits up to `timeout` for a matching event.
    /// Returns early when `cancel` becomes true or wakeAll() is called.
    Batch wait(uint64_t cursor, size_t limit, std::chrono::milliseconds timeout, const Filter& filter = {},
               const std::atomic<bool>* cancel = nullptr) const;
    /// Wakes every waiting reader (shutdown, cancellation).
    void wakeAll() const;

    uint64_t lastSeq() const;
    uint64_t oldestSeq() const;  // 0 when empty
    size_t size() const;
    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<Json> events_;
    uint64_t lastSeq_ = 0;
    mutable uint64_t wakeGeneration_ = 0;
};

}  // namespace sky
