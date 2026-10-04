#pragma once
// Metal renderer for Frame2D (2D world quads and UI), owned by MetalRenderer. Objective-C++ only.
//
//   encodeOccluders  before the scene pass: shadow-casting sprites -> occluder mask (2D shadows)
//   encodeWorld      inside the scene pass: sprites, tiles, world text, halos (depth-tested, HDR)
//   encodeUI         after post-processing: screen and world canvases into the final target
//
// Pipelines are built for whatever attachment formats/sample counts the host pass uses, so the 3D
// renderer can change its targets freely.

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <map>
#include <string>
#include <unordered_map>

#include "skywalker/render/Renderer.h"

namespace sky {

class MetalRenderer2D {
public:
    explicit MetalRenderer2D(id<MTLDevice> device);
    bool init();

    void encodeOccluders(id<MTLCommandBuffer> cmd, const FrameData& frame);
    void encodeWorld(id<MTLRenderCommandEncoder> enc, MTLRenderPassDescriptor* pass, const FrameData& frame);
    void encodeUI(id<MTLCommandBuffer> cmd, const FrameData& frame, id<MTLTexture> target, id<MTLTexture> depth);
    /// Drops a cached texture (file path) so it reloads.
    void invalidate(const std::string& key);

private:
    id<MTLRenderPipelineState> worldPipeline(MTLRenderPassDescriptor* pass, bool additive);
    id<MTLRenderPipelineState> uiPipeline(MTLPixelFormat color, MTLPixelFormat depth);
    id<MTLTexture> texture(const TextureRef& ref, bool srgb);

    id<MTLDevice> device_;
    MTKTextureLoader* loader_ = nil;
    id<MTLLibrary> spriteLib_, uiLib_;
    std::map<std::string, id<MTLRenderPipelineState>> pipelines_;
    id<MTLRenderPipelineState> occluderPipeline_;
    id<MTLSamplerState> linear_, nearest_;
    id<MTLDepthStencilState> depthTest_, depthOff_;
    id<MTLTexture> white_, flatNormal_, noOccluders_, occluders_;
    bool occludersValid_ = false;
    std::unordered_map<std::string, id<MTLTexture>> files_;
    struct Generated {
        id<MTLTexture> texture;
        uint64_t version = 0;
    };
    std::unordered_map<std::string, Generated> generated_;
};

}  // namespace sky
