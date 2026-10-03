#pragma once
// Child-process execution for the DCC bridge (Blender, Maya, Houdini, 3ds Max).
//
// Safety rules baked into the API:
//   * a process is started from an argv vector with posix_spawn — there is never a shell, so
//     file names and script arguments cannot be interpreted as commands;
//   * stdout/stderr are captured with a hard cap (head + tail are kept) so a chatty or runaway
//     tool cannot exhaust memory;
//   * every run has a timeout, and can be cancelled from another thread; the whole process
//     group is terminated (SIGTERM, then SIGKILL after a grace period).
// `runProcess` blocks the calling thread, so tools run it on a worker thread (see
// ToolResult::defer) and apply results on the main thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky::dcc {

/// Shared flag that asks a running process to stop. Thread-safe.
class CancelToken {
public:
    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }

private:
    std::atomic<bool> cancelled_{false};
};

struct ProcessSpec {
    std::string executable;          // absolute path, or a bare name looked up in PATH
    std::vector<std::string> args;   // argv[1..]; passed verbatim
    std::string workingDir;          // empty = inherit
    /// Extra environment on top of the current one (later entries win).
    std::vector<std::pair<std::string, std::string>> env;
    std::chrono::milliseconds timeout{std::chrono::minutes(10)};
    std::chrono::milliseconds killGrace{std::chrono::milliseconds(2000)};
    size_t maxOutputBytes = 256 * 1024;  // per stream; the head and the tail are kept
    std::shared_ptr<CancelToken> cancel;  // optional
};

struct ProcessResult {
    bool spawned = false;
    std::string spawnError;  // set when !spawned
    int exitCode = -1;       // valid when the process exited normally
    int signal = 0;          // terminating signal, 0 if none
    bool timedOut = false;
    bool cancelled = false;
    std::string out;
    std::string err;
    uint64_t outBytes = 0;  // total produced, including what was dropped by the cap
    uint64_t errBytes = 0;
    bool outTruncated = false;
    bool errTruncated = false;
    double seconds = 0;

    bool ok() const { return spawned && !timedOut && !cancelled && signal == 0 && exitCode == 0; }
};

/// Runs a process to completion. Never throws.
ProcessResult runProcess(const ProcessSpec& spec);

/// Starts a long-lived process (a Blender session) in its own session, with stdin closed and
/// stdout/stderr appended to `logPath`. Returns its pid; the caller owns the process.
Result<long> spawnDetached(const ProcessSpec& spec, const std::string& logPath);
/// True while `pid` exists (signal 0).
bool processAlive(long pid);
/// Asks a process to exit (SIGTERM), or kills it (`force`). The pid must be one we started.
void terminateProcess(long pid, bool force = false);

/// Keeps the first `cap/4` and the last `cap - cap/4` bytes of a stream; the middle is
/// replaced by a marker. Exposed for tests.
class BoundedCapture {
public:
    explicit BoundedCapture(size_t cap) : headCap_(cap / 4), tailCap_(cap - cap / 4) {}
    void append(const char* data, size_t n);
    uint64_t total() const { return total_; }
    /// Head + marker (if anything was dropped) + tail.
    std::string str() const;
    size_t keptBytes() const { return head_.size() + tail_.size(); }

private:
    size_t headCap_;
    size_t tailCap_;
    std::string head_;
    std::string tail_;
    uint64_t total_ = 0;
};

}  // namespace sky::dcc
