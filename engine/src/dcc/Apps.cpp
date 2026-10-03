#include "skywalker/dcc/Apps.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>

#include <unistd.h>

#include "skywalker/core/Strings.h"

namespace sky::dcc {

namespace fs = std::filesystem;

const char* toString(AppId id) {
    switch (id) {
        case AppId::Blender: return "blender";
        case AppId::Maya: return "maya";
        case AppId::Houdini: return "houdini";
        case AppId::Max: return "3dsmax";
    }
    return "blender";
}

std::optional<AppId> appFromString(std::string_view s) {
    std::string l = str::lower(s);
    if (l == "blender") return AppId::Blender;
    if (l == "maya" || l == "mayapy") return AppId::Maya;
    if (l == "houdini" || l == "hython") return AppId::Houdini;
    if (l == "3dsmax" || l == "3ds max" || l == "max" || l == "3dsmaxbatch") return AppId::Max;
    return std::nullopt;
}

std::vector<AppId> allApps() { return {AppId::Blender, AppId::Maya, AppId::Houdini, AppId::Max}; }

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

Host Host::real() {
    Host h;
#if defined(__APPLE__)
    h.os = "macos";
#elif defined(_WIN32)
    h.os = "windows";
#else
    h.os = "linux";
#endif
    auto env = [](const std::string& k) -> std::string {
        const char* v = std::getenv(k.c_str());
        return v ? v : "";
    };
    h.home = env(h.os == "windows" ? "USERPROFILE" : "HOME");
    h.getenv = env;
    h.isExecutable = [](const std::string& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
    };
    h.listDir = [](const std::string& dir) {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            names.push_back(it->path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    std::string path = env("PATH");
    char sep = h.os == "windows" ? ';' : ':';
    for (const auto& d : str::split(path, sep)) {
        if (!d.empty()) h.pathDirs.push_back(d);
    }
    return h;
}

namespace {

std::string join(const std::string& os, std::initializer_list<std::string> parts) {
    const char sep = os == "windows" ? '\\' : '/';
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty() && out.back() != sep) out += sep;
        out += p;
    }
    return out;
}

bool startsWithNoCase(const std::string& s, const std::string& prefix) {
    return str::startsWith(str::lower(s), str::lower(prefix));
}

/// Directories under `dir` whose name starts with `prefix` (case-insensitive).
std::vector<std::string> entriesWithPrefix(const Host& h, const std::string& dir, const std::string& prefix) {
    std::vector<std::string> out;
    for (const auto& name : h.listDir(dir)) {
        if (startsWithNoCase(name, prefix)) out.push_back(join(h.os, {dir, name}));
    }
    return out;
}

std::string programFiles(const Host& h) {
    std::string pf = h.getenv("ProgramFiles");
    return pf.empty() ? "C:\\Program Files" : pf;
}

/// The digits-and-dots that follow `keyword` in some path component: ("Houdini20.5.370",
/// "houdini") -> "20.5.370", ("3ds Max 2025", "3ds max") -> "2025".
std::string versionAfter(const std::string& path, const std::string& keyword) {
    std::string lower = str::lower(path);
    size_t from = 0;
    while (true) {
        size_t at = lower.find(keyword, from);
        if (at == std::string::npos) return "";
        size_t i = at + keyword.size();
        while (i < lower.size() && (lower[i] == ' ' || lower[i] == '_' || lower[i] == '-')) ++i;
        size_t j = i;
        while (j < lower.size() && (std::isdigit(static_cast<unsigned char>(lower[j])) || lower[j] == '.')) ++j;
        while (j > i && lower[j - 1] == '.') --j;
        if (j > i) return lower.substr(i, j - i);
        from = at + keyword.size();
    }
}

// --- Blender ------------------------------------------------------------------------

class BlenderAdapter final : public Adapter {
public:
    AppId id() const override { return AppId::Blender; }
    std::string displayName() const override { return "Blender"; }
    std::string envVar() const override { return "SKY_BLENDER"; }
    std::vector<std::string> pathNames(const std::string& os) const override {
        return os == "windows" ? std::vector<std::string>{"blender.exe"} : std::vector<std::string>{"blender", "Blender"};
    }
    std::vector<std::string> standardLocations(const Host& h) const override {
        std::vector<std::string> out;
        if (h.os == "macos") {
            for (const std::string& apps : {std::string("/Applications"), join(h.os, {h.home, "Applications"})}) {
                for (const auto& app : entriesWithPrefix(h, apps, "Blender")) {
                    if (app.size() > 4 && app.compare(app.size() - 4, 4, ".app") == 0) {
                        out.push_back(join(h.os, {app, "Contents", "MacOS", "Blender"}));
                    }
                }
            }
            out.push_back(join(h.os, {h.home, "Library/Application Support/Steam/steamapps/common/Blender/Blender.app/Contents/MacOS/Blender"}));
        } else if (h.os == "windows") {
            for (const std::string& pf : {programFiles(h), std::string("C:\\Program Files (x86)")}) {
                for (const auto& d : entriesWithPrefix(h, join(h.os, {pf, "Blender Foundation"}), "Blender")) {
                    out.push_back(join(h.os, {d, "blender.exe"}));
                }
                out.push_back(join(h.os, {pf, "Steam\\steamapps\\common\\Blender\\blender.exe"}));
            }
        } else {
            for (const char* p : {"/usr/bin/blender", "/usr/local/bin/blender", "/snap/bin/blender"}) out.push_back(p);
            for (const auto& d : entriesWithPrefix(h, "/opt", "blender")) out.push_back(join(h.os, {d, "blender"}));
            for (const auto& d : entriesWithPrefix(h, h.home, "blender")) out.push_back(join(h.os, {d, "blender"}));
        }
        return out;
    }
    std::string versionFromPath(const std::string& exe) const override {
        // "Blender 3.6" folders on Windows; macOS app bundles carry no version (probed instead).
        std::string lower = str::lower(exe);
        size_t at = lower.rfind("blender ");
        if (at != std::string::npos) return versionAfter(exe.substr(at), "blender");
        return "";
    }
    std::vector<std::string> capabilities() const override {
        return {"script",     "convert",    "export_glb", "export_fbx", "export_obj", "export_usd",
                "edit_asset", "generate",   "bake",       "live_session", "headless"};
    }
    bool tested() const override { return true; }
    ProcessSpec scriptCommand(const AppInfo& app, const ScriptRun& run) const override {
        ProcessSpec p;
        p.executable = app.executable;
        p.args = {"--background"};
        // Factory settings make runs reproducible (no user add-ons or startup file); the add-ons
        // the bridge needs (glTF, FBX) are bundled and enabled by the helper library.
        if (!run.userPrefs) p.args.push_back("--factory-startup");
        // Never auto-run scripts embedded in a .blend that came from somewhere else.
        p.args.push_back("--disable-autoexec");
        p.args.push_back("--python-exit-code");
        p.args.push_back("1");
        if (!run.inputFile.empty() && str::lower(run.inputFile).size() > 6 &&
            str::lower(run.inputFile).compare(str::lower(run.inputFile).size() - 6, 6, ".blend") == 0) {
            p.args.push_back(run.inputFile);
        }
        p.args.push_back("--python");
        p.args.push_back(run.bootstrapPath);
        if (!run.args.empty()) {
            p.args.push_back("--");
            for (const auto& a : run.args) p.args.push_back(a);
        }
        return p;
    }
    std::vector<std::string> versionArgs() const override { return {"--version"}; }
    std::string parseVersion(const std::string& output) const override {
        // "Blender 3.5.1\n\tbuild date: ..."
        std::string line = output.substr(0, output.find('\n'));
        size_t at = line.find("Blender ");
        if (at == std::string::npos) return "";
        std::string v = str::trim(line.substr(at + 8));
        size_t end = 0;
        while (end < v.size() && (std::isdigit(static_cast<unsigned char>(v[end])) || v[end] == '.')) ++end;
        return v.substr(0, end);
    }
};

// --- Maya ---------------------------------------------------------------------------

class MayaAdapter final : public Adapter {
public:
    AppId id() const override { return AppId::Maya; }
    std::string displayName() const override { return "Autodesk Maya"; }
    std::string envVar() const override { return "SKY_MAYAPY"; }
    std::vector<std::string> pathNames(const std::string& os) const override {
        return os == "windows" ? std::vector<std::string>{"mayapy.exe"} : std::vector<std::string>{"mayapy"};
    }
    std::vector<std::string> standardLocations(const Host& h) const override {
        std::vector<std::string> out;
        if (std::string loc = h.getenv("MAYA_LOCATION"); !loc.empty()) {
            out.push_back(join(h.os, {loc, "bin", h.os == "windows" ? "mayapy.exe" : "mayapy"}));
        }
        if (h.os == "macos") {
            for (const auto& d : entriesWithPrefix(h, "/Applications/Autodesk", "maya")) {
                out.push_back(join(h.os, {d, "Maya.app", "Contents", "bin", "mayapy"}));
            }
        } else if (h.os == "windows") {
            for (const auto& d : entriesWithPrefix(h, join(h.os, {programFiles(h), "Autodesk"}), "Maya")) {
                out.push_back(join(h.os, {d, "bin", "mayapy.exe"}));
            }
        } else {
            for (const auto& d : entriesWithPrefix(h, "/usr/autodesk", "maya")) out.push_back(join(h.os, {d, "bin", "mayapy"}));
        }
        return out;
    }
    std::string versionFromPath(const std::string& exe) const override { return versionAfter(exe, "maya"); }
    std::vector<std::string> capabilities() const override {
        return {"script", "export_fbx", "export_obj", "import_fbx", "headless"};
    }
    ProcessSpec scriptCommand(const AppInfo& app, const ScriptRun& run) const override {
        // mayapy is a plain Python interpreter; the bootstrap initializes maya.standalone.
        ProcessSpec p;
        p.executable = app.executable;
        p.args.push_back(run.bootstrapPath);
        for (const auto& a : run.args) p.args.push_back(a);
        return p;
    }
};

// --- Houdini ------------------------------------------------------------------------

class HoudiniAdapter final : public Adapter {
public:
    AppId id() const override { return AppId::Houdini; }
    std::string displayName() const override { return "SideFX Houdini"; }
    std::string envVar() const override { return "SKY_HOUDINI_HYTHON"; }
    std::vector<std::string> pathNames(const std::string& os) const override {
        return os == "windows" ? std::vector<std::string>{"hython.exe"} : std::vector<std::string>{"hython"};
    }
    std::vector<std::string> standardLocations(const Host& h) const override {
        std::vector<std::string> out;
        if (std::string hfs = h.getenv("HFS"); !hfs.empty()) {
            out.push_back(join(h.os, {hfs, "bin", h.os == "windows" ? "hython.exe" : "hython"}));
        }
        if (h.os == "macos") {
            for (const auto& d : entriesWithPrefix(h, "/Applications/Houdini", "Houdini")) {
                out.push_back(join(h.os, {d, "Frameworks/Houdini.framework/Versions/Current/Resources/bin/hython"}));
            }
            out.push_back("/Library/Frameworks/Houdini.framework/Versions/Current/Resources/bin/hython");
        } else if (h.os == "windows") {
            for (const auto& d : entriesWithPrefix(h, join(h.os, {programFiles(h), "Side Effects Software"}), "Houdini")) {
                out.push_back(join(h.os, {d, "bin", "hython.exe"}));
            }
        } else {
            for (const auto& d : entriesWithPrefix(h, "/opt", "hfs")) out.push_back(join(h.os, {d, "bin", "hython"}));
        }
        return out;
    }
    std::string versionFromPath(const std::string& exe) const override {
        // ".../Houdini20.5.370/..." (macOS, Windows) or "/opt/hfs20.5.370/..." (Linux).
        std::string v = versionAfter(exe, "houdini");
        return v.empty() ? versionAfter(exe, "hfs") : v;
    }
    std::vector<std::string> capabilities() const override {
        return {"script", "export_fbx", "export_obj", "headless"};
    }
    ProcessSpec scriptCommand(const AppInfo& app, const ScriptRun& run) const override {
        ProcessSpec p;
        p.executable = app.executable;
        p.args.push_back(run.bootstrapPath);
        for (const auto& a : run.args) p.args.push_back(a);
        return p;
    }
};

// --- 3ds Max ------------------------------------------------------------------------

class MaxAdapter final : public Adapter {
public:
    AppId id() const override { return AppId::Max; }
    std::string displayName() const override { return "Autodesk 3ds Max"; }
    std::string envVar() const override { return "SKY_3DSMAX_BATCH"; }
    std::vector<std::string> pathNames(const std::string& os) const override {
        return os == "windows" ? std::vector<std::string>{"3dsmaxbatch.exe"} : std::vector<std::string>{};
    }
    std::vector<std::string> standardLocations(const Host& h) const override {
        std::vector<std::string> out;
        if (h.os != "windows") return out;  // 3ds Max only exists on Windows
        for (const auto& d : entriesWithPrefix(h, join(h.os, {programFiles(h), "Autodesk"}), "3ds Max")) {
            out.push_back(join(h.os, {d, "3dsmaxbatch.exe"}));
        }
        return out;
    }
    std::string versionFromPath(const std::string& exe) const override { return versionAfter(exe, "3ds max"); }
    std::vector<std::string> capabilities() const override {
        return {"script", "export_fbx", "export_obj", "import_fbx", "headless"};
    }
    ProcessSpec scriptCommand(const AppInfo& app, const ScriptRun& run) const override {
        // 3dsmaxbatch runs a MAXScript or Python file against an optional scene.
        ProcessSpec p;
        p.executable = app.executable;
        p.args = {"-v", "2"};
        std::string lower = str::lower(run.inputFile);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".max") == 0) {
            p.args.push_back("-sceneFile");
            p.args.push_back(run.inputFile);
        }
        p.args.push_back(run.bootstrapPath);
        return p;
    }
};

}  // namespace

const Adapter& adapterFor(AppId id) {
    static const BlenderAdapter blender;
    static const MayaAdapter maya;
    static const HoudiniAdapter houdini;
    static const MaxAdapter max;
    switch (id) {
        case AppId::Blender: return blender;
        case AppId::Maya: return maya;
        case AppId::Houdini: return houdini;
        case AppId::Max: return max;
    }
    return blender;
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

int compareVersions(std::string_view a, std::string_view b) {
    auto parts = [](std::string_view s) {
        std::vector<long> v;
        for (const auto& p : str::split(s, '.')) {
            if (p.empty() || !std::all_of(p.begin(), p.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                break;
            }
            v.push_back(std::strtol(p.c_str(), nullptr, 10));
        }
        return v;
    };
    std::vector<long> x = parts(a), y = parts(b);
    if (x.empty() != y.empty()) return x.empty() ? -1 : 1;
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        long xi = i < x.size() ? x[i] : 0, yi = i < y.size() ? y[i] : 0;
        if (xi != yi) return xi < yi ? -1 : 1;
    }
    return 0;
}

Json AppInfo::toJson() const {
    Json caps = Json::array();
    for (const auto& c : capabilities) caps.push(c);
    Json alts = Json::array();
    for (const auto& a : alternatives) alts.push(a);
    Json j = Json::object({{"app", toString(id)},
                           {"name", name},
                           {"executable", executable},
                           {"version", version},
                           {"found_via", foundVia},
                           {"capabilities", caps},
                           {"tested", tested}});
    if (alts.size()) j["alternatives"] = alts;
    if (!note.empty()) j["note"] = note;
    return j;
}

bool AppInfo::has(std::string_view capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

std::vector<AppInfo> detectApps(const Host& host, const std::map<AppId, std::string>& configured,
                                const std::function<std::string(const AppInfo&)>& probe) {
    std::vector<AppInfo> found;
    for (AppId id : allApps()) {
        const Adapter& ad = adapterFor(id);
        struct Candidate {
            std::string path;
            std::string via;
        };
        std::vector<Candidate> candidates;
        std::string note;

        // 1. Environment override: authoritative when it works.
        std::string envPath = host.getenv ? host.getenv(ad.envVar()) : "";
        if (!envPath.empty()) {
            if (host.isExecutable(envPath)) {
                candidates.push_back({envPath, "env:" + ad.envVar()});
            } else {
                note = ad.envVar() + " is set to '" + envPath + "' but that is not an executable file";
            }
        }
        // 2. The user's config file.
        if (auto it = configured.find(id); it != configured.end() && !it->second.empty()) {
            if (host.isExecutable(it->second)) {
                candidates.push_back({it->second, "config"});
            } else {
                note += (note.empty() ? "" : "; ") + std::string("configured path '") + it->second + "' is not an executable file";
            }
        }
        // 3. Standard install locations, 4. PATH.
        std::vector<Candidate> discovered;
        for (const auto& p : ad.standardLocations(host)) {
            if (host.isExecutable(p)) discovered.push_back({p, "standard-location"});
        }
        for (const auto& dir : host.pathDirs) {
            for (const auto& name : ad.pathNames(host.os)) {
                std::string p = join(host.os, {dir, name});
                if (host.isExecutable(p)) discovered.push_back({p, "PATH"});
            }
        }
        // Newest first (stable: discovery order breaks ties).
        std::stable_sort(discovered.begin(), discovered.end(), [&](const Candidate& a, const Candidate& b) {
            return compareVersions(ad.versionFromPath(a.path), ad.versionFromPath(b.path)) > 0;
        });
        for (auto& c : discovered) candidates.push_back(c);

        // De-duplicate by path, keep order.
        std::vector<Candidate> unique;
        std::set<std::string> seen;
        for (auto& c : candidates) {
            if (seen.insert(c.path).second) unique.push_back(c);
        }
        if (unique.empty()) continue;

        AppInfo info;
        info.id = id;
        info.name = ad.displayName();
        info.executable = unique[0].path;
        info.foundVia = unique[0].via;
        info.version = ad.versionFromPath(info.executable);
        info.capabilities = ad.capabilities();
        info.tested = ad.tested();
        info.note = note;
        for (size_t i = 1; i < unique.size(); ++i) info.alternatives.push_back(unique[i].path);
        if (info.version.empty() && probe) info.version = probe(info);
        found.push_back(std::move(info));
    }
    return found;
}

}  // namespace sky::dcc
