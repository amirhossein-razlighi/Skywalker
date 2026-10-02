#pragma once
// Thread-safe logging with pluggable sinks. The editor console, the MCP server
// (as `notifications/message`) and stderr are all just sinks.

#include <functional>
#include <string>
#include <string_view>

namespace sky {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

const char* toString(LogLevel level);

struct LogRecord {
    LogLevel level;
    std::string_view category;
    std::string_view message;
};

using LogSink = std::function<void(const LogRecord&)>;

namespace log {

/// Adds a sink; returns a handle that can be passed to removeSink.
int addSink(LogSink sink);
void removeSink(int handle);
void setMinLevel(LogLevel level);
LogLevel minLevel();
/// Enables/disables the default stderr sink (disabled automatically in MCP stdio mode,
/// where stdout/stderr hygiene matters).
void setStderrEnabled(bool enabled);

void write(LogLevel level, std::string_view category, std::string_view message);

inline void trace(std::string_view cat, std::string_view msg) { write(LogLevel::Trace, cat, msg); }
inline void debug(std::string_view cat, std::string_view msg) { write(LogLevel::Debug, cat, msg); }
inline void info(std::string_view cat, std::string_view msg) { write(LogLevel::Info, cat, msg); }
inline void warn(std::string_view cat, std::string_view msg) { write(LogLevel::Warn, cat, msg); }
inline void error(std::string_view cat, std::string_view msg) { write(LogLevel::Error, cat, msg); }

}  // namespace log
}  // namespace sky
