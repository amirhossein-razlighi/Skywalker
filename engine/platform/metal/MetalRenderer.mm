// Metal backend.
//
// Memory management: all Metal objects are ARC-managed `id<...>` members of a C++
// class, so they are released deterministically when the renderer is destroyed.
// Every entry point is wrapped in @autoreleasepool so the renderer does not accumulate
// autoreleased objects when driven from non-AppKit threads (e.g. the headless CLI).
//
// Frame structure:
//   0. Environment (only when the sky changes): sky -> cubemap -> GGX-prefiltered mips
//   1. Shadow pass: 4 sun cascades in a 4096^2 atlas (bounding-sphere fit, texel snapped)
//   2. Main pass (4x MSAA, resolved): sky, opaque meshes, toon outlines, blended meshes,
//      selection outline, grid. Writes HDR color + HDR indirect light + resolved depth.
//   3. SSAO (half resolution) + blur
//   4. Post: bloom chain, composite (AO on indirect light, white balance, tonemap, grade)
//   5. Overlays (gizmos) into the LDR target; optional present into a CAMetalLayer

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "skywalker/core/Log.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/render/Renderer.h"

namespace sky {

namespace {

const char* kDefaultShaderSource =
#include "ShaderSource.inc"
    ;

// Must match Standard.metal.
struct FrameUniforms {
    simd_float4x4 viewProj;
    simd_float4x4 invViewProj;
    simd_float4x4 cascadeViewProj[4];
    simd_float4 cascadeSplits;
    simd_float4 cameraPos;
    simd_float4 cameraForward;
    simd_float4 sunDir;
    simd_float4 sunColor;
    simd_float4 skyTop;
    simd_float4 skyHorizon;
    simd_float4 ground;
    simd_float4 fog;
    simd_float4 params;
    simd_float4 viewport;
    simd_float4 sky;
    simd_float4 extra;
};

struct DrawUniforms {
    simd_float4x4 model;
    simd_float4x4 normalMatrix;
    simd_float4 color;
    simd_float4 emissive;
    simd_float4 material;
    simd_float4 material2;
    simd_float4 material3;
    simd_float4 maps;
    simd_float4 outlineColor;
};

struct PostUniforms {
    simd_float4 params;
    simd_float4 params2;
    simd_float4 texel;
    simd_float4 grade;
};

struct AOUniforms {
    simd_float4x4 proj;
    simd_float4x4 invProj;
    simd_float4 params;
};

struct EnvUniforms {
    simd_float4 face;
};

struct GPULight {
    simd_float4 positionRange;
    simd_float4 colorIntensity;
    simd_float4 directionCone;
    simd_float4 kind;
};

constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm_sRGB;  // final LDR image
constexpr MTLPixelFormat kHDRFormat = MTLPixelFormatRGBA16Float;     // scene, ambient, bloom, env
constexpr MTLPixelFormat kAOFormat = MTLPixelFormatR16Float;
constexpr int kBloomLevels = 6;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
constexpr NSUInteger kSamples = 4;
constexpr NSUInteger kShadowAtlas = 4096;  // 2x2 cascades of 2048
constexpr int kCascades = 4;
constexpr NSUInteger kEnvSize = 128;
constexpr NSUInteger kEnvMips = 6;
constexpr NSUInteger kBrdfSize = 64;

simd_float4x4 toSimd(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}
simd_float4 v4(Vec3 v, float w) { return simd_make_float4(v.x, v.y, v.z, w); }

// Authored colors ("#rrggbb") are sRGB; lighting math must happen in linear space.
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
simd_float4 lin(Vec4 c) { return simd_make_float4(toLinear(c.x), toLinear(c.y), toLinear(c.z), c.w); }
simd_float4 lin(Vec3 c, float w) { return simd_make_float4(toLinear(c.x), toLinear(c.y), toLinear(c.z), w); }

/// View-frustum planes extracted from a view-projection matrix (Gribb/Hartmann, depth [0,1]).
struct Frustum {
    simd_float4 planes[6];
    explicit Frustum(const Mat4& m) {
        auto row = [&](int r) { return simd_make_float4(m.at(0, r), m.at(1, r), m.at(2, r), m.at(3, r)); };
        simd_float4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        planes[0] = r3 + r0;
        planes[1] = r3 - r0;
        planes[2] = r3 + r1;
        planes[3] = r3 - r1;
        planes[4] = r2;
        planes[5] = r3 - r2;
    }
    bool intersects(const Aabb& b) const {
        for (const auto& p : planes) {
            Vec3 v{p.x >= 0 ? b.max.x : b.min.x, p.y >= 0 ? b.max.y : b.min.y, p.z >= 0 ? b.max.z : b.min.z};
            if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0) return false;
        }
        return true;
    }
};

/// Sun shadow cascades: practical split scheme, bounding-sphere fit (rotation-stable) and
/// texel snapping (no shimmering when the camera moves).
struct Cascades {
    Mat4 viewProj[kCascades];
    float splits[kCascades] = {};
};

Cascades computeCascades(const FrameData& frame) {
    Cascades c;
    const ViewCamera& cam = frame.camera;
    Vec3 fwd = normalize(cam.target - cam.eye);
    Vec3 right = normalize(cross(fwd, cam.up));
    Vec3 up = cross(right, fwd);
    float aspect = static_cast<float>(frame.width) / static_cast<float>(std::max(frame.height, 1));
    float n = std::max(cam.nearPlane, 0.05f);
    float viewDist = distance(cam.eye, cam.target);
    float far = std::min(cam.farPlane, std::clamp(viewDist * 3.5f + 25.f, 35.f, 260.f));
    float tanY = std::tan(radians(cam.fovDeg) * 0.5f), tanX = tanY * aspect;
    Vec3 dir = frame.environment.sunDirection();
    float prev = n;
    for (int i = 0; i < kCascades; ++i) {
        float t = static_cast<float>(i + 1) / kCascades;
        float d = 0.82f * n * std::pow(far / n, t) + 0.18f * (n + (far - n) * t);
        c.splits[i] = d;
        Vec3 corners[8];
        int k = 0;
        for (float z : {prev, d}) {
            float hx = cam.orthographic ? cam.orthoSize * aspect : tanX * z;
            float hy = cam.orthographic ? cam.orthoSize : tanY * z;
            for (float sx : {-1.f, 1.f}) {
                for (float sy : {-1.f, 1.f}) corners[k++] = cam.eye + fwd * z + right * (hx * sx) + up * (hy * sy);
            }
        }
        Vec3 center{0, 0, 0};
        for (const Vec3& p : corners) center = center + p * 0.125f;
        float radius = 0;
        for (const Vec3& p : corners) radius = std::max(radius, distance(p, center));
        radius = std::ceil(radius * 8.f) / 8.f + 0.5f;
        const float casterReach = 120.f;
        Mat4 view = Mat4::lookAt(center - dir * (radius + casterReach), center,
                                 std::fabs(dir.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
        Mat4 proj = Mat4::orthographic(radius, 1.f, 0.1f, radius * 2.f + casterReach * 2.f);
        Mat4 vp = proj * view;
        // Snap the projected origin to whole shadow-map texels.
        const float tile = static_cast<float>(kShadowAtlas / 2);
        Vec4 o = vp * Vec4(0, 0, 0, 1);
        float ox = o.x * tile * 0.5f, oy = o.y * tile * 0.5f;
        Vec3 snap{(std::round(ox) - ox) * 2.f / tile, (std::round(oy) - oy) * 2.f / tile, 0};
        c.viewProj[i] = Mat4::translate(snap) * vp;
        prev = d;
    }
    return c;
}

struct GpuMesh {
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
    uint32_t indexCount = 0;
};

class MetalRenderer final : public Renderer {
public:
    bool init() {
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) return false;
        queue_ = [device_ newCommandQueue];
        textureLoader_ = [[MTKTextureLoader alloc] initWithDevice:device_];
        Status s = buildPipelines(kDefaultShaderSource);
        if (!s) {
            log::error("render", "built-in shaders failed to compile: " + s.error().message);
            return false;
        }
        source_ = kDefaultShaderSource;

        MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionLess;
        ds.depthWriteEnabled = YES;
        depthWrite_ = [device_ newDepthStencilStateWithDescriptor:ds];
        ds.depthWriteEnabled = NO;
        depthRead_ = [device_ newDepthStencilStateWithDescriptor:ds];
        ds.depthCompareFunction = MTLCompareFunctionAlways;
        depthNone_ = [device_ newDepthStencilStateWithDescriptor:ds];

        MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                                       width:kShadowAtlas
                                                                                      height:kShadowAtlas
                                                                                   mipmapped:NO];
        sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        sd.storageMode = MTLStorageModePrivate;
        shadowMap_ = [device_ newTextureWithDescriptor:sd];

        // 1x1 white texture bound when a material has no texture.
        MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:1
                                                                                      height:1
                                                                                   mipmapped:NO];
        white_ = [device_ newTextureWithDescriptor:wd];
        const uint8_t px[4] = {255, 255, 255, 255};
        [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];

        // Environment: sky cube (mipmapped source) -> GGX-prefiltered cube; BRDF lookup.
        MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:kHDRFormat
                                                                                         size:kEnvSize
                                                                                    mipmapped:YES];
        cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModePrivate;
        skyCube_ = [device_ newTextureWithDescriptor:cd];
        cd.mipmapLevelCount = kEnvMips;
        envCube_ = [device_ newTextureWithDescriptor:cd];
        MTLTextureDescriptor* bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                                                                                       width:kBrdfSize
                                                                                      height:kBrdfSize
                                                                                   mipmapped:NO];
        bd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        bd.storageMode = MTLStorageModePrivate;
        brdfLut_ = [device_ newTextureWithDescriptor:bd];
        bakeBrdf();
        return true;
    }

    RendererInfo info() const override { return {"metal", device_ ? std::string(device_.name.UTF8String) : ""}; }

    std::string shaderSource() const override { return source_; }

    Status reloadShaders(const std::string& source) override {
        @autoreleasepool {
            Status s = buildPipelines(source);
            if (s) {
                source_ = source;
                envKey_.clear();
                bakeBrdf();
            }
            return s;
        }
    }

    Status uploadMesh(const std::string& key, const MeshData& mesh) override {
        @autoreleasepool {
            if (mesh.indices.empty()) return Error::make("invalid_mesh", "mesh has no triangles");
            meshes_[key] = makeMesh(mesh);
            return {};
        }
    }

    void invalidate(const std::string& key) override {
        meshes_.erase(key);
        textures_.erase(key + "#srgb");
        textures_.erase(key + "#linear");
    }

    Status render(const FrameData& frame) override {
        @autoreleasepool {
            ensureTargets(frame.width, frame.height);
            Cascades cascades = computeCascades(frame);
            FrameUniforms fu = frameUniforms(frame, cascades);
            std::vector<GPULight> lights = gpuLights(frame);

            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            cmd.label = @"Skywalker Frame";
            encodeEnvironment(cmd, frame, fu);
            encodeShadows(cmd, frame, fu, cascades);
            encodeMain(cmd, frame, fu, lights);
            encodeAO(cmd, frame);
            encodePost(cmd, frame);
            encodeOverlays(cmd, frame, fu);
            [cmd commit];
            lastCommand_ = cmd;
            ++frameIndex_;
            return {};
        }
    }

    Result<Image> readback() override {
        @autoreleasepool {
            if (!resolve_) return Error::make("no_frame", "nothing has been rendered yet");
            const NSUInteger w = resolve_.width, h = resolve_.height, bpr = w * 4;
            id<MTLBuffer> buffer = [device_ newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromTexture:resolve_
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:bpr
            destinationBytesPerImage:bpr * h];
            [blit endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if (cmd.status == MTLCommandBufferStatusError) {
                return Error::make("gpu_error", cmd.error ? cmd.error.localizedDescription.UTF8String : "readback failed");
            }
            Image img(static_cast<int>(w), static_cast<int>(h));
            const auto* src = static_cast<const uint8_t*>(buffer.contents);
            for (size_t i = 0; i < w * h; ++i) {  // BGRA -> RGBA
                img.pixels[i * 4 + 0] = src[i * 4 + 2];
                img.pixels[i * 4 + 1] = src[i * 4 + 1];
                img.pixels[i * 4 + 2] = src[i * 4 + 0];
                img.pixels[i * 4 + 3] = 255;
            }
            return img;
        }
    }

    Status present(void* surface) override {
        @autoreleasepool {
            if (!surface || !resolve_) return Error::make("invalid_surface", "no surface or frame to present");
            CAMetalLayer* layer = (__bridge CAMetalLayer*)surface;
            if (layer.device != device_) layer.device = device_;
            if (layer.pixelFormat != kColorFormat) layer.pixelFormat = kColorFormat;
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) return Error::make("no_drawable", "the layer has no drawable available");
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:presentPipeline_];
            [enc setFragmentTexture:resolve_ atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cmd presentDrawable:drawable];
            [cmd commit];
            return {};
        }
    }

private:
    Status buildPipelines(const std::string& source) {
        NSError* error = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeFast;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                   options:opts
                                                     error:&error];
        if (!lib) {
            return Error::make("shader_compile_error",
                               error ? std::string(error.localizedDescription.UTF8String) : "unknown error");
        }
        auto fn = [&](const char* name) { return [lib newFunctionWithName:[NSString stringWithUTF8String:name]]; };
        for (const char* required : {"fullscreenVertex", "skyFragment", "meshVertex", "meshFragment", "shadowVertex",
                                     "gridVertex", "gridFragment", "presentFragment", "outlineVertex",
                                     "outlineFragment", "overlayFragment", "bloomPrefilter", "bloomDown", "bloomUp",
                                     "compositeFragment", "envSkyFragment", "envPrefilterFragment", "brdfLutFragment",
                                     "ssaoFragment", "aoBlurFragment"}) {
            if (!fn(required)) {
                return Error::make("shader_missing_function", std::string("shader source must define ") + required);
            }
        }

        enum class Blend { None, Alpha, Additive };
        // `mainPass` pipelines render into the scene pass: HDR color + HDR indirect light.
        auto make = [&](const char* vs, const char* fs, MTLPixelFormat color, NSUInteger samples, Blend blend,
                        bool depth, NSError** err, bool mainPass = false) -> id<MTLRenderPipelineState> {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = fn(vs);
            d.fragmentFunction = fs ? fn(fs) : nil;
            d.rasterSampleCount = samples;
            auto setup = [&](MTLRenderPipelineColorAttachmentDescriptor* ca, MTLPixelFormat format) {
                ca.pixelFormat = format;
                if (blend == Blend::Additive) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                } else if (blend == Blend::Alpha) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
            };
            if (color != MTLPixelFormatInvalid) setup(d.colorAttachments[0], color);
            if (mainPass) setup(d.colorAttachments[1], kHDRFormat);
            if (depth) d.depthAttachmentPixelFormat = kDepthFormat;
            return [device_ newRenderPipelineStateWithDescriptor:d error:err];
        };

        NSError* e = nil;
        id<MTLRenderPipelineState> sky = make("fullscreenVertex", "skyFragment", kHDRFormat, kSamples, Blend::None, true, &e, true);
        id<MTLRenderPipelineState> mesh = sky ? make("meshVertex", "meshFragment", kHDRFormat, kSamples, Blend::None, true, &e, true) : nil;
        id<MTLRenderPipelineState> meshBlend = mesh ? make("meshVertex", "meshFragment", kHDRFormat, kSamples, Blend::Alpha, true, &e, true) : nil;
        id<MTLRenderPipelineState> grid = meshBlend ? make("gridVertex", "gridFragment", kHDRFormat, kSamples, Blend::Alpha, true, &e, true) : nil;
        id<MTLRenderPipelineState> shadow = grid ? make("shadowVertex", nullptr, MTLPixelFormatInvalid, 1, Blend::None, true, &e) : nil;
        id<MTLRenderPipelineState> present = shadow ? make("fullscreenVertex", "presentFragment", kColorFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> outline = present ? make("outlineVertex", "outlineFragment", kHDRFormat, kSamples, Blend::None, true, &e, true) : nil;
        id<MTLRenderPipelineState> overlay = outline ? make("meshVertex", "overlayFragment", kColorFormat, 1, Blend::Alpha, false, &e) : nil;
        id<MTLRenderPipelineState> prefilter = overlay ? make("fullscreenVertex", "bloomPrefilter", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> down = prefilter ? make("fullscreenVertex", "bloomDown", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> up = down ? make("fullscreenVertex", "bloomUp", kHDRFormat, 1, Blend::Additive, false, &e) : nil;
        id<MTLRenderPipelineState> composite = up ? make("fullscreenVertex", "compositeFragment", kColorFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> envSky = composite ? make("fullscreenVertex", "envSkyFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> envPrefilter = envSky ? make("fullscreenVertex", "envPrefilterFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> brdf = envPrefilter ? make("fullscreenVertex", "brdfLutFragment", MTLPixelFormatRG16Float, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> ssao = brdf ? make("fullscreenVertex", "ssaoFragment", kAOFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> aoBlur = ssao ? make("fullscreenVertex", "aoBlurFragment", kAOFormat, 1, Blend::None, false, &e) : nil;
        if (!aoBlur) {
            return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "pipeline creation failed");
        }
        skyPipeline_ = sky;
        meshPipeline_ = mesh;
        meshBlendPipeline_ = meshBlend;
        gridPipeline_ = grid;
        shadowPipeline_ = shadow;
        presentPipeline_ = present;
        outlinePipeline_ = outline;
        overlayPipeline_ = overlay;
        bloomPrefilterPipeline_ = prefilter;
        bloomDownPipeline_ = down;
        bloomUpPipeline_ = up;
        compositePipeline_ = composite;
        envSkyPipeline_ = envSky;
        envPrefilterPipeline_ = envPrefilter;
        brdfPipeline_ = brdf;
        ssaoPipeline_ = ssao;
        aoBlurPipeline_ = aoBlur;
        return {};
    }

    id<MTLTexture> target2D(MTLPixelFormat format, NSUInteger w, NSUInteger h, MTLTextureUsage usage,
                            MTLStorageMode storage = MTLStorageModePrivate) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        d.usage = usage;
        d.storageMode = storage;
        return [device_ newTextureWithDescriptor:d];
    }

    id<MTLTexture> targetMSAA(MTLPixelFormat format, NSUInteger w, NSUInteger h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = kSamples;
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModeMemoryless;  // tile memory only on Apple GPUs
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        if (!t) {
            d.storageMode = MTLStorageModePrivate;
            t = [device_ newTextureWithDescriptor:d];
        }
        return t;
    }

    void ensureTargets(int width, int height) {
        auto w = static_cast<NSUInteger>(std::max(width, 1));
        auto h = static_cast<NSUInteger>(std::max(height, 1));
        if (resolve_ && resolve_.width == w && resolve_.height == h) return;
        const MTLTextureUsage rt = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        resolve_ = target2D(kColorFormat, w, h, rt);
        hdr_ = target2D(kHDRFormat, w, h, rt);
        ambient_ = target2D(kHDRFormat, w, h, rt);
        depthResolved_ = target2D(kDepthFormat, w, h, rt);
        const NSUInteger hw = std::max<NSUInteger>(1, w / 2), hh = std::max<NSUInteger>(1, h / 2);
        aoRaw_ = target2D(kAOFormat, hw, hh, rt);
        aoBlurred_ = target2D(kAOFormat, hw, hh, rt);

        // Bloom mip chain (half resolution).
        MTLTextureDescriptor* bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kHDRFormat width:hw height:hh mipmapped:YES];
        bd.mipmapLevelCount = std::min<NSUInteger>(kBloomLevels, bd.mipmapLevelCount);
        bd.storageMode = MTLStorageModePrivate;
        bd.usage = rt | MTLTextureUsagePixelFormatView;
        bloom_ = [device_ newTextureWithDescriptor:bd];
        bloomViews_.clear();
        for (NSUInteger i = 0; i < bloom_.mipmapLevelCount; ++i) {
            bloomViews_.push_back([bloom_ newTextureViewWithPixelFormat:kHDRFormat
                                                            textureType:MTLTextureType2D
                                                                 levels:NSMakeRange(i, 1)
                                                                 slices:NSMakeRange(0, 1)]);
        }
        msaaColor_ = targetMSAA(kHDRFormat, w, h);
        msaaAmbient_ = targetMSAA(kHDRFormat, w, h);
        msaaDepth_ = targetMSAA(kDepthFormat, w, h);
    }

    GpuMesh makeMesh(const MeshData& m) {
        GpuMesh g;
        g.vertices = [device_ newBufferWithBytes:m.vertices.data()
                                          length:m.vertices.size() * sizeof(float)
                                         options:MTLResourceStorageModeShared];
        g.indices = [device_ newBufferWithBytes:m.indices.data()
                                         length:m.indices.size() * sizeof(uint32_t)
                                        options:MTLResourceStorageModeShared];
        g.indexCount = static_cast<uint32_t>(m.indices.size());
        return g;
    }

    const GpuMesh* mesh(const std::string& name) {
        auto it = meshes_.find(name);
        if (it != meshes_.end()) return &it->second;
        auto data = mesh::primitive(name);
        if (!data) {
            if (!warnedMeshes_.count(name)) {
                warnedMeshes_.insert(name);
                log::warn("render", "unknown mesh '" + name + "', drawing a cube");
            }
            return name == "cube" ? nullptr : mesh("cube");
        }
        return &(meshes_[name] = makeMesh(data.value()));
    }

    /// Color textures (base color, emission) are sRGB; data textures (normal, ORM) linear.
    id<MTLTexture> texture(const std::string& path, bool srgb) {
        if (path.empty()) return nil;
        std::string key = path + (srgb ? "#srgb" : "#linear");
        auto it = textures_.find(key);
        if (it != textures_.end()) return it->second;
        NSError* err = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        id<MTLTexture> tex = [textureLoader_ newTextureWithContentsOfURL:url
                                                                 options:@{
                                                                     MTKTextureLoaderOptionSRGB : @(srgb),
                                                                     MTKTextureLoaderOptionGenerateMipmaps : @YES,
                                                                     MTKTextureLoaderOptionTextureStorageMode : @(MTLStorageModePrivate)
                                                                 }
                                                                   error:&err];
        if (!tex) log::warn("render", "could not load texture '" + path + "'");
        textures_[key] = tex;  // cache failures too, to avoid retrying every frame
        return tex;
    }

    FrameUniforms frameUniforms(const FrameData& frame, const Cascades& cascades) const {
        const Environment& env = frame.environment;
        Mat4 vp = frame.viewProjection();
        FrameUniforms fu{};
        fu.viewProj = toSimd(vp);
        fu.invViewProj = toSimd(vp.inverse());
        for (int i = 0; i < kCascades; ++i) fu.cascadeViewProj[i] = toSimd(cascades.viewProj[i]);
        fu.cascadeSplits = simd_make_float4(cascades.splits[0], cascades.splits[1], cascades.splits[2], cascades.splits[3]);
        fu.cameraPos = v4(frame.camera.eye, frame.time);
        fu.cameraForward = v4(normalize(frame.camera.target - frame.camera.eye), frame.camera.orthographic ? 1.f : 0.f);
        fu.sunDir = v4(env.sunDirection(), env.sunElevation > -8.f ? env.sunIntensity : 0.f);
        fu.sunColor = lin(env.sunColor.xyz(), env.sunSize);
        fu.skyTop = lin(env.skyTop);
        fu.skyHorizon = lin(env.skyHorizon);
        fu.ground = lin(env.ground.xyz(), env.ambient);
        fu.fog = lin(env.fogColor.xyz(), env.fogDensity);
        bool shadows = env.sunElevation > 0.f && env.sunIntensity > 0.f;
        fu.params = simd_make_float4(env.exposure, static_cast<float>(std::min(frame.lights.size(), FrameData::kMaxLights)),
                                     shadows ? 1.f : 0.f, 1.f / static_cast<float>(kShadowAtlas));
        float w = static_cast<float>(std::max(frame.width, 1)), h = static_cast<float>(std::max(frame.height, 1));
        fu.viewport = simd_make_float4(w, h, 1.f / w, 1.f / h);
        fu.sky = simd_make_float4(env.skyMode == "atmosphere" ? 1.f : 0.f, env.clouds, env.stars, env.reflections);
        fu.extra = simd_make_float4(env.fogHeight, env.shadowSoftness, static_cast<float>(kEnvMips - 1), 0.f);
        return fu;
    }

    static std::vector<GPULight> gpuLights(const FrameData& frame) {
        std::vector<GPULight> out;
        for (const auto& l : frame.lights) {
            if (out.size() >= FrameData::kMaxLights) break;
            GPULight g{};
            g.positionRange = v4(l.position, l.range);
            g.colorIntensity = lin(l.color, l.intensity);
            g.directionCone = v4(l.direction, l.cosCone);
            g.kind = simd_make_float4(static_cast<float>(l.kind), 0, 0, 0);
            out.push_back(g);
        }
        if (out.empty()) out.push_back(GPULight{});  // Metal requires a bound buffer
        return out;
    }

    DrawUniforms drawUniforms(const DrawItem& d, simd_float4 maps = simd_make_float4(0, 0, 0, 0)) const {
        const Surface& s = d.surface;
        DrawUniforms du{};
        du.model = toSimd(d.model);
        du.normalMatrix = toSimd(d.model.inverse().transposed());
        du.color = lin(s.color);
        du.emissive = lin(s.emissive);
        du.material = simd_make_float4(s.metallic, s.roughness, d.selected ? 1.f : 0.f, static_cast<float>(s.shading));
        // Triplanar tiling is "repeats per meter"; UV tiling is "repeats per face".
        float tx = s.triplanar ? s.tiling.x * 0.5f : s.tiling.x, ty = s.triplanar ? s.tiling.y * 0.5f : s.tiling.y;
        du.material2 = simd_make_float4(tx, ty, s.normalStrength, s.triplanar ? 1.f : 0.f);
        du.material3 = simd_make_float4(s.clearcoat, s.subsurface, s.rim, s.outline);
        du.maps = maps;
        du.outlineColor = lin(s.outlineColor);
        return du;
    }

    // --- Environment (image-based lighting) ------------------------------------------------
    std::string environmentKey(const FrameData& frame) const {
        const Environment& e = frame.environment;
        char buf[512];
        Vec3 d = e.sunDirection();
        std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.4f|%s %.2f %.2f %.2f",
                      d.x, d.y, d.z, e.sunColor.x, e.sunColor.y, e.sunColor.z, e.sunIntensity, e.skyTop.x, e.skyTop.y, e.skyTop.z,
                      e.skyHorizon.x, e.skyHorizon.y, e.skyHorizon.z, e.ground.x, e.ground.y, e.ground.z, e.ambient,
                      e.fogColor.x, e.fogColor.y, e.fogColor.z, e.fogDensity, e.skyMode.c_str(), e.clouds, e.stars, e.sunSize);
        return buf;
    }

    void bakeBrdf() {
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        PostUniforms pu{};
        fullscreen(cmd, brdfPipeline_, brdfLut_, {}, &pu, sizeof(pu), false, @"BRDF LUT");
        [cmd commit];
    }

    void encodeEnvironment(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu) {
        std::string key = environmentKey(frame);
        if (key == envKey_) return;
        envKey_ = key;
        FrameUniforms envFu = fu;
        envFu.cameraPos.w = 0.f;  // static clouds in reflections
        for (NSUInteger face = 0; face < 6; ++face) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = skyCube_;
            rp.colorAttachments[0].slice = face;
            rp.colorAttachments[0].level = 0;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Env sky";
            [enc setRenderPipelineState:envSkyPipeline_];
            EnvUniforms eu{simd_make_float4(static_cast<float>(face), 0, kEnvSize, 0)};
            [enc setFragmentBytes:&envFu length:sizeof(envFu) atIndex:0];
            [enc setFragmentBytes:&eu length:sizeof(eu) atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
        }
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit generateMipmapsForTexture:skyCube_];
        [blit endEncoding];
        for (NSUInteger mip = 0; mip < kEnvMips; ++mip) {
            float rough = static_cast<float>(mip) / static_cast<float>(kEnvMips - 1);
            for (NSUInteger face = 0; face < 6; ++face) {
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = envCube_;
                rp.colorAttachments[0].slice = face;
                rp.colorAttachments[0].level = mip;
                rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
                enc.label = @"Env prefilter";
                [enc setRenderPipelineState:envPrefilterPipeline_];
                EnvUniforms eu{simd_make_float4(static_cast<float>(face), rough, kEnvSize, static_cast<float>(mip))};
                [enc setFragmentBytes:&eu length:sizeof(eu) atIndex:0];
                [enc setFragmentTexture:skyCube_ atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
        }
    }

    // --- Shadows ----------------------------------------------------------------------------
    void encodeShadows(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu, const Cascades& cascades) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = shadowMap_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Shadow cascades";
        if (fu.params.z > 0.5f) {
            [enc setRenderPipelineState:shadowPipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setCullMode:MTLCullModeNone];
            [enc setDepthBias:1.0f slopeScale:2.0f clamp:0.01f];
            const double tile = kShadowAtlas / 2;
            for (int c = 0; c < kCascades; ++c) {
                MTLViewport vp{(c % 2) * tile, (c / 2) * tile, tile, tile, 0.0, 1.0};
                [enc setViewport:vp];
                [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(vp.originX), static_cast<NSUInteger>(vp.originY),
                                                   static_cast<NSUInteger>(tile), static_cast<NSUInteger>(tile)}];
                simd_float4x4 lvp = toSimd(cascades.viewProj[c]);
                [enc setVertexBytes:&lvp length:sizeof(lvp) atIndex:2];
                const Frustum fr(cascades.viewProj[c]);
                for (const DrawItem& d : frame.draws) {
                    if (!d.castShadows || d.surface.color.w < 0.5f || d.surface.shading == Shading::Unlit) continue;
                    if (!fr.intersects(d.worldBounds)) continue;
                    const GpuMesh* m = mesh(d.mesh);
                    if (!m) continue;
                    DrawUniforms du = drawUniforms(d);
                    [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
                    [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:m->indexCount
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:m->indices
                             indexBufferOffset:0];
                }
            }
        }
        [enc endEncoding];
    }

    // --- Scene ------------------------------------------------------------------------------
    void drawMesh(id<MTLRenderCommandEncoder> enc, const DrawItem& d) {
        const GpuMesh* m = mesh(d.mesh);
        if (!m) return;
        const Surface& s = d.surface;
        id<MTLTexture> albedo = texture(s.texture, true);
        id<MTLTexture> normal = texture(s.normalMap, false);
        id<MTLTexture> orm = texture(s.ormMap, false);
        id<MTLTexture> emissive = texture(s.emissiveMap, true);
        // maps.z: 0 = no ORM map, otherwise 1 + occlusion strength.
        DrawUniforms du = drawUniforms(d, simd_make_float4(albedo ? 1 : 0, normal ? 1 : 0, orm ? 1.f + s.occlusionStrength : 0.f,
                                                           emissive ? 1 : 0));
        bool twoSided = s.doubleSided || d.mesh == "plane" || d.mesh == "quad";
        [enc setCullMode:twoSided ? MTLCullModeNone : MTLCullModeBack];
        [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc setFragmentTexture:(albedo ?: white_) atIndex:0];
        [enc setFragmentTexture:(normal ?: white_) atIndex:2];
        [enc setFragmentTexture:(orm ?: white_) atIndex:3];
        [enc setFragmentTexture:(emissive ?: white_) atIndex:4];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:m->indexCount
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:m->indices
                 indexBufferOffset:0];
    }

    void drawOutline(id<MTLRenderCommandEncoder> enc, const DrawItem& d, float width, simd_float4 color) {
        const GpuMesh* m = mesh(d.mesh);
        if (!m) return;
        DrawUniforms du = drawUniforms(d);
        du.material3.w = width;
        du.outlineColor = color;
        [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:m->indexCount
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:m->indices
                 indexBufferOffset:0];
    }

    void encodeMain(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu,
                    const std::vector<GPULight>& lights) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = msaaColor_;
        rp.colorAttachments[0].resolveTexture = hdr_;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[1].texture = msaaAmbient_;
        rp.colorAttachments[1].resolveTexture = ambient_;
        rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[1].storeAction = MTLStoreActionMultisampleResolve;
        rp.depthAttachment.texture = msaaDepth_;
        rp.depthAttachment.resolveTexture = depthResolved_;
        rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;

        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Main";
        [enc setFrontFacingWinding:MTLWindingCounterClockwise];

        // Sky
        [enc setRenderPipelineState:skyPipeline_];
        [enc setDepthStencilState:depthNone_];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // Opaque meshes
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight) atIndex:2];
        [enc setFragmentTexture:shadowMap_ atIndex:1];
        [enc setFragmentTexture:envCube_ atIndex:5];
        [enc setFragmentTexture:brdfLut_ atIndex:6];
        [enc setRenderPipelineState:meshPipeline_];
        [enc setDepthStencilState:depthWrite_];
        std::vector<const DrawItem*> blended, outlined;
        const Frustum frustum(frame.viewProjection());
        culled_ = 0;
        for (const DrawItem& d : frame.draws) {
            if (!frustum.intersects(d.worldBounds)) {
                ++culled_;
                continue;
            }
            if (d.surface.color.w < 0.999f) {
                blended.push_back(&d);
                continue;
            }
            if (d.surface.outline > 0.f) outlined.push_back(&d);
            drawMesh(enc, d);
        }

        // Toon outlines (inverted hulls; depth-tested so they only show at silhouettes)
        if (!outlined.empty()) {
            [enc setRenderPipelineState:outlinePipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setCullMode:MTLCullModeFront];
            for (const DrawItem* d : outlined) drawOutline(enc, *d, d->surface.outline, lin(d->surface.outlineColor));
        }

        // Transparent meshes, back to front
        if (!blended.empty()) {
            Vec3 eye = frame.camera.eye;
            std::sort(blended.begin(), blended.end(), [&](const DrawItem* a, const DrawItem* b) {
                return distance(eye, a->worldBounds.center()) > distance(eye, b->worldBounds.center());
            });
            [enc setRenderPipelineState:meshBlendPipeline_];
            [enc setDepthStencilState:depthRead_];
            for (const DrawItem* d : blended) drawMesh(enc, *d);
        }

        // Selection outline (inverted hull behind the selected meshes)
        bool anySelected = false;
        for (const DrawItem& d : frame.draws) anySelected = anySelected || d.selected;
        if (anySelected) {
            [enc setRenderPipelineState:outlinePipeline_];
            [enc setDepthStencilState:depthRead_];
            [enc setCullMode:MTLCullModeFront];
            // Selection orange in HDR: survives tonemapping and glows slightly.
            for (const DrawItem& d : frame.draws) {
                if (d.selected) drawOutline(enc, d, 2.5f, simd_make_float4(2.6f, 0.75f, 0.08f, 1.f));
            }
        }

        // Grid
        if (frame.drawGrid) {
            [enc setRenderPipelineState:gridPipeline_];
            [enc setDepthStencilState:depthRead_];
            [enc setCullMode:MTLCullModeNone];
            [enc setVertexBytes:&fu length:sizeof(fu) atIndex:0];
            [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        }

        [enc endEncoding];
    }

    void encodeAO(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        aoActive_ = frame.environment.ao > 0.001f;
        if (!aoActive_) return;
        AOUniforms au{};
        au.proj = toSimd(frame.projection);
        au.invProj = toSimd(frame.projection.inverse());
        au.params = simd_make_float4(frame.environment.aoRadius, frame.environment.ao, 1.f / depthResolved_.width,
                                     1.f / depthResolved_.height);
        fullscreen(cmd, ssaoPipeline_, aoRaw_, {depthResolved_}, &au, sizeof(au), false, @"SSAO");
        PostUniforms pu{};
        pu.texel = simd_make_float4(1.f / aoRaw_.width, 1.f / aoRaw_.height, 0, 0);
        fullscreen(cmd, aoBlurPipeline_, aoBlurred_, {aoRaw_}, &pu, sizeof(pu), false, @"SSAO blur");
    }

    void encodeOverlays(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu) {
        if (frame.overlays.empty()) return;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = resolve_;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Overlays";
        [enc setFrontFacingWinding:MTLWindingCounterClockwise];
        [enc setRenderPipelineState:overlayPipeline_];
        [enc setCullMode:MTLCullModeBack];
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        for (const OverlayItem& o : frame.overlays) {
            const GpuMesh* m = mesh(o.mesh);
            if (!m) continue;
            DrawUniforms du{};
            du.model = toSimd(o.model);
            du.normalMatrix = toSimd(o.model.inverse().transposed());
            du.color = lin(o.color);
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
            [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:m->indexCount
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:m->indices
                     indexBufferOffset:0];
        }
        [enc endEncoding];
    }

    void fullscreen(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> pso, id<MTLTexture> target,
                    std::initializer_list<id<MTLTexture>> inputs, const void* uniforms, size_t size, bool load, NSString* label) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = label;
        [enc setRenderPipelineState:pso];
        NSUInteger i = 0;
        for (id<MTLTexture> t : inputs) [enc setFragmentTexture:t atIndex:i++];
        [enc setFragmentBytes:uniforms length:size atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    static float tonemapIndex(const std::string& name) {
        if (name == "agx") return 1;
        if (name == "neutral") return 2;
        if (name == "filmic") return 3;
        if (name == "none") return 4;
        return 0;
    }

    void encodePost(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        const Environment& env = frame.environment;
        PostUniforms pu{};
        pu.params = simd_make_float4(env.exposure, env.bloomIntensity, env.bloomThreshold, env.saturation);
        pu.params2 = simd_make_float4(env.contrast, env.vignette,
                                      static_cast<float>(frame.width) / static_cast<float>(std::max(frame.height, 1)),
                                      tonemapIndex(env.tonemap));
        pu.grade = simd_make_float4(env.temperature, env.tint, 0, 0);
        const size_t levels = bloomViews_.size();
        if (env.bloomIntensity > 0.001f && levels > 0) {
            pu.texel = simd_make_float4(1.f / hdr_.width, 1.f / hdr_.height, 0, 0);
            fullscreen(cmd, bloomPrefilterPipeline_, bloomViews_[0], {hdr_}, &pu, sizeof(pu), false, @"Bloom prefilter");
            for (size_t i = 1; i < levels; ++i) {
                pu.texel = simd_make_float4(1.f / bloomViews_[i - 1].width, 1.f / bloomViews_[i - 1].height, 0, 0);
                fullscreen(cmd, bloomDownPipeline_, bloomViews_[i], {bloomViews_[i - 1]}, &pu, sizeof(pu), false, @"Bloom down");
            }
            for (size_t i = levels - 1; i > 0; --i) {
                pu.texel = simd_make_float4(1.f / bloomViews_[i].width, 1.f / bloomViews_[i].height, 0, 0);
                fullscreen(cmd, bloomUpPipeline_, bloomViews_[i - 1], {bloomViews_[i]}, &pu, sizeof(pu), true, @"Bloom up");
            }
        } else {
            pu.params.y = 0;
        }
        pu.texel = simd_make_float4(1.f / hdr_.width, 1.f / hdr_.height, aoActive_ ? env.ao : 0.f,
                                    static_cast<float>(frameIndex_ % 64));
        fullscreen(cmd, compositePipeline_, resolve_, {hdr_, levels ? bloomViews_[0] : hdr_, ambient_, aoBlurred_}, &pu,
                   sizeof(pu), false, @"Composite");
    }

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    MTKTextureLoader* textureLoader_;
    id<MTLRenderPipelineState> skyPipeline_, meshPipeline_, meshBlendPipeline_, gridPipeline_, shadowPipeline_,
        presentPipeline_, outlinePipeline_, overlayPipeline_, bloomPrefilterPipeline_, bloomDownPipeline_,
        bloomUpPipeline_, compositePipeline_, envSkyPipeline_, envPrefilterPipeline_, brdfPipeline_, ssaoPipeline_,
        aoBlurPipeline_;
    id<MTLTexture> hdr_, ambient_, bloom_, depthResolved_, aoRaw_, aoBlurred_;
    std::vector<id<MTLTexture>> bloomViews_;
    id<MTLTexture> skyCube_, envCube_, brdfLut_;
    std::string envKey_;
    id<MTLDepthStencilState> depthWrite_, depthRead_, depthNone_;
    id<MTLTexture> resolve_, msaaColor_, msaaAmbient_, msaaDepth_, shadowMap_, white_;
    id<MTLCommandBuffer> lastCommand_;
    std::unordered_map<std::string, GpuMesh> meshes_;
    std::unordered_map<std::string, id<MTLTexture>> textures_;
    std::unordered_set<std::string> warnedMeshes_;
    std::string source_;
    size_t culled_ = 0;
    uint64_t frameIndex_ = 0;
    bool aoActive_ = false;
};

}  // namespace

std::unique_ptr<Renderer> createMetalRenderer() {
    auto r = std::make_unique<MetalRenderer>();
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace sky
