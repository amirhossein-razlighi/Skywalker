// Shader library loading, pipeline binary archive, async pipeline creation (see MetalShaderCache.h).

#include "MetalShaderCache.h"

#include <dispatch/dispatch.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include "skywalker/core/Log.h"
#include "skywalker/render/ShaderCache.h"

// The precompiled built-in library (CMake: SKY_PRECOMPILE_SHADERS with the Metal toolchain installed).
#if defined(SKY_SHADER_METALLIB_FILE) && defined(__has_embed)
#if __has_embed(SKY_SHADER_METALLIB_FILE)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static const unsigned char kEmbeddedMetallib[] = {
#embed SKY_SHADER_METALLIB_FILE
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#define SKY_HAS_EMBEDDED_METALLIB 1
#endif
#endif

namespace sky {

namespace fs = std::filesystem;

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string errorText(NSError* e, const char* fallback) {
    return e ? std::string(e.localizedDescription.UTF8String) : std::string(fallback);
}

bool definesAll(id<MTLLibrary> lib, const std::vector<const char*>& required, std::string& missing) {
    NSArray<NSString*>* names = lib.functionNames;
    NSSet<NSString*>* have = [NSSet setWithArray:names];
    for (const char* r : required) {
        if (![have containsObject:[NSString stringWithUTF8String:r]]) {
            missing = r;
            return false;
        }
    }
    return true;
}

std::mutex gCacheMutex;
std::weak_ptr<MetalPipelineCache> gCurrent;

}  // namespace

bool hasPrecompiledShaderLibrary() {
#if defined(SKY_HAS_EMBEDDED_METALLIB)
    return true;
#else
    return false;
#endif
}

id<MTLLibrary> compileShaderLibrary(id<MTLDevice> device, const std::string& source, NSError** error) {
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.mathMode = MTLMathModeFast;
    return [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:opts error:error];
}

ShaderLibraryLoad loadBuiltinShaderLibrary(id<MTLDevice> device, const std::string& source,
                                           const std::vector<const char*>& required) {
    ShaderLibraryLoad out;
    const auto t0 = std::chrono::steady_clock::now();
    id<MTLLibrary> pre = nil;
    std::string note;
    if (shadercache::forceSourceCompile()) {
        note = "SKY_SHADER_SOURCE=1: compiled from source";
    } else if (const char* path = std::getenv("SKY_SHADER_METALLIB"); path && *path) {
        NSError* e = nil;
        pre = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&e];
        if (!pre) note = "SKY_SHADER_METALLIB '" + std::string(path) + "' failed to load: " + errorText(e, "unknown error");
    } else {
#if defined(SKY_HAS_EMBEDDED_METALLIB)
        dispatch_data_t data = dispatch_data_create(kEmbeddedMetallib, sizeof(kEmbeddedMetallib), nullptr, ^{});
        NSError* e = nil;
        pre = [device newLibraryWithData:data error:&e];
        if (!pre) note = "embedded .metallib failed to load (" + errorText(e, "unknown error") + "): compiled from source";
#else
        note = "built without the offline Metal toolchain (install it with `xcodebuild -downloadComponent MetalToolchain` "
               "and rebuild for a precompiled .metallib): compiled from source";
#endif
    }
    if (pre) {
        std::string missing;
        if (definesAll(pre, required, missing)) {
            out.library = pre;
            out.origin = "metallib";
            out.ms = msSince(t0);
            return out;
        }
        note = "precompiled library lacks '" + missing + "' (stale build?): compiled from source";
    }
    NSError* e = nil;
    out.library = compileShaderLibrary(device, source, &e);
    out.origin = "source";
    out.ms = msSince(t0);
    out.note = out.library ? note : note + "; " + errorText(e, "compile failed");
    return out;
}

std::shared_ptr<AsyncLibrary> compileShaderLibraryAsync(id<MTLDevice> device, const std::string& source) {
    auto job = std::make_shared<AsyncLibrary>();
    const auto t0 = std::chrono::steady_clock::now();
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.mathMode = MTLMathModeFast;
    [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                         options:opts
               completionHandler:^(id<MTLLibrary> lib, NSError* e) {
                   job->result = lib;
                   if (!lib) job->error = errorText(e, "compile failed");
                   job->ms = msSince(t0);
                   job->done.store(true, std::memory_order_release);
               }];
    return job;
}

// ---------------------------------------------------------------------------
// MetalPipelineCache
// ---------------------------------------------------------------------------

std::shared_ptr<MetalPipelineCache> MetalPipelineCache::acquire(id<MTLDevice> device, const std::string& key) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (auto c = gCurrent.lock(); c && c->key_ == key && c->device_ == device) return c;
    std::shared_ptr<MetalPipelineCache> c(new MetalPipelineCache(device, key));
    gCurrent = c;
    return c;
}

MetalPipelineCache* MetalPipelineCache::current() {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    return gCurrent.lock().get();  // owners keep it alive while they create pipelines
}

MetalPipelineCache::MetalPipelineCache(id<MTLDevice> device, const std::string& key) : device_(device), key_(key) {
    if (std::string why = shadercache::archiveDisabledReason(); !why.empty()) {
        loadError_ = why;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string dir = shadercache::cacheDir();
    path_ = shadercache::archivePath(dir, key);
    std::error_code ec;
    MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
    if (fs::exists(path_, ec)) ad.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path_.c_str()]];
    NSError* e = nil;
    archive_ = [device_ newBinaryArchiveWithDescriptor:ad error:&e];
    if (archive_ && ad.url) loaded_ = true;
    if (!archive_ && ad.url) {  // unreadable (another OS/GPU, corrupt): start over
        log::warn("render", "pipeline cache '" + path_ + "' could not be read (" + errorText(e, "unknown") + "); rebuilding it");
        fs::remove(path_, ec);
        ad.url = nil;
        archive_ = [device_ newBinaryArchiveWithDescriptor:ad error:&e];
    }
    if (!archive_) loadError_ = errorText(e, "MTLBinaryArchive unavailable");
    shadercache::pruneArchives(dir, key);
    loadMs_ = msSince(t0);
}

MetalPipelineCache::~MetalPipelineCache() {
    if (Status s = save(); !s) log::warn("render", "pipeline cache not saved: " + s.error().message);
}

bool MetalPipelineCache::record(MTLRenderPipelineDescriptor* d) {
    std::lock_guard<std::mutex> lock(mutex_);
    NSError* e = nil;
    if (![archive_ addRenderPipelineFunctionsWithDescriptor:d error:&e]) return false;
    dirty_ = true;
    return true;
}

bool MetalPipelineCache::record(MTLComputePipelineDescriptor* d) {
    std::lock_guard<std::mutex> lock(mutex_);
    NSError* e = nil;
    if (![archive_ addComputePipelineFunctionsWithDescriptor:d error:&e]) return false;
    dirty_ = true;
    return true;
}

id<MTLRenderPipelineState> MetalPipelineCache::render(MTLRenderPipelineDescriptor* d, NSError** error) {
    if (!archive_ || bypass_) {
        bypassed_.fetch_add(1);
        return [device_ newRenderPipelineStateWithDescriptor:d error:error];
    }
    MTLRenderPipelineDescriptor* c = [d copy];
    c.binaryArchives = @[ archive_ ];
    NSError* miss = nil;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:c
                                                                           options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                        reflection:nil
                                                                             error:&miss];
    if (pso) {
        hits_.fetch_add(1);
        return pso;
    }
    misses_.fetch_add(1);
    if (!record(c)) c.binaryArchives = nil;  // not archivable: plain compile
    return [device_ newRenderPipelineStateWithDescriptor:c options:MTLPipelineOptionNone reflection:nil error:error];
}

id<MTLComputePipelineState> MetalPipelineCache::compute(id<MTLFunction> fn, NSError** error) {
    if (!fn) return nil;
    if (!archive_ || bypass_) {
        bypassed_.fetch_add(1);
        return [device_ newComputePipelineStateWithFunction:fn error:error];
    }
    MTLComputePipelineDescriptor* c = [MTLComputePipelineDescriptor new];
    c.computeFunction = fn;
    c.binaryArchives = @[ archive_ ];
    NSError* miss = nil;
    id<MTLComputePipelineState> pso = [device_ newComputePipelineStateWithDescriptor:c
                                                                             options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                          reflection:nil
                                                                               error:&miss];
    if (pso) {
        hits_.fetch_add(1);
        return pso;
    }
    misses_.fetch_add(1);
    if (!record(c)) c.binaryArchives = nil;
    return [device_ newComputePipelineStateWithDescriptor:c options:MTLPipelineOptionNone reflection:nil error:error];
}

std::shared_ptr<AsyncRenderPipeline> MetalPipelineCache::requestRender(MTLRenderPipelineDescriptor* d) {
    auto job = std::make_shared<AsyncRenderPipeline>();
    if (!d || !d.vertexFunction) {  // Metal asserts on these; report instead
        job->error = "pipeline descriptor has no vertex function (missing or misspelled shader entry point?)";
        job->done.store(true, std::memory_order_release);
        return job;
    }
    MTLRenderPipelineDescriptor* c = [d copy];
    pending_.fetch_add(1);
    // The cache outlives its jobs: owners keep the shared_ptr, and the job holds its own result.
    std::weak_ptr<MetalPipelineCache> weak = gCurrent;
    id<MTLDevice> device = device_;
    MetalPipelineCache* self = this;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const auto t0 = std::chrono::steady_clock::now();
        NSError* e = nil;
        auto keep = weak.lock();
        id<MTLRenderPipelineState> pso = keep.get() == self ? self->render(c, &e)
                                                             : [device newRenderPipelineStateWithDescriptor:c error:&e];
        job->result = pso;
        if (!pso) job->error = errorText(e, "pipeline creation failed");
        job->ms = msSince(t0);
        if (keep.get() == self) {
            self->pending_.fetch_sub(1);
            self->asyncBuilt_.fetch_add(1);
        }
        job->done.store(true, std::memory_order_release);
    });
    return job;
}

std::shared_ptr<AsyncComputePipeline> MetalPipelineCache::requestCompute(id<MTLFunction> fn) {
    auto job = std::make_shared<AsyncComputePipeline>();
    if (!fn) {
        job->error = "no compute function (missing or misspelled kernel name?)";
        job->done.store(true, std::memory_order_release);
        return job;
    }
    pending_.fetch_add(1);
    std::weak_ptr<MetalPipelineCache> weak = gCurrent;
    id<MTLDevice> device = device_;
    MetalPipelineCache* self = this;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const auto t0 = std::chrono::steady_clock::now();
        NSError* e = nil;
        auto keep = weak.lock();
        id<MTLComputePipelineState> pso = keep.get() == self ? self->compute(fn, &e) : [device newComputePipelineStateWithFunction:fn error:&e];
        job->result = pso;
        if (!pso) job->error = errorText(e, "pipeline creation failed");
        job->ms = msSince(t0);
        if (keep.get() == self) {
            self->pending_.fetch_sub(1);
            self->asyncBuilt_.fetch_add(1);
        }
        job->done.store(true, std::memory_order_release);
    });
    return job;
}

Status MetalPipelineCache::save() {
    if (!archive_ || !dirty_.load() || path_.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    const std::string tmp = path_ + ".tmp" + std::to_string(static_cast<long long>(::getpid()));
    NSError* e = nil;
    if (![archive_ serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp.c_str()]] error:&e]) {
        fs::remove(tmp, ec);
        return Error::make("cache_write_failed", "could not write the pipeline cache: " + errorText(e, "unknown error"),
                           "check that " + fs::path(path_).parent_path().string() + " is writable, or set SKY_SHADER_CACHE_DIR");
    }
    fs::rename(tmp, path_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return Error::make("cache_write_failed", "could not replace " + path_ + ": " + ec.message());
    }
    dirty_ = false;
    saveMs_ = msSince(t0);
    return {};
}

Json MetalPipelineCache::stats() const {
    std::error_code ec;
    const auto bytes = path_.empty() ? 0 : fs::file_size(path_, ec);
    Json j = Json::object({{"enabled", archive_ != nil},
                           {"path", path_},
                           {"key", key_},
                           {"loadedFromDisk", loaded_},
                           {"hits", hits_.load()},
                           {"misses", misses_.load()},
                           {"bypassed", bypassed_.load()},
                           {"asyncBuilt", asyncBuilt_.load()},
                           {"pendingAsync", pending_.load()},
                           {"unsaved", dirty_.load()},
                           {"loadMs", std::round(loadMs_ * 100) / 100},
                           {"saveMs", std::round(saveMs_ * 100) / 100},
                           {"bytes", static_cast<int64_t>(ec ? 0 : bytes)}});
    if (!loadError_.empty()) j["note"] = loadError_;
    return j;
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

id<MTLRenderPipelineState> newRenderPipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor* d, NSError** error) {
    if (MetalPipelineCache* c = MetalPipelineCache::current()) return c->render(d, error);
    return [device newRenderPipelineStateWithDescriptor:d error:error];
}

id<MTLComputePipelineState> newComputePipeline(id<MTLDevice> device, id<MTLFunction> fn, NSError** error) {
    if (!fn) return nil;
    if (MetalPipelineCache* c = MetalPipelineCache::current()) return c->compute(fn, error);
    return [device newComputePipelineStateWithFunction:fn error:error];
}

}  // namespace sky
