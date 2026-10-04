// External tool hosts over MCP connections: the request/response plumbing (McpPeer) and the
// single-threaded stream transport used for stdio (McpStreamServer).

#include "skywalker/agent/ExternalHost.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>

#include "skywalker/agent/McpServer.h"
#include "skywalker/agent/SocketServer.h"

namespace sky {

// ---------------------------------------------------------------------------
// McpPeer
// ---------------------------------------------------------------------------

McpPeer::McpPeer(std::string id, Writer writer) : id_(std::move(id)), writer_(std::move(writer)) {}

std::string McpPeer::actor() const {
    std::lock_guard lock(mutex_);
    return actor_;
}

void McpPeer::setActor(std::string actor) {
    std::lock_guard lock(mutex_);
    actor_ = std::move(actor);
}

void McpPeer::setPoll(Poll poll) {
    std::lock_guard lock(mutex_);
    poll_ = std::move(poll);
}

bool McpPeer::send(const std::string& line) {
    if (!connected_.load()) return false;
    std::lock_guard lock(writeMutex_);
    return writer_ && writer_(line);
}

Result<Json> McpPeer::request(const std::string& method, const Json& params, std::chrono::milliseconds timeout,
                              const std::function<void()>& idle) {
    if (!connected_.load()) return Error::make("host_disconnected", "the client hosting this tool is disconnected");
    auto slot = std::make_shared<Slot>();
    std::string id;
    Poll poll;
    {
        std::lock_guard lock(mutex_);
        id = "sky-" + std::to_string(nextId_++);
        slots_[id] = slot;
        poll = poll_;
    }
    ++pending_;
    struct Retire {
        McpPeer& peer;
        const std::string& id;
        ~Retire() {
            std::lock_guard lock(peer.mutex_);
            peer.slots_.erase(id);
            --peer.pending_;
        }
    } retire{*this, id};

    Json message = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    if (!send(message.dump() + "\n")) return Error::make("host_disconnected", "could not send the request to the client");

    constexpr auto kSlice = std::chrono::milliseconds(5);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            if (!poll) cv_.wait_for(lock, kSlice, [&] { return slot->done || !connected_.load(); });
            if (slot->done) break;
        }
        if (poll) poll(kSlice);  // reads (and handles) the client's messages on this thread
        if (idle) idle();
        {
            std::lock_guard lock(mutex_);
            if (slot->done) break;
        }
        if (!connected_.load()) return Error::make("host_disconnected", "the client disconnected before answering");
        if (std::chrono::steady_clock::now() >= deadline) {
            return Error::make("timeout", "the client did not answer within " + std::to_string(timeout.count()) + " ms",
                               "raise limits.timeout_ms in the tool definition, or make the client answer faster");
        }
    }
    Json response;
    {
        std::lock_guard lock(mutex_);
        response = std::move(slot->response);
    }
    if (const Json* err = response.find("error")) {
        return Error::make("host_error", err->get("message").asString("the client reported an error"));
    }
    return response.get("result");
}

bool McpPeer::deliver(const Json& message) {
    if (!message.isObject() || message.contains("method")) return false;
    const Json& id = message.get("id");
    if (!id.isString()) return false;
    std::lock_guard lock(mutex_);
    auto it = slots_.find(id.asString());
    if (it == slots_.end()) return false;
    it->second->response = message;
    it->second->done = true;
    cv_.notify_all();
    return true;
}

void McpPeer::close() {
    connected_ = false;
    std::lock_guard lock(mutex_);
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// McpStreamServer
// ---------------------------------------------------------------------------

McpStreamServer::McpStreamServer(McpSession& session, int in, int out, std::string id)
    : session_(session), in_(in), out_(out) {
    peer_ = std::make_shared<McpPeer>(std::move(id), [this](const std::string& line) { return writeAll(out_, line); });
    peer_->setPoll([this](std::chrono::milliseconds slice) {
        std::string line;
        if (readLine(line, static_cast<int>(slice.count()))) dispatch(line);
    });
}

McpStreamServer::~McpStreamServer() { peer_->close(); }

bool McpStreamServer::readLine(std::string& line, int timeoutMs) {
    while (true) {
        size_t nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line.assign(buffer_, 0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (eof_) {
            if (buffer_.empty()) return false;
            line.swap(buffer_);
            buffer_.clear();
            return true;
        }
        if (timeoutMs >= 0) {
            pollfd p{in_, POLLIN, 0};
            int ready = ::poll(&p, 1, timeoutMs);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) return false;
        }
        char chunk[65536];
        ssize_t n = ::read(in_, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            eof_ = true;
            continue;
        }
        buffer_.append(chunk, static_cast<size_t>(n));
    }
}

void McpStreamServer::dispatch(const std::string& line) {
    if (line.empty()) return;
    if (auto parsed = Json::parse(line); parsed && peer_->deliver(*parsed)) return;  // an answer to our request
    if (auto response = session_.handle(line)) peer_->send(*response + "\n");
    if (auto note = session_.pendingNotification()) peer_->send(*note + "\n");
}

void McpStreamServer::run(const std::function<void()>& beforeMessage) {
    std::string line;
    while (readLine(line, -1)) {
        if (line.empty()) continue;
        if (beforeMessage) beforeMessage();
        dispatch(line);
    }
    peer_->close();
    session_.closeHost();
}

}  // namespace sky
