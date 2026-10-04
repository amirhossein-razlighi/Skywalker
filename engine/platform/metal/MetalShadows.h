#pragma once
// Local light shadows for the Metal backend (docs/RENDERING.md, "Point and spot light shadows").
//
//   * plan()    once per frame, before the lights are uploaded: shadows::LocalShadowPlanner decides
//               the shadowed lights, their atlas slots and which faces must re-render (static caching,
//               budgets); lightParams() then fills GPULight::shadow / shadow2.
//   * encode()  renders the planned faces into the atlas (a depth2d_array, one slice per quadrant):
//               per face a viewport, a far-plane clear and the caster callback (meshes, terrain,
//               foliage, hair, mesh particles) with the face's view-projection.
//   * atlas()   bound at fragment texture 32 wherever lights are shaded (Shadows.metal).
//   * encodeDebug()  the shadow_atlas debug view.
//
// Owned by MetalRenderer (search MetalRenderer.mm for "[local shadows]"). Objective-C++ only.

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <array>
#include <atomic>
#include <memory>
#include <functional>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render/ShadowAtlas.h"

namespace sky {

class MetalShadows {
public:
    /// Draws every shadow caster of one face (the encoder's viewport, pipeline-independent state and
    /// depth state are set; the callback binds its own pipelines and the view-projection).
    using Casters = std::function<void(id<MTLRenderCommandEncoder> enc, const shadows::ShadowFace& face)>;

    explicit MetalShadows(id<MTLDevice> device);
    MetalShadows(const MetalShadows&) = delete;
    MetalShadows& operator=(const MetalShadows&) = delete;

    /// (Re)creates pipelines from the renderer's shader library; cached shadows re-render.
    Status build(id<MTLLibrary> library, MTLPixelFormat outputFormat);
    /// Plans this frame (CPU). `still`: stills and movie frames render every face they need.
    const shadows::ShadowPlan& plan(const FrameData& frame, bool still);
    /// GPULight::shadow / shadow2 of frame light `index` (zeros = no shadow).
    std::array<simd_float4, 2> lightParams(size_t index) const;
    bool hasWork() const { return !plan_.faces.empty() && atlas_ != nil; }
    void encode(id<MTLCommandBuffer> cmd, const FrameData& frame, const Casters& casters);
    /// The atlas (a 1x1 placeholder until a light first needs it).
    id<MTLTexture> atlas() const { return atlas_ ?: dummy_; }
    void encodeDebug(id<MTLCommandBuffer> cmd, id<MTLTexture> target);
    void invalidate() { planner_.invalidate(); }
    /// GPU time of the last local shadow pass (ms): written by the renderer's completion handler.
    std::shared_ptr<std::atomic<double>> gpuMsSlot() const { return gpuMs_; }
    Json info() const;
    Json stats() const;

private:
    bool ensureAtlas(int size);

    id<MTLDevice> device_;
    id<MTLTexture> atlas_, dummy_;
    id<MTLRenderPipelineState> clear_, debug_;
    id<MTLDepthStencilState> always_, write_;
    shadows::LocalShadowPlanner planner_;
    shadows::ShadowPlan plan_;
    int atlasSize_ = 0;
    double planMs_ = 0.0;
    std::shared_ptr<std::atomic<double>> gpuMs_ = std::make_shared<std::atomic<double>>(0.0);
    uint64_t facesRendered_ = 0;
    bool allocationFailed_ = false;
};

}  // namespace sky
