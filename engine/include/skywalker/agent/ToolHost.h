#pragma once
// ToolHost: tools served by an external process (the Python agent layer, a script, another
// engine) and offered to every other client as ordinary engine tools named `py_<name>`.
//
// Flow (all over normal MCP connections, no extra protocol):
//   host process   tool_host_register {tools}         -> host id; `py_<name>` tools appear in tools/list
//   any client     py_<name> {args}                   -> queued for the host; the caller's connection waits
//   host process   tool_host_poll {host, wait_ms}     -> the queued calls (long poll on its own connection)
//   host process   tool_host_reply {host, call, ...}  -> the caller gets that result
//
// A host that stops polling for `ttl` seconds is considered gone: its tools are removed and its
// pending calls fail. Waiting happens on agent connection threads; a call made on the engine's
// main thread (headless stdio, the CLI runner) cannot wait for another connection, so it fails
// fast with a hint instead of deadlocking. Thread-safe.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

class ToolRegistry;
struct ToolResult;

class ToolHost : public std::enable_shared_from_this<ToolHost> {
public:
    struct Call {
        uint64_t id = 0;
        std::string host;
        std::string tool;   // the public name, e.g. "py_memory_recall"
        Json args;
        std::string actor;  // who called it
        double queuedAt = 0;
        Json toJson() const;
    };
    struct ToolSpec {
        std::string name;  // with or without the py_ prefix
        std::string title;
        std::string description;
        Json inputSchema;
        bool mutates = false;
    };

    /// `registry` must outlive the host; `mainThread` is the engine thread (calls made on it cannot wait).
    explicit ToolHost(ToolRegistry& registry, std::thread::id mainThread = std::this_thread::get_id());
    ~ToolHost();
    ToolHost(const ToolHost&) = delete;
    ToolHost& operator=(const ToolHost&) = delete;

    static constexpr const char* kPrefix = "py_";
    static constexpr double kDefaultTtl = 60;
    static constexpr auto kCallTimeout = std::chrono::seconds(120);
    /// "memory_recall" -> "py_memory_recall" (names already starting with py_ are kept).
    static std::string publicName(const std::string& name);
    /// Valid for every model provider: [A-Za-z0-9_-]{1,64} including the prefix.
    static Status validateName(const std::string& publicName);

    /// Registers tools for `host` (empty = a new host) and returns the host id. Re-registering a host
    /// replaces its tool set. A name owned by another live host is refused unless `replace`.
    Result<std::string> registerTools(const std::string& host, const std::string& owner, const std::string& label,
                                      const std::vector<ToolSpec>& tools, double ttlSeconds, bool replace);
    /// Removes a host and its tools; its pending calls fail with `reason`.
    bool unregister(const std::string& host, const std::string& reason);
    /// Removes hosts that have not polled within their ttl.
    void expireStale();

    // --- the calling side ----------------------------------------------------------
    /// Queues a call for the tool's host (main thread, from the py_* handler).
    Result<uint64_t> enqueue(const std::string& tool, const Json& args, const std::string& actor);
    /// Waits for the reply (an MCP CallToolResult) and forgets the call. nullopt on timeout.
    std::optional<Json> waitReply(uint64_t call, std::chrono::milliseconds timeout);
    /// Fails a call (timeout, cancellation); a waiter gets the error.
    void cancel(uint64_t call, const std::string& code, const std::string& message, const std::string& hint = {});

    // --- the host side ---------------------------------------------------------------
    /// Calls queued for `host` (marks them delivered). Waits up to `timeout` when none are queued.
    Result<std::vector<Call>> poll(const std::string& host, size_t max, std::chrono::milliseconds timeout,
                                   const std::atomic<bool>* cancel = nullptr);
    Status reply(const std::string& host, uint64_t call, Json mcpResult);
    /// Shutdown: every waiter returns now and pending calls fail.
    void shutdown();
    void wakeAll();

    bool onMainThread() const { return std::this_thread::get_id() == mainThread_; }
    /// Hosts, their tools, pending and in-flight calls (tool_host_list).
    Json info() const;
    /// {hosts, tools, queued, in_flight, served, failed}
    Json stats() const;

private:
    struct Host {
        std::string id, owner, label;
        std::vector<std::string> tools;  // public names
        double ttl = kDefaultTtl;
        double lastSeen = 0;
        uint64_t served = 0;
    };
    struct Pending {
        Call call;
        bool delivered = false;
        bool done = false;
        Json result;
    };
    static double now();
    void expireStaleLocked();
    void removeHostLocked(const std::string& host, const std::string& reason);
    void failLocked(Pending& p, const std::string& code, const std::string& message, const std::string& hint);

    ToolRegistry& registry_;
    std::thread::id mainThread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Host> hosts_;
    std::map<std::string, std::string> toolOwner_;  // public tool name -> host id
    std::map<uint64_t, Pending> pending_;
    uint64_t nextCall_ = 1;
    int nextHost_ = 1;
    uint64_t served_ = 0, failed_ = 0;
    bool shuttingDown_ = false;
};

/// An MCP CallToolResult (as JSON) back to a ToolResult (text, images, structuredContent, isError).
ToolResult toolResultFromMcp(const Json& mcp);

}  // namespace sky
