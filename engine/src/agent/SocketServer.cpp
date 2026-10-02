#include "skywalker/agent/SocketServer.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

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

SocketServer::SocketServer(const ToolRegistry& registry, McpSession::Executor executor)
    : registry_(registry), executor_(std::move(executor)) {}

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
    path_ = path;
    listenFd_ = std::move(fd);
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return {};
}

void SocketServer::stop() {
    if (!running_.exchange(false)) return;
    // Shutting down the listening socket wakes accept(); shutting down clients wakes read().
    ::shutdown(listenFd_.get(), SHUT_RDWR);
    listenFd_.reset();
    if (acceptThread_.joinable()) acceptThread_.join();
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

void SocketServer::serve(int raw) {
    UniqueFd fd(raw);
    ++connections_;
    McpSession session(registry_, executor_);
    LineReader reader(fd.get());
    std::string line;
    while (running_ && reader.next(line)) {
        if (line.empty()) continue;
        if (auto response = session.handle(line)) {
            if (!writeAll(fd.get(), *response + "\n")) break;
        }
    }
    --connections_;
    std::lock_guard lock(clientsMutex_);
    std::erase(clientFds_, raw);
    // fd closes via RAII after the lock is released (declared before the guard).
}

}  // namespace sky
