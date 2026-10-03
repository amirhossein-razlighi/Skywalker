#include "Process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "skywalker/core/Strings.h"

extern char** environ;

namespace sky::native {

namespace fs = std::filesystem;

std::string whichExecutable(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : "";
    const char* path = std::getenv("PATH");
    std::string paths = path ? path : "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin";
    for (const auto& dir : str::split(paths, ':')) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

Result<ProcessResult> runProcess(const std::vector<std::string>& argv, const std::string& cwd, int timeoutSeconds) {
    if (argv.empty()) return Error::make("invalid_argument", "no program to run");
    std::string exe = whichExecutable(argv[0]);
    if (exe.empty()) return Error::make("not_found", "cannot find '" + argv[0] + "'", "install it or add it to PATH");

    int pipefd[2];
    if (pipe(pipefd) != 0) return Error::make("io_error", std::string("pipe failed: ") + std::strerror(errno));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!cwd.empty()) {
#if defined(__APPLE__)
        posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
#endif
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return Error::make("spawn_failed", "cannot start " + exe + ": " + std::strerror(rc));
    }

    ProcessResult result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    char buf[4096];
    while (true) {
        pollfd p{pipefd[0], POLLIN, 0};
        int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (ms <= 0) {
            result.timedOut = true;
            kill(pid, SIGKILL);
            break;
        }
        int pr = poll(&p, 1, std::min(ms, 1000));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (result.output.size() < (8u << 20)) result.output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

Json CompilerDiagnostic::toJson() const {
    Json j = Json::object({{"file", file}, {"line", line}, {"column", column}, {"severity", severity}, {"message", message}});
    return j;
}

std::vector<CompilerDiagnostic> parseCompilerOutput(const std::string& output) {
    // file:line:col: severity: message
    std::vector<CompilerDiagnostic> out;
    for (const auto& line : str::split(output, '\n')) {
        for (const char* sev : {": error: ", ": warning: ", ": note: ", ": fatal error: "}) {
            size_t p = line.find(sev);
            if (p == std::string::npos) continue;
            std::string head = line.substr(0, p);
            CompilerDiagnostic d;
            d.severity = std::string(sev).substr(2, std::strlen(sev) - 4);
            if (d.severity == "fatal error") d.severity = "error";
            d.message = line.substr(p + std::strlen(sev));
            // head = file:line:col (file may contain ':' only on odd systems; split from the right)
            size_t c2 = head.rfind(':');
            size_t c1 = c2 == std::string::npos ? std::string::npos : head.rfind(':', c2 - 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                d.file = head.substr(0, c1);
                d.line = std::atoi(head.substr(c1 + 1, c2 - c1 - 1).c_str());
                d.column = std::atoi(head.substr(c2 + 1).c_str());
            } else {
                d.file = head;
            }
            out.push_back(std::move(d));
            break;
        }
    }
    return out;
}

}  // namespace sky::native
