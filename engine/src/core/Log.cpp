#include "skywalker/core/Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace sky {

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

namespace log {
namespace {

struct State {
    std::mutex mutex;
    std::vector<std::pair<int, LogSink>> sinks;
    int nextHandle = 1;
    std::atomic<LogLevel> minLevel{LogLevel::Info};
    std::atomic<bool> stderrEnabled{true};
};

State& state() {
    static State s;
    return s;
}

}  // namespace

int addSink(LogSink sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    int handle = s.nextHandle++;
    s.sinks.emplace_back(handle, std::move(sink));
    return handle;
}

void removeSink(int handle) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [handle](const auto& p) { return p.first == handle; });
}

void setMinLevel(LogLevel level) { state().minLevel = level; }
LogLevel minLevel() { return state().minLevel; }
void setStderrEnabled(bool enabled) { state().stderrEnabled = enabled; }

void write(LogLevel level, std::string_view category, std::string_view message) {
    auto& s = state();
    if (level < s.minLevel.load()) return;
    LogRecord record{level, category, message};
    if (s.stderrEnabled.load()) {
        std::fprintf(stderr, "[%s] %.*s: %.*s\n", toString(level), static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
    }
    // Copy sinks so a sink may log without deadlocking.
    std::vector<LogSink> sinks;
    {
        std::lock_guard lock(s.mutex);
        for (auto& [h, sink] : s.sinks) sinks.push_back(sink);
    }
    for (auto& sink : sinks) sink(record);
}

}  // namespace log
}  // namespace sky
