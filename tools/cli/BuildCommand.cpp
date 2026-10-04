// `skywalker build --project DIR --out DIR [options]`: package a project as a macOS app (Name.app) around the
// standalone player. The work happens in sky::game::buildGame (engine/src/game/Packager.cpp).

#include <cstdio>
#include <string>
#include <vector>

#include "skywalker/game/Packager.h"

namespace {

using namespace sky;

int buildUsage() {
    std::fprintf(stderr,
                 "usage: skywalker build --project DIR --out DIR [options]\n\n"
                 "Builds DIR/<Name>.app: the player, the scenes, the assets they reference, scripts, the compiled native\n"
                 "module, Info.plist, icon and an ad-hoc code signature. Settings come from the project's game.json.\n\n"
                 "options:\n"
                 "  --project DIR     the project (default: current directory)\n"
                 "  --out DIR         folder to write the app into (required)\n"
                 "  --name NAME       app name (default: game.json title)\n"
                 "  --icon FILE.png   app icon, 1024x1024 recommended (default: game.json icon)\n"
                 "  --release         strip the player's symbols (smaller app)\n"
                 "  --all-assets      ship every asset, not only the referenced ones\n"
                 "  --version X.Y.Z   --bundle-id com.you.game   --scene FILE   override game.json\n"
                 "  --no-sign         skip the ad-hoc code signature\n"
                 "  --no-native       do not compile native/*.cpp\n"
                 "  --player PATH     player executable to bundle (default: skywalker-player next to this binary)\n"
                 "  --dry-run         list what would ship; write nothing\n"
                 "  --json            machine-readable report\n");
    return 2;
}

std::string size(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1u << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    else std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

}  // namespace

int runBuild(const std::vector<std::string>& raw) {
    game::PackageOptions o;
    o.projectDir = ".";
    bool json = false, dryRun = false;
    for (size_t i = 1; i < raw.size(); ++i) {
        const std::string& a = raw[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= raw.size()) {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                return false;
            }
            out = raw[++i];
            return true;
        };
        bool ok = true;
        if (a == "--project") ok = value(o.projectDir);
        else if (a == "--out") ok = value(o.outDir);
        else if (a == "--name") ok = value(o.name);
        else if (a == "--icon") ok = value(o.icon);
        else if (a == "--version") ok = value(o.version);
        else if (a == "--bundle-id") ok = value(o.bundleId);
        else if (a == "--scene") ok = value(o.scene);
        else if (a == "--player") ok = value(o.player);
        else if (a == "--release") o.release = true;
        else if (a == "--all-assets") o.allAssets = true;
        else if (a == "--no-sign") o.sign = false;
        else if (a == "--no-native") o.buildNative = false;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--json") json = true;
        else if (a == "--help" || a == "-h") return buildUsage();
        else if (a.rfind("-", 0) == 0) {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return buildUsage();
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", a.c_str());
            return buildUsage();
        }
        if (!ok) return 2;
    }

    if (dryRun) {
        auto settings = game::GameSettings::load(o.projectDir);
        if (!settings) {
            std::fprintf(stderr, "error: %s\n  hint: %s\n", settings.error().message.c_str(), settings.error().hint.c_str());
            return 1;
        }
        game::CollectOptions co;
        co.allAssets = o.allAssets;
        co.startScene = o.scene;
        auto files = game::collectGameFiles(o.projectDir, *settings, co);
        if (!files) {
            std::fprintf(stderr, "error: %s\n  hint: %s\n", files.error().message.c_str(), files.error().hint.c_str());
            return 1;
        }
        if (json) {
            Json j = files->toJson();
            Json list = Json::array();
            for (const auto& f : files->files) list.push(f);
            j["list"] = list;
            std::printf("%s\n", j.dump(2).c_str());
        } else {
            for (const auto& f : files->files) std::printf("%s\n", f.c_str());
            std::printf("\n%zu files, %s; start scene %s\n", files->files.size(), size(files->bytes).c_str(), files->startScene.c_str());
            for (const auto& [ref, from] : files->missing) std::printf("warning: '%s' (in %s) does not exist\n", ref.c_str(), from.c_str());
        }
        return 0;
    }

    if (o.outDir.empty()) {
        std::fprintf(stderr, "error: --out DIR is required\n");
        return buildUsage();
    }
    auto report = game::buildGame(o);
    if (!report) {
        std::fprintf(stderr, "error: %s\n", report.error().message.c_str());
        if (!report.error().hint.empty()) std::fprintf(stderr, "  hint: %s\n", report.error().hint.c_str());
        return 1;
    }
    if (json) {
        std::printf("%s\n", report->toJson().dump(2).c_str());
        return 0;
    }
    std::printf("built %s\n", report->app.c_str());
    std::printf("  name %s, bundle id %s, version %s%s\n", report->name.c_str(), report->bundleId.c_str(), report->version.c_str(),
                report->release ? " (release)" : "");
    std::printf("  size %s, %zu game files, start scene %s, signature: %s, %.1f s\n", size(report->appBytes).c_str(), report->files,
                report->startScene.c_str(), report->signature.c_str(), report->seconds);
    std::printf("  contents:");
    for (const auto& [type, g] : report->contents.get("by_type").members()) {
        std::printf(" %s %lld (%s)", type.c_str(), static_cast<long long>(g.get("files").asInt()), size(static_cast<uint64_t>(g.get("bytes").asInt())).c_str());
    }
    std::printf("\n  largest:");
    for (const auto& f : report->contents.get("largest").elements()) {
        std::printf(" %s (%s)", f.get("path").asString().c_str(), size(static_cast<uint64_t>(f.get("bytes").asInt())).c_str());
    }
    std::printf("\n");
    for (const auto& w : report->warnings) std::printf("warning: %s\n", w.c_str());
    return 0;
}
