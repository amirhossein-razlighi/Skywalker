#pragma once
// Reflection probes for the Metal backend (docs/RENDERING.md "Reflection probes").
//
//   * plan()      once per frame, after the light grid: probes::Planner decides slots, captures and the
//                 shaded probes; the atlas (an RGBA16F cube array, kMips GGX mips per cube) is (re)sized;
//                 block() / clusterMasks() are what surfaces read (fragment buffers 10 and 11).
//   * capture()   renders the planned faces through the renderer's scene path (the Face callback binds
//                 its pipelines, frame uniforms and lights; no post, no screen-space effects) into a
//                 scratch cube, one command buffer per probe (bounded GPU work), then GGX-filters the
//                 completed cubes into their atlas slots (compute, bounds-checked writes).
//   * atlas()     bound at fragment texture 33 wherever surfaces are lit (Probes.metal).
//   * encodeDebug()  the reflection_probes debug view; faceImage() the probe's cubemap as an image.
//
// Owned by MetalRenderer (search MetalRenderer.mm for "[reflection probes]"). Objective-C++ only.

#import <Metal/Metal.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/Image.h"
#include "skywalker/render/ReflectionProbes.h"
#include "skywalker/render/Renderer.h"

namespace sky {

struct LightGrid;

struct ProbeFormats {
    MTLPixelFormat color, gbufA, gbufB, velocity, depth, output;
    NSUInteger samples;
};

class MetalProbes {
public:
    /// One capture face for the renderer to draw: the face's camera and the probe.
    struct Face {
        const ProbeItem* probe = nullptr;
        int face = 0;
        int size = 0;        // px
        Mat4 viewProj;       // probes::faceViewProj (mirrored: front faces wind clockwise)
        Mat4 sunViewProj;    // the probe's sun shadow, in the top-left quadrant of `sunShadow`
        float sunRadius = 0.f;
        id<MTLTexture> sunShadow;  // bind it as the cascade map (FrameUniforms texel = 1 / its width)
        /// The probes that light the captured surfaces (their previous captures: bounce light), probes off
        /// in the sense of the main pass (flag 1), interior ambient (flag 2). One cluster: `mask` has a bit
        /// per probe of the block.
        const probes::GpuProbeBlock* block = nullptr;
        uint32_t mask = 0;
    };
    struct Callbacks {
        /// Renders the probe's sun shadow (probes::sunViewProj) into the top-left quadrant of `target`.
        std::function<void(id<MTLCommandBuffer>, const ProbeItem&, const Mat4& sunViewProj, id<MTLTexture> target)> sunShadow;
        /// Draws one face into `enc` (sky, opaque and transparent meshes, terrain). The pass, viewport and
        /// winding are set; the callback binds its pipelines, uniforms, lights and probe resources.
        std::function<void(id<MTLRenderCommandEncoder>, const Face&)> drawFace;
    };

    MetalProbes(id<MTLDevice> device, id<MTLCommandQueue> queue);
    MetalProbes(const MetalProbes&) = delete;
    MetalProbes& operator=(const MetalProbes&) = delete;

    Status build(id<MTLLibrary> library, const ProbeFormats& formats);
    /// Plans this frame (CPU) and sizes the atlas. `still`: stills and movie frames capture everything now.
    const probes::Plan& plan(const FrameData& frame, const LightGrid& grid, bool still);
    bool hasCaptures() const { return !plan_.jobs.empty() && atlas_ != nil && skyPipeline_ != nil; }
    /// Captures and filters the planned jobs (own command buffers, committed in order; GPU faults counted).
    void capture(const FrameData& frame, const Callbacks& cb, std::shared_ptr<std::atomic<int>> gpuFaults);
    /// The sky pipeline for capture passes (fullscreen, main pass layout).
    id<MTLRenderPipelineState> skyPipeline() const { return skyPipeline_; }

    const probes::GpuProbeBlock& block() const { return block_; }
    const std::vector<uint32_t>& clusterMasks() const { return masks_; }
    id<MTLTexture> atlas() const { return atlas_ ?: dummy_; }
    bool active() const { return block_.info[0] > 0.5f; }

    /// reflection_probes debug view over `target` (alpha-blended); `uniforms` = the frame's FrameUniforms.
    void encodeDebug(id<MTLCommandBuffer> cmd, id<MTLTexture> target, id<MTLTexture> depth, const FrameData& frame,
                     const void* uniforms, size_t uniformsSize);
    /// A probe's six faces as a horizontal cross (the frame's exposure, filmic curve, sRGB), `mip` levels
    /// below its base.
    Result<Image> faceImage(EntityId entity, int mip);

    void invalidate(EntityId entity) { planner_.invalidate(entity); }
    Json info(const FrameData* frame) const;
    Json stats() const;

private:
    struct Targets {
        id<MTLTexture> scratch;  // the capture's cube (mips for the GGX filter)
        id<MTLTexture> color, gbufA, gbufB, velocity, depth;  // MSAA, memoryless where possible
    };
    Targets& targets(int size);
    bool ensureAtlas(int resolution, int slots);
    /// The block for one capture pass of `probe`: the probes with a valid capture that reach it.
    probes::GpuProbeBlock captureBlock(const FrameData& frame, const ProbeItem& probe, const std::vector<char>& valid, uint32_t& mask) const;
    void filter(id<MTLCommandBuffer> cmd, const ProbeItem& probe, const probes::ProbeState& state);
    void encodeFaces(id<MTLCommandBuffer> cmd, Targets& t, Face& face, int first, int last, const Callbacks& cb);

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    ProbeFormats formats_{};
    id<MTLRenderPipelineState> skyPipeline_, debugPipeline_;
    id<MTLComputePipelineState> filterKernel_;
    id<MTLTexture> atlas_, dummy_, sunShadow_;
    int atlasResolution_ = 0, atlasSlots_ = 0;
    std::map<int, Targets> targets_;
    probes::Planner planner_;
    probes::Plan plan_;
    probes::GpuProbeBlock block_{};
    std::vector<uint32_t> masks_;
    double planMs_ = 0.0;
    float exposure_ = 1.f;  // the last frame's exposure (faceImage)
    bool allocationFailed_ = false;
    struct Timing {
        std::mutex mutex;
        std::vector<std::pair<EntityId, double>> done;  // completed captures (GPU ms), drained on the next plan
        double lastMs = 0.0;
    };
    std::shared_ptr<Timing> timing_ = std::make_shared<Timing>();
};

}  // namespace sky
