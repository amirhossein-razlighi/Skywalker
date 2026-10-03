#pragma once
// DCC manager: owns detection results, runs headless design-app jobs and talks to live
// sessions. Engine-independent: it never touches the scene, so everything here is safe to call
// from a worker thread (the tools in agent/DccTools.cpp apply results on the main thread).
//
// A *job* is: stage a job folder (bootstrap stub, the agent's script, parameters), start the
// app headless with SKY_* environment variables, capture its output, collect the files it
// produced and the structured result the script recorded with sky.result().
//
// Files shipped inside the engine (embedded at build time from integrations/): the Python
// helper library `skywalker_dcc` and the Blender add-on `skywalker_bridge`. ensureRuntime()
// writes them under ~/.skywalker/dcc/runtime/<hash> so jobs, sessions and the add-on installer
// all use the exact version that matches this engine.

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/dcc/Apps.h"
#include "skywalker/dcc/Process.h"

namespace sky::dcc {

struct EmbeddedFile {
    const char* path;     // "lib/skywalker_dcc/blender.py" or "addon/skywalker_bridge/__init__.py"
    const char* content;
};
const std::vector<EmbeddedFile>& embeddedFiles();

struct Machine {
    Host host;
    std::string stateDir;  // ~/.skywalker/dcc : session files, runtime copies, paths.json
    static Machine real();
};

struct JobSpec {
    std::string label;      // for logs and job listings ("convert", "run_script")
    std::string task = "script";  // script | convert | export | edit | generate
    std::string script;     // Python source: the agent's script (task=script) or an extra script (edit/generate)
    Json params = Json::object();  // task parameters (params.json)
    std::string inputFile;  // optional file the app opens first / scripts read (SKY_INPUT)
    std::string projectDir; // SKY_PROJECT
    std::string outDir;     // SKY_OUT (created); new/changed files here are reported
    std::vector<std::string> args;  // SKY_ARGS
    std::vector<std::pair<std::string, std::string>> env;  // extra environment
    std::chrono::milliseconds timeout{std::chrono::minutes(5)};
    bool userPrefs = false;
};

struct ProducedFile {
    std::string path;  // absolute
    uint64_t size = 0;
};

struct JobResult {
    bool ok = false;
    std::string error;  // short, agent-readable reason when !ok
    std::string hint;
    ProcessResult process;
    Json result = Json::object();  // what the script recorded with sky.result()
    std::vector<ProducedFile> files;
    std::string log;  // app output without startup noise
    double seconds = 0;
};

class Manager {
public:
    explicit Manager(Machine env = Machine::real());
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    const Machine& machine() const { return machine_; }

    // --- Detection --------------------------------------------------------------------------
    /// Detected apps (cached; `refresh` rescans).
    std::vector<AppInfo> apps(bool refresh = false);
    /// The app to use: `name` (empty = the best one that has `capability`).
    Result<AppInfo> pick(const std::string& name, const std::string& capability);

    // --- Headless jobs ----------------------------------------------------------------------
    JobResult run(const JobSpec& spec, const AppInfo& app, const std::shared_ptr<CancelToken>& cancel = nullptr);

    /// Registers a running job for cancellation / listing; returns its id.
    int beginJob(const std::string& label, std::shared_ptr<CancelToken> cancel);
    void endJob(int id);
    bool cancelJob(int id);
    Json runningJobs() const;

    // --- Runtime files ---------------------------------------------------------------------
    /// Directory containing skywalker_dcc/ and skywalker_bridge/ for this engine version.
    Result<std::string> ensureRuntime();
    /// Copies the Blender add-on (with the helper library) into `scriptsDir/addons`.
    Result<std::string> installAddon(const std::string& scriptsDir);

    // --- Live sessions ---------------------------------------------------------------------
    struct SessionInfo {
        std::string host = "127.0.0.1";
        int port = 0;
        std::string token;
        long pid = 0;
        std::string app = "blender";
        std::string version;
        std::string file;
        std::string mode;  // "ui" | "headless"
    };
    /// The advertised Blender session, if its process is alive.
    Result<SessionInfo> session() const;
    /// Sends one request to the add-on's server and waits for the response.
    Result<Json> sessionCall(const SessionInfo& s, const std::string& method, const Json& params,
                             std::chrono::milliseconds timeout, const std::shared_ptr<CancelToken>& cancel = nullptr) const;
    /// Starts Blender with the bridge running (`headless` = no window). `open` is a model file to
    /// load first. Returns once the session answers.
    Result<SessionInfo> startSession(const AppInfo& blender, bool headless, const std::string& open,
                                     const std::string& projectDir, std::chrono::milliseconds wait,
                                     const std::shared_ptr<CancelToken>& cancel = nullptr);
    Status stopSession();

    std::string sessionFilePath() const;
    std::string logPath(const std::string& name) const;

private:
    Machine machine_;
    mutable std::mutex mutex_;
    std::vector<AppInfo> apps_;
    bool detected_ = false;
    std::string runtimeDir_;

    struct Job {
        int id;
        std::string label;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<CancelToken> cancel;
    };
    std::vector<Job> jobs_;
    int nextJob_ = 1;
    std::vector<long> startedHeadless_;  // sessions this manager launched without a window
};

/// Filters Blender/Maya start-up chatter and trims the log to the last `maxChars` characters.
std::string tidyLog(const std::string& text, size_t maxChars);
/// The last Python exception line in `text` ("ValueError: ..."), or "".
std::string lastPythonError(const std::string& text);

}  // namespace sky::dcc
