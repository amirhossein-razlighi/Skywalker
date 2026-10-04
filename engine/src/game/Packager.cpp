// `skywalker build`: assembles, icons, signs and verifies a macOS app bundle around the standalone player.

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "../native/Process.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/Packager.h"
#include "skywalker/native/NativeModules.h"

#ifndef SKY_PLAYER_DEFAULT_PATH
#define SKY_PLAYER_DEFAULT_PATH ""
#endif

namespace sky::game {

namespace fs = std::filesystem;

namespace {

/// Raised internally; buildGame() converts it to a Result error (keeps the long pipeline free of nested checks).
struct BuildError {
    Error error;
};
[[noreturn]] void fail(std::string code, std::string message, std::string hint = {}) {
    throw BuildError{Error::make(std::move(code), std::move(message), std::move(hint))};
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

/// File-name-safe app name: keeps spaces and letters, drops path separators and control characters.
std::string appFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '/' || c == ':' || c == '\\' || static_cast<unsigned char>(c) < 32) c = '-';
        out += c;
    }
    out = str::trim(out);
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    return out.empty() ? std::string("Game") : out;
}

std::string executableName(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])) ? "Game" + out : out;
}

std::string isoNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
    if (!f) fail("io_error", "cannot write " + p.string());
}

native::ProcessResult run(const std::vector<std::string>& argv, int timeout = 120) {
    auto r = native::runProcess(argv, {}, timeout);
    if (!r) fail("tool_failed", "cannot run " + argv[0] + ": " + r.error().message);
    return *r;
}

std::string infoPlist(const std::string& exe, const std::string& name, const std::string& bundleId, const std::string& version,
                      const std::string& copyright, bool hasIcon) {
    std::ostringstream os;
    auto entry = [&](const char* key, const std::string& value) {
        os << "    <key>" << key << "</key>\n    <string>" << xmlEscape(value) << "</string>\n";
    };
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\">\n<dict>\n";
    entry("CFBundleDevelopmentRegion", "en");
    entry("CFBundleExecutable", exe);
    entry("CFBundleIdentifier", bundleId);
    entry("CFBundleInfoDictionaryVersion", "6.0");
    entry("CFBundleName", name);
    entry("CFBundleDisplayName", name);
    entry("CFBundlePackageType", "APPL");
    entry("CFBundleShortVersionString", version);
    entry("CFBundleVersion", version);
    if (hasIcon) entry("CFBundleIconFile", "AppIcon");
    entry("LSApplicationCategoryType", "public.app-category.games");
    entry("LSMinimumSystemVersion", "15.0");  // CMAKE_OSX_DEPLOYMENT_TARGET of the player
    entry("NSPrincipalClass", "NSApplication");
    if (!copyright.empty()) entry("NSHumanReadableCopyright", copyright);
    os << "    <key>NSHighResolutionCapable</key>\n    <true/>\n"
          "    <key>LSSupportsGameMode</key>\n    <true/>\n"
          "</dict>\n</plist>\n";
    return os.str();
}

/// Converts a PNG into AppIcon.icns with sips + iconutil (both ship with macOS). Square-crops the middle first.
void makeIcns(const fs::path& png, const fs::path& icnsOut, std::vector<std::string>& warnings) {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path() / ("skywalker-icon-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "AppIcon.iconset", ec);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{tmp};

    auto dims = run({"sips", "-g", "pixelWidth", "-g", "pixelHeight", png.string()});
    int w = 0, h = 0;
    for (const auto& line : str::split(dims.output, '\n')) {
        if (auto p = line.find("pixelWidth:"); p != std::string::npos) w = std::atoi(line.c_str() + p + 11);
        if (auto p = line.find("pixelHeight:"); p != std::string::npos) h = std::atoi(line.c_str() + p + 12);
    }
    if (dims.exitCode != 0 || w <= 0 || h <= 0) fail("bad_icon", "cannot read the icon " + png.string() + " (is it a PNG?)");
    int side = std::min(w, h);
    if (side < 512) warnings.push_back("the icon is " + std::to_string(w) + "x" + std::to_string(h) + ": 1024x1024 looks sharp on Retina displays");
    fs::path square = tmp / "square.png";
    if (w != h) {
        warnings.push_back("the icon is not square (" + std::to_string(w) + "x" + std::to_string(h) + "): its center was cropped");
        auto crop = run({"sips", "-c", std::to_string(side), std::to_string(side), png.string(), "--out", square.string()});
        if (crop.exitCode != 0) fail("bad_icon", "sips could not crop the icon: " + str::trim(crop.output));
    } else {
        fs::copy_file(png, square, ec);
    }
    static const std::pair<int, const char*> sizes[] = {{16, "icon_16x16"},      {32, "icon_16x16@2x"},   {32, "icon_32x32"},
                                                        {64, "icon_32x32@2x"},   {128, "icon_128x128"},   {256, "icon_128x128@2x"},
                                                        {256, "icon_256x256"},   {512, "icon_256x256@2x"}, {512, "icon_512x512"},
                                                        {1024, "icon_512x512@2x"}};
    for (const auto& [px, name] : sizes) {
        auto r = run({"sips", "-z", std::to_string(px), std::to_string(px), square.string(), "--out",
                      (tmp / "AppIcon.iconset" / (std::string(name) + ".png")).string()});
        if (r.exitCode != 0) fail("bad_icon", "sips failed: " + str::trim(r.output));
    }
    fs::create_directories(icnsOut.parent_path(), ec);
    auto r = run({"iconutil", "-c", "icns", (tmp / "AppIcon.iconset").string(), "-o", icnsOut.string()});
    if (r.exitCode != 0) fail("bad_icon", "iconutil failed: " + str::trim(r.output));
}

uint64_t treeBytes(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += fs::file_size(it->path(), ec);
    }
    return total;
}

bool looksLikeOurApp(const fs::path& app) {
    std::error_code ec;
    return app.extension() == ".app" && fs::is_regular_file(app / "Contents" / "Info.plist", ec);
}

std::string typeLabel(const std::string& rel) {
    AssetType t = assetTypeForPath(rel);
    if (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".meta") == 0) return "metadata";
    return t == AssetType::Unknown ? "other" : toString(t);
}

}  // namespace

std::string findPlayerBinary() {
    std::error_code ec;
    auto usable = [&](const fs::path& p) { return !p.empty() && fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0; };
    if (const char* env = std::getenv("SKYWALKER_PLAYER"); env && usable(env)) return env;
    fs::path self;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) self = buf;
#else
    self = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (!self.empty()) {
        fs::path dir = fs::weakly_canonical(self, ec).parent_path();
        for (const fs::path& candidate : {dir / "skywalker-player", dir.parent_path() / "bin" / "skywalker-player"}) {
            if (usable(candidate)) return candidate.string();
        }
    }
    if (usable(SKY_PLAYER_DEFAULT_PATH)) return SKY_PLAYER_DEFAULT_PATH;
    return {};
}

Json PackageReport::toJson() const {
    Json w = Json::array();
    for (const auto& s : warnings) w.push(s);
    return Json::object({{"app", app},           {"name", name},       {"bundle_id", bundleId}, {"version", version},
                         {"start_scene", startScene}, {"bytes", appBytes}, {"files", files},       {"signature", signature},
                         {"release", release},   {"seconds", seconds}, {"contents", contents},   {"warnings", w}});
}

Result<PackageReport> buildGame(const PackageOptions& options) {
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::path project = fs::weakly_canonical(options.projectDir.empty() ? "." : options.projectDir, ec);
    if (ec || !fs::is_directory(project)) return Error::make("not_found", "project folder not found: " + options.projectDir);
    if (options.outDir.empty()) return Error::make("invalid_arguments", "no output folder given", "pass --out DIR (the app is written to DIR/<Name>.app)");
    fs::path outDir = fs::absolute(options.outDir, ec).lexically_normal();

    fs::path staging;
    try {
        PackageReport report;
        report.release = options.release;

        auto loaded = GameSettings::load(project.string());
        if (!loaded) return loaded.error();
        GameSettings settings = *loaded;
        if (!options.version.empty()) settings.version = options.version;
        if (!options.bundleId.empty()) settings.bundleId = options.bundleId;
        if (!options.name.empty()) settings.title = options.name;
        // Re-validate overrides with the same rules as game.json.
        if (auto again = GameSettings::fromJson(settings.toJson()); !again) {
            return Error::make(again.error().code, again.error().message, again.error().hint);
        }
        if (!settings.fromFile) report.warnings.push_back("the project has no game.json: using defaults (add one with game_settings)");
        const std::string name = settings.displayName(project.filename().string());
        settings.title = name;
        report.name = name;
        report.bundleId = settings.effectiveBundleId();
        report.version = settings.version;

        std::string player = options.player.empty() ? findPlayerBinary() : options.player;
        if (player.empty() || !fs::is_regular_file(player, ec)) {
            return Error::make("no_player", "the player executable (skywalker-player) was not found",
                               "build it (cmake --build build/release) or pass --player PATH; SKYWALKER_PLAYER also works");
        }

        CollectOptions co;
        co.allAssets = options.allAssets;
        co.startScene = options.scene;
        auto collected = collectGameFiles(project.string(), settings, co);
        if (!collected) return collected.error();
        report.startScene = collected->startScene;
        settings.startScene = collected->startScene;
        for (const auto& [ref, from] : collected->missing) {
            report.warnings.push_back("'" + ref + "' (in " + from + ") looks like an asset but the file does not exist");
            if (report.warnings.size() > 40) break;
        }
        for (const auto& f : collected->excludedReferenced) {
            report.warnings.push_back(f + " is referenced by the game but excluded by game.json: it will be missing at run time");
        }

        const std::string appName = appFileName(name);
        const std::string exeName = executableName(name);
        const fs::path finalApp = outDir / (appName + ".app");
        if (fs::exists(finalApp, ec) && !looksLikeOurApp(finalApp)) {
            return Error::make("output_exists", finalApp.string() + " exists and is not an app bundle: refusing to replace it");
        }
        if (finalApp == project || finalApp.string().rfind(project.string() + "/", 0) == 0) {
            // Allowed (apps are never shipped), but keeping builds outside the project is cleaner.
            report.warnings.push_back("the app is inside the project folder; consider --out outside it");
        }
        fs::create_directories(outDir, ec);
        if (ec) return Error::make("io_error", "cannot create " + outDir.string() + ": " + ec.message());
        staging = outDir / ("." + appName + ".app.partial");
        fs::remove_all(staging, ec);
        const fs::path contents = staging / "Contents", macos = contents / "MacOS", resources = contents / "Resources",
                       gameDir = resources / "Game";
        fs::create_directories(macos, ec);
        fs::create_directories(gameDir, ec);

        // The player.
        fs::copy_file(player, macos / exeName, fs::copy_options::overwrite_existing, ec);
        if (ec) fail("io_error", "cannot copy the player: " + ec.message());
        fs::permissions(macos / exeName, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);

        // The project.
        std::vector<std::pair<std::string, uint64_t>> shipped;
        for (const auto& rel : collected->files) {
            if (rel == "game.json") continue;  // written below, with the resolved settings
            fs::path to = gameDir / rel;
            fs::create_directories(to.parent_path(), ec);
            fs::copy_file(project / rel, to, fs::copy_options::overwrite_existing, ec);
            if (ec) fail("io_error", "cannot copy " + rel + ": " + ec.message());
            shipped.emplace_back(rel, fs::file_size(to, ec));
        }
        {
            GameSettings shippedSettings = settings;
            shippedSettings.include.clear();  // build-time only
            shippedSettings.exclude.clear();
            std::string text = shippedSettings.toJson().dump(2) + "\n";
            writeFile(gameDir / "game.json", text);
            shipped.emplace_back("game.json", text.size());
        }

        // Native C++ module: compiled now, shipped as a library (the player never needs a compiler).
        std::string nativeRel;
        {
            std::unique_ptr<Engine> own;
            Engine* engine = options.host;
            if (engine) {
                std::error_code e2;
                if (fs::weakly_canonical(engine->config().projectDir, e2) != project) engine = nullptr;
            }
            bool hasNative = false;
            if (fs::is_directory(project / "native", ec)) {
                for (const auto& e : fs::directory_iterator(project / "native", ec)) {
                    auto ext = e.path().extension().string();
                    hasNative = hasNative || ext == ".cpp" || ext == ".cc" || ext == ".c";
                }
            }
            if (hasNative && options.buildNative) {
                if (!engine) {
                    EngineConfig cfg;
                    cfg.projectDir = project.string();
                    cfg.renderer = RendererBackend::Null;
                    cfg.audio = audio::AudioMode::Off;
                    own = std::make_unique<Engine>(cfg);
                    engine = own.get();
                }
                auto built = engine->native().build(false);
                if (!built) fail(built.error().code, "native module: " + built.error().message, built.error().hint);
                if (!built->ok) {
                    std::string first = "see native_build for the diagnostics";
                    for (const auto& d : built->diagnostics.elements()) {
                        if (d.get("severity").asString() == "error") {
                            first = d.get("file").asString() + ":" + std::to_string(d.get("line").asInt()) + ": " + d.get("message").asString();
                            break;
                        }
                    }
                    fail("native_build_failed", "the native module does not compile: " + first, "fix it with native_build, then build again");
                }
                fs::path lib = built->library;
                fs::create_directories(contents / "Frameworks", ec);
                fs::copy_file(lib, contents / "Frameworks" / lib.filename(), fs::copy_options::overwrite_existing, ec);
                if (ec) fail("io_error", "cannot copy the native module: " + ec.message());
                nativeRel = "Frameworks/" + lib.filename().string();
                auto deps = native::runProcess({"otool", "-L", lib.string()}, {}, 30);
                if (deps && deps->exitCode == 0) {
                    for (const auto& line : str::split(deps->output, '\n')) {
                        std::string dep = str::trim(line);
                        if (dep.empty() || dep.back() == ':') continue;
                        dep = dep.substr(0, dep.find(" ("));
                        if (!str::startsWith(dep, "/usr/lib/") && !str::startsWith(dep, "/System/") && !str::startsWith(dep, "@") &&
                            dep != lib.string()) {
                            report.warnings.push_back("the native module links against " + dep + ", which players may not have installed");
                        }
                    }
                }
            } else if (hasNative) {
                report.warnings.push_back("native/ sources exist but were not compiled (buildNative is off): native builtins will be missing");
            }
        }

        // Icon.
        bool hasIcon = false;
        {
            std::string iconArg = !options.icon.empty() ? options.icon : settings.icon;
            if (!iconArg.empty()) {
                fs::path icon = fs::path(iconArg).is_absolute() ? fs::path(iconArg) : (!options.icon.empty() && fs::exists(iconArg, ec) ? fs::absolute(iconArg) : project / iconArg);
                if (!fs::is_regular_file(icon, ec)) fail("not_found", "icon not found: " + iconArg, "pass a PNG (1024x1024 recommended)");
                makeIcns(icon, resources / "AppIcon.icns", report.warnings);
                hasIcon = true;
            } else {
                report.warnings.push_back("no icon: set \"icon\" in game.json or pass --icon (the app shows the generic icon)");
            }
        }

        writeFile(contents / "Info.plist", infoPlist(exeName, name, report.bundleId, report.version, settings.copyright, hasIcon));
        writeFile(contents / "PkgInfo", "APPL????");

        // Manifest.
        {
            std::sort(shipped.begin(), shipped.end());
            Json files = Json::array();
            std::map<std::string, std::pair<size_t, uint64_t>> byType;
            for (const auto& [rel, bytes] : shipped) {
                files.push(Json::object({{"path", rel}, {"bytes", bytes}}));
                auto& g = byType[typeLabel(rel)];
                ++g.first;
                g.second += bytes;
            }
            Json types = Json::object();
            for (const auto& [type, g] : byType) types[type] = Json::object({{"files", g.first}, {"bytes", g.second}});
            auto largest = shipped;
            std::sort(largest.begin(), largest.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            Json top = Json::array();
            for (size_t i = 0; i < largest.size() && i < 8; ++i) top.push(Json::object({{"path", largest[i].first}, {"bytes", largest[i].second}}));
            report.contents = Json::object({{"by_type", types}, {"largest", top}});
            report.files = shipped.size();

            Json manifest = Json::object({{"format", "skywalker.build"},
                                          {"version", 1},
                                          {"engine", SKY_VERSION_STRING},
                                          {"name", name},
                                          {"executable", exeName},
                                          {"bundleId", report.bundleId},
                                          {"gameVersion", report.version},
                                          {"startScene", report.startScene},
                                          {"release", options.release},
                                          {"builtAt", isoNow()},
                                          {"native", nativeRel},
                                          {"files", files}});
            writeFile(resources / "build.json", manifest.dump(1) + "\n");
        }

        // Strip, sign, verify.
        if (options.release) {
            auto r = run({"strip", "-x", (macos / exeName).string()});
            if (r.exitCode != 0) report.warnings.push_back("strip failed: " + str::trim(r.output));
        }
        (void)run({"xattr", "-cr", staging.string()});  // Finder info / iCloud attributes invalidate signatures
        report.signature = "none";
        if (options.sign) {
            bool ok = true;
            std::string detail;
            if (!nativeRel.empty()) {
                auto r = run({"codesign", "--force", "--sign", "-", "--timestamp=none", (contents / nativeRel).string()});
                ok = r.exitCode == 0;
                detail = r.output;
            }
            if (ok) {
                auto r = run({"codesign", "--force", "--sign", "-", "--timestamp=none", staging.string()});
                ok = r.exitCode == 0;
                detail = r.output;
            }
            if (ok) {
                auto v = run({"codesign", "--verify", "--strict", staging.string()});
                ok = v.exitCode == 0;
                detail = v.output;
            }
            report.signedOk = ok;
            report.signature = ok ? "ad-hoc" : "none";
            if (!ok) report.warnings.push_back("ad-hoc signing failed: " + str::trim(detail));
        }

        // Replace the previous build only now that the new one is complete.
        if (fs::exists(finalApp, ec)) fs::remove_all(finalApp, ec);
        fs::rename(staging, finalApp, ec);
        if (ec) fail("io_error", "cannot move the finished app into place: " + ec.message());
        staging.clear();
        report.app = finalApp.string();
        report.appBytes = treeBytes(finalApp);
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return report;
    } catch (const BuildError& e) {
        if (!staging.empty()) fs::remove_all(staging, ec);
        return e.error;
    }
}

}  // namespace sky::game
