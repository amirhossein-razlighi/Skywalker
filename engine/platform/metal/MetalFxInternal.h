#pragma once
// Internals of MetalFx: the GPU particle and hair subsystems and the constant blocks shared
// with GpuParticles.metal / Hair.metal (layouts must match field by field).

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "MetalFx.h"
#include "skywalker/fx/GpuParticles.h"

namespace sky {

/// Mirror of DrawUniforms in MetalRenderer.mm / Common.metal (mesh particles reuse the
/// standard `meshFragment`). Keep in sync if the surface uniforms change.
struct FxDrawUniforms {
    simd_float4x4 model;
    simd_float4x4 normalMatrix;
    simd_float4 color;
    simd_float4 emissive;
    simd_float4 material;   // metallic, roughness, selected, shading
    simd_float4 material2;  // tiling xy, normal strength, triplanar
    simd_float4 material3;  // clearcoat, subsurface, rim, outline
    simd_float4 maps;
    simd_float4 outlineColor;
    simd_float4 material4;  // alpha cutoff
    simd_float4x4 prevModel;  // velocity buffer (appended)
    simd_float4 motion;
    simd_float4 material5;  // car paint: clearcoat roughness, flakes, flake size (appended)
};
static_assert(sizeof(FxDrawUniforms) == 352);

/// Mirror of GpuStep in GpuParticles.metal.
struct GpuStepUniforms {
    simd_float4x4 prevViewProj;
    simd_float4x4 prevInvViewProj;
    simd_float4 eye;
    uint32_t capacity, cur, spawn, seed;
    uint32_t requestCap, triangles, writeTrail, trailSlots;
    uint32_t indexCount, sortCount, pad0, pad1;
};
static_assert(sizeof(GpuStepUniforms) == 192);

struct GpuDrawUniforms {
    uint32_t stride, offset, trailSlots, pad;
};

/// Mirror of HairParams in Hair.metal.
struct HairParamsUniforms {
    simd_float4x4 model;
    simd_float4x4 domViewProj;
    simd_float4 dims, width, sigma, dye, rootColor, tipColor, shade, dom, view, sim, sim2, wind, cards;
    simd_float4 colliders[16];
};

/// Mirror of HairChild in Hair.metal.
struct HairChildGpu {
    simd_float4 root;
    simd_float4 weights;
    simd_uint4 guides;
};
static_assert(sizeof(HairChildGpu) == 48);

/// Measured GPU times (written from Metal completion handlers).
struct FxGpuTimer {
    // Shared with completion handlers, which may run after the owner is gone.
    std::shared_ptr<std::atomic<double>> ms = std::make_shared<std::atomic<double>>(0.0);
    void track(id<MTLCommandBuffer> cmd);
    double value() const { return ms->load(); }
};

class MetalGpuParticles {
public:
    MetalGpuParticles(id<MTLDevice> device, MetalFx::MeshLookup meshes, MetalFx::TextureLookup textures);
    bool build(id<MTLLibrary> lib, const FxFormats& formats);
    bool ready() const { return ready_; }
    void simulate(id<MTLCommandBuffer> cmd, const FrameData& frame, id<MTLTexture> prevDepth, id<MTLTexture> prevNormals,
                  const Mat4& prevViewProj, bool prevValid);
    void encodeShadowCaster(id<MTLRenderCommandEncoder> enc);
    void encodeOpaque(id<MTLRenderCommandEncoder> enc);
    bool hasTransparent() const;
    void encodeTransparent(id<MTLCommandBuffer> cmd, const FrameData& frame, const FxSceneInputs& in);
    id<MTLTexture> reactive() const { return reactive_; }
    std::vector<LightItem> lights() const;
    Json stats() const;
    FxGpuTimer timer;

private:
    struct Emitter {
        EntityId entity = 0;
        uint32_t capacity = 0, trailSlots = 0, sortCount = 0;
        id<MTLBuffer> particles, dead, alive[2], counters, args, requests, history, keys, lightOut;
        id<MTLBuffer> surfacePos, surfaceNrm, surfaceCdf;
        const MeshSurface* surfaceSource = nullptr;
        uint32_t triangles = 0;
        id<MTLTexture> field;
        uint64_t fieldVersion = 0;
        id<MTLTexture> sheet;
        FxMesh mesh;
        uint32_t cur = 0;
        double lastTime = -1;
        float spawnCarry = 0;
        uint64_t burstSeen = 0;
        bool started = false;
        uint64_t steps = 0;
        float trailClock = 0;
        uint32_t trailHead = 0;
        Mat4 prevWorld;
        fx::GpuEmitterParams params{};
        uint32_t requestsPerEvent = 1;  // as a sub-emitter: particles per parent event
        bool sorted = false, drawable = false, initialized = false;
        int facing = 0;
        Vec3 position{0, 0, 0};
        float light = 0, lightRange = 8, expected = 1;
        bool lightShadows = false;  // the cast lights opt into local shadows
        Vec3 lightColor{1, 1, 1};
    };
    void ensure(Emitter& e, const GpuEmitterItem& item, id<MTLCommandBuffer> cmd);
    void step(id<MTLComputeCommandEncoder> enc, Emitter& e, const GpuEmitterItem& item, Emitter* sub, float dt, float time,
              uint32_t spawn, bool writeTrail, const GpuStepUniforms& base, bool last);

    id<MTLDevice> device_;
    MetalFx::MeshLookup meshes_;
    MetalFx::TextureLookup textures_;
    bool ready_ = false;
    std::unordered_map<std::string, id<MTLComputePipelineState>> kernels_;
    id<MTLRenderPipelineState> quadPipeline_, ribbonPipeline_, meshPipeline_, meshShadowPipeline_;
    id<MTLDepthStencilState> depthWrite_;
    id<MTLTexture> reactive_, white_;
    id<MTLBuffer> dummy_;
    id<MTLTexture> dummyField_;
    std::unordered_map<EntityId, Emitter> emitters_;
    std::unordered_map<std::string, FxMesh> builtinMeshes_;
    std::vector<EntityId> drawOrder_;
    uint64_t frame_ = 0;
};

class MetalHair {
public:
    MetalHair(id<MTLDevice> device, MetalFx::MeshLookup meshes);
    bool build(id<MTLLibrary> lib, const FxFormats& formats);
    bool ready() const { return ready_; }
    void simulate(id<MTLCommandBuffer> cmd, const FrameData& frame);
    /// Deep opacity maps (after simulate, in its own command buffer for timing).
    void renderShadowMaps(id<MTLCommandBuffer> cmd, const FrameData& frame);
    void encodeShadowCaster(id<MTLRenderCommandEncoder> enc, const FrameData& frame, simd_float4x4 lightViewProj);
    void encodeOpaque(id<MTLRenderCommandEncoder> enc);
    Json stats() const;
    FxGpuTimer timer, domTimer;

private:
    struct GroomGpu {
        uint64_t hash = 0;
        uint32_t P = 0, G = 0, N = 0;
        id<MTLBuffer> rest, pos, prev, children, offsets, render;
        id<MTLBuffer> renderPrev;  // last frame's interpolated strands (velocity buffer)
        bool motion = false;       // renderPrev holds last frame's strands and they moved
        id<MTLTexture> domDepth, domOpaque, domDensity;
        double lastTime = -1;
        Mat4 lastModel;
        bool posValid = false, interpolated = false;
        uint32_t lastDrawn = 0;
        HairParamsUniforms params{};
        uint32_t drawn = 0;
        bool cards = false, castShadows = true, visible = true;
        Vec3 center{0, 0, 0};
        float radius = 1;
    };
    void upload(GroomGpu& g, const GroomItem& item);
    void renderDom(id<MTLCommandBuffer> cmd, GroomGpu& g, const GroomItem& item, const FrameData& frame);

    id<MTLDevice> device_;
    MetalFx::MeshLookup meshes_;
    bool ready_ = false;
    NSUInteger shadowTile_ = 2048;
    std::unordered_map<std::string, id<MTLComputePipelineState>> kernels_;
    id<MTLRenderPipelineState> strandPipeline_, cardPipeline_, shadowPipeline_, domDepthPipeline_, domDensityPipeline_,
        domMeshPipeline_;
    id<MTLDepthStencilState> depthWrite_;
    std::unordered_map<EntityId, GroomGpu> grooms_;
    std::vector<EntityId> order_;
    static constexpr NSUInteger kDomSize = 512;
};

}  // namespace sky
