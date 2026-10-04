// Local (point / spot) light shadows for the Metal backend; see MetalShadows.h.

#import "MetalShadows.h"

#include "MetalProfiler.h"     // [profiler] pass timing
#include "MetalShaderCache.h"  // [shader cache] pipeline archive

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sky {

namespace {
constexpr MTLPixelFormat kAtlasFormat = MTLPixelFormatDepth32Float;
}

MetalShadows::MetalShadows(id<MTLDevice> device) : device_(device) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = kAtlasFormat;
    d.width = d.height = 1;
    d.arrayLength = 1;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    dummy_ = [device_ newTextureWithDescriptor:d];
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionAlways;
    ds.depthWriteEnabled = YES;
    always_ = [device_ newDepthStencilStateWithDescriptor:ds];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    write_ = [device_ newDepthStencilStateWithDescriptor:ds];
}

Status MetalShadows::build(id<MTLLibrary> lib, MTLPixelFormat outputFormat) {
    id<MTLFunction> clearFn = [lib newFunctionWithName:@"shadowClearVertex"];
    id<MTLFunction> fullscreen = [lib newFunctionWithName:@"fullscreenVertex"];
    id<MTLFunction> debugFn = [lib newFunctionWithName:@"shadowAtlasDebugFragment"];
    if (!clearFn || !fullscreen || !debugFn) {
        return Error::make("shader_missing_function", "shader source must define shadowClearVertex and shadowAtlasDebugFragment",
                           "restore engine/platform/metal/shaders/Shadows.metal");
    }
    NSError* e = nil;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = clearFn;
    d.depthAttachmentPixelFormat = kAtlasFormat;
    id<MTLRenderPipelineState> clear = newRenderPipeline(device_, d, &e);
    MTLRenderPipelineDescriptor* dd = [MTLRenderPipelineDescriptor new];
    dd.vertexFunction = fullscreen;
    dd.fragmentFunction = debugFn;
    dd.colorAttachments[0].pixelFormat = outputFormat;
    id<MTLRenderPipelineState> debug = clear ? newRenderPipeline(device_, dd, &e) : nil;
    if (!clear || !debug) {
        return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "local shadow pipelines failed");
    }
    clear_ = clear;
    debug_ = debug;
    planner_.invalidate();  // caster shaders may have changed
    return {};
}

bool MetalShadows::ensureAtlas(int size) {
    if (atlas_ && atlasSize_ == size) return true;
    atlas_ = nil;
    atlasSize_ = 0;
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = kAtlasFormat;
    d.width = d.height = static_cast<NSUInteger>(size / 2);
    d.arrayLength = shadows::kQuadrants;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    atlas_ = [device_ newTextureWithDescriptor:d];
    allocationFailed_ = atlas_ == nil;
    if (atlas_) {
        atlas_.label = @"Local shadow atlas";
        atlasSize_ = size;
    }
    return atlas_ != nil;
}

const shadows::ShadowPlan& MetalShadows::plan(const FrameData& frame, bool still) {
    auto t0 = std::chrono::steady_clock::now();
    plan_ = planner_.plan(frame, shadows::settingsFor(frame.environment, frame.quality, still));
    const bool needAtlas = plan_.shadowed > 0;
    if (needAtlas && !ensureAtlas(plan_.settings.atlas.size)) {
        // Out of GPU memory: light without shadows rather than sample garbage; retry next frame.
        planner_.invalidate();
        for (auto& ls : plan_.lights) {
            if (ls.projection == shadows::Projection::None) continue;
            ls.projection = shadows::Projection::None;
            ls.reason = "atlas_unavailable";
        }
        plan_.faces.clear();
        plan_.shadowed = 0;
        plan_.warnings.push_back("the local shadow atlas could not be allocated (" + std::to_string(plan_.settings.atlas.size) +
                                 " px); lower Environment.localShadowAtlas");
    }
    facesRendered_ = plan_.faces.size();
    planMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return plan_;
}

std::array<simd_float4, 2> MetalShadows::lightParams(size_t index) const {
    if (index >= plan_.lights.size()) return {simd_make_float4(0, 0, 0, 0), simd_make_float4(0, 0, 0, 0)};
    auto p = shadows::gpuShadowParams(plan_.lights[index], plan_.settings.atlas);
    return {simd_make_float4(p[0].x, p[0].y, p[0].z, p[0].w), simd_make_float4(p[1].x, p[1].y, p[1].z, p[1].w)};
}

void MetalShadows::encode(id<MTLCommandBuffer> cmd, const FrameData& frame, const Casters& casters) {
    (void)frame;
    if (!hasWork() || !clear_) return;
    for (int q = 0; q < shadows::kQuadrants; ++q) {
        bool any = false;
        for (const auto& f : plan_.faces) any = any || f.quadrant == q;
        if (!any) continue;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = atlas_;
        rp.depthAttachment.slice = static_cast<NSUInteger>(q);
        rp.depthAttachment.loadAction = MTLLoadActionLoad;  // other slots keep their cached shadows
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        static const char* kQuadrantLabels[] = {"Local shadows q0", "Local shadows q1", "Local shadows q2", "Local shadows q3"};
        profileRenderPass(rp, kQuadrantLabels[q % 4], "shadows");  // [profiler]
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = [NSString stringWithFormat:@"Local shadows (quadrant %d)", q];
        const NSUInteger limit = atlas_.width;
        for (const shadows::ShadowFace& f : plan_.faces) {
            if (f.quadrant != q) continue;
            // Bounds-checked: a face never writes outside its quadrant slice.
            const NSUInteger x = static_cast<NSUInteger>(std::max(f.viewport.x, 0)), y = static_cast<NSUInteger>(std::max(f.viewport.y, 0));
            if (x >= limit || y >= limit || f.viewport.w <= 0 || f.viewport.h <= 0) continue;
            const NSUInteger w = std::min<NSUInteger>(static_cast<NSUInteger>(f.viewport.w), limit - x);
            const NSUInteger h = std::min<NSUInteger>(static_cast<NSUInteger>(f.viewport.h), limit - y);
            [enc setViewport:MTLViewport{static_cast<double>(x), static_cast<double>(y), static_cast<double>(w), static_cast<double>(h), 0.0, 1.0}];
            [enc setScissorRect:MTLScissorRect{x, y, w, h}];
            [enc setRenderPipelineState:clear_];
            [enc setDepthStencilState:always_];
            [enc setDepthBias:0.f slopeScale:0.f clamp:0.f];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc setDepthStencilState:write_];
            [enc setCullMode:MTLCullModeNone];
            [enc setDepthBias:0.f slopeScale:1.5f clamp:0.f];
            casters(enc, f);
        }
        [enc endEncoding];
    }
}

void MetalShadows::encodeDebug(id<MTLCommandBuffer> cmd, id<MTLTexture> target) {
    if (!debug_ || !target) return;
    std::vector<simd_float4> rects;
    const float full = static_cast<float>(plan_.settings.atlas.size);
    for (const auto& ls : plan_.lights) {
        if (ls.projection == shadows::Projection::None || !ls.slot.valid()) continue;
        const float qx = static_cast<float>(ls.slot.quadrant % 2) * 0.5f, qy = static_cast<float>(ls.slot.quadrant / 2) * 0.5f;
        rects.push_back(simd_make_float4(qx + static_cast<float>(ls.slot.x) / full, qy + static_cast<float>(ls.slot.y) / full,
                                         static_cast<float>(ls.slot.size) / full, ls.updated ? 1.f : (ls.stale ? 2.f : 0.f)));
        rects.push_back(simd_make_float4(ls.near, ls.far, ls.projection == shadows::Projection::DualParaboloid ? 1.f : 0.f, 0.f));
        if (rects.size() >= 2 * static_cast<size_t>(shadows::kMaxShadowedLights)) break;
    }
    const size_t count = rects.size() / 2;
    if (rects.empty()) rects.assign(2, simd_make_float4(0, 0, 0, 0));
    struct {
        simd_float4 params;
    } p{simd_make_float4(static_cast<float>(count), static_cast<float>(target.width) / std::max<float>(1.f, target.height), 0, 0)};
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    profileRenderPass(rp, "Debug view", "debug");  // [profiler]
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    enc.label = @"Debug view: shadow atlas";
    [enc setRenderPipelineState:debug_];
    [enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
    [enc setFragmentBytes:rects.data() length:rects.size() * sizeof(simd_float4) atIndex:1];
    [enc setFragmentTexture:atlas() atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

Json MetalShadows::stats() const {
    return Json::object({{"lights", plan_.shadowed},
                         {"requested", plan_.requested},
                         {"cachedLights", plan_.cachedLights},
                         {"overBudget", plan_.overBudget},
                         {"facesRendered", static_cast<int64_t>(facesRendered_)},
                         {"facesDeferred", plan_.deferredFaces},
                         {"planMs", std::round(planMs_ * 1000.0) / 1000.0},
                         {"gpuMs", std::round(gpuMs_->load() * 100.0) / 100.0},
                         {"atlasMB", atlas_ ? std::round(static_cast<double>(atlasSize_) * atlasSize_ * 4.0 / 1048576.0) : 0.0}});
}

Json MetalShadows::info() const {
    Json j = planner_.info();
    j["allocated"] = atlas_ != nil;
    if (allocationFailed_) j["allocationFailed"] = true;
    j["planMs"] = std::round(planMs_ * 1000.0) / 1000.0;
    j["gpuMs"] = std::round(gpuMs_->load() * 100.0) / 100.0;
    return j;
}

}  // namespace sky
