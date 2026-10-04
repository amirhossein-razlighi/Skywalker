#pragma once
// Engine internals shared by Engine.cpp and EngineDebug.cpp: the state of a debugged run (ticks on a
// thread that hands control back to the main thread when the Wander debugger stops).

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include "skywalker/engine/Engine.h"

namespace sky {

struct Engine::DebugRun {
    std::thread thread;
    std::thread::id simThread;  // the thread the VM may pause on
    std::mutex mutex;
    std::condition_variable cv;
    bool simTurn = false;   // the simulation thread may run (the main thread waits)
    bool running = false;   // debugged ticks are in progress (the thread is alive)
    bool holding = false;   // the VM is paused mid-tick
    std::exception_ptr error;

    /// Main thread: waits until the simulation thread stops at a breakpoint or finishes.
    void waitForMainTurn() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return !simTurn; });
    }
};

}  // namespace sky
