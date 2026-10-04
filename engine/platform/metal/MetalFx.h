#pragma once
// GPU effects for the Metal backend: GPU particles and strand hair / fur
// (docs/HAIR_AND_VFX.md). Owned by MetalRenderer, which calls these hooks at fixed points
// of its frame (search MetalRenderer.mm for "[hair+vfx]"):
//
//   simulate()            once per frame, before the shadow pass: particle compute, hair
//                         simulation + interpolation and the hair deep opacity maps, in their
//                         own command buffers (same queue, so the frame sees the results)
//   encodeShadowCaster()  inside each sun cascade: hair and mesh particles cast shadows
//   encodeOpaque()        inside the main MSAA pass, after opaque meshes: hair (G-buffer,
//                         alpha-to-coverage-style sample masks) and lit mesh particles
//   encodeTransparent()   over the lit scene with the other effects: sprite / ribbon particles
//                         (+ a reactive mask that keeps TAA from ghosting them)
//
// Objective-C++ only (included from .mm files).

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/render/Renderer.h"

namespace sky {

struct FxMesh {
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> indices = nil;
    uint32_t indexCount = 0;
};

struct FxFormats {
    MTLPixelFormat hdr = MTLPixelFormatRGBA16Float;
    MTLPixelFormat gbufA = MTLPixelFormatRGBA8Unorm_sRGB;
    MTLPixelFormat gbufB = MTLPixelFormatRGBA16Float;
    MTLPixelFormat depth = MTLPixelFormatDepth32Float;
    NSUInteger samples = 4;           // main pass MSAA
    NSUInteger shadowTileTexels = 2048;  // one cascade tile of the sun shadow atlas
};

/// Scene resources and per-pass constants the effects read.
struct FxSceneInputs {
    id<MTLTexture> shadowAtlas = nil;
    id<MTLTexture> envCube = nil;
    id<MTLTexture> sceneDepth = nil;  // resolved depth of the lit scene (soft particles)
    id<MTLTexture> target = nil;      // lit HDR scene (transparent effects draw into it)
    const void* frameUniforms = nullptr;
    size_t frameUniformsSize = 0;
    const void* lights = nullptr;
    size_t lightsSize = 0;
};

class MetalGpuParticles;
class MetalHair;

class MetalFx {
public:
    using MeshLookup = std::function<FxMesh(const std::string& key)>;
    using TextureLookup = std::function<id<MTLTexture>(const std::string& path, bool srgb)>;

    MetalFx(id<MTLDevice> device, id<MTLCommandQueue> queue, MeshLookup meshes, TextureLookup textures);
    ~MetalFx();
    MetalFx(const MetalFx&) = delete;
    MetalFx& operator=(const MetalFx&) = delete;

    /// (Re)creates pipelines from the renderer's shader library. Missing shader functions
    /// disable the affected feature with a warning instead of failing the renderer.
    void build(id<MTLLibrary> library, const FxFormats& formats);

    void simulate(const FrameData& frame, id<MTLTexture> prevDepth, id<MTLTexture> prevNormals, const Mat4& prevViewProj,
                  bool prevValid);
    void encodeShadowCaster(id<MTLRenderCommandEncoder> enc, const FrameData& frame, simd_float4x4 lightViewProj);
    void encodeOpaque(id<MTLRenderCommandEncoder> enc, const FrameData& frame);
    bool hasTransparent(const FrameData& frame) const;
    void encodeTransparent(id<MTLCommandBuffer> cmd, const FrameData& frame, const FxSceneInputs& in);
    /// TAA hint: where GPU particles drew this frame (black 1x1 when none did).
    id<MTLTexture> reactiveMask() const;
    /// Measures the frame's GPU time (completion handler).
    void trackFrame(id<MTLCommandBuffer> cmd);

    std::vector<LightItem> effectLights() const;
    Json stats() const;

private:
    struct Timing;
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    std::unique_ptr<MetalGpuParticles> particles_;
    std::unique_ptr<MetalHair> hair_;
    std::shared_ptr<Timing> timing_;
    id<MTLTexture> black_;
    bool reactiveUsed_ = false;
};

}  // namespace sky
