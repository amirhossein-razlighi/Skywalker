#pragma once
// Shader and pipeline cache bookkeeping (CPU side, unit-tested). The Metal backend stores compiled
// pipeline binaries in an MTLBinaryArchive under `cacheDir()`; the file name carries a key hashed
// from everything that invalidates GPU binaries (shader source, engine version, GPU, OS build), so
// a changed shader never loads stale code and old archives are simply pruned.
//
// Environment:
//   SKY_SHADER_CACHE_DIR=<dir>  cache directory (default: ~/Library/Caches/Skywalker/shaders on
//                               macOS, $XDG_CACHE_HOME/skywalker/shaders or ~/.cache/... elsewhere)
//   SKY_SHADER_CACHE=0          disable the pipeline archive (always compile)
//   SKY_SHADER_SOURCE=1         ignore the precompiled .metallib and compile the embedded source

#include <cstdint>
#include <string>
#include <string_view>

namespace sky::shadercache {

/// 64-bit FNV-1a hash (stable across platforms and runs).
uint64_t fnv1a64(std::string_view data, uint64_t seed = 14695981039346656037ULL);
/// 16 lowercase hex digits.
std::string hex64(uint64_t v);
/// Cache key for a shader library + pipeline set: changes whenever any input changes.
std::string cacheKey(std::string_view source, std::string_view engineVersion, std::string_view device, std::string_view os);
/// The per-user cache directory (not created).
std::string cacheDir();
/// `<dir>/pipelines-<key>.binarchive`.
std::string archivePath(const std::string& dir, const std::string& key);
/// Deletes `pipelines-*.binarchive` files in `dir` other than the current key's, keeping the `keep`
/// most recently modified others (several engine builds may share a machine). Returns files removed.
size_t pruneArchives(const std::string& dir, const std::string& currentKey, size_t keep = 3);
/// Why the pipeline archive is off ("" = on): SKY_SHADER_CACHE=0, or Metal API/shader validation
/// (MTL_DEBUG_LAYER / MTL_SHADER_VALIDATION instrument every pipeline, and loading an archive under
/// the GPU validation device crashes inside Metal).
std::string archiveDisabledReason();
bool archiveEnabled();
/// True when SKY_SHADER_SOURCE=1 (skip the precompiled library).
bool forceSourceCompile();

}  // namespace sky::shadercache
