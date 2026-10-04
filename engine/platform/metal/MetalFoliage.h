#pragma once
// GPU-driven instanced foliage for the Metal backend (docs/RENDERING.md, "Foliage impostors").
//
//   * A compute pass culls every instance of every chunk against the camera and the 4 sun
//     cascades and sorts it into mesh distance bands (per-instance LOD) and/or the impostor,
//     writing compact instance lists and indirect draw arguments.
//   * Mesh bands crossfade (dithered, TAA-resolved) into octahedral impostors beyond the
//     layer's transition distance; impostors write the G-buffer and real depth.
//   * Shadows: mesh bands near the camera, impostor cards facing the sun beyond the transition
//     and in the far cascades.
//   * Impostors are baked on the GPU with the meshes' own material path, post-processed on the
//     CPU (render/Impostor.h) and cached in the project (.skywalker/cache/impostors).
//
// Owned by MetalRenderer, which calls (search MetalRenderer.mm for "[foliage]"):
//   prepare()        once per frame, before the shadow pass: bakes or loads impostors within the
//                    frame's budget, then encodes the cull pass into the frame's command buffer
//   encodeShadows()  inside each sun cascade
//   encodeMain()     inside the main MSAA pass (opaque geometry)
//   trackFrame()     after the frame is encoded (reads the GPU counters when it completes)
//
// Objective-C++ only.

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "MetalFxInternal.h"
#include "MetalMesh.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/Renderer.h"

namespace sky {

struct ImpostorAtlasData;

struct FoliageFormats {
    MTLPixelFormat hdr = MTLPixelFormatRGBA16Float;
    MTLPixelFormat gbufA = MTLPixelFormatRGBA8Unorm_sRGB;
    MTLPixelFormat gbufB = MTLPixelFormatRGBA16Float;
    MTLPixelFormat depth = MTLPixelFormatDepth32Float;
    NSUInteger samples = 4;
    MTLPixelFormat velocity = MTLPixelFormatRG16Float;  // main pass color(3): object motion
};

/// Per-frame camera inputs of the cull pass.
struct FoliageView {
    Mat4 viewProj;                // camera, unjittered
    Mat4 cascadeViewProj[4];      // sun cascades
    bool shadows = false;         // the sun casts shadows this frame
};

class MetalFoliage {
public:
    using MeshLookup = std::function<const GpuMesh*(const std::string& key)>;
    using TextureLookup = std::function<id<MTLTexture>(const std::string& path, bool srgb)>;
    /// The standard surface uniforms (DrawUniforms) of a surface, with its texture-map flags.
    using SurfaceUniforms = std::function<FxDrawUniforms(const Surface& surface, simd_float4 maps)>;

    MetalFoliage(id<MTLDevice> device, id<MTLCommandQueue> queue, MeshLookup meshes, TextureLookup textures,
                 SurfaceUniforms surfaceUniforms);
    ~MetalFoliage();
    MetalFoliage(const MetalFoliage&) = delete;
    MetalFoliage& operator=(const MetalFoliage&) = delete;

    /// (Re)creates pipelines from the renderer's shader library.
    Status build(id<MTLLibrary> library, const FoliageFormats& formats);

    void prepare(id<MTLCommandBuffer> cmd, const FrameData& frame, const FoliageView& view, uint64_t frameIndex);
    void encodeShadows(id<MTLRenderCommandEncoder> enc, const FrameData& frame, int cascade, simd_float4x4 lightViewProj);
    void encodeMain(id<MTLRenderCommandEncoder> enc, const FrameData& frame);
    void trackFrame(id<MTLCommandBuffer> cmd);
    void evict(uint64_t frameIndex);

    /// Bakes the impostors of these models now (or loads their caches unless `force`).
    Result<Json> bake(const std::vector<ImpostorModel>& models, bool force);
    Json stats() const;
    /// Triangles drawn by foliage (camera + shadows) in the last completed frame.
    uint64_t triangles() const;
    /// Safe mode (after a GPU fault): a quarter of the triangle budget.
    void setSafeMode(bool safe) { safeMode_ = safe; }

private:
    struct Impostor;
    struct Chunk;
    struct Counters;

    Impostor* impostor(const ImpostorModel& model, bool allowWork, bool force, bool* worked = nullptr);
    Result<std::unique_ptr<Impostor>> bakeModel(const ImpostorModel& model);
    Result<std::unique_ptr<Impostor>> loadModel(const ImpostorModel& model);
    std::unique_ptr<Impostor> upload(const ImpostorAtlasData& data);
    id<MTLBuffer> instanceBuffer(const InstanceBatch& b);
    void bindPart(id<MTLRenderCommandEncoder> enc, const InstancePart& part, bool shadow, bool& cutoutOut);

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    MeshLookup meshes_;
    TextureLookup textures_;
    SurfaceUniforms surfaceUniforms_;
    id<MTLTexture> white_;

    id<MTLComputePipelineState> cull_;
    id<MTLRenderPipelineState> mesh_, meshCutout_, meshShadow_, meshShadowAlpha_;
    id<MTLRenderPipelineState> impostor_, impostorShadow_, bake_;
    id<MTLRenderPipelineState> overdrawMesh_, overdrawImpostor_;  // [debug views] overdraw counting
    id<MTLDepthStencilState> bakeDepth_;

    // Per frame: the chunks to draw and their GPU-written lists / indirect arguments.
    static constexpr int kRing = 3;
    id<MTLBuffer> lists_[kRing], args_[kRing];
    int ring_ = 0;
    std::vector<Chunk> chunks_;
    // GPU culling can be switched off (SKY_GPU_CULL=0); it also turns itself off when a frame's
    // indirect arguments fail validation (checked on the CPU when the frame completes).
    bool gpuCull_ = true;
    uint32_t debugSkip_ = 0;  // SKY_FOLIAGE_DEBUG: 1 = no meshes, 2 = no impostors, 4 = no shadows
    struct ArgCheck {
        NSUInteger argsOffset, viewArgs;
        uint32_t views, parts, instances, listStride, maxIndices;
    };
    std::vector<ArgCheck> frameChecks_;
    NSUInteger frameArgBytes_ = 0;
    std::vector<Impostor*> frameImpostors_;  // per FrameData::impostors index (null = not ready)
    id<MTLBuffer> frameStats_;

    std::unordered_map<std::string, std::unique_ptr<Impostor>> impostors_;
    struct InstanceGpu {
        id<MTLBuffer> buffer;
        uint64_t lastUse = 0;
    };
    std::unordered_map<uint64_t, InstanceGpu> instanceBuffers_;
    uint64_t frameIndex_ = 0;
    std::shared_ptr<Counters> counters_;
    /// Camera-view foliage triangles allowed per frame before every LOD is coarsened (the
    /// safety net against GPU watchdog stalls; impostors normally keep frames far below it).
    static constexpr uint64_t kTriangleBudget = 120'000'000;
    int budgetBias_ = 0;
    bool safeMode_ = false;
    uint64_t lastEstimate_ = 0;
    struct ModelEstimate {
        double triangles = 0;
        float transition = 0, cull = 0;
    };
    std::unordered_map<std::string, ModelEstimate> layerEstimate_;  // camera mesh triangles per model (first part)
    size_t bakes_ = 0, cacheLoads_ = 0;
    double bakeMs_ = 0.0;
};

}  // namespace sky
