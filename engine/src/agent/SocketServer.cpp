#include "skywalker/agent/SocketServer.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "skywalker/agent/ExternalHost.h"
#include "skywalker/core/Log.h"

namespace sky {

void UniqueFd::reset(int fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

bool LineReader::next(std::string& line) {
    while (true) {
        size_t nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line.assign(buffer_, 0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (buffer_.size() > maxLine_) return false;  // refuse unbounded messages
        char chunk[65536];
        ssize_t n = ::read(fd_, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!buffer_.empty()) {  // final line without newline
                line.swap(buffer_);
                buffer_.clear();
                return true;
            }
            return false;
        }
        buffer_.append(chunk, static_cast<size_t>(n));
    }
}

bool writeAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK) n = ::write(fd, data.data() + off, data.size() - off);
#else
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

namespace {

bool fillAddress(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

}  // namespace

Result<UniqueFd> connectUnixSocket(const std::string& path) {
    sockaddr_un addr;
    if (!fillAddress(path, addr)) return Error::make("invalid_path", "socket path too long: " + path);
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) return Error::make("io_error", std::strerror(errno));
    noSigpipe(fd.get());
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return Error::make("connect_failed", "cannot connect to " + path + ": " + std::strerror(errno),
                           "is the Skywalker editor running with the agent server enabled?");
    }
    return fd;
}

/// One client connection: its session, the peer for external tools and the work queue.
struct SocketServer::Connection {
    int fd = -1;
    std::unique_ptr<McpSession> session;
    std::shared_ptr<McpPeer> peer;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> queue;  // requests, handled in order by the worker
    bool notify = false;            // the tool list changed
    bool closing = false;
    struct Helper {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Helper> helpers;  // callbacks served while the engine waits for this client
};

SocketServer::SocketServer(const ToolRegistry& registry, McpSession::ContextExecutor executor, HostHandlers hosts)
    : registry_(registry), executor_(std::move(executor)), hosts_(std::move(hosts)) {}

SocketServer::SocketServer(const ToolRegistry& registry, McpSession::Executor executor)
    : registry_(registry),
      executor_([fn = std::move(executor)](const std::string& tool, const Json& args, const ToolContext& ctx) {
          return fn(tool, args, ctx.actor);
      }) {}

SocketServer::~SocketServer() { stop(); }

Status SocketServer::start(const std::string& path) {
    sockaddr_un addr;
    if (!fillAddress(path, addr)) return Error::make("invalid_path", "socket path too long: " + path);
    ::unlink(path.c_str());  // stale socket from a previous run
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) return Error::make("io_error", std::strerror(errno));
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return Error::make("bind_failed", "cannot bind " + path + ": " + std::strerror(errno));
    }
    ::chmod(path.c_str(), 0600);  // only the current user may drive the editor
    if (::listen(fd.get(), 8) != 0) return Error::make("listen_failed", std::strerror(errno));
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) return Error::make("io_error", std::strerror(errno));
    wakeRead_.reset(pipeFds[0]);
    wakeWrite_.reset(pipeFds[1]);
    path_ = path;
    listenFd_ = std::move(fd);
    running_ = true;
    listener_ = registry_.addListener([this] {
        std::lock_guard lock(clientsMutex_);
        for (const auto& c : live_) {
            std::lock_guard cl(c->mutex);
            c->notify = true;
            c->cv.notify_all();
        }
    });
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return {};
}

void SocketServer::stop() {
    if (!running_.exchange(false)) return;
    registry_.removeListener(listener_);
    // Shutting down the listening socket wakes accept(); shutting down clients wakes read().
    // Wake the accept loop through the self-pipe (shutdown() does not interrupt accept()
    // on macOS), join it, and only then close the descriptors — closing first would race
    // with the accept thread and could hand it a recycled descriptor.
    const char byte = 1;
    (void)!::write(wakeWrite_.get(), &byte, 1);
    if (acceptThread_.joinable()) acceptThread_.join();
    listenFd_.reset();
    wakeRead_.reset();
    wakeWrite_.reset();
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(clientsMutex_);
        for (int c : clientFds_) ::shutdown(c, SHUT_RDWR);
        threads.swap(clientThreads_);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    ::unlink(path_.c_str());
}

void SocketServer::acceptLoop() {
    while (running_) {
        pollfd fds[2] = {{listenFd_.get(), POLLIN, 0}, {wakeRead_.get(), POLLIN, 0}};
        int ready = ::poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents != 0 || !running_) break;  // stop requested
        if ((fds[0].revents & POLLIN) == 0) continue;
        int c = ::accept(listenFd_.get(), nullptr, nullptr);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        noSigpipe(c);
        std::lock_guard lock(clientsMutex_);
        if (!running_) {
            ::close(c);
            break;
        }
        clientFds_.push_back(c);
        clientThreads_.emplace_back([this, c] { serve(c); });
    }
}

void SocketServer::work(Connection& c) {
    auto reply = [&c](const std::optional<std::string>& message) {
        if (message) c.peer->send(*message + "\n");
    };
    while (true) {
        std::string line;
        bool notify = false;
        {
            std::unique_lock lock(c.mutex);
            c.cv.wait(lock, [&] { return c.closing || c.notify || !c.queue.empty(); });
            if (!c.queue.empty()) {
                line = std::move(c.queue.front());
                c.queue.pop_front();
            } else if (c.notify) {
                notify = true;
            } else {
                return;  // closing and nothing left
            }
            c.notify = false;
        }
        if (!line.empty()) reply(c.session->handle(line));
        if (notify || !line.empty()) reply(c.session->pendingNotification());
    }
}

void SocketServer::serve(int raw) {
    UniqueFd fd(raw);
    ++connections_;
    auto c = std::make_shared<Connection>();
    c->fd = raw;
    c->peer = std::make_shared<McpPeer>("socket-" + std::to_string(nextConnection_++),
                                        [raw](const std::string& line) { return writeAll(raw, line); });
    c->session = std::make_unique<McpSession>(registry_, executor_);
    if (hosts_.request) c->session->setHost(c->peer, hosts_);
    {
        std::lock_guard lock(clientsMutex_);
        live_.push_back(c);
    }
    std::thread worker([this, c] { work(*c); });
    LineReader reader(fd.get());
    std::string line;
    while (running_ && reader.next(line)) {
        if (line.empty()) continue;
        auto parsed = Json::parse(line);
        if (parsed && c->peer->deliver(*parsed)) continue;  // the answer to one of our requests
        if (c->peer->pending() > 0) {
            // The engine is waiting for this client (one of its tools is running): serve the
            // request now, even if the worker is busy, so the tool can call back into the engine.
            std::lock_guard lock(c->mutex);
            for (auto& h : c->helpers) {  // reap finished helpers
                if (h.done->load() && h.thread.joinable()) h.thread.join();
            }
            std::erase_if(c->helpers, [](const Connection::Helper& h) { return !h.thread.joinable(); });
            auto done = std::make_shared<std::atomic<bool>>(false);
            c->helpers.push_back({std::thread([c, line, done] {
                                      if (auto response = c->session->handle(line)) c->peer->send(*response + "\n");
                                      done->store(true);
                                  }),
                                  done});
            continue;
        }
        std::lock_guard lock(c->mutex);
        c->queue.push_back(std::move(line));
        c->cv.notify_all();
    }
    c->peer->close();  // fail calls still waiting for this client
    {
        std::lock_guard lock(c->mutex);
        c->closing = true;
        c->cv.notify_all();
    }
    worker.join();
    std::vector<Connection::Helper> helpers;
    {
        std::lock_guard lock(c->mutex);
        helpers.swap(c->helpers);
    }
    for (auto& h : helpers) {
        if (h.thread.joinable()) h.thread.join();
    }
    c->session->closeHost();
    --connections_;
    std::lock_guard lock(clientsMutex_);
    std::erase(live_, c);
    std::erase(clientFds_, raw);
    // fd closes via RAII after the lock is released (declared before the guard).
}

}  // namespace sky
