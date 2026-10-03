#pragma once
// DCC application adapters: detection, capabilities and headless-run command lines for the
// design apps an agent can drive (Blender, Maya, Houdini, 3ds Max).
//
// Detection looks, in order, at: an environment override (SKY_BLENDER, SKY_MAYAPY,
// SKY_HOUDINI_HYTHON, SKY_3DSMAX_BATCH), the user's own config (~/.skywalker/dcc/paths.json),
// the standard install locations of each OS, and PATH. When several installs exist the
// newest one wins; the others are listed as alternatives.
//
// Only Blender is exercised by the test-suite (it is what the project's CI machine has). The
// Maya, Houdini and 3ds Max adapters follow each vendor's documented headless interface
// (mayapy, hython, 3dsmaxbatch) and are reported with `tested: false`.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/dcc/Process.h"

namespace sky::dcc {

enum class AppId { Blender, Maya, Houdini, Max };

const char* toString(AppId id);
/// Accepts the id and common aliases ("3dsmax", "mayapy", "hython").
std::optional<AppId> appFromString(std::string_view s);
std::vector<AppId> allApps();

/// The machine as detection sees it. Injectable so tests can describe fake installs.
struct Host {
    std::string os;    // "macos" | "windows" | "linux"
    std::string home;  // home directory
    std::function<std::string(const std::string&)> getenv;  // "" when unset
    std::function<bool(const std::string&)> isExecutable;   // regular file with the execute bit
    std::function<std::vector<std::string>(const std::string&)> listDir;  // entry names, sorted; {} if missing
    std::vector<std::string> pathDirs;                                    // PATH entries

    static Host real();
};

struct AppInfo {
    AppId id = AppId::Blender;
    std::string name;        // "Blender"
    std::string executable;  // what we run (blender / mayapy / hython / 3dsmaxbatch)
    std::string version;     // "3.5.1", "2024", "20.5.370"; empty when unknown
    std::string foundVia;    // "env:SKY_BLENDER" | "config" | "standard-location" | "PATH"
    std::vector<std::string> capabilities;
    bool tested = false;
    std::vector<std::string> alternatives;  // other installs found (executable paths)
    std::string note;                       // problems (e.g. an override that does not exist)

    Json toJson() const;
    bool has(std::string_view capability) const;
};

/// A headless run: the app starts, executes the bootstrap script (which then runs the agent's
/// script), and exits.
struct ScriptRun {
    std::string bootstrapPath;     // Python file passed to the app
    std::string inputFile;         // optional scene to open first (.blend, .ma/.mb, .hip, .max)
    std::vector<std::string> args; // extra arguments for the script
    bool userPrefs = false;        // Blender: load the user's preferences/add-ons instead of factory settings
};

class Adapter {
public:
    virtual ~Adapter() = default;
    virtual AppId id() const = 0;
    virtual std::string displayName() const = 0;
    virtual std::string envVar() const = 0;
    /// Executable names to look for on PATH.
    virtual std::vector<std::string> pathNames(const std::string& os) const = 0;
    /// Executables in the standard install locations of `host.os` (best effort, may not exist).
    virtual std::vector<std::string> standardLocations(const Host& host) const = 0;
    /// Version encoded in an install path ("Maya2024", "Houdini20.5.370"), or "".
    virtual std::string versionFromPath(const std::string& executable) const = 0;
    virtual std::vector<std::string> capabilities() const = 0;
    virtual bool tested() const { return false; }
    /// Command line for a headless run.
    virtual ProcessSpec scriptCommand(const AppInfo& app, const ScriptRun& run) const = 0;
    /// Command line that prints the version, if the app supports it ({} = rely on the path).
    virtual std::vector<std::string> versionArgs() const { return {}; }
    /// Extracts the version from the output of versionArgs().
    virtual std::string parseVersion(const std::string&) const { return {}; }
};

const Adapter& adapterFor(AppId id);

/// Orders dotted numeric versions ("3.5.1" < "3.10"); unknown/empty sorts lowest.
int compareVersions(std::string_view a, std::string_view b);

/// Finds installed apps. `configured` maps an app to an explicit executable path (from the
/// user's config file). `probe`, when given, is asked for the version of apps whose version
/// is not encoded in the path.
std::vector<AppInfo> detectApps(const Host& host, const std::map<AppId, std::string>& configured = {},
                                const std::function<std::string(const AppInfo&)>& probe = {});

}  // namespace sky::dcc
