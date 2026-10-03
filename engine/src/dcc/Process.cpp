#include "skywalker/dcc/Process.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace sky::dcc {

// ---------------------------------------------------------------------------
// BoundedCapture
// ---------------------------------------------------------------------------

void BoundedCapture::append(const char* data, size_t n) {
    total_ += n;
    size_t used = 0;
    if (head_.size() < headCap_) {
        used = std::min(n, headCap_ - head_.size());
        head_.append(data, used);
    }
    if (used == n) return;
    tail_.append(data + used, n - used);
    // Trim lazily so appends stay amortized O(1): only when the buffer doubles the cap.
    if (tail_.size() > tailCap_ * 2) tail_.erase(0, tail_.size() - tailCap_);
}

std::string BoundedCapture::str() const {
    std::string tail = tail_.size() > tailCap_ ? tail_.substr(tail_.size() - tailCap_) : tail_;
    uint64_t dropped = total_ - head_.size() - tail.size();
    if (dropped == 0) return head_ + tail;
    return head_ + "\n... [" + std::to_string(dropped) + " bytes omitted] ...\n" + tail;
}

#if defined(_WIN32)

ProcessResult runProcess(const ProcessSpec&) {
    ProcessResult r;
    r.spawnError = "process execution is not implemented on Windows yet";
    return r;
}

Result<long> spawnDetached(const ProcessSpec&, const std::string&) {
    return Error::make("unsupported", "starting design-app sessions is not implemented on Windows yet");
}
bool processAlive(long) { return false; }
void terminateProcess(long, bool) {}

#else

namespace {

using Clock = std::chrono::steady_clock;

/// Closes a descriptor on scope exit.
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd& operator=(Fd&& o) noexcept {
        reset();
        fd = o.fd;
        o.fd = -1;
        return *this;
    }
    ~Fd() { reset(); }
    void reset() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

bool makePipe(Fd& readEnd, Fd& writeEnd) {
    int p[2];
    if (::pipe(p) != 0) return false;
    // Close-on-exec so unrelated children spawned concurrently never inherit our pipes; the
    // spawn file actions dup2 the write end onto stdout/stderr (which clears the flag).
    for (int i : {0, 1}) {
        ::fcntl(p[i], F_SETFD, ::fcntl(p[i], F_GETFD) | FD_CLOEXEC);
    }
    readEnd = Fd(p[0]);
    writeEnd = Fd(p[1]);
    ::fcntl(p[0], F_SETFL, ::fcntl(p[0], F_GETFL) | O_NONBLOCK);
    return true;
}

std::vector<std::string> buildEnvironment(const ProcessSpec& spec) {
    std::map<std::string, std::string> merged;
    std::vector<std::string> order;
    auto put = [&](std::string k, std::string v) {
        if (!merged.count(k)) order.push_back(k);
        merged[k] = std::move(v);
    };
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (!eq) continue;
        put(std::string(*e, static_cast<size_t>(eq - *e)), eq + 1);
    }
    for (const auto& [k, v] : spec.env) put(k, v);
    std::vector<std::string> out;
    out.reserve(order.size());
    for (const auto& k : order) out.push_back(k + "=" + merged[k]);
    return out;
}

/// The child leads its own process group (POSIX_SPAWN_SETPGROUP), so this reaches any helpers
/// it started. Only call while the child has not been reaped yet.
void killGroup(pid_t pid, int sig) {
    if (::kill(-pid, sig) != 0) ::kill(pid, sig);
}

}  // namespace

ProcessResult runProcess(const ProcessSpec& spec) {
    ProcessResult res;
    const auto start = Clock::now();
    auto finish = [&](ProcessResult& r) -> ProcessResult& {
        r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        return r;
    };

    if (spec.executable.empty()) {
        res.spawnError = "no executable given";
        return finish(res);
    }

    Fd outR, outW, errR, errW;
    if (!makePipe(outR, outW) || !makePipe(errR, errW)) {
        res.spawnError = std::string("cannot create pipes: ") + std::strerror(errno);
        return finish(res);
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    struct Cleanup {
        posix_spawn_file_actions_t* a;
        posix_spawnattr_t* t;
        ~Cleanup() {
            posix_spawn_file_actions_destroy(a);
            posix_spawnattr_destroy(t);
        }
    } cleanup{&actions, &attr};

    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outW.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errW.fd, STDERR_FILENO);
    if (!spec.workingDir.empty()) {
        if (int rc = posix_spawn_file_actions_addchdir_np(&actions, spec.workingDir.c_str()); rc != 0) {
            res.spawnError = std::string("cannot use working directory: ") + std::strerror(rc);
            return finish(res);
        }
    }

    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> argv;
    std::vector<std::string> argStorage;
    argStorage.reserve(spec.args.size() + 1);
    argStorage.push_back(spec.executable);
    for (const auto& a : spec.args) argStorage.push_back(a);
    for (auto& s : argStorage) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStorage = buildEnvironment(spec);
    std::vector<char*> envp;
    for (auto& s : envStorage) envp.push_back(s.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    // A bare name is looked up in PATH (posix_spawnp); anything with a slash is used as is.
    bool hasSlash = spec.executable.find('/') != std::string::npos;
    int rc = hasSlash ? posix_spawn(&pid, spec.executable.c_str(), &actions, &attr, argv.data(), envp.data())
                      : posix_spawnp(&pid, spec.executable.c_str(), &actions, &attr, argv.data(), envp.data());
    if (rc != 0) {
        res.spawnError = "cannot start '" + spec.executable + "': " + std::strerror(rc);
        return finish(res);
    }
    res.spawned = true;
    outW.reset();  // the child owns the write ends now
    errW.reset();

    BoundedCapture outCap(spec.maxOutputBytes), errCap(spec.maxOutputBytes);
    bool outOpen = true, errOpen = true;
    bool exited = false;
    int status = 0;
    bool terminating = false;
    Clock::time_point termSent{};
    Clock::time_point exitedAt{};
    const auto deadline = start + spec.timeout;

    auto drain = [&](Fd& fd, BoundedCapture& cap, bool& open) {
        char buf[16384];
        while (open) {
            ssize_t n = ::read(fd.fd, buf, sizeof(buf));
            if (n > 0) {
                cap.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                open = false;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) open = false;
                break;
            }
        }
    };

    while (true) {
        pollfd fds[2];
        nfds_t nfds = 0;
        if (outOpen) fds[nfds++] = {outR.fd, POLLIN, 0};
        if (errOpen) fds[nfds++] = {errR.fd, POLLIN, 0};
        if (nfds > 0) {
            ::poll(fds, nfds, 25);
        } else if (!exited) {
            ::usleep(10000);
        }
        if (outOpen) drain(outR, outCap, outOpen);
        if (errOpen) drain(errR, errCap, errOpen);

        if (!exited) {
            // WNOWAIT leaves the zombie in place: the process group id stays valid (and cannot
            // be recycled) until we reap it below.
            siginfo_t info{};
            int w = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
            if ((w == 0 && info.si_pid == pid) || (w < 0 && errno != EINTR)) {
                exited = true;
                exitedAt = Clock::now();
            }
        }
        if (exited) {
            // Grandchildren may keep the pipes open after the main process is gone; do not
            // wait on them forever.
            bool drained = !outOpen && !errOpen;
            if (drained || Clock::now() - exitedAt > std::chrono::milliseconds(300)) break;
            continue;
        }

        const auto now = Clock::now();
        if (!terminating) {
            if (spec.cancel && spec.cancel->cancelled()) {
                res.cancelled = true;
                terminating = true;
            } else if (now >= deadline) {
                res.timedOut = true;
                terminating = true;
            }
            if (terminating) {
                killGroup(pid, SIGTERM);
                termSent = now;
            }
        } else if (now - termSent >= spec.killGrace) {
            killGroup(pid, SIGKILL);
            termSent = now + std::chrono::hours(1);  // only once
        }
    }

    // Whatever is left in the process group (helpers that outlived the main process) is
    // stopped too. The leader is still an unreaped zombie here, so the group id is ours.
    killGroup(pid, SIGKILL);
    pid_t reaped;
    while ((reaped = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }

    if (reaped != pid) {
        res.spawnError = "lost track of the child process";
    } else if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.signal = WTERMSIG(status);
    }
    res.out = outCap.str();
    res.err = errCap.str();
    res.outBytes = outCap.total();
    res.errBytes = errCap.total();
    res.outTruncated = outCap.total() > outCap.keptBytes();
    res.errTruncated = errCap.total() > errCap.keptBytes();
    return finish(res);
}

Result<long> spawnDetached(const ProcessSpec& spec, const std::string& logPath) {
    if (spec.executable.empty()) return Error::make("invalid_arguments", "no executable given");
    Fd log(::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
    if (log.fd < 0) return Error::make("io_error", "cannot open " + logPath + ": " + std::strerror(errno));

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    struct Cleanup {
        posix_spawn_file_actions_t* a;
        posix_spawnattr_t* t;
        ~Cleanup() {
            posix_spawn_file_actions_destroy(a);
            posix_spawnattr_destroy(t);
        }
    } cleanup{&actions, &attr};
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, log.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, log.fd, STDERR_FILENO);
    if (!spec.workingDir.empty()) posix_spawn_file_actions_addchdir_np(&actions, spec.workingDir.c_str());
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;  // survive the terminal/editor that started us
#else
    posix_spawnattr_setpgroup(&attr, 0);
    flags |= POSIX_SPAWN_SETPGROUP;
#endif
    posix_spawnattr_setflags(&attr, flags);

    std::vector<std::string> argStorage{spec.executable};
    for (const auto& a : spec.args) argStorage.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argStorage) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStorage = buildEnvironment(spec);
    std::vector<char*> envp;
    for (auto& s : envStorage) envp.push_back(s.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    bool hasSlash = spec.executable.find('/') != std::string::npos;
    int rc = hasSlash ? posix_spawn(&pid, spec.executable.c_str(), &actions, &attr, argv.data(), envp.data())
                      : posix_spawnp(&pid, spec.executable.c_str(), &actions, &attr, argv.data(), envp.data());
    if (rc != 0) return Error::make("spawn_failed", "cannot start '" + spec.executable + "': " + std::strerror(rc));
    return static_cast<long>(pid);
}

bool processAlive(long pid) {
    if (pid <= 1) return false;
    int status = 0;
    if (::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == static_cast<pid_t>(pid)) return false;  // our child, now reaped
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return errno == EPERM;
    return true;
}

void terminateProcess(long pid, bool force) {
    if (pid > 1) ::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM);
}

#endif  // !_WIN32

}  // namespace sky::dcc
