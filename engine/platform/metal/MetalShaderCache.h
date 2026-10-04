#pragma once
// Shader library loading, the pipeline binary archive and asynchronous pipeline creation.
//
// Library: the built-in shaders are precompiled to a .metallib at build time when the offline
// Metal toolchain is installed (CMake `SKY_PRECOMPILE_SHADERS`), embedded in the binary and loaded
// with newLibraryWithData (no MSL front-end compile at startup). Without it, or with
// SKY_SHADER_SOURCE=1, or for `shader_set` hot reloads, the embedded source is compiled at runtime.
//
// Pipelines: every pipeline state the engine creates goes through `newRenderPipeline` /
// `newComputePipeline`, which consult an MTLBinaryArchive in the user cache directory
// (render/ShaderCache.h: versioned by a hash of the shader source, engine version, GPU and OS).
// A hit skips the GPU back-end compile; a miss compiles once and records the binary.
//
// Async: `requestRenderPipeline` / `compileLibraryAsync` build on a background queue and hand back
// a handle the renderer polls each frame (draw with a fallback until `ready()`). This is the API
// surface shaders (W22) build on.

#import <Metal/Metal.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

/// Result of loading the built-in shader library.
struct ShaderLibraryLoad {
    id<MTLLibrary> library;
    std::string origin;  // "metallib" (precompiled) | "source" (runtime compile)
    double ms = 0;       // time to load or compile
    std::string note;    // why the source path was taken (no toolchain, override, failure)
};

/// Loads the built-in library: the embedded .metallib when present (and SKY_SHADER_SOURCE != 1),
/// else compiles `source`. `required` lists functions the library must define (a metallib that
/// lacks one falls back to the source).
ShaderLibraryLoad loadBuiltinShaderLibrary(id<MTLDevice> device, const std::string& source,
                                           const std::vector<const char*>& required);
/// Runtime MSL compile with the engine's options (fast math).
id<MTLLibrary> compileShaderLibrary(id<MTLDevice> device, const std::string& source, NSError** error);
/// True when this binary embeds a precompiled library.
bool hasPrecompiledShaderLibrary();

/// A pipeline (or library) being built on a background queue.
template <typename T>
struct AsyncBuild {
    std::atomic<bool> done{false};
    T result;           // nil until done, nil on failure
    std::string error;  // compiler / pipeline diagnostics on failure
    double ms = 0;
    bool ready() const { return done.load(std::memory_order_acquire); }
};
using AsyncRenderPipeline = AsyncBuild<id<MTLRenderPipelineState>>;
using AsyncComputePipeline = AsyncBuild<id<MTLComputePipelineState>>;
using AsyncLibrary = AsyncBuild<id<MTLLibrary>>;

class MetalPipelineCache {
public:
    /// The shared cache for this device and key (created on first use; the archive file is
    /// `shadercache::archivePath(cacheDir(), key)`).
    static std::shared_ptr<MetalPipelineCache> acquire(id<MTLDevice> device, const std::string& key);
    /// The cache pipelines are currently created through (nullptr: plain compiles).
    static MetalPipelineCache* current();
    ~MetalPipelineCache();

    id<MTLRenderPipelineState> render(MTLRenderPipelineDescriptor* d, NSError** error);
    id<MTLComputePipelineState> compute(id<MTLFunction> fn, NSError** error);
    /// Background pipeline creation (also recorded in the archive). Poll `ready()`.
    std::shared_ptr<AsyncRenderPipeline> requestRender(MTLRenderPipelineDescriptor* d);
    std::shared_ptr<AsyncComputePipeline> requestCompute(id<MTLFunction> fn);

    /// While true, pipelines bypass the archive (hot-reloaded shader sources are not cached).
    void setBypass(bool bypass) { bypass_ = bypass; }
    /// Writes new archive entries to disk (atomic replace). No-op when nothing changed.
    Status save();
    /// {enabled, path, key, loaded, entries hits/misses/bypassed, loadMs, saveMs, bytes, pendingAsync}
    Json stats() const;

private:
    MetalPipelineCache(id<MTLDevice> device, const std::string& key);
    bool record(MTLRenderPipelineDescriptor* d);
    bool record(MTLComputePipelineDescriptor* d);

    id<MTLDevice> device_;
    id<MTLBinaryArchive> archive_;
    std::string key_, path_, loadError_;
    bool loaded_ = false;
    std::atomic<bool> dirty_{false}, bypass_{false};
    std::atomic<int> hits_{0}, misses_{0}, bypassed_{0}, pending_{0}, asyncBuilt_{0};
    double loadMs_ = 0, saveMs_ = 0;
    mutable std::mutex mutex_;  // archive mutation and serialization
};

/// Pipeline creation for every Metal file: through the current cache when there is one.
id<MTLRenderPipelineState> newRenderPipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor* d, NSError** error);
id<MTLComputePipelineState> newComputePipeline(id<MTLDevice> device, id<MTLFunction> fn, NSError** error);
/// Compiles MSL on a background queue (surface shaders, hot reload without a hitch).
std::shared_ptr<AsyncLibrary> compileShaderLibraryAsync(id<MTLDevice> device, const std::string& source);

}  // namespace sky
