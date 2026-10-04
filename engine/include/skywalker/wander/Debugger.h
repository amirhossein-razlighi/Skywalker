#pragma once
// The Wander debugger (docs/WANDER.md "Debugging"): breakpoints by script:line with conditions, hit
// counts and logpoints; break on runtime errors; step over / into / out; pause; the call stack with
// locals, arguments and the entity's vars; expressions evaluated in a paused frame; setting variables.
//
// Cost when idle: none. The VM runs its normal loop unless active() is true (breakpoints, stepping,
// a pause request or break-on-error); then a separate instantiation of the loop sees each statement.
//
// Pausing: the VM stops *inside* the handler, before the statement on the line. The host decides
// how a stop waits: the engine runs debugged ticks on its own thread and hands control back to the
// main thread, so tools (wander_stack, wander_eval, wander_continue...) keep working while the
// simulation is frozen mid-tick (Engine "debug runs"). A synchronous `onStop` handler (tests, a DAP
// adapter) can answer stops in place instead. Resuming continues exactly where it stopped, so a
// paused game replays identically.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/wander/Value.h"

namespace sky::wander {

class Runtime;
struct ExecState;
struct Program;

struct Breakpoint {
    int id = 0;
    std::string script;         // behavior script name ("Guard") or module file ("scripts/ai.wander"); "" = any
    int line = 0;               // where it stops (the first line with code at or after the requested one)
    int requestedLine = 0;
    uint64_t entity = 0;        // only this entity (0 = any)
    std::string condition;      // Wander expression: stop only when it is true
    std::string hitCondition;   // "5" (the 5th hit), ">=5" (from the 5th), "%3" (every 3rd)
    std::string log;            // logpoint: logs this message ({expr} interpolated) instead of stopping
    bool enabled = true;
    int hits = 0;
    bool verified = false;      // a compiled script has code on `line`
    std::string message;        // why it is not verified, or the last condition error
    Json toJson() const;
};

enum class StepMode { Continue, Over, Into, Out };

class Debugger {
public:
    explicit Debugger(Runtime& runtime);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    // --- Breakpoints (main thread) -----------------------------------------------------------
    /// Adds a breakpoint, moved to the first line with code at or after `line` in the scripts compiled so far.
    Result<Breakpoint> setBreakpoint(Breakpoint bp);
    bool clearBreakpoint(int id);
    /// Clears every breakpoint (of one script when given). Returns how many.
    size_t clearBreakpoints(const std::string& script = {});
    std::vector<Breakpoint> breakpoints() const;
    void setBreakOnError(bool on);
    bool breakOnError() const { return breakOnError_; }

    // --- Control ----------------------------------------------------------------------------
    /// Stops at the next statement any behavior runs.
    void requestPause();
    /// Continues a paused VM (or steps). Returns once it stopped again or the debugged ticks finished.
    Status resume(StepMode how);
    /// Unwinds a paused VM: the tick in progress is abandoned (play is stopping).
    void abort();
    bool paused() const { return paused_.load(); }
    /// Debugged code runs the VM's debug loop; false = zero cost.
    bool active() const { return active_ && !evaluating_; }

    // --- Inspection while paused --------------------------------------------------------------
    /// paused, reason, where (script, line, column, function, entity), step mode, breakpoints.
    Json state() const;
    /// Frames innermost first: function, script, line, entity and behavior; with `vars`: locals, args, the entity's vars.
    Json stack(bool vars = true) const;
    /// Evaluates a Wander expression in a frame (0 = innermost): locals, args, vars and `self` are in scope.
    /// Read-only: assignments to the scene fail.
    Result<Json> eval(int frame, const std::string& expression);
    /// Sets a local, an argument or a var of the frame's entity.
    Status setVariable(int frame, const std::string& name, const Json& value);
    uint64_t stopCount() const { return stops_.load(); }
    /// Thread-safe: waits until stopCount() exceeds `after` (true) or the timeout passes (false).
    bool waitForStop(uint64_t after, std::chrono::milliseconds timeout);

    // --- Host (the engine) ---------------------------------------------------------------------
    struct Host {
        std::function<bool()> canPause;           // the VM may stop here (it runs on a thread that can wait)
        std::function<void()> waitWhilePaused;    // blocks the VM until resume() / abort()
        std::function<void()> drive;              // main thread: let the VM run until it stops again or finishes
        std::function<void(const Json&)> event;   // wander.paused / wander.resumed
    };
    void setHost(Host host) { host_ = std::move(host); }
    /// Synchronous stop handler (tests, adapters): called on the VM's thread with state(); returns how to go on.
    std::function<StepMode(const Json& state)> onStop;
    /// Play started or stopped: stepping and pause requests end, hit counts restart (breakpoints stay).
    void onReset();

    // --- VM hooks (Runtime.cpp) -------------------------------------------------------------------
    void enterFrame(ExecState& st, int proto, Value* regs, const size_t* pc);
    void leaveFrame();
    void statement(ExecState& st, int proto, size_t pc);
    void runtimeError(ExecState& st, int proto, size_t pc, const std::string& message);

    struct Frame {
        ExecState* st = nullptr;
        int proto = -1;
        Value* regs = nullptr;
        const size_t* pc = nullptr;
        uint64_t run = 0;  // handler run the frame belongs to
    };
    struct Stop;

private:
    std::string scriptOf(const Frame& f) const;
    int lineOf(const Frame& f) const;
    void stopHere(const std::string& reason, int breakpoint, const std::string& message);
    bool breakpointHit(Breakpoint& bp, const Frame& f);
    Result<Value> evaluate(const Frame& f, const std::string& expression, bool guard);
    std::string interpolate(const Frame& f, const std::string& text);
    void refreshActive();
    Json frameJson(size_t index, bool vars) const;

    Runtime& rt_;
    Host host_;
    std::vector<Breakpoint> breakpoints_;
    std::set<int> lines_;  // lines with an enabled breakpoint (fast reject)
    int nextId_ = 1;
    bool breakOnError_ = false;
    bool pauseRequested_ = false;
    bool active_ = false;
    bool evaluating_ = false;
    StepMode step_ = StepMode::Continue;
    uint64_t stepRun_ = 0;
    size_t stepDepth_ = 0;
    bool stepOrphaned_ = false;  // the run a step started in ended: stop at the next statement anywhere
    std::vector<Frame> frames_;
    uint64_t runs_ = 0;
    bool errorReported_ = false;  // the error being thrown stopped already (outer frames rethrow it quietly)
    bool aborting_ = false;
    std::unique_ptr<Stop> stop_;
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> stops_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<const Program>> evalCache_;
};

/// Thrown through a paused VM by Debugger::abort(): the tick is abandoned.
struct DebugAbort {};

}  // namespace sky::wander
