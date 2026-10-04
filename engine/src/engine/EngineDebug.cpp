// The engine as host of the Wander debugger (docs/WANDER.md "Debugging").
//
// While the debugger is active, step() runs its ticks on a short-lived thread. The two threads never
// run at the same time: the main thread waits while the simulation thread runs, and when a breakpoint
// stops the VM mid-statement the simulation thread waits and hands control back. step() then returns
// with the tick held open, and the main thread goes on serving tools (wander_stack, wander_eval,
// wander_continue...), rendering and pumping jobs, so neither the editor nor the agent server
// deadlocks. Resuming hands control to the simulation thread again until the next stop or the end of
// the ticks. Nothing advances while a tick is held, so a paused game replays identically.

#include <exception>
#include <thread>

#include "EngineDebug.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Debugger.h"

namespace sky {


void Engine::initDebugger() {
    debugRun_ = std::make_unique<DebugRun>();
    wander::Debugger::Host host;
    host.canPause = [this] { return debugRun_->running && std::this_thread::get_id() == debugRun_->simThread; };
    host.waitWhilePaused = [this] {  // simulation thread, inside the VM
        DebugRun& r = *debugRun_;
        std::unique_lock lock(r.mutex);
        r.holding = true;
        r.simTurn = false;
        r.cv.notify_all();
        r.cv.wait(lock, [&] { return r.simTurn; });
        r.holding = false;
    };
    host.drive = [this] { driveDebugRun(); };
    // The threads hand over strictly, so the event log and feed see one writer at a time.
    host.event = [this](const Json& e) { emitEvent(e); };
    runtime_->debugger().setHost(std::move(host));
}

bool Engine::debugHolding() const { return debugRun_ && debugRun_->holding; }

bool Engine::debugStep(int ticks) {
    DebugRun& r = *debugRun_;
    if (r.running) {
        log::warn("wander", "the game is paused in the Wander debugger: no ticks run until it continues (wander_continue)");
        return true;
    }
    if (!runtime_->debugger().active() || runtime_->debugger().onStop) return false;  // synchronous handlers stop in place
    if (r.thread.joinable()) r.thread.join();
    {
        std::lock_guard lock(r.mutex);
        r.running = true;
        r.simTurn = true;
        r.error = nullptr;
    }
    r.thread = std::thread([this, ticks] {
        DebugRun& run = *debugRun_;
        run.simThread = std::this_thread::get_id();
        try {
            runTicks(ticks);
        } catch (const wander::DebugAbort&) {
            // play stopped while paused: the tick is abandoned (stop() restores the scene)
        } catch (...) {
            run.error = std::current_exception();
        }
        std::lock_guard lock(run.mutex);
        run.running = false;
        run.simTurn = false;
        run.simThread = std::thread::id();
        run.cv.notify_all();
    });
    r.waitForMainTurn();
    if (!r.running) {
        r.thread.join();
        if (r.error) std::rethrow_exception(std::exchange(r.error, nullptr));
    }
    return true;
}

void Engine::driveDebugRun() {
    DebugRun& r = *debugRun_;
    if (!r.running || !r.holding) return;
    {
        std::lock_guard lock(r.mutex);
        r.simTurn = true;
    }
    r.cv.notify_all();
    r.waitForMainTurn();
    if (!r.running) {
        r.thread.join();
        if (r.error) {
            try {
                std::rethrow_exception(std::exchange(r.error, nullptr));
            } catch (const std::exception& e) {
                log::error("wander", std::string("a debugged tick failed: ") + e.what());
            } catch (...) {
                log::error("wander", "a debugged tick failed");
            }
        }
    }
}

void Engine::abortDebugRun() {
    if (!debugRun_) return;
    if (debugRun_->running && debugRun_->holding) runtime_->debugger().abort();
    if (debugRun_->thread.joinable() && !debugRun_->running) debugRun_->thread.join();
}

ToolResult Engine::debugGuard(std::string_view tool) const {
    if (!debugHolding()) return {};
    const ToolDef* def = tools_.find(tool);
    if (!def || !def->mutates || str::startsWith(tool, "wander_") || tool == "sim_control") return {};
    return ToolResult::error(Error::make("paused_in_debugger",
                                         "the game is paused at a Wander breakpoint: '" + std::string(tool) +
                                             "' changes the world and must wait",
                                         "inspect with wander_stack / wander_eval, then wander_continue or wander_step; "
                                         "sim_control stop abandons the tick"));
}

}  // namespace sky
