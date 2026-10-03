// Metal renderer for 2D world quads and UI (see MetalRenderer2D.h).

#include "MetalRenderer2D.h"

#include <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "skywalker/core/Log.h"

namespace sky {

namespace {

const char* kSpriteSource =
#include "Sprite2DSource.inc"
    ;
const char* kUISource =
#include "UISource.inc"
    ;

// Must match Sprite2D.metal.
struct Sprite2DUniforms {
    simd_float4x4 viewProj;
    simd_float4 cameraPos;
    simd_float4 fog;
    simd_float4 ambient;
    simd_float4 params;
    simd_float4 viewport;
};

struct GPULight2D {
    simd_float4 positionRadius;
    simd_float4 colorFalloff;
    simd_float4 directionCone;
    simd_float4 extra;
};

// Must match UI.metal.
struct UIUniforms {
    simd_float4x4 transform;
    simd_float4 info;
};

simd_float4x4 toSimd(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

id<MTLLibrary> compile(id<MTLDevice> device, const char* source, const char* name) {
    NSError* error = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.mathMode = MTLMathModeFast;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source] options:opts error:&error];
    if (!lib) {
        log::error("render", std::string(name) + " shaders failed to compile: " +
                                 (error ? error.localizedDescription.UTF8String : "unknown error"));
    }
    return lib;
}

id<MTLTexture> solid(id<MTLDevice> device, MTLPixelFormat format, const uint8_t* px, NSUInteger bytesPerPixel) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:1 height:1 mipmapped:NO];
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:bytesPerPixel];
    return t;
}

}  // namespace

MetalRenderer2D::MetalRenderer2D(id<MTLDevice> device) : device_(device) {}

bool MetalRenderer2D::init() {
    @autoreleasepool {
        loader_ = [[MTKTextureLoader alloc] initWithDevice:device_];
        spriteLib_ = compile(device_, kSpriteSource, "Sprite2D");
        uiLib_ = compile(device_, kUISource, "UI");
        if (!spriteLib_ || !uiLib_) return false;

        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.maxAnisotropy = 4;
        linear_ = [device_ newSamplerStateWithDescriptor:sd];
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        sd.maxAnisotropy = 1;
        nearest_ = [device_ newSamplerStateWithDescriptor:sd];

        MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionLessEqual;
        ds.depthWriteEnabled = NO;
        depthTest_ = [device_ newDepthStencilStateWithDescriptor:ds];
        ds.depthCompareFunction = MTLCompareFunctionAlways;
        depthOff_ = [device_ newDepthStencilStateWithDescriptor:ds];

        const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, zero[1] = {0};
        white_ = solid(device_, MTLPixelFormatRGBA8Unorm, white, 4);
        flatNormal_ = solid(device_, MTLPixelFormatRGBA8Unorm, flat, 4);
        noOccluders_ = solid(device_, MTLPixelFormatR8Unorm, zero, 1);

        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [spriteLib_ newFunctionWithName:@"spriteVertex"];
        pd.fragmentFunction = [spriteLib_ newFunctionWithName:@"occluderFragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        NSError* error = nil;
        occluderPipeline_ = [device_ newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!occluderPipeline_) {
            log::error("render", std::string("2D occluder pipeline: ") + (error ? error.localizedDescription.UTF8String : "?"));
            return false;
        }
        return true;
    }
}

void MetalRenderer2D::invalidate(const std::string& key) {
    files_.erase(key + "#srgb");
    files_.erase(key + "#linear");
}

id<MTLTexture> MetalRenderer2D::texture(const TextureRef& ref, bool srgb) {
    if (ref.image) {
        const TextureImage& img = *ref.image;
        Generated& g = generated_[img.key];
        if (!g.texture || g.version != img.version || g.texture.width != static_cast<NSUInteger>(img.width)) {
            // A fresh texture per version: frames still in flight keep reading the old one.
            MTLPixelFormat format = img.channels == 1 ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm_sRGB;
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                         width:static_cast<NSUInteger>(img.width)
                                                                                        height:static_cast<NSUInteger>(img.height)
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
            [t replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(img.width), static_cast<NSUInteger>(img.height))
                 mipmapLevel:0
                   withBytes:img.pixels.data()
                 bytesPerRow:static_cast<NSUInteger>(img.width * img.channels)];
            g.texture = t;
            g.version = img.version;
        }
        return g.texture;
    }
    if (ref.path.empty()) return nil;
    std::string key = ref.path + (srgb ? "#srgb" : "#linear");
    if (auto it = files_.find(key); it != files_.end()) return it->second;
    NSError* err = nil;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:ref.path.c_str()]];
    id<MTLTexture> t = [loader_ newTextureWithContentsOfURL:url
                                                    options:@{
                                                        MTKTextureLoaderOptionSRGB : @(srgb),
                                                        MTKTextureLoaderOptionGenerateMipmaps : @YES,
                                                        MTKTextureLoaderOptionTextureStorageMode : @(MTLStorageModePrivate)
                                                    }
                                                      error:&err];
    if (!t) log::warn("render", "could not load 2D texture '" + ref.path + "'");
    files_[key] = t;
    return t;
}

id<MTLRenderPipelineState> MetalRenderer2D::worldPipeline(MTLRenderPassDescriptor* pass, bool additive) {
    std::string key = additive ? "world+" : "world";
    NSUInteger samples = 1;
    for (NSUInteger i = 0; i < 8; ++i) {
        id<MTLTexture> t = pass.colorAttachments[i].texture;
        if (!t) continue;
        key += "|" + std::to_string(i) + ":" + std::to_string(static_cast<int>(t.pixelFormat));
        samples = std::max<NSUInteger>(samples, t.sampleCount);
    }
    id<MTLTexture> depth = pass.depthAttachment.texture;
    key += "|d" + std::to_string(depth ? static_cast<int>(depth.pixelFormat) : 0) + "|s" + std::to_string(samples);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [spriteLib_ newFunctionWithName:@"spriteVertex"];
    pd.fragmentFunction = [spriteLib_ newFunctionWithName:@"spriteFragment"];
    pd.rasterSampleCount = samples;
    for (NSUInteger i = 0; i < 8; ++i) {
        id<MTLTexture> t = pass.colorAttachments[i].texture;
        if (!t) continue;
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[i];
        ca.pixelFormat = t.pixelFormat;
        if (i != 0) {
            // G-buffer targets: overwritten with "no lighting" by opaque-ish quads; halos leave them alone.
            ca.writeMask = additive ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
            continue;
        }
        ca.blendingEnabled = YES;
        if (additive) {
            ca.sourceRGBBlendFactor = MTLBlendFactorOne;
            ca.destinationRGBBlendFactor = MTLBlendFactorOne;
            ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
            ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        } else {
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
            ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    if (depth) pd.depthAttachmentPixelFormat = depth.pixelFormat;
    if (id<MTLTexture> stencil = pass.stencilAttachment.texture) pd.stencilAttachmentPixelFormat = stencil.pixelFormat;
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pso) log::error("render", std::string("2D sprite pipeline: ") + (error ? error.localizedDescription.UTF8String : "?"));
    pipelines_[key] = pso;
    return pso;
}

id<MTLRenderPipelineState> MetalRenderer2D::uiPipeline(MTLPixelFormat color, MTLPixelFormat depth) {
    std::string key = "ui|" + std::to_string(static_cast<int>(color)) + "|" + std::to_string(static_cast<int>(depth));
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [uiLib_ newFunctionWithName:@"uiVertex"];
    pd.fragmentFunction = [uiLib_ newFunctionWithName:@"uiFragment"];
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.pixelFormat = color;
    ca.blendingEnabled = YES;
    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.depthAttachmentPixelFormat = depth;
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pso) log::error("render", std::string("UI pipeline: ") + (error ? error.localizedDescription.UTF8String : "?"));
    pipelines_[key] = pso;
    return pso;
}

void MetalRenderer2D::encodeOccluders(id<MTLCommandBuffer> cmd, const FrameData& frame) {
    const Frame2D& f = frame.render2d;
    occludersValid_ = false;
    bool shadowLights = std::any_of(f.lights.begin(), f.lights.end(), [](const Light2DItem& l) { return l.shadows; });
    bool casters = std::any_of(f.sprites.begin(), f.sprites.end(), [](const SpriteInstance& s) { return s.extra[0] > 0.5f; });
    if (!f.lit || !shadowLights || !casters) return;
    const NSUInteger w = std::max<NSUInteger>(1, static_cast<NSUInteger>(frame.width) / 2);
    const NSUInteger h = std::max<NSUInteger>(1, static_cast<NSUInteger>(frame.height) / 2);
    if (!occluders_ || occluders_.width != w || occluders_.height != h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        occluders_ = [device_ newTextureWithDescriptor:d];
    }
    id<MTLBuffer> instances = [device_ newBufferWithBytes:f.sprites.data()
                                                   length:f.sprites.size() * sizeof(SpriteInstance)
                                                  options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = occluders_;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    enc.label = @"2D occluders";
    [enc setRenderPipelineState:occluderPipeline_];
    [enc setCullMode:MTLCullModeNone];
    Sprite2DUniforms u{};
    u.viewProj = toSimd(frame.viewProjection());
    [enc setVertexBuffer:instances offset:0 atIndex:0];
    [enc setFragmentBuffer:instances offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    for (const SpriteBatch& b : f.spriteBatches) {
        if (b.additive) continue;
        id<MTLTexture> t = texture(b.texture, true);
        [enc setFragmentTexture:(t ?: white_) atIndex:0];
        [enc setFragmentSamplerState:(b.nearest ? nearest_ : linear_) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:b.count baseInstance:b.first];
    }
    [enc endEncoding];
    occludersValid_ = true;
}

void MetalRenderer2D::encodeWorld(id<MTLRenderCommandEncoder> enc, MTLRenderPassDescriptor* pass, const FrameData& frame) {
    const Frame2D& f = frame.render2d;
    if (f.sprites.empty()) return;
    id<MTLBuffer> instances = [device_ newBufferWithBytes:f.sprites.data()
                                                   length:f.sprites.size() * sizeof(SpriteInstance)
                                                  options:MTLResourceStorageModeShared];
    const Environment& env = frame.environment;
    Sprite2DUniforms u{};
    u.viewProj = toSimd(frame.viewProjection());
    u.cameraPos = simd_make_float4(frame.camera.eye.x, frame.camera.eye.y, frame.camera.eye.z, 1);
    u.fog = simd_make_float4(srgbToLinear(env.fogColor.x), srgbToLinear(env.fogColor.y), srgbToLinear(env.fogColor.z), env.fogDensity);
    u.ambient = simd_make_float4(f.ambient.x, f.ambient.y, f.ambient.z, f.lit ? 1.f : 0.f);
    std::vector<GPULight2D> lights;
    for (const auto& l : f.lights) {
        if (lights.size() >= Frame2D::kMaxLights) break;
        GPULight2D g{};
        g.positionRadius = simd_make_float4(l.position.x, l.position.y, l.position.z, l.radius);
        g.colorFalloff = simd_make_float4(l.color.x, l.color.y, l.color.z, l.falloff);
        g.directionCone = simd_make_float4(l.direction.x, l.direction.y, l.direction.z, l.kind == Light2DItem::Kind::Spot ? 2.f : 1.f);
        g.extra = simd_make_float4(l.cosInner, l.cosOuter, l.height, l.shadows ? l.shadowSoftness : -1.f);
        lights.push_back(g);
    }
    if (lights.empty()) lights.push_back(GPULight2D{});  // a valid buffer even with no lights
    u.params = simd_make_float4(f.lights.empty() ? 0.f : static_cast<float>(std::min(f.lights.size(), Frame2D::kMaxLights)), frame.time,
                                occludersValid_ ? 1.f : 0.f, 0.f);
    // The scene pass may run below output resolution (upscaling): use its real size.
    id<MTLTexture> target = pass.colorAttachments[0].texture;
    const float vw = target ? static_cast<float>(target.width) : static_cast<float>(frame.width);
    const float vh = target ? static_cast<float>(target.height) : static_cast<float>(frame.height);
    u.viewport = simd_make_float4(vw, vh, 1.f / std::max(1.f, vw), 1.f / std::max(1.f, vh));

    [enc pushDebugGroup:@"2D world"];
    [enc setDepthStencilState:depthTest_];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:instances offset:0 atIndex:0];
    [enc setFragmentBuffer:instances offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight2D) atIndex:2];
    [enc setFragmentTexture:(occludersValid_ ? occluders_ : noOccluders_) atIndex:2];
    id<MTLRenderPipelineState> bound = nil;
    for (const SpriteBatch& b : f.spriteBatches) {
        id<MTLRenderPipelineState> pso = worldPipeline(pass, b.additive);
        if (!pso) continue;
        if (pso != bound) {
            [enc setRenderPipelineState:pso];
            bound = pso;
        }
        id<MTLTexture> albedo = texture(b.texture, true);
        id<MTLTexture> normal = b.normalMap.empty() ? nil : texture(b.normalMap, false);
        [enc setFragmentTexture:(albedo ?: white_) atIndex:0];
        [enc setFragmentTexture:(normal ?: flatNormal_) atIndex:1];
        [enc setFragmentSamplerState:(b.nearest ? nearest_ : linear_) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:b.count baseInstance:b.first];
    }
    [enc popDebugGroup];
}

void MetalRenderer2D::encodeUI(id<MTLCommandBuffer> cmd, const FrameData& frame, id<MTLTexture> target, id<MTLTexture> depth) {
    const Frame2D& f = frame.render2d;
    if (f.ui.empty() || !target) return;
    id<MTLBuffer> quads = [device_ newBufferWithBytes:f.ui.data() length:f.ui.size() * sizeof(UIQuad) options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    const bool useDepth = depth && depth.width == target.width && depth.height == target.height && depth.sampleCount == 1;
    if (useDepth) {
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    }
    id<MTLRenderPipelineState> pso = uiPipeline(target.pixelFormat, useDepth ? depth.pixelFormat : MTLPixelFormatInvalid);
    if (!pso) return;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    enc.label = @"UI";
    [enc setRenderPipelineState:pso];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:quads offset:0 atIndex:0];
    [enc setFragmentBuffer:quads offset:0 atIndex:0];
    const float W = static_cast<float>(frame.width), H = static_cast<float>(frame.height);
    // Screen canvases: pixels (y down) -> clip space.
    Mat4 screen;
    screen.m[0] = 2.f / W;
    screen.m[5] = -2.f / H;
    screen.m[12] = -1.f;
    screen.m[13] = 1.f;
    int boundCanvas = -2;
    for (const UIBatch& b : f.uiBatches) {
        if (b.canvas != boundCanvas) {
            boundCanvas = b.canvas;
            const UICanvasItem* c = b.canvas >= 0 && static_cast<size_t>(b.canvas) < f.uiCanvases.size() ? &f.uiCanvases[static_cast<size_t>(b.canvas)] : nullptr;
            UIUniforms u{};
            if (c && c->world) {
                Mat4 m = frame.viewProjection() * c->model;
                u.transform = toSimd(m);
                // Approximate output pixels per canvas unit (for anti-aliasing widths).
                Vec4 a = m * Vec4(0, 0, 0, 1), bb = m * Vec4(100, 0, 0, 1);
                float px = 0.f;
                if (a.w > 1e-4f && bb.w > 1e-4f) {
                    float dx = (bb.x / bb.w - a.x / a.w) * 0.5f * W, dy = (bb.y / bb.w - a.y / a.w) * 0.5f * H;
                    px = std::sqrt(dx * dx + dy * dy) / 100.f;
                }
                u.info = simd_make_float4(std::max(px, 0.05f), 1, 0, 0);
                [enc setDepthStencilState:useDepth ? depthTest_ : depthOff_];
            } else {
                u.transform = toSimd(screen);
                u.info = simd_make_float4(1, 0, 0, 0);
                [enc setDepthStencilState:depthOff_];
            }
            [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
            [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        }
        id<MTLTexture> t = b.texture.empty() ? nil : texture(b.texture, true);
        [enc setFragmentTexture:(t ?: white_) atIndex:0];
        [enc setFragmentSamplerState:(b.nearest ? nearest_ : linear_) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:b.count baseInstance:b.first];
    }
    [enc endEncoding];
}

}  // namespace sky
