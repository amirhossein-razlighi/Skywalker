// DCC bridge tests: process runner, app detection, adapters, session protocol, deferred tool
// calls, and (when Blender is installed) real round trips through Blender.

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "skywalker/agent/SocketServer.h"
#include "skywalker/dcc/Apps.h"
#include "skywalker/dcc/Manager.h"
#include "skywalker/dcc/Process.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
using namespace sky::dcc;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct TempDir {
    fs::path dir;
    explicit TempDir(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-dcc-test-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string str() const { return dir.string(); }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
};

/// Sets an environment variable for the lifetime of the object.
struct EnvGuard {
    std::string name;
    std::string old;
    bool had;
    EnvGuard(const char* n, const std::string& value) : name(n) {
        const char* o = std::getenv(n);
        had = o != nullptr;
        if (o) old = o;
        ::setenv(n, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (had) ::setenv(name.c_str(), old.c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

ProcessSpec sh(const std::string& script, std::chrono::milliseconds timeout = 10s) {
    ProcessSpec s;
    s.executable = "/bin/sh";
    s.args = {"-c", script};
    s.timeout = timeout;
    return s;
}

// --- fake machines for detection tests ------------------------------------------------------------

Host fakeHost(const std::string& os, std::set<std::string> executables, std::map<std::string, std::vector<std::string>> dirs,
              std::map<std::string, std::string> env = {}, std::vector<std::string> path = {}) {
    Host h;
    h.os = os;
    h.home = os == "windows" ? "C:\\Users\\me" : "/Users/me";
    h.getenv = [env](const std::string& k) {
        auto it = env.find(k);
        return it == env.end() ? std::string() : it->second;
    };
    h.isExecutable = [executables](const std::string& p) { return executables.count(p) > 0; };
    h.listDir = [dirs](const std::string& d) {
        auto it = dirs.find(d);
        return it == dirs.end() ? std::vector<std::string>{} : it->second;
    };
    h.pathDirs = std::move(path);
    return h;
}

const AppInfo* findApp(const std::vector<AppInfo>& apps, AppId id) {
    for (const auto& a : apps) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

// --- Blender availability ---------------------------------------------------------------------------

/// Manager with a private state folder so tests never touch ~/.skywalker.
struct Rig {
    TempDir state{"state"};
    EnvGuard stateEnv{"SKY_DCC_STATE", ""};
    Rig() { ::setenv("SKY_DCC_STATE", state.str().c_str(), 1); }
};

bool blenderInstalled() {
    static const bool found = [] {
        Machine env = Machine::real();
        Manager m(env);
        return m.pick("blender", "script").ok();
    }();
    return found;
}

#define REQUIRE_BLENDER()                                          \
    if (!blenderInstalled()) {                                     \
        MESSAGE("Blender is not installed here: skipping");        \
        return;                                                    \
    }

struct Project {
    TempDir dir{"project"};
    Rig rig;
    std::unique_ptr<Engine> engine;
    Project() {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir.str();
        engine = std::make_unique<Engine>(cfg);
        (void)engine->newScene("dcc", true);
    }
    ToolResult call(const std::string& tool, const std::string& args) {
        return engine->callTool(tool, Json::parse(args).value(), "agent:test");
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Process runner
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc process: argv is passed verbatim, never through a shell") {
    ProcessSpec s;
    s.executable = "/bin/echo";
    s.args = {"a; touch /tmp/should-not-exist-skywalker", "$HOME", "`id`", "two words"};
    ProcessResult r = runProcess(s);
    REQUIRE(r.spawned);
    CHECK(r.ok());
    CHECK(r.out == "a; touch /tmp/should-not-exist-skywalker $HOME `id` two words\n");
    CHECK_FALSE(fs::exists("/tmp/should-not-exist-skywalker"));
}

TEST_CASE("dcc process: exit codes, stderr, signals, spawn failures") {
    ProcessResult r = runProcess(sh("echo out; echo err 1>&2; exit 3"));
    CHECK(r.spawned);
    CHECK(r.exitCode == 3);
    CHECK_FALSE(r.ok());
    CHECK(r.out == "out\n");
    CHECK(r.err == "err\n");

    r = runProcess(sh("kill -TERM $$"));
    CHECK(r.signal == SIGTERM);
    CHECK_FALSE(r.ok());

    ProcessSpec bad;
    bad.executable = "/definitely/not/here";
    r = runProcess(bad);
    CHECK_FALSE(r.spawned);
    CHECK(r.spawnError.find("/definitely/not/here") != std::string::npos);

    bad.executable = "no-such-program-skywalker";  // PATH lookup
    CHECK_FALSE(runProcess(bad).spawned);
}

TEST_CASE("dcc process: working directory and environment") {
    TempDir d("cwd");
    ProcessSpec s;
    s.executable = "/bin/sh";
    s.args = {"-c", "pwd; echo \"$SKY_TEST_VALUE\""};
    s.workingDir = d.str();
    s.env = {{"SKY_TEST_VALUE", "hello world"}, {"SKY_TEST_VALUE", "override wins"}};
    ProcessResult r = runProcess(s);
    REQUIRE(r.ok());
    std::istringstream in(r.out);
    std::string pwd, val;
    std::getline(in, pwd);
    std::getline(in, val);
    CHECK(fs::equivalent(pwd, d.dir));
    CHECK(val == "override wins");

    s.workingDir = "/no/such/dir";
    ProcessResult bad = runProcess(s);
    CHECK_FALSE(bad.spawned);
}

TEST_CASE("dcc process: timeout kills the whole process group") {
    ProcessSpec s = sh("sleep 30 & sleep 30 & wait", 400ms);
    s.killGrace = 300ms;
    auto t0 = std::chrono::steady_clock::now();
    ProcessResult r = runProcess(s);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.spawned);
    CHECK(r.timedOut);
    CHECK_FALSE(r.ok());
    CHECK(took < 5.0);

    // A process that ignores SIGTERM is killed after the grace period.
    ProcessSpec stubborn = sh("trap '' TERM; sleep 30", 300ms);
    stubborn.killGrace = 300ms;
    t0 = std::chrono::steady_clock::now();
    r = runProcess(stubborn);
    took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.timedOut);
    CHECK(took < 5.0);
}

TEST_CASE("dcc process: cancellation from another thread") {
    auto cancel = std::make_shared<CancelToken>();
    ProcessSpec s;
    s.executable = "/bin/sleep";
    s.args = {"30"};
    s.cancel = cancel;
    s.killGrace = 300ms;
    std::thread t([&] {
        std::this_thread::sleep_for(150ms);
        cancel->cancel();
    });
    auto t0 = std::chrono::steady_clock::now();
    ProcessResult r = runProcess(s);
    t.join();
    CHECK(r.cancelled);
    CHECK_FALSE(r.timedOut);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0);
}

TEST_CASE("dcc process: output is capped, keeping the head and the tail") {
    BoundedCapture cap(100);
    std::string big;
    for (int i = 0; i < 1000; ++i) big += std::to_string(i % 10);
    cap.append(big.data(), 300);
    cap.append(big.data() + 300, 700);
    CHECK(cap.total() == 1000);
    std::string out = cap.str();
    CHECK(out.find("omitted") != std::string::npos);
    CHECK(out.substr(0, 25) == big.substr(0, 25));          // head
    CHECK(out.substr(out.size() - 10) == big.substr(990));  // tail
    CHECK(out.size() < 250);

    ProcessSpec s = sh("i=0; while [ $i -lt 20000 ]; do echo line-$i; i=$((i+1)); done", 20s);
    s.maxOutputBytes = 4096;
    ProcessResult r = runProcess(s);
    REQUIRE(r.ok());
    CHECK(r.outTruncated);
    CHECK(r.out.size() < 5000);
    CHECK(r.outBytes > 100000);
    CHECK(r.out.find("line-19999") != std::string::npos);  // the tail survives
    CHECK(r.out.find("line-0\n") != std::string::npos);    // so does the head
}

TEST_CASE("dcc process: detached sessions and liveness") {
    TempDir d("detached");
    ProcessSpec s;
    s.executable = "/bin/sleep";
    s.args = {"30"};
    auto pid = spawnDetached(s, (d.dir / "log.txt").string());
    REQUIRE(pid.ok());
    CHECK(processAlive(*pid));
    terminateProcess(*pid);
    for (int i = 0; i < 50 && processAlive(*pid); ++i) std::this_thread::sleep_for(20ms);
    CHECK_FALSE(processAlive(*pid));
    CHECK_FALSE(processAlive(0));
}

// ---------------------------------------------------------------------------------------------
// Detection & adapters
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc detection: macOS install locations, newest version wins") {
    Host h = fakeHost(
        "macos",
        {"/Applications/Blender.app/Contents/MacOS/Blender",
         "/Applications/Autodesk/maya2024/Maya.app/Contents/bin/mayapy",
         "/Applications/Autodesk/maya2025/Maya.app/Contents/bin/mayapy",
         "/Applications/Houdini/Houdini19.5.569/Frameworks/Houdini.framework/Versions/Current/Resources/bin/hython",
         "/Applications/Houdini/Houdini20.5.370/Frameworks/Houdini.framework/Versions/Current/Resources/bin/hython"},
        {{"/Applications", {"Blender.app", "Safari.app"}},
         {"/Applications/Autodesk", {"maya2024", "maya2025", "Shared"}},
         {"/Applications/Houdini", {"Houdini19.5.569", "Houdini20.5.370", "HoudiniLauncher"}}});
    auto apps = detectApps(h);
    const AppInfo* blender = findApp(apps, AppId::Blender);
    REQUIRE(blender);
    CHECK(blender->executable == "/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(blender->foundVia == "standard-location");
    CHECK(blender->tested);
    CHECK(blender->has("export_glb"));
    CHECK(blender->has("live_session"));

    const AppInfo* maya = findApp(apps, AppId::Maya);
    REQUIRE(maya);
    CHECK(maya->version == "2025");
    CHECK(maya->executable == "/Applications/Autodesk/maya2025/Maya.app/Contents/bin/mayapy");
    REQUIRE(maya->alternatives.size() == 1);
    CHECK(maya->alternatives[0].find("maya2024") != std::string::npos);
    CHECK_FALSE(maya->tested);

    const AppInfo* houdini = findApp(apps, AppId::Houdini);
    REQUIRE(houdini);
    CHECK(houdini->version == "20.5.370");
    CHECK(houdini->has("export_fbx"));
    CHECK_FALSE(houdini->tested);

    CHECK(findApp(apps, AppId::Max) == nullptr);  // Windows only
}

TEST_CASE("dcc detection: Windows install locations") {
    Host h = fakeHost(
        "windows",
        {"C:\\Program Files\\Blender Foundation\\Blender 3.6\\blender.exe",
         "C:\\Program Files\\Blender Foundation\\Blender 4.1\\blender.exe",
         "C:\\Program Files\\Autodesk\\3ds Max 2025\\3dsmaxbatch.exe",
         "C:\\Program Files\\Autodesk\\Maya2024\\bin\\mayapy.exe",
         "C:\\Program Files\\Side Effects Software\\Houdini 20.5.370\\bin\\hython.exe"},
        {{"C:\\Program Files\\Blender Foundation", {"Blender 3.6", "Blender 4.1"}},
         {"C:\\Program Files\\Autodesk", {"3ds Max 2025", "Maya2024", "AutoCAD 2025"}},
         {"C:\\Program Files\\Side Effects Software", {"Houdini 20.5.370"}}});
    auto apps = detectApps(h);
    REQUIRE(apps.size() == 4);
    CHECK(findApp(apps, AppId::Blender)->version == "4.1");
    CHECK(findApp(apps, AppId::Blender)->alternatives.size() == 1);
    CHECK(findApp(apps, AppId::Max)->version == "2025");
    CHECK(findApp(apps, AppId::Max)->executable == "C:\\Program Files\\Autodesk\\3ds Max 2025\\3dsmaxbatch.exe");
    CHECK(findApp(apps, AppId::Maya)->version == "2024");
    CHECK(findApp(apps, AppId::Houdini)->version == "20.5.370");
}

TEST_CASE("dcc detection: Linux, PATH, environment overrides and config") {
    Host h = fakeHost("linux",
                      {"/usr/bin/blender", "/opt/hfs20.5.370/bin/hython", "/home/me/bin/mayapy", "/custom/blender"},
                      {{"/opt", {"hfs20.5.370", "other"}}}, {}, {"/home/me/bin", "/usr/bin"});
    h.home = "/home/me";
    auto apps = detectApps(h);
    CHECK(findApp(apps, AppId::Blender)->executable == "/usr/bin/blender");
    CHECK(findApp(apps, AppId::Houdini)->version == "20.5.370");
    const AppInfo* maya = findApp(apps, AppId::Maya);
    REQUIRE(maya);
    CHECK(maya->foundVia == "PATH");

    // An override wins even though a system Blender exists; the system one stays listed.
    Host o = fakeHost("linux", {"/usr/bin/blender", "/custom/blender"}, {}, {{"SKY_BLENDER", "/custom/blender"}});
    auto overridden = detectApps(o);
    const AppInfo* b = findApp(overridden, AppId::Blender);
    REQUIRE(b);
    CHECK(b->executable == "/custom/blender");
    CHECK(b->foundVia == "env:SKY_BLENDER");
    REQUIRE(b->alternatives.size() == 1);
    CHECK(b->alternatives[0] == "/usr/bin/blender");

    // A broken override is reported but does not hide a working install.
    Host broken = fakeHost("linux", {"/usr/bin/blender"}, {}, {{"SKY_BLENDER", "/nope/blender"}});
    auto brokenApps = detectApps(broken);
    const AppInfo* nb = findApp(brokenApps, AppId::Blender);
    REQUIRE(nb);
    CHECK(nb->executable == "/usr/bin/blender");
    CHECK(nb->note.find("SKY_BLENDER") != std::string::npos);

    // Config file paths work like overrides.
    Host plain = fakeHost("linux", {"/opt/tools/mayapy"}, {});
    auto cfg = detectApps(plain, {{AppId::Maya, "/opt/tools/mayapy"}});
    CHECK(findApp(cfg, AppId::Maya)->foundVia == "config");

    // Houdini via $HFS and Maya via $MAYA_LOCATION.
    Host env = fakeHost("macos", {"/opt/sidefx/hfs19.5.100/bin/hython", "/Applications/Autodesk/maya2023/Maya.app/Contents/bin/mayapy"}, {},
                        {{"HFS", "/opt/sidefx/hfs19.5.100"}, {"MAYA_LOCATION", "/Applications/Autodesk/maya2023/Maya.app/Contents"}});
    auto fromEnv = detectApps(env);
    CHECK(findApp(fromEnv, AppId::Houdini)->executable == "/opt/sidefx/hfs19.5.100/bin/hython");
    CHECK(findApp(fromEnv, AppId::Maya)->version == "2023");

    CHECK(detectApps(fakeHost("linux", {}, {})).empty());
}

TEST_CASE("dcc detection: the version probe runs only when the path has no version") {
    Host h = fakeHost("macos", {"/Applications/Blender.app/Contents/MacOS/Blender", "/Applications/Autodesk/maya2024/Maya.app/Contents/bin/mayapy"},
                      {{"/Applications", {"Blender.app"}}, {"/Applications/Autodesk", {"maya2024"}}});
    int probes = 0;
    auto apps = detectApps(h, {}, [&](const AppInfo& a) {
        ++probes;
        return a.id == AppId::Blender ? std::string("4.2.1") : std::string("bad");
    });
    CHECK(probes == 1);
    CHECK(findApp(apps, AppId::Blender)->version == "4.2.1");
    CHECK(findApp(apps, AppId::Maya)->version == "2024");
}

TEST_CASE("dcc versions compare numerically") {
    CHECK(compareVersions("3.5.1", "3.10") < 0);
    CHECK(compareVersions("20.5.370", "19.5.569") > 0);
    CHECK(compareVersions("2025", "2024") > 0);
    CHECK(compareVersions("4.0", "4.0.0") == 0);
    CHECK(compareVersions("", "1") < 0);
    CHECK(compareVersions("", "") == 0);
    CHECK(adapterFor(AppId::Blender).parseVersion("Blender 3.5.1\n\tbuild date: 2023-04-24\n") == "3.5.1");
    CHECK(adapterFor(AppId::Blender).parseVersion("Blender 4.2.0 LTS\n") == "4.2.0");
    CHECK(adapterFor(AppId::Blender).parseVersion("garbage").empty());
    CHECK(appFromString("Blender") == AppId::Blender);
    CHECK(appFromString("3dsmax") == AppId::Max);
    CHECK(appFromString("hython") == AppId::Houdini);
    CHECK_FALSE(appFromString("cinema4d").has_value());
}

TEST_CASE("dcc adapters build headless command lines (argv, never a shell)") {
    ScriptRun run;
    run.bootstrapPath = "/job/bootstrap.py";
    run.inputFile = "/proj/models/scene.blend";
    run.args = {"--size", "3"};

    AppInfo blender;
    blender.id = AppId::Blender;
    blender.executable = "/Applications/Blender.app/Contents/MacOS/Blender";
    ProcessSpec p = adapterFor(AppId::Blender).scriptCommand(blender, run);
    CHECK(p.executable == blender.executable);
    CHECK(p.args == std::vector<std::string>{"--background", "--factory-startup", "--disable-autoexec", "--python-exit-code", "1",
                                             "/proj/models/scene.blend", "--python", "/job/bootstrap.py", "--", "--size", "3"});
    run.userPrefs = true;
    run.inputFile = "/proj/models/tree.fbx";  // not a .blend: stays off the command line
    run.args.clear();
    p = adapterFor(AppId::Blender).scriptCommand(blender, run);
    CHECK(p.args == std::vector<std::string>{"--background", "--disable-autoexec", "--python-exit-code", "1", "--python", "/job/bootstrap.py"});

    AppInfo maya;
    maya.id = AppId::Maya;
    maya.executable = "/m/mayapy";
    run.args = {"x"};
    p = adapterFor(AppId::Maya).scriptCommand(maya, run);
    CHECK(p.args == std::vector<std::string>{"/job/bootstrap.py", "x"});

    AppInfo houdini;
    houdini.id = AppId::Houdini;
    houdini.executable = "/h/hython";
    p = adapterFor(AppId::Houdini).scriptCommand(houdini, run);
    CHECK(p.args == std::vector<std::string>{"/job/bootstrap.py", "x"});

    AppInfo max;
    max.id = AppId::Max;
    max.executable = "C:\\max\\3dsmaxbatch.exe";
    run.inputFile = "C:\\proj\\scene.max";
    p = adapterFor(AppId::Max).scriptCommand(max, run);
    CHECK(p.args == std::vector<std::string>{"-v", "2", "-sceneFile", "C:\\proj\\scene.max", "/job/bootstrap.py"});
}

// ---------------------------------------------------------------------------------------------
// Log helpers
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc logs: noise filtered, Python errors extracted") {
    std::string raw =
        "Blender 3.5.1 (hash e1ccd9d4a1d3 built 2023-04-24 23:46:26)\nRead prefs: /x\nDevice with name Apple M1 supports\n"
        "20:59:03 | INFO: Starting glTF 2.0 export\n[sky] hello\nWarning: something real\nBlender quit\n";
    CHECK(tidyLog(raw, 1000) == "[sky] hello\nWarning: something real");
    std::string longText(5000, 'x');
    std::string trimmed = tidyLog(longText, 100);
    CHECK(trimmed.size() < 160);
    CHECK(trimmed.find("trimmed") != std::string::npos);

    std::string trace =
        "Traceback (most recent call last):\n  File \"a.py\", line 3, in <module>\n    foo()\n  File \"b.py\", line 9, in foo\n"
        "    raise ValueError(\"bad\")\nValueError: bad value\n";
    CHECK(lastPythonError(trace) == "ValueError: bad value");
    CHECK(lastPythonError("no traceback here").empty());
    CHECK(lastPythonError("Traceback (most recent call last):\n  File \"a\", line 1\nSystemExit: 1\n") == "SystemExit: 1");
}

// ---------------------------------------------------------------------------------------------
// Runtime files, add-on install, session protocol
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc runtime: helper library and add-on are embedded and written once") {
    CHECK_FALSE(embeddedFiles().empty());
    std::set<std::string> names;
    for (const auto& f : embeddedFiles()) names.insert(f.path);
    CHECK(names.count("lib/skywalker_dcc/__init__.py"));
    CHECK(names.count("lib/skywalker_dcc/blender.py"));
    CHECK(names.count("lib/skywalker_dcc/procedural.py"));
    CHECK(names.count("addon/skywalker_bridge/__init__.py"));
    CHECK(names.count("addon/skywalker_bridge/server.py"));

    TempDir state("runtime");
    Machine env = Machine::real();
    env.stateDir = state.str();
    Manager m(env);
    auto rt = m.ensureRuntime();
    REQUIRE(rt.ok());
    CHECK(fs::exists(fs::path(*rt) / "skywalker_dcc" / "blender.py"));
    CHECK(fs::exists(fs::path(*rt) / "skywalker_bridge" / "server.py"));
    CHECK(fs::exists(fs::path(*rt) / ".complete"));
    // A second manager reuses the same folder.
    Manager m2(env);
    CHECK(*m2.ensureRuntime() == *rt);

    TempDir scripts("scripts");
    auto installed = m.installAddon(scripts.str());
    REQUIRE(installed.ok());
    CHECK(fs::exists(fs::path(*installed) / "__init__.py"));
    CHECK(fs::exists(fs::path(*installed) / "skywalker_dcc" / "blender.py"));
    CHECK(fs::equivalent(*installed, scripts.dir / "addons" / "skywalker_bridge"));
    // Re-installing replaces the old copy.
    std::ofstream(fs::path(*installed) / "stale.py") << "x";
    REQUIRE(m.installAddon(scripts.str()).ok());
    CHECK_FALSE(fs::exists(fs::path(*installed) / "stale.py"));
}

namespace {

/// A stand-in for the Blender add-on: speaks the bridge protocol on loopback.
class FakeBridge {
public:
    explicit FakeBridge(std::string token) : token_(std::move(token)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        ::listen(fd_, 4);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~FakeBridge() {
        stop_ = true;
        int c = ::socket(AF_INET, SOCK_STREAM, 0);  // wake accept()
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port_));
        ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::close(c);
        thread_.join();
        ::close(fd_);
    }
    int port() const { return port_; }
    std::atomic<int> requests{0};

private:
    void loop() {
        while (!stop_) {
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) break;
            if (stop_) {
                ::close(c);
                break;
            }
            std::string buf;
            char chunk[4096];
            while (buf.find('\n') == std::string::npos) {
                ssize_t n = ::recv(c, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                buf.append(chunk, static_cast<size_t>(n));
            }
            ++requests;
            auto req = Json::parse(buf.substr(0, buf.find('\n')));
            std::string reply;
            if (req.ok()) {
                Json resp = Json::object({{"id", req->get("id")}});
                const std::string method = req->get("method").asString();
                if (req->get("token").asString() != token_) {
                    resp["ok"] = false;
                    resp["error"] = Json::object({{"type", "unauthorized"}, {"message", "wrong token"}});
                } else if (method == "echo") {
                    resp["ok"] = true;
                    resp["result"] = req->get("params");
                } else if (method == "fail") {
                    resp["ok"] = false;
                    resp["error"] = Json::object({{"type", "ValueError"}, {"message", "nope"}, {"trace", "Traceback...\nValueError: nope\n"}});
                } else if (method == "slow") {
                    std::this_thread::sleep_for(700ms);
                    resp["ok"] = true;
                    resp["result"] = Json::object();
                } else if (method == "close") {
                    ::close(c);
                    continue;
                } else {
                    resp["ok"] = true;
                    resp["result"] = Json::object({{"pong", true}});
                }
                reply = resp.dump() + "\n";
            } else {
                reply = "this is not json\n";
            }
            ::send(c, reply.data(), reply.size(), 0);
            ::close(c);
        }
    }

    std::string token_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

TEST_CASE("dcc session: protocol client, token, errors, timeouts") {
    TempDir state("session");
    Machine env = Machine::real();
    env.stateDir = state.str();
    Manager m(env);

    CHECK_FALSE(m.session().ok());  // no file yet
    FakeBridge bridge("secret-token");
    state.write("blender.json", Json::object({{"port", bridge.port()}, {"token", "secret-token"}, {"pid", static_cast<double>(::getpid())},
                                              {"version", "3.5.1"}, {"mode", "headless"}})
                                    .dump());
    auto s = m.session();
    REQUIRE(s.ok());
    CHECK(s->port == bridge.port());
    CHECK(s->mode == "headless");

    auto echo = m.sessionCall(*s, "echo", Json::object({{"a", 1}, {"b", "two"}}), 5s);
    REQUIRE(echo.ok());
    CHECK(echo->get("b").asString() == "two");

    auto err = m.sessionCall(*s, "fail", Json::object(), 5s);
    REQUIRE_FALSE(err.ok());
    CHECK(err.error().code == "ValueError");
    CHECK(err.error().hint.find("Traceback") != std::string::npos);

    Manager::SessionInfo wrong = *s;
    wrong.token = "guess";
    auto denied = m.sessionCall(wrong, "echo", Json::object(), 5s);
    REQUIRE_FALSE(denied.ok());
    CHECK(denied.error().code == "unauthorized");

    auto t0 = std::chrono::steady_clock::now();
    auto slow = m.sessionCall(*s, "slow", Json::object(), 300ms);
    REQUIRE_FALSE(slow.ok());
    CHECK(slow.error().code == "timeout");
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.65);

    auto closed = m.sessionCall(*s, "close", Json::object(), 5s);
    REQUIRE_FALSE(closed.ok());
    CHECK(closed.error().code == "session_closed");

    auto cancel = std::make_shared<CancelToken>();
    std::thread t([&] {
        std::this_thread::sleep_for(100ms);
        cancel->cancel();
    });
    auto cancelled = m.sessionCall(*s, "slow", Json::object(), 10s, cancel);
    t.join();
    REQUIRE_FALSE(cancelled.ok());
    CHECK(cancelled.error().code == "cancelled");

    // Nobody listens any more: a clear error rather than a hang.
    Manager::SessionInfo gone = *s;
    {
        int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&a), &len);
        gone.port = ntohs(a.sin_port);
        ::close(probe);
    }
    auto refused = m.sessionCall(gone, "echo", Json::object(), 2s);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == "session_unreachable");

    // A session file whose process is gone is stale.
    state.write("blender.json", Json::object({{"port", 1}, {"token", "x"}, {"pid", 2147483000.0}}).dump());
    auto stale = m.session();
    REQUIRE_FALSE(stale.ok());
    CHECK(stale.error().message.find("stale") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Deferred tool calls
// ---------------------------------------------------------------------------------------------

TEST_CASE("deferred tools: inline completion, and the agent server keeps the main thread free") {
    TempDir d("deferred");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = d.str();
    Engine e(cfg);
    (void)e.newScene("Deferred", true);

    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> workRan{false}, workOnOtherThread{false}, finishOnMain{false};
    e.tools().add({"test_slow", "Slow", "Deferred test tool", "dcc", schema::object({}), false, false,
                   [&](const Json&, ToolContext&) {
                       return ToolResult::defer(
                           [&] {
                               workOnOtherThread = std::this_thread::get_id() != mainThread;
                               std::this_thread::sleep_for(600ms);
                               workRan = true;
                           },
                           [&] {
                               finishOnMain = std::this_thread::get_id() == mainThread;
                               return ToolResult::text(workRan ? "finished after work" : "work did not run");
                           });
                   }});
    e.tools().add({"test_crash", "Crash", "Deferred tool whose work throws", "dcc", schema::object({}), false, false,
                   [&](const Json&, ToolContext&) {
                       return ToolResult::defer([] { throw std::runtime_error("boom"); }, [] { return ToolResult::text("unreachable"); });
                   }});

    // 1. In-process callers (editor, CLI, batch) get the completed result.
    ToolResult inline1 = e.callTool("test_slow", Json::object(), "test");
    CHECK_FALSE(inline1.isError);
    CHECK(inline1.content.front().text == "finished after work");
    CHECK_FALSE(workOnOtherThread);  // inline: the same thread runs both halves
    ToolResult crashed = e.callTool("test_crash", Json::object(), "test");
    CHECK(crashed.isError);
    CHECK(crashed.content.front().text.find("boom") != std::string::npos);

    // 2. Through the agent socket: work on the connection thread, main thread stays responsive.
    workRan = false;
    std::string sock = (fs::temp_directory_path() / ("sky-dcc-" + std::to_string(::getpid()) + ".sock")).string();
    REQUIRE(e.startAgentServer(sock).ok());
    auto slowFd = connectUnixSocket(sock);
    auto fastFd = connectUnixSocket(sock);
    REQUIRE(slowFd.ok());
    REQUIRE(fastFd.ok());
    std::string slowResp, fastResp;
    std::atomic<bool> slowDone{false}, fastDone{false};
    std::chrono::steady_clock::time_point slowAt, fastAt;
    std::thread slowClient([&] {
        writeAll(slowFd->get(), R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"test_slow","arguments":{}}})" "\n");
        LineReader r(slowFd->get());
        r.next(slowResp);
        slowAt = std::chrono::steady_clock::now();
        slowDone = true;
    });
    std::this_thread::sleep_for(100ms);  // let the slow call start its work
    std::thread fastClient([&] {
        writeAll(fastFd->get(), R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scene_overview","arguments":{}}})" "\n");
        LineReader r(fastFd->get());
        r.next(fastResp);
        fastAt = std::chrono::steady_clock::now();
        fastDone = true;
    });
    for (int i = 0; i < 1500 && !(slowDone && fastDone); ++i) {
        e.update(0.0);
        std::this_thread::sleep_for(2ms);
    }
    slowClient.join();
    fastClient.join();
    e.stopAgentServer();
    CHECK(workOnOtherThread);
    CHECK(finishOnMain);
    CHECK(fastAt < slowAt);  // the quick call was served while the slow tool was still working
    auto slowJson = Json::parse(slowResp);
    REQUIRE(slowJson.ok());
    CHECK(slowJson->get("result").get("content")[0].get("text").asString() == "finished after work");
    auto fastJson = Json::parse(fastResp);
    REQUIRE(fastJson.ok());
    CHECK_FALSE(fastJson->get("result").get("isError").asBool());
}

// ---------------------------------------------------------------------------------------------
// Tool surface that needs no design app
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc tools: registered in the dcc category, executable ones are open-world") {
    Project p;
    for (const char* name : {"dcc_list", "dcc_run_script", "dcc_convert", "dcc_export", "dcc_edit_asset", "dcc_generate",
                             "dcc_install_addon", "dcc_session_start", "dcc_session_status", "dcc_session_exec",
                             "dcc_session_pull_selection", "dcc_session_send", "dcc_session_stop", "dcc_receive", "dcc_cancel"}) {
        const ToolDef* def = p.engine->tools().find(name);
        REQUIRE_MESSAGE(def, name);
        CHECK_MESSAGE(def->category == "dcc", name);
        CHECK_MESSAGE(def->description.size() > 60, name);
    }
    for (const char* name : {"dcc_run_script", "dcc_convert", "dcc_export", "dcc_edit_asset", "dcc_generate", "dcc_session_exec"}) {
        CHECK_MESSAGE(p.engine->tools().find(name)->openWorld, name);
    }
    CHECK_FALSE(p.engine->tools().find("dcc_list")->mutates);

    // Argument validation and did-you-mean on the recipe list.
    ToolResult r = p.call("dcc_generate", R"({"recipe":"towr"})");
    CHECK(r.isError);
    CHECK(r.content.front().text.find("tower") != std::string::npos);
    r = p.call("dcc_session_exec", R"({"code":"1"})");
    CHECK(r.isError);  // no session
    CHECK(r.content.front().text.find("session") != std::string::npos);
    r = p.call("dcc_cancel", R"({"job":999})");
    CHECK(r.isError);
    r = p.call("dcc_run_script", R"J({"script":"print(1)","out_dir":"../escape"})J");
    CHECK(r.isError);
    CHECK(r.content.front().text.find("inside the project") != std::string::npos);
}

TEST_CASE("dcc tools: receive copies a model into the project, then updates it in place") {
    Project p;
    TempDir tmp("receive");
    // One-triangle binary glTF.
    float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::string bin(reinterpret_cast<char*>(pos), sizeof(pos));
    bin.resize(36);
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"buffers":[{"byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";
    while (json.size() % 4) json += ' ';
    auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<char*>(&v), 4); };
    std::string chunks = u32(static_cast<uint32_t>(json.size())) + "JSON" + json + u32(36) + std::string("BIN\0", 4) + bin;
    std::string glb = "glTF" + u32(2) + u32(static_cast<uint32_t>(12 + chunks.size())) + chunks;
    tmp.write("tri.glb", glb);

    ToolResult r = p.call("dcc_receive", R"({"file":")" + (tmp.dir / "tri.glb").string() + R"(","name":"Tri"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/live/Tri.glb"));
    CHECK(r.structured.get("updated").asBool() == false);
    CHECK(r.structured.contains("entity"));  // new asset: placed
    CHECK(p.engine->scene().find("Tri") != kNoEntity);
    size_t entities = p.engine->scene().entities().size();

    r = p.call("dcc_receive", R"({"file":")" + (tmp.dir / "tri.glb").string() + R"(","name":"Tri"})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("updated").asBool());
    CHECK(p.engine->scene().entities().size() == entities);  // updated in place: nothing new placed

    CHECK(p.call("dcc_receive", R"({"file":"relative.glb"})").isError);
    CHECK(p.call("dcc_receive", R"({"file":")" + (tmp.dir / "tri.glb").string() + R"(.fbx"})").isError);
    tmp.write("model.fbx", "x");
    CHECK(p.call("dcc_receive", R"({"file":")" + (tmp.dir / "model.fbx").string() + R"("})").isError);
}

// ---------------------------------------------------------------------------------------------
// Real Blender
// ---------------------------------------------------------------------------------------------

TEST_CASE("dcc blender: dcc_list reports the detected app") {
    REQUIRE_BLENDER();
    Project p;
    ToolResult r = p.call("dcc_list", "{}");
    REQUIRE_FALSE(r.isError);
    const Json& apps = r.structured.get("apps");
    REQUIRE(apps.size() >= 1);
    CHECK(apps.elements()[0].get("app").asString() == "blender");
    CHECK_FALSE(apps.elements()[0].get("version").asString().empty());
    CHECK(apps.elements()[0].get("tested").asBool());
    CHECK(r.structured.get("recipes").size() == 10);
}

TEST_CASE("dcc blender: script makes a beveled cube, imports and places it") {
    REQUIRE_BLENDER();
    Project p;
    const char* script =
        "import bpy\n"
        "from skywalker_dcc import blender as B\n"
        "B.reset_scene()\n"
        "bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1))\n"
        "cube = bpy.context.active_object\n"
        "cube.name = 'BeveledCube'\n"
        "B.bevel([cube], width=0.1, segments=3)\n"
        "sky.log('triangles', B.stats()['triangles'])\n"
        "sky.result(note='made a cube', tris=B.stats()['triangles'])\n"
        "B.export_glb(sky.out_path('cube.glb'))\n";
    Json args = Json::object({{"script", script}, {"name", "cube_test"}, {"import", true}, {"description", "a beveled cube"},
                              {"place", Json::object({{"name", "Crate"}, {"position", Json::array({1, 0, 2})}})}});
    ToolResult r = p.engine->callTool("dcc_run_script", args, "agent:Forge");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/cube_test/cube.glb"));
    CHECK(r.structured.get("result").get("note").asString() == "made a cube");
    CHECK(r.structured.get("result").get("tris").asInt() > 12);  // bevel added geometry
    CHECK(r.structured.get("log").asString().find("[sky] triangles") != std::string::npos);
    REQUIRE(r.structured.get("imported").size() == 1);
    const Json& imp = r.structured.get("imported").elements()[0];
    CHECK(imp.get("mesh").asString() == "asset:dcc/cube_test/cube.glb");
    // Real-world size kept: a 2 m cube.
    CHECK(imp.get("size").elements()[0].asFloat() == doctest::Approx(2.f).epsilon(0.01));

    EntityId id = p.engine->scene().find("Crate");
    REQUIRE(id != kNoEntity);
    const Transform* t = p.engine->scene().get<Transform>(id);
    REQUIRE(t);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(t->position.z == doctest::Approx(2.f));

    // Provenance lands in the asset's metadata.
    const AssetRecord* rec = p.engine->assets().find("dcc/cube_test/cube.glb");
    REQUIRE(rec);
    CHECK(rec->source.get("tool").asString() == "dcc_run_script");
    CHECK(rec->source.get("generator").asString().find("blender") == 0);
    CHECK(rec->source.get("by").asString() == "agent:Forge");
    CHECK(rec->description == "a beveled cube");
    bool dccTag = false;
    for (const auto& tag : rec->tags) dccTag = dccTag || tag == "dcc";
    CHECK(dccTag);

    // Placement is one undoable step.
    CHECK(p.call("history", R"({"action":"undo"})").isError == false);
    CHECK(p.engine->scene().find("Crate") == kNoEntity);
}

TEST_CASE("dcc blender: script errors come back with the Python error and the log") {
    REQUIRE_BLENDER();
    Project p;
    ToolResult r = p.call("dcc_run_script", R"J({"script":"print('before')\nraise ValueError('the model is wrong')"})J");
    REQUIRE(r.isError);
    const std::string text = r.content.front().text;
    CHECK(text.find("ValueError: the model is wrong") != std::string::npos);
    CHECK(text.find("before") != std::string::npos);

    r = p.call("dcc_run_script", R"J({"script":"import sys\nsys.exit(3)"})J");
    CHECK(r.isError);
    r = p.call("dcc_run_script", R"J({"script":"print(1)","app":"maya"})J");
    REQUIRE(r.isError);  // not installed here
    CHECK(r.content.front().text.find("SKY_MAYAPY") != std::string::npos);
}

TEST_CASE("dcc blender: Manager enforces timeouts and cancellation") {
    REQUIRE_BLENDER();
    Rig rig;
    Machine env = Machine::real();
    Manager m(env);
    AppInfo blender = *m.pick("blender", "script");
    TempDir out("out");
    JobSpec spec;
    spec.script = "import time\nprint('started')\ntime.sleep(60)\n";
    spec.projectDir = out.str();
    spec.outDir = out.str();
    spec.timeout = 2500ms;
    auto t0 = std::chrono::steady_clock::now();
    JobResult jr = m.run(spec, blender);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(jr.ok);
    CHECK(jr.process.timedOut);
    CHECK(jr.error.find("timed out") != std::string::npos);
    CHECK(took < 15.0);

    auto cancel = std::make_shared<CancelToken>();
    int id = m.beginJob("cancel-test", cancel);
    CHECK(m.runningJobs().size() == 1);
    spec.timeout = 60s;
    std::thread t([&] {
        std::this_thread::sleep_for(1500ms);
        m.cancelJob(id);
    });
    jr = m.run(spec, blender, cancel);
    t.join();
    m.endJob(id);
    CHECK(jr.process.cancelled);
    CHECK(jr.error == "cancelled");
    CHECK(m.runningJobs().size() == 0);
}

TEST_CASE("dcc blender: convert OBJ and FBX to glb, then import") {
    REQUIRE_BLENDER();
    Project p;
    // An OBJ cube with a colored material, in a "downloaded pack" folder.
    p.dir.write("downloads/pack/crate.obj",
                "mtllib crate.mtl\no Crate\nv -0.5 0 -0.5\nv 0.5 0 -0.5\nv 0.5 0 0.5\nv -0.5 0 0.5\nv -0.5 1 -0.5\nv 0.5 1 -0.5\nv 0.5 1 0.5\nv -0.5 1 0.5\n"
                "usemtl Wood\nf 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf 4 8 5 1\n");
    p.dir.write("downloads/pack/crate.mtl", "newmtl Wood\nKd 0.6 0.4 0.2\nNs 50\n");

    ToolResult r = p.call("dcc_convert", R"({"path":"downloads/pack/crate.obj"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "downloads/pack/crate.glb"));
    REQUIRE(r.structured.get("imported").size() == 1);
    const Json& imp = r.structured.get("imported").elements()[0];
    CHECK(imp.get("mesh").asString() == "asset:downloads/pack/crate.glb");
    CHECK(imp.get("size").elements()[1].asFloat() == doctest::Approx(1.f).epsilon(0.02));  // 1 m tall, real size kept
    CHECK(imp.contains("material"));
    auto mat = p.engine->resolveMaterial(imp.get("material").asString());
    REQUIRE(mat);
    CHECK(mat->color.x > mat->color.z);  // brownish: the OBJ material survived

    // Make an FBX with Blender itself (a hand-written FBX is not practical), then convert the folder.
    ToolResult mk = p.call("dcc_run_script", R"({"name":"fbxmaker","script":"import bpy\nfrom skywalker_dcc import blender as B\nB.reset_scene()\nbpy.ops.mesh.primitive_cylinder_add(radius=0.5, depth=2, location=(0,0,1))\nbpy.context.active_object.name='Pillar'\nB.export_fbx(sky.project_path('downloads/fbxpack/pillar.fbx'))\n"})");
    INFO(mk.content.front().text);
    REQUIRE_FALSE(mk.isError);
    REQUIRE(fs::exists(p.dir.dir / "downloads/fbxpack/pillar.fbx"));
    // Scale 0.5, recentering and a skip-existing second run.
    r = p.call("dcc_convert", R"({"path":"downloads/fbxpack","scale":0.5,"recenter":"bottom","place":{"position":[3,0,0]}})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "downloads/fbxpack/pillar.glb"));
    const Json& pil = r.structured.get("imported").elements()[0];
    CHECK(pil.get("size").elements()[1].asFloat() == doctest::Approx(1.f).epsilon(0.03));  // 2 m * 0.5
    CHECK(pil.contains("entity"));
    r = p.call("dcc_convert", R"({"path":"downloads/fbxpack","skip_existing":true,"import":false})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("skipped").size() == 1);

    // Unsupported and empty inputs are explained.
    r = p.call("dcc_convert", R"({"path":"downloads/pack/crate.mtl"})");
    CHECK(r.isError);
    r = p.call("dcc_convert", R"({"path":"downloads/nothing"})");
    CHECK(r.isError);
}

TEST_CASE("dcc blender: generate recipes, then round-trip edit as a new version") {
    REQUIRE_BLENDER();
    Project p;
    ToolResult r = p.call("dcc_generate", R"({"recipe":"rock","params":{"radius":1.5,"seed":4},"name":"Boulder","description":"mossy boulder"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/generated/Boulder.glb"));
    int tris = static_cast<int>(r.structured.get("imported").elements()[0].get("triangles").asInt());
    CHECK(tris > 500);

    // Same seed, same model; unknown parameters are rejected with the valid list.
    ToolResult again = p.call("dcc_generate", R"({"recipe":"rock","params":{"radius":1.5,"seed":4},"name":"Boulder2"})");
    REQUIRE_FALSE(again.isError);
    CHECK(again.structured.get("imported").elements()[0].get("triangles").asInt() == tris);
    ToolResult badParam = p.call("dcc_generate", R"({"recipe":"rock","params":{"radiuss":1}})");
    REQUIRE(badParam.isError);
    CHECK(badParam.content.front().text.find("radius") != std::string::npos);

    // Place the original, then decimate it into a new version and re-point the scene at it.
    ToolResult placed = p.call("asset_import", R"({"path":"dcc/generated/Boulder.glb","create_entity":"BoulderEntity"})");
    REQUIRE_FALSE(placed.isError);
    // The source asset came from somewhere with a license: derived versions keep that trail.
    REQUIRE_FALSE(p.call("asset_tag", R"J({"asset":"dcc/generated/Boulder.glb","tags":["scenery","dcc"],"source":{"license":"CC-BY-4.0","author":"Ada"}})J").isError);
    r = p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Boulder.glb","ops":[{"op":"decimate","ratio":0.25},{"op":"shade_smooth"}],"update_references":true})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("asset").asString() == "dcc/generated/Boulder_v2.glb");
    CHECK(fs::exists(p.dir.dir / "dcc/generated/Boulder.glb"));  // original untouched
    int after = static_cast<int>(r.structured.get("after").get("triangles").asInt());
    CHECK(after < tris / 2);
    CHECK(r.structured.get("rewiredReferences").asInt() == 1);
    const AssetRecord* v2 = p.engine->assets().find("dcc/generated/Boulder_v2.glb");
    REQUIRE(v2);
    CHECK(v2->source.get("derivedFrom").asString() == "dcc/generated/Boulder.glb");
    CHECK(v2->source.get("tool").asString() == "dcc_edit_asset");
    CHECK(v2->source.get("version").asInt() == 2);
    CHECK(v2->source.get("ops").size() == 2);
    CHECK(v2->source.get("license").asString() == "CC-BY-4.0");
    CHECK(v2->source.get("author").asString() == "Ada");
    std::set<std::string> v2tags(v2->tags.begin(), v2->tags.end());
    CHECK(v2tags.count("scenery"));
    CHECK(v2tags.count("edited"));
    const MeshRenderer* mr = p.engine->scene().get<MeshRenderer>(p.engine->scene().find("BoulderEntity"));
    REQUIRE(mr);
    CHECK(mr->mesh == "asset:dcc/generated/Boulder_v2.glb");

    // A second edit makes v3 and a custom script sees the asset's objects.
    r = p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Boulder.glb","script":"sky.result(count=len(objects), names=[o.name for o in objects])\nfor o in objects:\n    o.scale = (2, 2, 2)\nB.bake_transforms(objects)\n"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("asset").asString() == "dcc/generated/Boulder_v3.glb");
    CHECK(r.structured.get("result").get("count").asInt() == 1);

    // replace mode overwrites a .glb and keeps a backup.
    r = p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Boulder2.glb","ops":[{"op":"decimate","ratio":0.5}],"mode":"replace"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("asset").asString() == "dcc/generated/Boulder2.glb");
    REQUIRE(r.structured.contains("backup"));
    CHECK(fs::exists(p.dir.dir / r.structured.get("backup").asString()));
    CHECK(p.engine->assets().find("dcc/generated/Boulder2.glb")->size > 0);

    // Errors: missing ops, unknown op, unknown asset.
    CHECK(p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Boulder.glb"})").isError);
    ToolResult unknownOp = p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Boulder.glb","ops":[{"op":"explode"}]})");
    REQUIRE(unknownOp.isError);
    CHECK(unknownOp.content.front().text.find("unknown op") != std::string::npos);
    CHECK(p.call("dcc_edit_asset", R"({"asset":"dcc/generated/Bolder.glb","ops":[{"op":"triangulate"}]})").isError);
}

TEST_CASE("dcc blender: every procedural recipe builds a sane model") {
    REQUIRE_BLENDER();
    Project p;
    struct Case {
        const char* recipe;
        const char* params;
        float minHeight, maxHeight;
    };
    const Case cases[] = {{"building", R"J({"floors":2,"wall_style":"brick"})J", 5.5f, 11.f},
                          {"tower", R"J({"height":10,"ruin":0.5,"seed":2})J", 3.f, 11.5f},
                          {"wall", R"J({"length":6,"height":2})J", 1.0f, 2.2f},
                          {"rock", R"J({"radius":1})J", 0.5f, 2.5f},
                          {"stairs", R"J({"steps":8})J", 1.2f, 1.6f},
                          {"arch", R"J({"height":3.5})J", 3.0f, 4.0f},
                          {"fence", R"J({"length":4})J", 0.9f, 1.4f},
                          {"column", R"J({"height":3})J", 2.9f, 3.1f},
                          {"barrel", "{}", 0.9f, 1.0f},
                          {"terrain_chunk", R"J({"size":12,"resolution":16,"height":2})J", 0.5f, 6.f}};
    // One Blender launch builds them all (the python names must match the tool's recipe list).
    Json list = Json::array();
    for (const auto& c : cases) list.push(Json::object({{"recipe", c.recipe}, {"params", Json::parse(c.params).value()}}));
    const char* script =
        "import json\n"
        "from skywalker_dcc import blender as B, procedural as P\n"
        "out = []\n"
        "for case in json.loads(sky.ARGS[0]):\n"
        "    B.reset_scene()\n"
        "    P.generate(case['recipe'], **case['params'])\n"
        "    st = B.stats()\n"
        "    B.export_glb(sky.out_path(case['recipe'] + '.glb'))\n"
        "    st['recipe'] = case['recipe']\n"
        "    out.append(st)\n"
        "sky.result(models=out, recipes=[r['name'] for r in P.describe()])\n";
    ToolResult r = p.engine->callTool("dcc_run_script", Json::object({{"script", script}, {"args", Json::array({list.dump()})}, {"name", "recipes"}}), "test");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    const Json& models = r.structured.get("result").get("models");
    REQUIRE(models.size() == std::size(cases));
    for (size_t i = 0; i < std::size(cases); ++i) {
        const Json& m = models.elements()[i];
        const Case& c = cases[i];
        float height = m.get("size_m").elements()[1].asFloat();
        CHECK_MESSAGE(height >= c.minHeight, c.recipe);
        CHECK_MESSAGE(height <= c.maxHeight, c.recipe);
        CHECK_MESSAGE(m.get("triangles").asInt() > 8, c.recipe);
        // Everything stands on the ground (origin at the feet).
        CHECK_MESSAGE(m.get("min_m").elements()[1].asFloat() >= -0.05f, c.recipe);
    }
    // The tool's recipe list and the python library agree.
    ToolResult list2 = p.call("dcc_list", "{}");
    std::set<std::string> cpp, py;
    for (const auto& n : list2.structured.get("recipes").elements()) cpp.insert(n.asString());
    for (const auto& n : r.structured.get("result").get("recipes").elements()) py.insert(n.asString());
    CHECK(cpp == py);
    // And the tool path works for a custom script on top of a recipe.
    ToolResult g = p.call("dcc_generate", R"J({"recipe":"fence","script":"for o in objects:\n    o.location.x += 5\n","name":"Moved"})J");
    INFO(g.content.front().text);
    REQUIRE_FALSE(g.isError);
}

TEST_CASE("dcc blender: export collections of a .blend, collection-per-asset") {
    REQUIRE_BLENDER();
    Project p;
    const char* maker =
        "import bpy\n"
        "from skywalker_dcc import blender as B\n"
        "B.reset_scene()\n"
        "sc = bpy.context.scene\n"
        "sc.unit_settings.scale_length = 0.01\n"   // authored in centimeters
        "def make(name, size, loc):\n"
        "    coll = bpy.data.collections.new(name)\n"
        "    sc.collection.children.link(coll)\n"
        "    bpy.ops.mesh.primitive_cube_add(size=size, location=loc)\n"
        "    o = bpy.context.active_object\n"
        "    o.name = name + '_mesh'\n"
        "    for c in o.users_collection: c.objects.unlink(o)\n"
        "    coll.objects.link(o)\n"
        "make('Crate', 100, (500, 0, 50))\n"       // 1 m crate in cm units
        "make('Pillar', 200, (-500, 0, 100))\n"    // 2 m
        "import os\n"
        "os.makedirs(sky.project_path('assets'), exist_ok=True)\n"
        "bpy.ops.wm.save_as_mainfile(filepath=sky.project_path('assets/kit.blend'))\n";
    ToolResult mk = p.engine->callTool("dcc_run_script", Json::object({{"script", maker}, {"name", "kitmaker"}}), "test");
    INFO(mk.content.front().text);
    REQUIRE_FALSE(mk.isError);
    REQUIRE(fs::exists(p.dir.dir / "assets/kit.blend"));

    ToolResult r = p.call("dcc_export", R"({"blend":"assets/kit.blend","origin":"bottom_center","place":{"spacing":4}})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/kit/Crate.glb"));
    CHECK(fs::exists(p.dir.dir / "dcc/kit/Pillar.glb"));
    REQUIRE(r.structured.get("imported").size() == 2);
    // The unit scale is honored (cm scene exported in meters) and origins are re-centered.
    for (const auto& imp : r.structured.get("imported").elements()) {
        std::string file = imp.get("file").asString();
        float expected = file.find("Crate") != std::string::npos ? 1.f : 2.f;
        CHECK_MESSAGE(imp.get("size").elements()[1].asFloat() == doctest::Approx(expected).epsilon(0.02), file);
        CHECK_MESSAGE(imp.get("bounds").get("min").elements()[1].asFloat() == doctest::Approx(0.f).epsilon(0.02), file);
        CHECK(imp.contains("entity"));
    }

    // Explicit selection and an unknown collection.
    r = p.call("dcc_export", R"({"blend":"assets/kit.blend","collections":["Crate"],"import":false,"out_dir":"dcc/only"})");
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/only/Crate.glb"));
    CHECK_FALSE(fs::exists(p.dir.dir / "dcc/only/Pillar.glb"));
    r = p.call("dcc_export", R"({"blend":"assets/kit.blend","collections":["Crat"]})");
    REQUIRE(r.isError);
    CHECK(r.content.front().text.find("Crat") != std::string::npos);
}

TEST_CASE("dcc blender: install the add-on, then drive a live headless session") {
    REQUIRE_BLENDER();
    Project p;
    TempDir scripts("blender-scripts"), config("blender-config");

    ToolResult r = p.call("dcc_install_addon", R"({"scripts_dir":")" + scripts.str() + R"(","config_dir":")" + config.str() + R"("})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(scripts.dir / "addons/skywalker_bridge/__init__.py"));
    CHECK(r.structured.get("enabled").asBool());  // Blender found, loaded and enabled it

    // No session yet.
    r = p.call("dcc_session_status", "{}");
    CHECK_FALSE(r.structured.get("connected").asBool());

    r = p.call("dcc_session_start", "{}");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("mode").asString() == "headless");
    r = p.call("dcc_session_status", "{}");
    CHECK(r.structured.get("connected").asBool());
    CHECK(r.structured.get("objects").asInt() == 0);  // agents start from an empty scene
    CHECK(fs::status(p.rig.state.dir / "blender.json").permissions() == (fs::perms::owner_read | fs::perms::owner_write));

    // State persists between calls; errors carry the agent's own traceback.
    r = p.call("dcc_session_exec", R"J({"code":"import bpy\nbpy.ops.mesh.primitive_uv_sphere_add(radius=1, location=(0,0,1))\nball = bpy.context.active_object\nball.name = 'Ball'\nball.select_set(True)\nprint('sphere made')"})J");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("stdout").asString() == "sphere made\n");
    r = p.call("dcc_session_exec", R"({"code":"ball.dimensions.x"})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("result").asNumber() == doctest::Approx(2.0).epsilon(0.01));
    r = p.call("dcc_session_exec", R"({"code":"x = 1\nundefined_name + x"})");
    REQUIRE(r.isError);
    CHECK(r.content.front().text.find("NameError") != std::string::npos);
    CHECK(r.content.front().text.find("undefined_name + x") != std::string::npos);  // the offending line of the agent's code

    // Pull the selection into the project (placed), edit in Blender, pull again (updated in place).
    r = p.call("dcc_session_pull_selection", R"({"origin":"bottom_center"})");
    INFO(r.content.front().text);
    REQUIRE_FALSE(r.isError);
    CHECK(fs::exists(p.dir.dir / "dcc/live/Ball.glb"));
    EntityId ball = p.engine->scene().find("Ball");
    REQUIRE(ball != kNoEntity);
    uint64_t sizeBefore = p.engine->assets().find("dcc/live/Ball.glb")->size;
    size_t entityCount = p.engine->scene().entities().size();
    REQUIRE_FALSE(p.call("dcc_session_exec", R"J({"code":"B.decimate([ball], 0.2)"})J").isError);
    r = p.call("dcc_session_pull_selection", R"({"origin":"bottom_center"})");
    REQUIRE_FALSE(r.isError);
    CHECK(r.structured.get("updated").asBool());
    CHECK(p.engine->scene().entities().size() == entityCount);
    CHECK(p.engine->assets().find("dcc/live/Ball.glb")->size < sizeBefore);

    // Send a project asset into the session.
    REQUIRE_FALSE(p.call("dcc_generate", R"({"recipe":"barrel","name":"SentBarrel"})").isError);
    r = p.call("dcc_session_send", R"({"asset":"dcc/generated/SentBarrel.glb"})");
    REQUIRE_FALSE(r.isError);
    r = p.call("dcc_session_exec", R"J({"code":"sorted(o.name for o in bpy.data.objects)"})J");
    CHECK(r.structured.get("result").dump().find("SentBarrel") != std::string::npos);

    // A wrong request shape is reported, not fatal.
    CHECK(p.call("dcc_session_send", R"({"asset":"dcc/generated/nope.glb"})").isError);

    r = p.call("dcc_session_stop", "{}");
    REQUIRE_FALSE(r.isError);
    CHECK_FALSE(p.call("dcc_session_status", "{}").structured.get("connected").asBool());
    CHECK(p.call("dcc_session_exec", R"({"code":"1"})").isError);
}

TEST_CASE("dcc blender: concurrent agents are served while Blender works") {
    REQUIRE_BLENDER();
    Project p;
    std::string sock = (fs::temp_directory_path() / ("sky-dcc-live-" + std::to_string(::getpid()) + ".sock")).string();
    REQUIRE(p.engine->startAgentServer(sock).ok());
    auto slow = connectUnixSocket(sock);
    auto fast = connectUnixSocket(sock);
    REQUIRE(slow.ok());
    REQUIRE(fast.ok());
    std::string slowResp, fastResp;
    std::atomic<int> done{0};
    std::chrono::steady_clock::time_point slowAt, fastAt;
    std::thread a([&] {
        // Blender sleeps for 1.5 s inside the script.
        writeAll(slow->get(), R"J({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"dcc_run_script","arguments":{"script":"import time\ntime.sleep(1.5)\nsky.result(ok=True)"}}})J" "\n");
        LineReader r(slow->get());
        r.next(slowResp);
        slowAt = std::chrono::steady_clock::now();
        ++done;
    });
    std::this_thread::sleep_for(500ms);
    std::thread b([&] {
        writeAll(fast->get(), R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scene_overview","arguments":{}}})" "\n");
        LineReader r(fast->get());
        r.next(fastResp);
        fastAt = std::chrono::steady_clock::now();
        ++done;
    });
    for (int i = 0; i < 4000 && done < 2; ++i) {
        p.engine->update(0.0);
        std::this_thread::sleep_for(2ms);
    }
    a.join();
    b.join();
    p.engine->stopAgentServer();
    CHECK(fastAt < slowAt);
    auto sj = Json::parse(slowResp);
    REQUIRE(sj.ok());
    CHECK_FALSE(sj->get("result").get("isError").asBool());
    CHECK(sj->get("result").get("structuredContent").get("result").get("ok").asBool());
}

TEST_CASE("dcc blender: stopping the agent server cancels running design-app jobs") {
    REQUIRE_BLENDER();
    Project p;
    std::string sock = (fs::temp_directory_path() / ("sky-dcc-stop-" + std::to_string(::getpid()) + ".sock")).string();
    REQUIRE(p.engine->startAgentServer(sock).ok());
    auto fd = connectUnixSocket(sock);
    REQUIRE(fd.ok());
    std::string resp;
    std::thread client([&] {
        writeAll(fd->get(), R"J({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"dcc_run_script","arguments":{"script":"import time\ntime.sleep(60)"}}})J" "\n");
        LineReader r(fd->get());
        r.next(resp);
    });
    // Pump until the job is registered (its work has started on the connection thread).
    bool running = false;
    for (int i = 0; i < 1500 && !running; ++i) {
        p.engine->update(0.0);
        std::this_thread::sleep_for(2ms);
        ToolResult list = p.engine->callTool("dcc_list", Json::object(), "test");
        running = list.structured.get("jobs").size() > 0;
    }
    REQUIRE(running);
    std::this_thread::sleep_for(800ms);  // Blender is up and sleeping
    auto t0 = std::chrono::steady_clock::now();
    p.engine->stopAgentServer();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    client.join();
    CHECK(took < 10.0);
    ToolResult after = p.engine->callTool("dcc_list", Json::object(), "test");
    CHECK(after.structured.get("jobs").size() == 0);
}
