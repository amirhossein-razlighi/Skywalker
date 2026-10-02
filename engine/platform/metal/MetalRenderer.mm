// Metal backend.
//
// Memory management: all Metal objects are ARC-managed `id<...>` members of a C++
// class, so they are released deterministically when the renderer is destroyed.
// Every entry point is wrapped in @autoreleasepool so the renderer does not accumulate
// autoreleased objects when driven from non-AppKit threads (e.g. the headless CLI).
//
// Frame structure:
//   1. Shadow pass: sun depth map (2048^2, orthographic, fitted around the view target)
//   2. Main pass (4x MSAA -> resolved sRGB target): sky, opaque meshes, blended meshes, grid
//   3. Optional present pass into a CAMetalLayer drawable

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>

#include <algorithm>
#include <cmath>
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
    simd_float4x4 lightViewProj;
    simd_float4 cameraPos;
    simd_float4 sunDir;
    simd_float4 sunColor;
    simd_float4 skyTop;
    simd_float4 skyHorizon;
    simd_float4 ground;
    simd_float4 fog;
    simd_float4 params;
};

struct DrawUniforms {
    simd_float4x4 model;
    simd_float4x4 normalMatrix;
    simd_float4 color;
    simd_float4 emissive;
    simd_float4 material;
};

struct GPULight {
    simd_float4 positionRange;
    simd_float4 colorIntensity;
    simd_float4 directionCone;
    simd_float4 kind;
};

constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm_sRGB;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
constexpr NSUInteger kSamples = 4;
constexpr NSUInteger kShadowSize = 2048;

simd_float4x4 toSimd(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}
simd_float4 v4(Vec3 v, float w) { return simd_make_float4(v.x, v.y, v.z, w); }
simd_float4 v4(Vec4 v) { return simd_make_float4(v.x, v.y, v.z, v.w); }

// Authored colors ("#rrggbb") are sRGB; lighting math must happen in linear space.
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
simd_float4 lin(Vec4 c) { return simd_make_float4(toLinear(c.x), toLinear(c.y), toLinear(c.z), c.w); }
simd_float4 lin(Vec3 c, float w) { return simd_make_float4(toLinear(c.x), toLinear(c.y), toLinear(c.z), w); }

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
                                                                                       width:kShadowSize
                                                                                      height:kShadowSize
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
        return true;
    }

    RendererInfo info() const override { return {"metal", device_ ? std::string(device_.name.UTF8String) : ""}; }

    std::string shaderSource() const override { return source_; }

    Status reloadShaders(const std::string& source) override {
        @autoreleasepool {
            Status s = buildPipelines(source);
            if (s) source_ = source;
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

    Status render(const FrameData& frame) override {
        @autoreleasepool {
            ensureTargets(frame.width, frame.height);
            FrameUniforms fu = frameUniforms(frame);
            std::vector<GPULight> lights = gpuLights(frame);

            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            cmd.label = @"Skywalker Frame";
            encodeShadows(cmd, frame, fu);
            encodeMain(cmd, frame, fu, lights);
            [cmd commit];
            lastCommand_ = cmd;
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
                                     "gridVertex", "gridFragment", "presentFragment"}) {
            if (!fn(required)) {
                return Error::make("shader_missing_function", std::string("shader source must define ") + required);
            }
        }

        auto make = [&](const char* vs, const char* fs, MTLPixelFormat color, NSUInteger samples, bool blend,
                        bool depth, NSError** err) -> id<MTLRenderPipelineState> {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = fn(vs);
            d.fragmentFunction = fs ? fn(fs) : nil;
            d.rasterSampleCount = samples;
            if (color != MTLPixelFormatInvalid) {
                d.colorAttachments[0].pixelFormat = color;
                if (blend) {
                    auto* ca = d.colorAttachments[0];
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
            }
            if (depth) d.depthAttachmentPixelFormat = kDepthFormat;
            return [device_ newRenderPipelineStateWithDescriptor:d error:err];
        };

        NSError* e = nil;
        id<MTLRenderPipelineState> sky = make("fullscreenVertex", "skyFragment", kColorFormat, kSamples, false, true, &e);
        id<MTLRenderPipelineState> mesh = sky ? make("meshVertex", "meshFragment", kColorFormat, kSamples, false, true, &e) : nil;
        id<MTLRenderPipelineState> meshBlend = mesh ? make("meshVertex", "meshFragment", kColorFormat, kSamples, true, true, &e) : nil;
        id<MTLRenderPipelineState> grid = meshBlend ? make("gridVertex", "gridFragment", kColorFormat, kSamples, true, true, &e) : nil;
        id<MTLRenderPipelineState> shadow = grid ? make("shadowVertex", nullptr, MTLPixelFormatInvalid, 1, false, true, &e) : nil;
        id<MTLRenderPipelineState> present = shadow ? make("fullscreenVertex", "presentFragment", kColorFormat, 1, false, false, &e) : nil;
        if (!present) {
            return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "pipeline creation failed");
        }
        skyPipeline_ = sky;
        meshPipeline_ = mesh;
        meshBlendPipeline_ = meshBlend;
        gridPipeline_ = grid;
        shadowPipeline_ = shadow;
        presentPipeline_ = present;
        return {};
    }

    void ensureTargets(int width, int height) {
        auto w = static_cast<NSUInteger>(std::max(width, 1));
        auto h = static_cast<NSUInteger>(std::max(height, 1));
        if (resolve_ && resolve_.width == w && resolve_.height == h) return;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        resolve_ = [device_ newTextureWithDescriptor:d];

        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = kSamples;
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModeMemoryless;  // tile memory only on Apple GPUs
        msaaColor_ = [device_ newTextureWithDescriptor:d];
        if (!msaaColor_) {
            d.storageMode = MTLStorageModePrivate;
            msaaColor_ = [device_ newTextureWithDescriptor:d];
        }
        d.pixelFormat = kDepthFormat;
        d.storageMode = MTLStorageModeMemoryless;
        msaaDepth_ = [device_ newTextureWithDescriptor:d];
        if (!msaaDepth_) {
            d.storageMode = MTLStorageModePrivate;
            msaaDepth_ = [device_ newTextureWithDescriptor:d];
        }
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

    id<MTLTexture> texture(const std::string& path) {
        if (path.empty()) return nil;
        auto it = textures_.find(path);
        if (it != textures_.end()) return it->second;
        NSError* err = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        id<MTLTexture> tex = [textureLoader_ newTextureWithContentsOfURL:url
                                                                 options:@{
                                                                     MTKTextureLoaderOptionSRGB : @YES,
                                                                     MTKTextureLoaderOptionGenerateMipmaps : @YES,
                                                                     MTKTextureLoaderOptionTextureStorageMode : @(MTLStorageModePrivate)
                                                                 }
                                                                   error:&err];
        if (!tex) log::warn("render", "could not load texture '" + path + "'");
        textures_[path] = tex;  // cache failures too, to avoid retrying every frame
        return tex;
    }

    Mat4 lightMatrix(const FrameData& frame) const {
        Vec3 dir = frame.environment.sunDirection();
        Vec3 center = frame.camera.target;
        float viewDist = distance(frame.camera.eye, frame.camera.target);
        float radius = std::clamp(viewDist * 1.6f, 8.f, 150.f);
        Mat4 view = Mat4::lookAt(center - dir * radius * 2.f, center, std::fabs(dir.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
        Mat4 proj = Mat4::orthographic(radius, 1.f, 0.1f, radius * 4.f);
        return proj * view;
    }

    FrameUniforms frameUniforms(const FrameData& frame) const {
        const Environment& env = frame.environment;
        Mat4 vp = frame.viewProjection();
        FrameUniforms fu{};
        fu.viewProj = toSimd(vp);
        fu.invViewProj = toSimd(vp.inverse());
        fu.lightViewProj = toSimd(lightMatrix(frame));
        fu.cameraPos = v4(frame.camera.eye, frame.time);
        fu.sunDir = v4(env.sunDirection(), env.sunElevation > -5.f ? env.sunIntensity : 0.f);
        fu.sunColor = lin(env.sunColor);
        fu.skyTop = lin(env.skyTop);
        fu.skyHorizon = lin(env.skyHorizon);
        fu.ground = lin(env.ground.xyz(), env.ambient);
        fu.fog = lin(env.fogColor.xyz(), env.fogDensity);
        bool shadows = env.sunElevation > 0.f && env.sunIntensity > 0.f;
        fu.params = simd_make_float4(env.exposure, static_cast<float>(std::min(frame.lights.size(), FrameData::kMaxLights)),
                                     shadows ? 1.f : 0.f, 1.f / static_cast<float>(kShadowSize));
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

    DrawUniforms drawUniforms(const DrawItem& d, bool hasTexture) const {
        DrawUniforms du{};
        du.model = toSimd(d.model);
        du.normalMatrix = toSimd(d.model.inverse().transposed());
        du.color = lin(d.color);
        du.emissive = lin(d.emissive);
        du.material = simd_make_float4(d.metallic, d.roughness, d.selected ? 1.f : 0.f, hasTexture ? 1.f : 0.f);
        return du;
    }

    void encodeShadows(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = shadowMap_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Shadows";
        if (fu.params.z > 0.5f) {
            [enc setRenderPipelineState:shadowPipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setCullMode:MTLCullModeNone];
            [enc setDepthBias:1.0f slopeScale:2.0f clamp:0.01f];
            [enc setVertexBytes:&fu.lightViewProj length:sizeof(simd_float4x4) atIndex:2];
            for (const DrawItem& d : frame.draws) {
                if (d.color.w < 0.5f) continue;  // mostly transparent things don't cast shadows
                const GpuMesh* m = mesh(d.mesh);
                if (!m) continue;
                DrawUniforms du = drawUniforms(d, false);
                [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
                [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:m->indexCount
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:m->indices
                         indexBufferOffset:0];
            }
        }
        [enc endEncoding];
    }

    void drawMesh(id<MTLRenderCommandEncoder> enc, const DrawItem& d) {
        const GpuMesh* m = mesh(d.mesh);
        if (!m) return;
        id<MTLTexture> tex = texture(d.texture);
        DrawUniforms du = drawUniforms(d, tex != nil);
        [enc setCullMode:(d.mesh == "plane" || d.mesh == "quad") ? MTLCullModeNone : MTLCullModeBack];
        [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc setFragmentTexture:(tex ?: white_) atIndex:0];
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
        rp.colorAttachments[0].resolveTexture = resolve_;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.depthAttachment.texture = msaaDepth_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;

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
        [enc setRenderPipelineState:meshPipeline_];
        [enc setDepthStencilState:depthWrite_];
        std::vector<const DrawItem*> blended;
        for (const DrawItem& d : frame.draws) {
            if (d.color.w < 0.999f) {
                blended.push_back(&d);
                continue;
            }
            drawMesh(enc, d);
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

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    MTKTextureLoader* textureLoader_;
    id<MTLRenderPipelineState> skyPipeline_, meshPipeline_, meshBlendPipeline_, gridPipeline_, shadowPipeline_,
        presentPipeline_;
    id<MTLDepthStencilState> depthWrite_, depthRead_, depthNone_;
    id<MTLTexture> resolve_, msaaColor_, msaaDepth_, shadowMap_, white_;
    id<MTLCommandBuffer> lastCommand_;
    std::unordered_map<std::string, GpuMesh> meshes_;
    std::unordered_map<std::string, id<MTLTexture>> textures_;
    std::unordered_set<std::string> warnedMeshes_;
    std::string source_;
};

}  // namespace

std::unique_ptr<Renderer> createMetalRenderer() {
    auto r = std::make_unique<MetalRenderer>();
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace sky
