#include "skywalker/dcc/Manager.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"

namespace sky::dcc {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Status writeFile(const fs::path& path, const std::string& content, mode_t mode = 0644) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Create with the final permissions: session files hold a token.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return Error::make("io_error", "cannot write " + path.string() + ": " + std::strerror(errno));
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            return Error::make("io_error", "cannot write " + path.string());
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);
    return {};
}

std::string randomHex(size_t bytes) {
    std::random_device rd;
    std::string out;
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        unsigned v = rd() & 0xFF;
        out += digits[v >> 4];
        out += digits[v & 15];
    }
    return out;
}

uint64_t fnv1a(uint64_t h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

/// path -> (mtime, size) for every regular file below `dir`.
using FileMap = std::map<std::string, std::pair<int64_t, uint64_t>>;
FileMap snapshot(const std::string& dir) {
    FileMap out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        auto t = fs::last_write_time(it->path(), e2);
        out[it->path().string()] = {static_cast<int64_t>(t.time_since_epoch().count()), static_cast<uint64_t>(it->file_size(e2))};
    }
    return out;
}

const char* kBootstrapStub =
    "import os, sys\n"
    "sys.path.insert(0, os.environ[\"SKY_LIB\"])\n"
    "import skywalker_dcc._bootstrap as _sky_bootstrap\n"
    "_sky_bootstrap.main()\n";

}  // namespace

// ---------------------------------------------------------------------------
// Log helpers
// ---------------------------------------------------------------------------

std::string tidyLog(const std::string& text, size_t maxChars) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        // Start-up and exporter chatter that never helps an agent.
        if ((str::startsWith(line, "Blender ") && line.find("(hash") != std::string::npos) ||
            str::startsWith(line, "Blender quit") || str::startsWith(line, "Read prefs:") ||
            line.find("Device with name") != std::string::npos || str::startsWith(line, "Color management:") ||
            line.find("| INFO: ") != std::string::npos) {
            continue;
        }
        out += line;
        out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (out.size() > maxChars) out = "...[earlier output trimmed]\n" + out.substr(out.size() - maxChars);
    return out;
}

std::string lastPythonError(const std::string& text) {
    size_t tb = text.rfind("Traceback (most recent call last):");
    if (tb == std::string::npos) return "";
    std::istringstream in(text.substr(tb));
    std::string line, last;
    std::getline(in, line);  // the Traceback line itself
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') continue;  // frames and source lines
        last = line;
        if (line.find("Error") != std::string::npos || line.find("Exception") != std::string::npos ||
            line.find("Exit") != std::string::npos) {
            break;
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
// Machine & detection
// ---------------------------------------------------------------------------

Machine Machine::real() {
    Machine e;
    e.host = Host::real();
    std::string override = e.host.getenv("SKY_DCC_STATE");
    e.stateDir = !override.empty() ? override : (fs::path(e.host.home.empty() ? "/tmp" : e.host.home) / ".skywalker" / "dcc").string();
    return e;
}

Manager::Manager(Machine env) : machine_(std::move(env)) {}

Manager::~Manager() {
    std::vector<long> pids;
    {
        std::lock_guard lock(mutex_);
        for (auto& j : jobs_) j.cancel->cancel();
        pids = startedHeadless_;
    }
    // Headless sessions we launched die with us; a window the user opened stays.
    for (long pid : pids) terminateProcess(pid);
}

std::vector<AppInfo> Manager::apps(bool refresh) {
    std::lock_guard lock(mutex_);
    if (detected_ && !refresh) return apps_;
    std::map<AppId, std::string> configured;
    if (auto cfg = Json::parse(readFile((fs::path(machine_.stateDir) / "paths.json").string()))) {
        for (AppId id : allApps()) {
            const std::string& p = cfg->get(toString(id)).asString();
            if (!p.empty()) configured[id] = p;
        }
    }
    apps_ = detectApps(machine_.host, configured, [](const AppInfo& app) {
        const Adapter& ad = adapterFor(app.id);
        std::vector<std::string> va = ad.versionArgs();
        if (va.empty()) return std::string();
        ProcessSpec spec;
        spec.executable = app.executable;
        spec.args = va;
        spec.timeout = std::chrono::seconds(30);
        spec.maxOutputBytes = 8192;
        ProcessResult r = runProcess(spec);
        return r.ok() ? ad.parseVersion(r.out) : std::string();
    });
    detected_ = true;
    return apps_;
}

Result<AppInfo> Manager::pick(const std::string& name, const std::string& capability) {
    std::vector<AppInfo> found = apps();
    if (!name.empty()) {
        auto id = appFromString(name);
        if (!id) {
            return Error::make("unknown_app", "unknown design app '" + name + "'", "use one of: blender, maya, houdini, 3dsmax");
        }
        for (const auto& a : found) {
            if (a.id != *id) continue;
            if (!capability.empty() && !a.has(capability)) {
                return Error::make("unsupported", std::string(a.name) + " does not support '" + capability + "'",
                                   "this works with Blender; call dcc_list to see capabilities");
            }
            return a;
        }
        const Adapter& ad = adapterFor(*id);
        return Error::make("app_not_found", ad.displayName() + " was not found on this machine",
                           "install it, put it on PATH, or set " + ad.envVar() + " to its executable (call dcc_list)");
    }
    for (const auto& a : found) {
        if (capability.empty() || a.has(capability)) return a;
    }
    std::string wanted = capability.empty() ? "any design app" : "an app with '" + capability + "'";
    return Error::make("app_not_found", "no design app found (needed " + wanted + ")",
                       "install Blender (free, blender.org), put it on PATH, or set SKY_BLENDER to its executable; then call dcc_list");
}

// ---------------------------------------------------------------------------
// Runtime files
// ---------------------------------------------------------------------------

Result<std::string> Manager::ensureRuntime() {
    {
        std::lock_guard lock(mutex_);
        if (!runtimeDir_.empty() && fs::exists(fs::path(runtimeDir_) / ".complete")) return runtimeDir_;
    }
    uint64_t h = 1469598103934665603ull;
    for (const auto& f : embeddedFiles()) {
        h = fnv1a(h, f.path);
        h = fnv1a(h, f.content);
    }
    fs::path dir = fs::path(machine_.stateDir) / "runtime" / hex64(h);
    if (!fs::exists(dir / ".complete")) {
        fs::path tmp = fs::path(machine_.stateDir) / "runtime" / (hex64(h) + ".tmp-" + randomHex(4));
        std::error_code ec;
        fs::create_directories(tmp, ec);
        for (const auto& f : embeddedFiles()) {
            std::string rel = f.path;
            // "lib/skywalker_dcc/x.py" -> skywalker_dcc/x.py ; "addon/skywalker_bridge/x.py" -> skywalker_bridge/x.py
            rel = rel.substr(rel.find('/') + 1);
            if (Status s = writeFile(tmp / rel, f.content); !s) return s.error();
        }
        if (Status s = writeFile(tmp / ".complete", "ok\n"); !s) return s.error();
        fs::rename(tmp, dir, ec);
        if (ec) {  // lost a race with another process that wrote the same version
            std::error_code ec2;
            fs::remove_all(tmp, ec2);
            if (!fs::exists(dir / ".complete")) return Error::make("io_error", "cannot create " + dir.string() + ": " + ec.message());
        }
    }
    std::lock_guard lock(mutex_);
    runtimeDir_ = dir.string();
    return runtimeDir_;
}

Result<std::string> Manager::installAddon(const std::string& scriptsDir) {
    auto rt = ensureRuntime();
    if (!rt) return rt.error();
    fs::path target = fs::path(scriptsDir) / "addons" / "skywalker_bridge";
    std::error_code ec;
    fs::remove_all(target, ec);
    fs::create_directories(target.parent_path(), ec);
    fs::copy(fs::path(*rt) / "skywalker_bridge", target, fs::copy_options::recursive, ec);
    if (ec) return Error::make("io_error", "cannot install the add-on into " + target.string() + ": " + ec.message());
    // The add-on carries its own copy of the helper library.
    fs::copy(fs::path(*rt) / "skywalker_dcc", target / "skywalker_dcc", fs::copy_options::recursive, ec);
    if (ec) return Error::make("io_error", "cannot install the helper library: " + ec.message());
    return target.string();
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

int Manager::beginJob(const std::string& label, std::shared_ptr<CancelToken> cancel) {
    std::lock_guard lock(mutex_);
    int id = nextJob_++;
    jobs_.push_back({id, label, Clock::now(), std::move(cancel)});
    return id;
}

void Manager::endJob(int id) {
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_, [id](const Job& j) { return j.id == id; });
}

bool Manager::cancelJob(int id) {
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_) {
        if (j.id == id) {
            j.cancel->cancel();
            return true;
        }
    }
    return false;
}

Json Manager::runningJobs() const {
    std::lock_guard lock(mutex_);
    Json list = Json::array();
    for (const auto& j : jobs_) {
        double s = std::chrono::duration<double>(Clock::now() - j.started).count();
        list.push(Json::object({{"id", j.id}, {"label", j.label}, {"seconds", s}}));
    }
    return list;
}

JobResult Manager::run(const JobSpec& spec, const AppInfo& app, const std::shared_ptr<CancelToken>& cancel) {
    JobResult jr;
    auto fail = [&](std::string error, std::string hint = {}) -> JobResult& {
        jr.ok = false;
        jr.error = std::move(error);
        jr.hint = std::move(hint);
        return jr;
    };
    auto rt = ensureRuntime();
    if (!rt) return fail(rt.error().message);

    std::error_code ec;
    fs::path job = fs::temp_directory_path() / ("skywalker-dcc-" + randomHex(6));
    fs::create_directories(job, ec);
    if (ec) return fail("cannot create a job folder: " + ec.message());
    ::chmod(job.c_str(), 0700);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{job};

    fs::create_directories(spec.outDir, ec);
    if (ec) return fail("cannot create the output folder " + spec.outDir + ": " + ec.message());

    if (Status s = writeFile(job / "bootstrap.py", kBootstrapStub); !s) return fail(s.error().message);
    if (!spec.script.empty()) {
        if (Status s = writeFile(job / "user.py", spec.script); !s) return fail(s.error().message);
    }
    if (Status s = writeFile(job / "params.json", spec.params.dump()); !s) return fail(s.error().message);

    Json argsJson = Json::array();
    for (const auto& a : spec.args) argsJson.push(a);

    const Adapter& ad = adapterFor(app.id);
    ScriptRun run;
    run.bootstrapPath = (job / "bootstrap.py").string();
    run.inputFile = spec.inputFile;
    run.args = spec.args;
    run.userPrefs = spec.userPrefs;
    ProcessSpec ps = ad.scriptCommand(app, run);
    ps.workingDir = spec.outDir;
    ps.timeout = spec.timeout;
    ps.cancel = cancel;
    ps.env = {{"SKY_APP", toString(app.id)},
              {"SKY_PROJECT", spec.projectDir},
              {"SKY_OUT", spec.outDir},
              {"SKY_INPUT", spec.inputFile},
              {"SKY_JOB", job.string()},
              {"SKY_LIB", *rt},
              {"SKY_TASK", spec.task},
              {"SKY_PARAMS", (job / "params.json").string()},
              {"SKY_SCRIPT", (job / "user.py").string()},
              {"SKY_ARGS", argsJson.dump()},
              {"SKY_RESULT", (job / "result.json").string()},
              {"PYTHONDONTWRITEBYTECODE", "1"}};
    for (const auto& kv : spec.env) ps.env.push_back(kv);

    FileMap before = snapshot(spec.outDir);
    jr.process = runProcess(ps);
    FileMap after = snapshot(spec.outDir);
    jr.seconds = jr.process.seconds;

    for (const auto& [path, meta] : after) {
        auto it = before.find(path);
        if (it == before.end() || it->second != meta) jr.files.push_back({path, meta.second});
    }
    if (auto r = Json::parse(readFile((job / "result.json").string())); r && r->isObject()) jr.result = *r;
    jr.log = tidyLog(jr.process.out + (jr.process.err.empty() ? "" : "\n" + jr.process.err), 12000);

    const ProcessResult& p = jr.process;
    if (!p.spawned) return fail(p.spawnError, "check the app path with dcc_list (or set " + ad.envVar() + ")");
    if (p.cancelled) return fail("cancelled");
    if (p.timedOut) {
        return fail("timed out after " + std::to_string(static_cast<int>(spec.timeout.count() / 1000)) + " s",
                    "raise timeout_s, or simplify the script (high subdivision levels and bakes are slow)");
    }
    if (!p.ok()) {
        std::string why = lastPythonError(p.err + "\n" + p.out);
        if (why.empty()) {
            why = p.signal ? "the app crashed (signal " + std::to_string(p.signal) + ")"
                           : "the app exited with code " + std::to_string(p.exitCode);
        }
        return fail(why, "the app's output is in the result log");
    }
    jr.ok = true;
    return jr;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string Manager::sessionFilePath() const { return (fs::path(machine_.stateDir) / "blender.json").string(); }
std::string Manager::logPath(const std::string& name) const { return (fs::path(machine_.stateDir) / name).string(); }

Result<Manager::SessionInfo> Manager::session() const {
    auto parsed = Json::parse(readFile(sessionFilePath()));
    if (!parsed || !parsed->isObject()) {
        return Error::make("no_session", "no Blender session is running",
                           "start one with dcc_session_start, or open Blender and click Start Bridge in the Skywalker panel "
                           "(install the add-on first with dcc_install_addon)");
    }
    SessionInfo s;
    s.port = static_cast<int>(parsed->get("port").asInt());
    s.token = parsed->get("token").asString();
    s.pid = static_cast<long>(parsed->get("pid").asInt());
    s.version = parsed->get("version").asString();
    s.file = parsed->get("file").asString();
    s.mode = parsed->get("mode").asString();
    if (s.port <= 0 || s.token.empty()) return Error::make("no_session", "the session file is incomplete");
    if (!processAlive(s.pid)) {
        return Error::make("no_session", "the Blender session in " + sessionFilePath() + " is stale (process " + std::to_string(s.pid) + " is gone)",
                           "start a new one with dcc_session_start");
    }
    return s;
}

namespace {

struct SockFd {
    int fd = -1;
    ~SockFd() {
        if (fd >= 0) ::close(fd);
    }
};

}  // namespace

Result<Json> Manager::sessionCall(const SessionInfo& s, const std::string& method, const Json& params,
                                  std::chrono::milliseconds timeout, const std::shared_ptr<CancelToken>& cancel) const {
    static std::atomic<int> nextId{1};
    const auto deadline = Clock::now() + timeout;
    SockFd sock;
    sock.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock.fd < 0) return Error::make("io_error", std::strerror(errno));
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(sock.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    ::fcntl(sock.fd, F_SETFL, ::fcntl(sock.fd, F_GETFL) | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(s.port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // the bridge only ever listens on loopback
    int rc = ::connect(sock.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        return Error::make("session_unreachable", std::string("cannot connect to the Blender bridge: ") + std::strerror(errno),
                           "the session may have been closed; call dcc_session_status");
    }

    auto waitFor = [&](short events) -> Status {
        while (true) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return Error::make("timeout", "the Blender session did not answer in time",
                                              "Blender may be busy (a long operation) or showing a modal dialog");
            if (cancel && cancel->cancelled()) return Error::make("cancelled", "cancelled");
            pollfd pfd{sock.fd, events, 0};
            int r = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(left, 100)));
            if (r > 0) return {};
            if (r < 0 && errno != EINTR) return Error::make("io_error", std::strerror(errno));
        }
    };
    if (rc != 0) {
        if (Status w = waitFor(POLLOUT); !w) return w.error();
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            return Error::make("session_unreachable", std::string("cannot connect to the Blender bridge: ") + std::strerror(err));
        }
    }

    Json req = Json::object({{"id", nextId++}, {"token", s.token}, {"method", method}, {"params", params}});
    std::string line = req.dump() + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        ssize_t n = ::send(sock.fd, line.data() + sent, line.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (Status w = waitFor(POLLOUT); !w) return w.error();
        } else {
            return Error::make("io_error", std::string("send failed: ") + std::strerror(errno));
        }
    }

    std::string buf;
    constexpr size_t kMaxResponse = size_t{64} << 20;
    while (buf.find('\n') == std::string::npos) {
        if (Status w = waitFor(POLLIN); !w) return w.error();
        char chunk[65536];
        ssize_t n = ::recv(sock.fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buf.append(chunk, static_cast<size_t>(n));
            if (buf.size() > kMaxResponse) return Error::make("too_large", "the Blender bridge sent an oversized response");
        } else if (n == 0) {
            return Error::make("session_closed", "Blender closed the connection before answering");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return Error::make("io_error", std::string("receive failed: ") + std::strerror(errno));
        }
    }
    auto resp = Json::parse(buf.substr(0, buf.find('\n')));
    if (!resp || !resp->isObject()) return Error::make("protocol_error", "the Blender bridge sent something that is not JSON");
    if (!resp->get("ok").asBool()) {
        const Json& e = resp->get("error");
        std::string msg = e.get("message").asString("the bridge reported an error");
        std::string trace = e.get("trace").asString();
        return Error::make(e.get("type").asString("session_error"), msg, trace.empty() ? "" : "traceback:\n" + trace);
    }
    return resp->get("result");
}

Result<Manager::SessionInfo> Manager::startSession(const AppInfo& blender, bool headless, const std::string& open,
                                                   const std::string& projectDir, std::chrono::milliseconds wait) {
    if (auto existing = session(); existing) {
        auto pong = sessionCall(*existing, "ping", Json::object(), std::chrono::seconds(5));
        if (pong) return *existing;  // idempotent: reuse the live session
    }
    auto rt = ensureRuntime();
    if (!rt) return rt.error();
    std::error_code ec;
    fs::remove(sessionFilePath(), ec);  // stale file from a dead session

    ProcessSpec ps;
    ps.executable = blender.executable;
    if (headless) {
        ps.args = {"--background", "--factory-startup", "--disable-autoexec"};
    }
    ps.args.push_back("--python-expr");
    ps.args.push_back("import os, sys; sys.path.insert(0, os.environ['SKY_LIB']); import skywalker_bridge; skywalker_bridge.run_from_env()");
    ps.env = {{"SKY_LIB", *rt},
              {"SKY_APP", "blender"},
              {"SKY_PROJECT", projectDir},
              {"SKY_SESSION_FILE", sessionFilePath()},
              {"SKY_SESSION_MODE", headless ? "headless" : "ui"},
              {"SKY_OPEN", open},
              {"SKY_PARENT_PID", headless ? std::to_string(static_cast<long>(::getpid())) : std::string()},
              {"PYTHONDONTWRITEBYTECODE", "1"}};
    std::string log = logPath("blender-session.log");
    fs::create_directories(fs::path(log).parent_path(), ec);
    auto pid = spawnDetached(ps, log);
    if (!pid) return pid.error();
    if (headless) {
        std::lock_guard lock(mutex_);
        startedHeadless_.push_back(*pid);
    }

    const auto deadline = Clock::now() + wait;
    while (Clock::now() < deadline) {
        if (!processAlive(*pid)) {
            return Error::make("session_failed", "Blender exited while starting the session",
                               "see " + log + " for Blender's output");
        }
        if (auto s = session(); s) {
            if (sessionCall(*s, "ping", Json::object(), std::chrono::seconds(5))) return *s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return Error::make("timeout", "the Blender session did not come up in time", "see " + log + " for Blender's output");
}

Status Manager::stopSession() {
    auto s = session();
    if (!s) return s.error();
    (void)sessionCall(*s, "shutdown", Json::object(), std::chrono::seconds(5));
    for (int i = 0; i < 30 && processAlive(s->pid) && s->mode == "headless"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    bool ours = false;
    {
        std::lock_guard lock(mutex_);
        ours = std::find(startedHeadless_.begin(), startedHeadless_.end(), s->pid) != startedHeadless_.end();
        std::erase(startedHeadless_, s->pid);
    }
    if (ours && processAlive(s->pid)) terminateProcess(s->pid);
    return {};
}

}  // namespace sky::dcc
