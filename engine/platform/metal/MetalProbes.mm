// Reflection probes for the Metal backend; see MetalProbes.h.

#import "MetalProbes.h"

#include "MetalProfiler.h"     // [profiler] pass timing
#include "MetalShaderCache.h"  // [shader cache] pipeline archive
#include "skywalker/render/LightClusters.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace sky {

namespace {

constexpr MTLPixelFormat kAtlasFormat = MTLPixelFormatRGBA16Float;

struct FilterParams {
    float params[4];  // ProbeFilterParams in Probes.metal
};

struct DebugParams {
    float params[4];  // ProbeDebugParams in Probes.metal
};

float linearToSrgb(float c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

}  // namespace

MetalProbes::MetalProbes(id<MTLDevice> device, id<MTLCommandQueue> queue) : device_(device), queue_(queue) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureTypeCubeArray;
    d.pixelFormat = kAtlasFormat;
    d.width = d.height = 1;
    d.arrayLength = 1;
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    dummy_ = [device_ newTextureWithDescriptor:d];
    masks_.assign(1, 0u);
}

Status MetalProbes::build(id<MTLLibrary> lib, const ProbeFormats& formats) {
    formats_ = formats;
    id<MTLFunction> fullscreen = [lib newFunctionWithName:@"fullscreenVertex"];
    id<MTLFunction> sky = [lib newFunctionWithName:@"probeSkyFragment"];
    id<MTLFunction> filter = [lib newFunctionWithName:@"probeFilterKernel"];
    id<MTLFunction> debug = [lib newFunctionWithName:@"probeDebugFragment"];
    if (!fullscreen || !sky || !filter || !debug) {
        return Error::make("shader_missing_function", "shader source must define probeSkyFragment, probeFilterKernel and probeDebugFragment",
                           "restore engine/platform/metal/shaders/Probes.metal");
    }
    NSError* e = nil;
    MTLRenderPipelineDescriptor* sd = [MTLRenderPipelineDescriptor new];
    sd.vertexFunction = fullscreen;
    sd.fragmentFunction = sky;
    sd.rasterSampleCount = formats.samples;
    sd.colorAttachments[0].pixelFormat = formats.color;
    sd.colorAttachments[1].pixelFormat = formats.gbufA;
    sd.colorAttachments[2].pixelFormat = formats.gbufB;
    sd.colorAttachments[3].pixelFormat = formats.velocity;
    sd.depthAttachmentPixelFormat = formats.depth;
    id<MTLRenderPipelineState> skyPso = newRenderPipeline(device_, sd, &e);
    id<MTLComputePipelineState> filterPso = skyPso ? newComputePipeline(device_, filter, &e) : nil;
    MTLRenderPipelineDescriptor* dd = [MTLRenderPipelineDescriptor new];
    dd.vertexFunction = fullscreen;
    dd.fragmentFunction = debug;
    dd.colorAttachments[0].pixelFormat = formats.output;
    dd.colorAttachments[0].blendingEnabled = YES;
    dd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    dd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    dd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    dd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> debugPso = filterPso ? newRenderPipeline(device_, dd, &e) : nil;
    if (!debugPso) {
        return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "reflection probe pipelines failed");
    }
    skyPipeline_ = skyPso;
    filterKernel_ = filterPso;
    debugPipeline_ = debugPso;
    planner_.invalidate();  // the scene shaders may have changed: captures re-render
    return {};
}

bool MetalProbes::ensureAtlas(int resolution, int slots) {
    if (atlas_ && atlasResolution_ == resolution && atlasSlots_ == slots) return true;
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureTypeCubeArray;
    d.pixelFormat = kAtlasFormat;
    d.width = d.height = static_cast<NSUInteger>(resolution);
    d.mipmapLevelCount = probes::kMips;
    d.arrayLength = static_cast<NSUInteger>(slots);
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> next = [device_ newTextureWithDescriptor:d];
    allocationFailed_ = next == nil;
    if (!next) return false;
    next.label = @"Reflection probe atlas";
    if (atlas_ && atlasResolution_ == resolution) {
        // More slots, same layout: the cached cubes move over (no re-capture).
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        cmd.label = @"Probe atlas grow";
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        const NSUInteger cubes = static_cast<NSUInteger>(std::min(atlasSlots_, slots));
        [blit copyFromTexture:atlas_
                  sourceSlice:0
                  sourceLevel:0
                    toTexture:next
             destinationSlice:0
             destinationLevel:0
                   sliceCount:cubes * probes::kFaces
                   levelCount:probes::kMips];
        [blit endEncoding];
        [cmd commit];
    }
    atlas_ = next;
    atlasResolution_ = resolution;
    atlasSlots_ = slots;
    return true;
}

MetalProbes::Targets& MetalProbes::targets(int size) {
    auto it = targets_.find(size);
    if (it != targets_.end()) return it->second;
    Targets t;
    const NSUInteger s = static_cast<NSUInteger>(size);
    MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:formats_.color size:s mipmapped:YES];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    t.scratch = [device_ newTextureWithDescriptor:cd];
    t.scratch.label = @"Probe capture cube";
    auto msaa = [&](MTLPixelFormat format) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:s height:s mipmapped:NO];
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = formats_.samples;
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModeMemoryless;  // tile memory only on Apple GPUs
        id<MTLTexture> tex = [device_ newTextureWithDescriptor:d];
        if (!tex) {
            d.storageMode = MTLStorageModePrivate;
            tex = [device_ newTextureWithDescriptor:d];
        }
        return tex;
    };
    t.color = msaa(formats_.color);
    t.gbufA = msaa(formats_.gbufA);
    t.gbufB = msaa(formats_.gbufB);
    t.velocity = msaa(formats_.velocity);
    t.depth = msaa(formats_.depth);
    return targets_[size] = t;
}

const probes::Plan& MetalProbes::plan(const FrameData& frame, const LightGrid& grid, bool still) {
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(timing_->mutex);
        for (const auto& [entity, ms] : timing_->done) planner_.reportCaptureMs(entity, ms);
        timing_->done.clear();
    }
    plan_ = planner_.plan(frame, probes::settingsFor(frame.environment, frame.quality, still));
    exposure_ = frame.environment.exposure * std::exp2(frame.environment.exposureCompensation);
    const bool needAtlas = plan_.atlasSlots > 0 && (!plan_.jobs.empty() || !plan_.shaded.empty());
    if (needAtlas && !ensureAtlas(plan_.atlasResolution, plan_.atlasSlots)) {
        // Out of GPU memory: fall back to the sky rather than sample garbage; retry next frame.
        planner_.atlasLost();
        plan_.jobs.clear();
        plan_.shaded.clear();
        for (auto& s : plan_.probes) s.shaded = false, s.reason = "atlas_unavailable";
        plan_.warnings.push_back("the reflection probe atlas could not be allocated (" + std::to_string(plan_.atlasSlots) + " x " +
                                 std::to_string(plan_.atlasResolution) + " px cubes); lower Environment.probeBudget or probe resolution");
    }
    block_ = probes::gpuBlock(frame, plan_);
    if (!plan_.shaded.empty()) {
        masks_ = probes::clusterMasks(frame, grid, plan_);
    } else {
        masks_.assign(1, 0u);
    }
    planMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return plan_;
}

probes::GpuProbeBlock MetalProbes::captureBlock(const FrameData& frame, const ProbeItem& probe, const std::vector<char>& valid,
                                                uint32_t& mask) const {
    probes::GpuProbeBlock b{};
    b.info[1] = static_cast<float>(probes::kMips - 1);
    b.info[2] = probe.interior ? 3.f : 1.f;  // capture pass (+ interior: its ambient instead of the sky)
    b.ambient[0] = probe.ambientColor.x, b.ambient[1] = probe.ambientColor.y, b.ambient[2] = probe.ambientColor.z;
    std::vector<int> order;
    for (size_t i = 0; i < plan_.probes.size() && i < frame.probes.size(); ++i) {
        const ProbeItem& q = frame.probes[i];
        if (!valid[i] || plan_.probes[i].slot < 0) continue;
        if (distance(q.center, probe.capture) > probes::boundingRadius(q) + probe.farPlane) continue;
        order.push_back(static_cast<int>(i));
    }
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return probes::shadesBefore(frame.probes[static_cast<size_t>(x)], frame.probes[static_cast<size_t>(y)]);
    });
    int n = 0;
    for (int idx : order) {
        if (n >= probes::kMaxProbes) break;
        const probes::ProbeState& s = plan_.probes[static_cast<size_t>(idx)];
        b.probes[n++] = probes::gpuProbe(frame.probes[static_cast<size_t>(idx)], s.slot, s.baseMip, s.slot);
    }
    b.info[0] = static_cast<float>(n);
    mask = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
    return b;
}

void MetalProbes::filter(id<MTLCommandBuffer> cmd, const ProbeItem& probe, const probes::ProbeState& state) {
    if (state.slot < 0 || state.slot >= atlasSlots_ || !atlas_) return;  // bounds: never write outside the atlas
    Targets& t = targets(probe.resolution);
    id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Probe mips", "probes");
    [blit generateMipmapsForTexture:t.scratch];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> enc = profiledCompute(cmd, "Probe filter", "probes");
    [enc setComputePipelineState:filterKernel_];
    [enc setTexture:t.scratch atIndex:0];
    const int maxMip = probes::kMips - 1;
    const int base = std::clamp(state.baseMip, 0, maxMip - 1);
    for (int level = base; level <= maxMip; ++level) {
        const int size = std::max(atlasResolution_ >> level, 1);
        id<MTLTexture> view = [atlas_ newTextureViewWithPixelFormat:kAtlasFormat
                                                        textureType:MTLTextureType2DArray
                                                             levels:NSMakeRange(static_cast<NSUInteger>(level), 1)
                                                             slices:NSMakeRange(static_cast<NSUInteger>(state.slot) * probes::kFaces, probes::kFaces)];
        if (!view) continue;
        FilterParams p{{static_cast<float>(level - base) / static_cast<float>(maxMip - base), static_cast<float>(size),
                        static_cast<float>(probe.resolution), static_cast<float>(t.scratch.mipmapLevelCount)}};
        [enc setTexture:view atIndex:1];
        [enc setBytes:&p length:sizeof(p) atIndex:0];
        const NSUInteger group = std::min<NSUInteger>(8, static_cast<NSUInteger>(size));
        [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(size), static_cast<NSUInteger>(size), probes::kFaces)
            threadsPerThreadgroup:MTLSizeMake(group, group, 1)];
    }
    [enc endEncoding];
}

void MetalProbes::encodeFaces(id<MTLCommandBuffer> cmd, Targets& t, Face& face, int first, int last, const Callbacks& cb) {
    const ProbeItem& item = *face.probe;
    for (int f = std::max(first, 0); f < std::min(last, probes::kFaces); ++f) {
        face.face = f;
        face.viewProj = probes::faceViewProj(item.capture, f, item.nearPlane, item.farPlane);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.color;
        rp.colorAttachments[0].resolveTexture = t.scratch;
        rp.colorAttachments[0].resolveSlice = static_cast<NSUInteger>(f);
        rp.colorAttachments[0].resolveLevel = 0;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        id<MTLTexture> gbuf[3] = {t.gbufA, t.gbufB, t.velocity};
        for (NSUInteger a = 0; a < 3; ++a) {
            rp.colorAttachments[a + 1].texture = gbuf[a];
            rp.colorAttachments[a + 1].loadAction = MTLLoadActionClear;
            rp.colorAttachments[a + 1].storeAction = MTLStoreActionDontCare;
        }
        rp.depthAttachment.texture = t.depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        profileRenderPass(rp, "Probe capture", "probes");  // [profiler]
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = [NSString stringWithFormat:@"Probe capture (face %d)", f];
        const double s = static_cast<double>(item.resolution);
        [enc setViewport:MTLViewport{0.0, 0.0, s, s, 0.0, 1.0}];
        [enc setFrontFacingWinding:MTLWindingClockwise];  // cube faces are mirrored images
        cb.drawFace(enc, face);
        [enc endEncoding];
    }
}

void MetalProbes::capture(const FrameData& frame, const Callbacks& cb, std::shared_ptr<std::atomic<int>> gpuFaults) {
    if (!hasCaptures() || !cb.drawFace) return;
    if (!sunShadow_) {  // the captures' own sun shadow map (the frame's cascades stay untouched)
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats_.depth width:2048 height:2048 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        sunShadow_ = [device_ newTextureWithDescriptor:d];
        sunShadow_.label = @"Probe sun shadow";
        if (!sunShadow_) return;
    }
    const Vec3 sunDir = frame.environment.sunDirection();
    // Which probes hold a capture that captured surfaces may use (bounce light); updated as jobs finish.
    std::vector<char> valid(plan_.probes.size(), 0);
    for (size_t i = 0; i < plan_.probes.size(); ++i) valid[i] = plan_.probes[i].hadCapture && plan_.probes[i].slot >= 0 ? 1 : 0;
    for (const probes::CaptureJob& job : plan_.jobs) {
        if (job.probe < 0 || static_cast<size_t>(job.probe) >= frame.probes.size()) continue;
        const ProbeItem& item = frame.probes[static_cast<size_t>(job.probe)];
        const probes::ProbeState& state = plan_.probes[static_cast<size_t>(job.probe)];
        Targets& t = targets(item.resolution);
        if (!t.scratch || !t.color || !t.depth) continue;
        // One command buffer per probe: a capture is a handful of small passes, never a long GPU stall.
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        cmd.label = @"Reflection probe capture";
        Face face;
        face.probe = &item;
        face.size = item.resolution;
        face.sunRadius = std::clamp(probes::boundingRadius(item) * 1.25f, 4.f, item.farPlane);
        face.sunViewProj = probes::sunViewProj(item.capture, face.sunRadius, sunDir);
        face.sunShadow = sunShadow_;
        if (cb.sunShadow) cb.sunShadow(cmd, item, face.sunViewProj, sunShadow_);
        // Pass 0: the planned faces. Pass 1 (first captures): the whole cube again, lit by pass 0.
        for (int pass = 0; pass < (job.bounce ? 2 : 1); ++pass) {
            uint32_t mask = 0;
            const probes::GpuProbeBlock block = captureBlock(frame, item, valid, mask);
            face.block = &block;
            face.mask = mask;
            if (pass == 0) {
                encodeFaces(cmd, t, face, job.firstFace, job.firstFace + job.faceCount, cb);
            } else {
                encodeFaces(cmd, t, face, 0, probes::kFaces, cb);
            }
            if (!job.completes) break;
            filter(cmd, item, state);
            valid[static_cast<size_t>(job.probe)] = 1;  // later captures (and the bounce pass) see it
        }
        auto timing = timing_;
        const EntityId entity = item.entity;
        auto faults = gpuFaults;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            const double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
            if (done.status == MTLCommandBufferStatusError && faults) faults->fetch_add(1);
            std::lock_guard<std::mutex> lock(timing->mutex);
            if (ms > 0.0) {
                timing->done.emplace_back(entity, ms);
                timing->lastMs = ms;
            }
        }];
        [cmd commit];
    }
}

void MetalProbes::encodeDebug(id<MTLCommandBuffer> cmd, id<MTLTexture> target, id<MTLTexture> depth, const FrameData& frame,
                              const void* uniforms, size_t uniformsSize) {
    if (!debugPipeline_ || !target || !depth) return;
    // Every probe (up to the cap) in shading order; probes without a capture are outlined, not tinted.
    std::vector<int> order;
    for (size_t i = 0; i < frame.probes.size() && i < plan_.probes.size(); ++i) order.push_back(static_cast<int>(i));
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return probes::shadesBefore(frame.probes[static_cast<size_t>(a)], frame.probes[static_cast<size_t>(b)]);
    });
    probes::GpuProbeBlock volumes{};
    int n = 0;
    for (int idx : order) {
        if (n >= probes::kMaxProbes) break;
        const probes::ProbeState& s = plan_.probes[static_cast<size_t>(idx)];
        const int color = s.slot >= 0 ? s.slot : 12 + n;
        volumes.probes[n] = probes::gpuProbe(frame.probes[static_cast<size_t>(idx)], s.ready ? s.slot : -1, s.baseMip, color);
        ++n;
    }
    volumes.info[0] = static_cast<float>(n);
    const float fov = radians(std::clamp(frame.camera.fovDeg, 1.f, 179.f));
    DebugParams dp{{2.f * std::tan(fov * 0.5f) / std::max<float>(1.f, static_cast<float>(target.height)),
                    static_cast<float>(target.height), 0.f, 0.f}};
    if (frame.camera.orthographic) dp.params[0] = 1e-3f;
    id<MTLBuffer> vb = [device_ newBufferWithBytes:&volumes length:sizeof(volumes) options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    profileRenderPass(rp, "Debug view", "debug");  // [profiler]
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    enc.label = @"Debug view: reflection probes";
    [enc setRenderPipelineState:debugPipeline_];
    [enc setFragmentBytes:uniforms length:uniformsSize atIndex:0];
    [enc setFragmentBuffer:vb offset:0 atIndex:1];
    [enc setFragmentBytes:&dp length:sizeof(dp) atIndex:2];
    [enc setFragmentTexture:depth atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

Result<Image> MetalProbes::faceImage(EntityId entity, int mip) {
    const probes::ProbeState* state = nullptr;
    for (const auto& s : plan_.probes) {
        if (s.entity == entity) state = &s;
    }
    if (!state) {
        return Error::make("probe_not_rendered", "probe #" + std::to_string(entity) + " was not in the last frame",
                           "render a frame that contains it first (viewport_capture or probe_bake)");
    }
    if (!state->ready || state->slot < 0 || !atlas_ || state->slot >= atlasSlots_) {
        return Error::make("probe_not_ready", "probe #" + std::to_string(entity) + " has no capture yet" +
                                                  (state->reason.empty() ? std::string() : " (" + state->reason + ")"),
                           "run probe_bake, or check probe_info for why it has no atlas slot");
    }
    const int level = std::clamp(state->baseMip + std::max(mip, 0), 0, probes::kMips - 1);
    const NSUInteger size = static_cast<NSUInteger>(std::max(atlasResolution_ >> level, 1));
    const NSUInteger faceBytes = size * size * 8;
    id<MTLBuffer> buffer = [device_ newBufferWithLength:faceBytes * probes::kFaces options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    for (NSUInteger f = 0; f < static_cast<NSUInteger>(probes::kFaces); ++f) {
        [blit copyFromTexture:atlas_
                         sourceSlice:static_cast<NSUInteger>(state->slot) * probes::kFaces + f
                         sourceLevel:static_cast<NSUInteger>(level)
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(size, size, 1)
                            toBuffer:buffer
                   destinationOffset:f * faceBytes
              destinationBytesPerRow:size * 8
            destinationBytesPerImage:faceBytes];
    }
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status == MTLCommandBufferStatusError) return Error::make("gpu_error", "reading the probe atlas failed");
    const auto* px = static_cast<const __fp16*>(buffer.contents);
    // The scene's exposure and a filmic curve, so the faces read like the frame they light.
    const float k = std::max(exposure_, 1e-3f);
    // Horizontal cross: +Y above +Z; -X, +Z, +X, -Z across; -Y below.
    const int cell[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}};
    const int s = static_cast<int>(size);
    Image img(4 * s, 3 * s);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = 24;
        img.pixels[i + 3] = 255;
    }
    for (int f = 0; f < probes::kFaces; ++f) {
        for (int y = 0; y < s; ++y) {
            for (int x = 0; x < s; ++x) {
                const __fp16* c = px + (static_cast<size_t>(f) * size * size + static_cast<size_t>(y) * size + static_cast<size_t>(x)) * 4;
                uint8_t* o = img.at(cell[f][0] * s + x, cell[f][1] * s + y);
                for (int ch = 0; ch < 3; ++ch) {
                    const float v = std::max(static_cast<float>(c[ch]), 0.f) * k;
                    const float mapped = (v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f);  // ACES fit
                    o[ch] = static_cast<uint8_t>(linearToSrgb(mapped) * 255.f + 0.5f);
                }
            }
        }
    }
    return img;
}

Json MetalProbes::info(const FrameData* frame) const {
    Json j = planner_.info(frame);
    j["allocated"] = atlas_ != nil;
    if (allocationFailed_) j["allocationFailed"] = true;
    j["planMs"] = std::round(planMs_ * 1000.0) / 1000.0;
    {
        std::lock_guard<std::mutex> lock(timing_->mutex);
        j["lastCaptureGpuMs"] = std::round(timing_->lastMs * 100.0) / 100.0;
    }
    return j;
}

Json MetalProbes::stats() const {
    int ready = 0;
    for (const auto& s : plan_.probes) ready += s.ready ? 1 : 0;
    const double mb = probes::slotBytes(atlasResolution_) * atlasSlots_ / (1024.0 * 1024.0);
    double last = 0.0;
    {
        std::lock_guard<std::mutex> lock(timing_->mutex);
        last = timing_->lastMs;
    }
    return Json::object({{"probes", static_cast<int64_t>(plan_.probes.size())},
                         {"ready", ready},
                         {"shaded", static_cast<int64_t>(plan_.shaded.size())},
                         {"slots", atlasSlots_},
                         {"atlasResolution", atlasResolution_},
                         {"atlasMB", atlas_ ? std::round(mb * 10.0) / 10.0 : 0.0},
                         {"facesCaptured", plan_.facesCaptured},
                         {"facesDeferred", plan_.facesDeferred},
                         {"overBudget", plan_.overBudget},
                         {"planMs", std::round(planMs_ * 1000.0) / 1000.0},
                         {"lastCaptureGpuMs", std::round(last * 100.0) / 100.0}});
}

}  // namespace sky
