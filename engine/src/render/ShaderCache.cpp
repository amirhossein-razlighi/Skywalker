#include "skywalker/render/ShaderCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

namespace sky::shadercache {

namespace fs = std::filesystem;

uint64_t fnv1a64(std::string_view data, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(uint64_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return out;
}

std::string cacheKey(std::string_view source, std::string_view engineVersion, std::string_view device, std::string_view os) {
    // Each part is hashed with a separator so ("ab", "c") and ("a", "bc") differ.
    uint64_t h = fnv1a64(source);
    for (std::string_view part : {engineVersion, device, os}) {
        h = fnv1a64("\x1f", h);
        h = fnv1a64(part, h);
    }
    return hex64(h);
}

std::string cacheDir() {
    if (const char* d = std::getenv("SKY_SHADER_CACHE_DIR"); d && *d) return d;
    const char* home = std::getenv("HOME");
    const std::string h = home && *home ? home : "/tmp";
#if defined(__APPLE__)
    return h + "/Library/Caches/Skywalker/shaders";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/skywalker/shaders";
    return h + "/.cache/skywalker/shaders";
#endif
}

std::string archivePath(const std::string& dir, const std::string& key) {
    return (fs::path(dir) / ("pipelines-" + key + ".binarchive")).string();
}

size_t pruneArchives(const std::string& dir, const std::string& currentKey, size_t keep) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    const std::string current = "pipelines-" + currentKey + ".binarchive";
    std::vector<std::pair<fs::file_time_type, fs::path>> others;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name == current || name.rfind("pipelines-", 0) != 0 || entry.path().extension() != ".binarchive") continue;
        std::error_code tec;
        others.emplace_back(fs::last_write_time(entry.path(), tec), entry.path());
    }
    std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t removed = 0;
    for (size_t i = keep; i < others.size(); ++i) {
        std::error_code rec;
        if (fs::remove(others[i].second, rec)) ++removed;
    }
    return removed;
}

std::string archiveDisabledReason() {
    auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && *v && std::string_view(v) != "0";
    };
    if (const char* v = std::getenv("SKY_SHADER_CACHE"); v && std::string_view(v) == "0") return "disabled (SKY_SHADER_CACHE=0)";
    if (on("MTL_SHADER_VALIDATION") || on("MTL_DEBUG_LAYER")) return "disabled under Metal validation (MTL_DEBUG_LAYER / MTL_SHADER_VALIDATION)";
    return {};
}

bool archiveEnabled() { return archiveDisabledReason().empty(); }

bool forceSourceCompile() {
    const char* v = std::getenv("SKY_SHADER_SOURCE");
    return v && std::string_view(v) == "1";
}

}  // namespace sky::shadercache
