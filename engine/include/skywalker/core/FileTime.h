#pragma once
// Portable file modification times for hot reload and cache keys.
//
// std::filesystem::file_time_type has an implementation-defined epoch: libc++ (macOS) counts from
// 1970, libstdc++ (Linux) from 2174, so `last_write_time(p).time_since_epoch().count()` is negative
// for every real file on Linux. Code that used a negative value to mean "missing" then treated every
// file as absent. fileModifiedNs() always counts nanoseconds since the Unix epoch, so any existing
// file has a positive stamp and -1 means "missing or unreadable" on every platform.

#include <sys/stat.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace sky {

/// Last modification time of `path` in nanoseconds since 1970-01-01 UTC, or -1 if it does not exist.
inline int64_t fileModifiedNs(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return -1;
#if defined(__APPLE__)
    const auto& ts = st.st_mtimespec;
#else
    const auto& ts = st.st_mtim;
#endif
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<int64_t>(ts.tv_nsec);
}

inline int64_t fileModifiedNs(const std::filesystem::path& path) { return fileModifiedNs(path.string()); }

}  // namespace sky
