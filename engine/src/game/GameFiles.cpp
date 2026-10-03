// Which project files a game needs (asset reference collection), and bundle location / verification.

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/core/Strings.h"
#include "skywalker/game/Packager.h"

namespace sky::game {

namespace fs = std::filesystem;

namespace {

bool endsWithSuffix(const std::string& s, const char* suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/// Folders that are never part of a shipped game.
bool neverShippedDir(const std::string& name) {
    static const std::set<std::string> dirs{"build",   "node_modules", "DerivedData", "__MACOSX", "studio",
                                            "agents",  "native",       "playtests",   "Pods",     "film"};
    return name.empty() || name[0] == '.' || dirs.count(name) > 0 || endsWithSuffix(name, ".app");  // .app: earlier builds
}

/// Source files of DCC tools, editors and backups: not loadable by the engine.
bool editorOnlyFile(const std::string& name) {
    static const std::set<std::string> exts{".blend", ".blend1", ".psd",  ".kra", ".xcf", ".aseprite", ".ma",  ".mb",
                                            ".max",   ".hip",    ".hiplc", ".zip", ".tmp", ".swp",      ".orig", ".bak", ".log"};
    if (name.empty() || name[0] == '.' || name.back() == '~') return true;  // .DS_Store, ._resource forks, backups
    auto dot = name.rfind('.');
    return dot != std::string::npos && exts.count(str::lower(name.substr(dot))) > 0;
}

std::string readText(const fs::path& p, uint64_t limit = uint64_t{256} << 20) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec || size > limit) return {};
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/// Strings in a path position: extension that the engine loads.
bool looksLikeAssetPath(const std::string& s) {
    if (s.empty() || s.size() > 200 || s.find('\n') != std::string::npos || s.find("://") != std::string::npos) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '/' || c == '-' || c == ' ' || c == '+' ||
              c == ':' || c == '#' || c == '(' || c == ')')) {
            return false;
        }
    }
    std::string l = str::lower(s);
    if (l.find('#') != std::string::npos) l = l.substr(0, l.find('#'));
    return assetTypeForPath(l) != AssetType::Unknown || endsWith(l, ".cube") || endsWith(l, ".mtl") || endsWith(l, ".bin");
}

bool hasGlobChars(const std::string& p) { return p.find_first_of("*?") != std::string::npos; }

/// Extracts "double quoted" literals from Wander source (and from any text with embedded source).
void quotedStrings(const std::string& src, const std::function<void(const std::string&)>& fn) {
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] != '"') continue;
        std::string lit;
        size_t j = i + 1;
        for (; j < src.size() && src[j] != '"' && src[j] != '\n'; ++j) {
            if (src[j] == '\\' && j + 1 < src.size()) ++j;
            lit += src[j];
        }
        if (j < src.size() && src[j] == '"') {
            if (!lit.empty()) fn(lit);
            i = j;
        }
    }
}

class Collector {
public:
    explicit Collector(const fs::path& root) : root_(root) {}

    void scanProject() {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            std::string name = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (neverShippedDir(name)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || editorOnlyFile(name)) continue;
            std::string rel = fs::relative(it->path(), root_, ec).generic_string();
            if (rel.empty()) continue;
            all_.insert(rel);
        }
        // GUID references ("guid:...") resolve through the .meta sidecars, which ship with their asset.
        for (const auto& rel : all_) {
            if (!endsWith(rel, ".meta")) continue;
            std::string target = rel.substr(0, rel.size() - 5);
            if (!all_.count(target)) continue;
            if (auto j = Json::parse(readText(root_ / rel, 1 << 20)); j && j->isObject()) {
                const std::string& guid = j->get("guid").asString();
                if (!guid.empty()) guids_.emplace(guid, target);
            }
        }
    }

    const std::set<std::string>& all() const { return all_; }
    bool has(const std::string& rel) const { return all_.count(rel) > 0; }

    void addRoot(const std::string& rel) {
        if (has(rel)) enqueue(rel, /*viaReference=*/false);
    }

    void addGlob(std::string pattern) {
        std::replace(pattern.begin(), pattern.end(), '\\', '/');
        if (!hasGlobChars(pattern)) {
            std::string dir = pattern;
            while (!dir.empty() && dir.back() == '/') dir.pop_back();
            bool isDir = false;
            std::string prefix = dir + "/";
            for (const auto& f : all_) {
                if (str::startsWith(f, prefix)) {
                    isDir = true;
                    break;
                }
            }
            if (!isDir) {
                addRoot(dir);
                return;
            }
            pattern = dir + "/*";
        }
        for (const auto& f : all_) {
            if (str::globMatch(pattern, f)) enqueue(f, false);
        }
    }

    void addEverything() {
        for (const auto& f : all_) enqueue(f, false);
    }

    /// Follows references from every queued file until nothing new appears.
    void run() {
        while (!queue_.empty()) {
            std::string rel = queue_.front();
            queue_.pop_front();
            visit(rel);
        }
    }

    std::set<std::string> take() { return std::move(shipped_); }
    std::map<std::string, std::string> takeMissing() { return std::move(missing_); }
    size_t referenced() const { return referenced_; }

private:
    void enqueue(const std::string& rel, bool viaReference) {
        if (!shipped_.insert(rel).second) return;
        if (viaReference) ++referenced_;
        queue_.push_back(rel);
        // The importer reads the sidecar (GUID, import settings: animation libraries), so it travels with the asset.
        std::string meta = rel + ".meta";
        if (has(meta) && shipped_.insert(meta).second) queue_.push_back(meta);
    }

    std::string guidTarget(const std::string& s) const {
        auto it = guids_.find(s);
        return it == guids_.end() ? std::string() : it->second;
    }

    /// Resolves one referenced string (relative to the project, or to the referring file's folder).
    void reference(std::string ref, const std::string& from) {
        if (ref.empty() || ref.size() > 300 || ref.find('\n') != std::string::npos || ref.find('\0') != std::string::npos) return;
        const std::string original = ref;
        for (const char* prefix : {"asset:", "prefab:"}) {
            if (str::startsWith(ref, prefix)) ref = ref.substr(std::char_traits<char>::length(prefix));
        }
        if (str::startsWith(ref, "guid:")) ref = ref.substr(5);
        if (std::string t = guidTarget(ref); !t.empty()) {
            enqueue(t, true);
            return;
        }
        if (auto hash = ref.find('#'); hash != std::string::npos) ref = ref.substr(0, hash);  // "model.gltf#2"
        if (ref.empty() || ref[0] == '/' || ref[0] == '~') return;
        std::string normal = fs::path(ref).lexically_normal().generic_string();
        if (normal.empty() || str::startsWith(normal, "..")) return;
        if (has(normal)) {
            enqueue(normal, true);
            return;
        }
        std::string dir = fs::path(from).parent_path().generic_string();
        if (!dir.empty()) {
            std::string sibling = fs::path(dir + "/" + normal).lexically_normal().generic_string();
            if (has(sibling)) {
                enqueue(sibling, true);
                return;
            }
        }
        if (looksLikeAssetPath(ref) && !missing_.count(original)) missing_.emplace(original, from);
    }

    void visitJson(const Json& j, const std::string& from) {
        switch (j.type()) {
            case Json::Type::String: {
                const std::string& s = j.asString();
                reference(s, from);
                // Behavior sources are strings of Wander code: their literals name sounds, prefabs, modules.
                if (s.find('\n') != std::string::npos || s.find("behavior") != std::string::npos) {
                    quotedStrings(s, [&](const std::string& lit) { reference(lit, from); });
                }
                break;
            }
            case Json::Type::Array:
                for (const auto& e : j.elements()) visitJson(e, from);
                break;
            case Json::Type::Object:
                for (const auto& m : j.members()) visitJson(m.second, from);
                break;
            default:
                break;
        }
    }

    void visit(const std::string& rel) {
        std::string l = str::lower(rel);
        fs::path path = root_ / rel;
        if (endsWith(l, ".json") || endsWith(l, ".meta") || endsWith(l, ".gltf") || endsWith(l, ".anim")) {
            std::string text = readText(path);
            size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || (text[first] != '{' && text[first] != '[')) return;
            if (auto doc = Json::parse(text)) visitJson(*doc, rel);
        } else if (endsWith(l, ".wander")) {
            quotedStrings(readText(path), [&](const std::string& lit) { reference(lit, rel); });
        } else if (endsWith(l, ".obj") || endsWith(l, ".mtl")) {
            // mtllib file.mtl / map_Kd tex.png: the last token of the line names the file.
            std::istringstream in(readText(path));
            std::string line;
            while (std::getline(in, line)) {
                std::string t = str::trim(line);
                bool wanted = str::startsWith(t, "mtllib ") || str::startsWith(t, "map_") || str::startsWith(t, "bump ") ||
                              str::startsWith(t, "norm ") || str::startsWith(t, "disp ") || str::startsWith(t, "refl ");
                if (!wanted) continue;
                size_t sp = t.rfind(' ');
                if (sp != std::string::npos) reference(str::trim(t.substr(sp + 1)), rel);
            }
        }
    }

    fs::path root_;
    std::set<std::string> all_, shipped_;
    std::unordered_map<std::string, std::string> guids_;
    std::map<std::string, std::string> missing_;
    std::deque<std::string> queue_;
    size_t referenced_ = 0;
};

bool excludedByGame(const GameSettings& settings, const std::string& rel) {
    for (std::string pattern : settings.exclude) {
        std::replace(pattern.begin(), pattern.end(), '\\', '/');
        if (!hasGlobChars(pattern)) {
            while (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
            if (rel == pattern || str::startsWith(rel, pattern + "/")) return true;
        } else if (str::globMatch(pattern, rel)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Json CollectedFiles::toJson() const {
    Json miss = Json::array();
    for (const auto& [ref, from] : missing) miss.push(Json::object({{"reference", ref}, {"in", from}}));
    Json excluded = Json::array();
    for (const auto& f : excludedReferenced) excluded.push(f);
    return Json::object({{"files", files.size()}, {"bytes", bytes}, {"referenced", referenced}, {"startScene", startScene},
                         {"missing", miss}, {"excluded_but_referenced", excluded}});
}

Result<CollectedFiles> collectGameFiles(const std::string& projectDir, const GameSettings& settings, const CollectOptions& options) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(projectDir, ec);
    if (ec || !fs::is_directory(root)) return Error::make("not_found", "project folder not found: " + projectDir);

    CollectedFiles out;
    auto scene = resolveStartScene(root.string(), settings, options.startScene);
    if (!scene) return scene.error();
    out.startScene = *scene;

    Collector c(root);
    c.scanProject();
    for (const char* rootFile : {"game.json", "input.json", "audio.json", "CREDITS.md", "LICENSE", "LICENSE.md", "LICENSE.txt",
                                 "NOTICE", "NOTICE.md", "NOTICE.txt"}) {
        c.addRoot(rootFile);
    }
    c.addRoot(out.startScene);
    for (const auto& f : c.all()) {
        // Every scene ships (scripts can switch scenes), and every Wander file (modules are loaded by name).
        if (endsWith(f, ".sky.json") || endsWith(f, ".wander")) c.addRoot(f);
    }
    for (const auto& pattern : settings.include) c.addGlob(pattern);
    if (options.allAssets) c.addEverything();
    c.run();

    out.files = c.take();
    out.referenced = c.referenced();
    out.missing = c.takeMissing();
    for (auto it = out.files.begin(); it != out.files.end();) {
        if (excludedByGame(settings, *it)) {
            if (!endsWith(*it, ".meta")) out.excludedReferenced.push_back(*it);
            it = out.files.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& f : out.files) {
        auto size = fs::file_size(root / f, ec);
        if (!ec) out.bytes += size;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Bundles
// ---------------------------------------------------------------------------------------------------

Result<GameLocation> locateGame(const std::string& arg) {
    std::error_code ec;
    fs::path p = fs::absolute(arg, ec);
    if (!fs::is_directory(p, ec)) return Error::make("not_found", "no such project or app: " + arg);
    GameLocation loc;
    if (fs::is_directory(p / "Contents" / "Resources" / "Game", ec)) {
        loc.appPath = p.string();
        loc.projectDir = (p / "Contents" / "Resources" / "Game").string();
        loc.bundled = true;
        if (auto m = Json::parse(readText(p / "Contents" / "Resources" / "build.json", 64 << 20))) loc.manifest = *m;
        return loc;
    }
    if (p.extension() == ".app") {
        return Error::make("invalid_app", arg + " is not a Skywalker game (Contents/Resources/Game is missing)",
                           "build one with `skywalker build --project DIR --out DIR`");
    }
    loc.projectDir = p.lexically_normal().string();
    return loc;
}

GameLocation locateBundledGame(const std::string& executable) {
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(executable, ec);
    // <App>.app/Contents/MacOS/<exe>
    fs::path app = exe.parent_path().parent_path().parent_path();
    if (exe.parent_path().filename() == "MacOS" && app.extension() == ".app") {
        auto loc = locateGame(app.string());
        if (loc) return *loc;
    }
    return {};
}

Json BundleCheck::toJson() const {
    Json p = Json::array();
    for (const auto& s : problems) p.push(s);
    return Json::object({{"ok", ok}, {"files", files}, {"bytes", bytes}, {"problems", p}});
}

BundleCheck verifyBundle(const GameLocation& game) {
    BundleCheck check;
    auto fail = [&](const std::string& why) {
        check.ok = false;
        check.problems.push_back(why);
    };
    std::error_code ec;
    if (!game.bundled) {
        fail("not an app bundle");
        return check;
    }
    fs::path app = game.appPath;
    if (!fs::is_regular_file(app / "Contents" / "Info.plist", ec)) fail("Contents/Info.plist is missing");
    if (game.manifest.isNull()) {
        fail("Contents/Resources/build.json is missing or unreadable");
        return check;
    }
    const std::string exeName = game.manifest.get("executable").asString();
    fs::path exe = app / "Contents" / "MacOS" / exeName;
    struct stat st {};
    if (exeName.empty() || ::stat(exe.c_str(), &st) != 0) fail("the player executable Contents/MacOS/" + exeName + " is missing");
    else if ((st.st_mode & S_IXUSR) == 0) fail("Contents/MacOS/" + exeName + " is not executable");
    const std::string native = game.manifest.get("native").asString();
    if (!native.empty() && !fs::is_regular_file(app / "Contents" / native, ec)) fail("native module Contents/" + native + " is missing");
    if (!fs::is_regular_file(fs::path(game.projectDir) / game.manifest.get("startScene").asString(), ec)) {
        fail("start scene " + game.manifest.get("startScene").asString() + " is missing");
    }
    for (const auto& f : game.manifest.get("files").elements()) {
        const std::string rel = f.get("path").asString();
        fs::path p = fs::path(game.projectDir) / rel;
        auto size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : static_cast<uintmax_t>(-1);
        if (size == static_cast<uintmax_t>(-1)) {
            fail("missing file " + rel);
        } else if (size != static_cast<uintmax_t>(f.get("bytes").asInt())) {
            fail("size of " + rel + " differs from the build manifest (modified after the build?)");
        } else {
            ++check.files;
            check.bytes += size;
        }
        if (check.problems.size() > 50) {
            fail("... more problems");
            break;
        }
    }
    return check;
}

}  // namespace sky::game
