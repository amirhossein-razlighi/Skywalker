#pragma once
// External tool hosts: connected clients (a Python agent layer, another process) that serve tool
// calls for the engine over their MCP connection.
//
// A client registers tools with the `skywalker/tools/register` request; the engine then forwards
// calls of those tools to it as `skywalker/tools/call` requests on the same connection and waits
// for the result. While the engine waits, the client may call back into the engine (`tools/call`
// with `_meta["skywalker/call_id"]`): those calls run with the external tool's capabilities. The
// protocol is documented in docs/CUSTOM_TOOLS.md ("External tools").

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

class McpSession;

/// A connected client that can serve tool calls.
class ExternalHost {
public:
    virtual ~ExternalHost() = default;
    /// Unique per connection ("socket-3", "stdio-1").
    virtual const std::string& id() const = 0;
    /// Who the client is ("mcp:python-agent"), for attribution.
    virtual std::string actor() const = 0;
    /// The client introduced itself (MCP initialize): "mcp:<client name>".
    virtual void setActor(std::string actor) = 0;
    virtual bool connected() const = 0;
    /// Sends a JSON-RPC request to the client and waits for its result. Thread-safe. `idle`
    /// (optional) runs every few milliseconds while waiting.
    virtual Result<Json> request(const std::string& method, const Json& params, std::chrono::milliseconds timeout,
                                 const std::function<void()>& idle) = 0;
};

/// Engine side of the host protocol (implemented by the custom tool manager). Transports call it.
struct HostHandlers {
    /// A `skywalker/tools/*` request from a client: returns the JSON-RPC result.
    std::function<Result<Json>(const std::string& method, const Json& params, const std::shared_ptr<ExternalHost>& host)> request;
    /// The client disconnected: its tools go offline.
    std::function<void(const std::shared_ptr<ExternalHost>& host)> closed;
};

/// A ExternalHost over a newline-delimited JSON-RPC connection (MCP framing). The transport hands it
/// the client's responses (deliver) and a writer; requests it sends get ids "sky-<n>".
class McpPeer final : public ExternalHost {
public:
    using Writer = std::function<bool(const std::string& line)>;
    /// Reads and dispatches incoming messages for up to the given time. Set by transports that
    /// read on the waiting thread (stdio), so a request can wait without a reader thread.
    using Poll = std::function<void(std::chrono::milliseconds)>;

    McpPeer(std::string id, Writer writer);

    const std::string& id() const override { return id_; }
    std::string actor() const override;
    void setActor(std::string actor) override;
    bool connected() const override { return connected_.load(); }
    Result<Json> request(const std::string& method, const Json& params, std::chrono::milliseconds timeout,
                         const std::function<void()>& idle) override;

    /// Writes one message line (thread-safe).
    bool send(const std::string& line);
    /// A message from the client: if it answers one of our requests, completes it and returns true.
    bool deliver(const Json& message);
    /// Requests sent to the client that are still waiting for an answer.
    size_t pending() const { return pending_.load(); }
    void setPoll(Poll poll);
    /// The connection closed: waiting requests fail, new ones fail at once.
    void close();

private:
    struct Slot {
        bool done = false;
        Json response;
    };

    std::string id_;
    Writer writer_;
    Poll poll_;
    mutable std::mutex mutex_;  // actor_, slots_, poll_
    std::mutex writeMutex_;
    std::condition_variable cv_;
    std::string actor_ = "mcp:client";
    std::map<std::string, std::shared_ptr<Slot>> slots_;
    uint64_t nextId_ = 1;
    std::atomic<bool> connected_{true};
    std::atomic<size_t> pending_{0};
};

/// Serves one MCP session over a pair of file descriptors on the calling thread (the stdio transport
/// of `skywalker mcp`). Supports external tools: while the engine waits for a client's tool, incoming
/// messages are read and handled re-entrantly on the same thread.
class McpStreamServer {
public:
    McpStreamServer(McpSession& session, int in, int out, std::string id = "stdio");
    ~McpStreamServer();
    McpStreamServer(const McpStreamServer&) = delete;
    McpStreamServer& operator=(const McpStreamServer&) = delete;

    const std::shared_ptr<McpPeer>& peer() const { return peer_; }
    /// Handles messages until EOF. `beforeMessage` runs before each one (e.g. pumping engine jobs).
    void run(const std::function<void()>& beforeMessage = {});

private:
    bool readLine(std::string& line, int timeoutMs);
    void dispatch(const std::string& line);

    McpSession& session_;
    int in_;
    int out_;
    std::string buffer_;
    bool eof_ = false;
    std::shared_ptr<McpPeer> peer_;
};

}  // namespace sky
