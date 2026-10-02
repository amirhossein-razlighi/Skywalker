#pragma once
// MCP over a Unix domain socket (newline-delimited JSON-RPC, same framing as stdio).
// Lets external agents attach to a *running* editor: `skywalker mcp --attach` bridges
// an MCP client's stdio to this socket. One thread per connection; tool calls are
// executed via the Executor (which hops to the engine's main thread).

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "skywalker/agent/McpServer.h"

namespace sky {

/// RAII file descriptor.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1);
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

/// Reads newline-delimited messages from a file descriptor (handles partial reads).
class LineReader {
public:
    explicit LineReader(int fd, size_t maxLine = 64 * 1024 * 1024) : fd_(fd), maxLine_(maxLine) {}
    /// Returns false on EOF/error.
    bool next(std::string& line);

private:
    int fd_;
    size_t maxLine_;
    std::string buffer_;
};

/// Writes all bytes, retrying on partial writes / EINTR.
bool writeAll(int fd, const std::string& data);

class SocketServer {
public:
    SocketServer(const ToolRegistry& registry, McpSession::Executor executor);
    ~SocketServer();

    Status start(const std::string& path);
    void stop();
    size_t connectionCount() const { return connections_.load(); }

private:
    void acceptLoop();
    void serve(int fd);

    const ToolRegistry& registry_;
    McpSession::Executor executor_;
    std::string path_;
    UniqueFd listenFd_;
    UniqueFd wakeRead_, wakeWrite_;  // self-pipe: stop() wakes the accept loop portably
    std::atomic<bool> running_{false};
    std::atomic<size_t> connections_{0};
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<int> clientFds_;
    std::vector<std::thread> clientThreads_;
};

/// Client side used by `skywalker mcp --attach`: connects to the socket.
Result<UniqueFd> connectUnixSocket(const std::string& path);

}  // namespace sky
