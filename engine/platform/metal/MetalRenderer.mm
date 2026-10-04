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
//   Per sub-sample (1 in real time; N jittered sub-samples for stills/cinematics):
//   2. Main pass (4x MSAA, memoryless, resolved; jittered projection): sky, opaque meshes,
//      toon outlines, blended meshes, selection outline, grid. Writes HDR color + G-buffer
//      (albedo/AO, octahedral normal/roughness/metallic) + depth.
//   3. SSAO, SSGI and SSR at half resolution (+ temporal accumulation in real time)
//   4. Lighting resolve: swaps sky-probe indirect light for GI/reflections, applies SSAO
//   5. Effects over the lit scene: FFT water, fluid volumes, particles
//   6. Volumetric light (half resolution)
//   7. Temporal resolve: TAA (real time) or sub-sample accumulation (stills)
//   8. Post: bloom chain, composite (white balance, tonemap, grade, sharpen), debug views
//   9. Overlays (gizmos) into the LDR target; optional present into a CAMetalLayer

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <dispatch/dispatch.h>

#include "MetalFoliage.h"  // [foliage] GPU-driven foliage and impostors
#include "MetalFx.h"  // [hair+vfx] GPU particles and strand hair
#include "MetalMesh.h"
#include "MetalShadows.h"  // [local shadows] point / spot light shadow atlas
#include "MetalProbes.h"  // [reflection probes] probe atlas, captures, filtering
#include "skywalker/core/Log.h"
#include "skywalker/render/ColorGrading.h"
#include "skywalker/render/Hdr.h"
#include "skywalker/render/LightClusters.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/render/MotionHistory.h"
#include "skywalker/render/Renderer.h"
#include "MetalRenderer2D.h"  // 2D world quads + UI (Frame2D)
#include "MetalProfiler.h"     // [profiler] per-pass GPU timing
#include "MetalShaderCache.h"  // [shader cache] metallib, binary archive, async pipelines
#include "skywalker/core/Profiler.h"
#include "skywalker/render/DebugViews.h"
#include "skywalker/render/ShaderCache.h"

namespace sky {

namespace {

const char* kDefaultShaderSource =
#include "ShaderSource.inc"
    ;

// --- Animation: GPU skinning (compute pre-pass, Skinning.metal) --------------------------
const char* kSkinningShaderSource =
#include "SkinningShaderSource.inc"
    ;
struct SkinGpuVertex {  // must match Skinning.metal
    float position[3];
    float normal[3];
    uint16_t joints[4];
    float weights[4];
};
static_assert(sizeof(SkinGpuVertex) == 48);
struct SkinParams {
    uint32_t vertexCount, jointCount, pad0, pad1;
};
static_assert(sizeof(Mat4) == sizeof(simd_float4x4));
// -----------------------------------------------------------------------------------------

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
    simd_float4 hdri;
    simd_float4x4 prevViewProj;
    simd_float4x4 viewProjNoJitter;
    simd_float4 temporal;
    simd_float4 clouds;
    simd_float4 clouds2;
    simd_float4 cluster;
    simd_float4 cluster2;
    simd_float4 debug;  // [debug views] x = surface debug view id (0 = off), yzw = unused
};
static_assert(sizeof(FrameUniforms) == 832, "FrameUniforms must match Common.metal (grow only at the end)");

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
    simd_float4 material4;
    // --- appended (velocity buffer) ---
    simd_float4x4 prevModel;  // previous frame's model matrix (= model when static)
    simd_float4 motion;       // x = moves (prevModel differs or a previous skinned pose is bound)
    // --- appended (character material models) ---
    simd_float4 character[3];  // Surface::model (skin, eye, cloth, hair_card parameters)
    // --- appended (car paint) ---
    simd_float4 material5;    // x = clearcoat roughness, y = flakes, z = flake size (m)
};
static_assert(sizeof(DrawUniforms) == 400, "must match DrawUniforms in Common.metal");

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

struct WaterUniforms {
    simd_float4 levelSize;  // x = level, y = size (0 endless), zw = center
    simd_float4 deep;       // rgb linear
    simd_float4 shallow;    // rgb linear
    simd_float4 params;     // x = clarity, y = foam, z = reflections, w = refraction
    simd_float4 params2;    // x = roughness, y = 1 / N, z = endless, w = unused
    simd_float4 patch;      // xyz = cascade tile sizes (m)
    simd_float4 origin;     // xz = grid origin (endless)
    simd_float4 shore;      // xyz = terrain center, w = terrain size (m)
    simd_float4 shore2;     // x = heightmap resolution, y = terrain bound, z = shoaling depth (m)
};

struct FluidParams {
    simd_float4 dims, step, source, feed, physics, decay, wind;
};

struct VolumeUniforms {
    simd_float4x4 model, invModel;
    simd_float4 size, flame, smokeColor, glow;
};

struct VolumetricUniforms {
    simd_float4 params;
};

struct SSUniforms {
    simd_float4 params, params2, texel;
};

struct ResolveUniforms {
    simd_float4 params, params2, texel;
};

struct TemporalUniforms {
    simd_float4 params, texel, clouds;
};

struct TerrainUniformsGpu {
    simd_float4 origin, grid, water;
    simd_float4 layerParams[8];
    simd_float4 layerColor[8];
    simd_float4 overlay;  // x = opacity (0 = none), y = blend (0 mix, 1 multiply, 2 glow)
};

struct TerrainNodeGpu {
    simd_float4 node, morph;
};

struct LensUniformsGpu {
    simd_float4 lens, motion, texel, view;
};

struct GradeUniformsGpu {
    simd_float4 params;
};

struct MotionBlurUniformsGpu {  // MotionBlurUniforms in Post.metal
    simd_float4 params;  // x = shutter, y = tile size (velocity texels), z = max blur (output px), w = samples
    simd_float4 size;    // xy = velocity texture size, zw = output size
};

struct GPULight {
    simd_float4 positionRange;
    simd_float4 colorIntensity;
    simd_float4 directionCone;
    simd_float4 kind;
    // --- appended (light v2) ---
    simd_float4 params;   // x = specular, y = layer mask (as float), z = cos(inner cone) (0 = auto), w = inverse square
    simd_float4 params2;  // x = emitter radius, y = indirect, z = volumetric, w = unused
    // --- appended (local shadows) ---
    simd_float4 shadow;   // local shadows (shadows::gpuShadowParams; Common.metal)
    simd_float4 shadow2;
};
static_assert(sizeof(GPULight) == 128, "must match GPULight in Common.metal");

constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm_sRGB;  // final LDR image
constexpr MTLPixelFormat kHDRFormat = MTLPixelFormatRGBA16Float;     // scene, ambient, bloom, env
constexpr MTLPixelFormat kAOFormat = MTLPixelFormatR16Float;
constexpr MTLPixelFormat kGbufAFormat = MTLPixelFormatRGBA8Unorm_sRGB;  // albedo + material AO
constexpr MTLPixelFormat kGbufBFormat = MTLPixelFormatRGBA16Float;      // normal (oct) + roughness + metallic/flags
constexpr MTLPixelFormat kVelocityFormat = MTLPixelFormatRG16Float;     // motion vectors (uv, current -> previous)
constexpr int kBloomLevels = 6;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
constexpr NSUInteger kSamples = 4;
constexpr NSUInteger kShadowAtlas = 4096;  // 2x2 cascades of 2048
constexpr int kCascades = 4;
constexpr NSUInteger kEnvSize = 128;
constexpr NSUInteger kEnvMips = 6;
constexpr NSUInteger kBrdfSize = 64;
constexpr int kDebugMotion = debugview::kMotion;  // FrameData::debugView "motion": the velocity buffer

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
    float far = frame.environment.shadowDistance > 0.f
                    ? std::min(cam.farPlane, frame.environment.shadowDistance)
                    : std::min(cam.farPlane, std::clamp(viewDist * 3.5f + 25.f, 35.f, 260.f));
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

/// [profiler] Timeline group of a pass label (perf_stats {passes: true} sums groups).
const char* passGroup(NSString* label) {
    static const std::pair<NSString*, const char*> kPrefixes[] = {
        {@"SSGI", "ssgi"},           {@"SSR", "ssr"},          {@"SSAO", "ao"},         {@"Clouds", "clouds"},
        {@"Lighting resolve", "resolve"}, {@"TAA", "temporal"}, {@"Accumulate", "temporal"}, {@"Scene resolve", "temporal"},
        {@"Motion vectors", "upscale"}, {@"Debug view", "debug"}, {@"BRDF LUT", "environment"}};
    for (const auto& [prefix, group] : kPrefixes) {
        if ([label hasPrefix:prefix]) return group;
    }
    return "post";  // bloom, exposure, depth of field, motion blur, composite
}

class MetalRenderer final : public Renderer {
    struct OceanGpu {
        id<MTLTexture> disp[OceanCascades::kCascades];
        id<MTLTexture> slope[OceanCascades::kCascades];
        uint64_t version = 0;
    };
    struct GridMesh {
        id<MTLBuffer> vertices, indices;
        uint32_t indexCount = 0;
    };
    struct FluidGpu {
        id<MTLTexture> vel[2], scal[2], tmpA, tmpB, curl, div, pressure[2];
        int nx = 0, ny = 0, nz = 0;
        float cell = 0;
        double lastTime = -1;
        double age = 0;  // seconds simulated
    };
    struct TerrainGpu {
        id<MTLTexture> height, normal, weights0, weights1;
        id<MTLTexture> seabed;  // low-passed heights (~16 m) the water uses for shore depth
        uint64_t version = 0;
        const world::TerrainData* source = nullptr;
        int levels = 0;                                    // quadtree levels above the finest node
        std::vector<std::vector<simd_float2>> minMax;      // per level, per node: min/max height
        int nodesPerSide0 = 0;                             // finest level
        uint64_t lastUse = 0;
    };

public:
    bool init() {
        const auto initStart = std::chrono::steady_clock::now();
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) return false;
        queue_ = [device_ newCommandQueue];
        profiler_ = std::make_unique<MetalPassProfiler>(device_);  // [profiler]
        {  // [shader cache] pipeline binaries persisted per shader source / engine / GPU / OS
            builtinSource_ = kDefaultShaderSource;
            // SKY_SHADER_NONCE (benchmarking): a different library source, i.e. a cold shader compile.
            if (const char* nonce = std::getenv("SKY_SHADER_NONCE"); nonce && *nonce) builtinSource_ += "\n// nonce " + std::string(nonce) + "\n";
            NSString* os = [[NSProcessInfo processInfo] operatingSystemVersionString];
            pipelineCache_ = MetalPipelineCache::acquire(
                device_, shadercache::cacheKey(builtinSource_ + kSkinningShaderSource, SKY_VERSION_STRING,
                                               device_.name.UTF8String, os.UTF8String));
        }
        textureLoader_ = [[MTKTextureLoader alloc] initWithDevice:device_];
        fx_ = std::make_unique<MetalFx>(  // [hair+vfx]
            device_, queue_,
            [this](const std::string& key) {
                const GpuMesh* m = mesh(key);
                return m ? FxMesh{m->vertices, m->indices, m->indexCount} : FxMesh{};
            },
            [this](const std::string& path, bool srgb) { return texture(path, srgb); });
        foliage_ = std::make_unique<MetalFoliage>(  // [foliage]
            device_, queue_, [this](const std::string& key) { return mesh(key); },
            [this](const std::string& path, bool srgb) { return texture(path, srgb); },
            [this](const Surface& surface, simd_float4 maps) {
                DrawItem d;
                d.surface = surface;
                DrawUniforms du = drawUniforms(d, maps);
                FxDrawUniforms out;
                static_assert(sizeof(out) == sizeof(du));
                std::memcpy(&out, &du, sizeof(du));
                return out;
            });
        shadows_ = std::make_unique<MetalShadows>(device_);  // [local shadows]
        probes_ = std::make_unique<MetalProbes>(device_, queue_);  // [reflection probes]
        Status s = buildPipelines(builtinSource_, true);
        if (!s) {
            log::error("render", "built-in shaders failed to compile: " + s.error().message);
            return false;
        }
        source_ = builtinSource_;
        buildSkinningPipeline();  // animation

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
        r2d_ = std::make_unique<MetalRenderer2D>(device_);  // 2D + UI
        if (!r2d_->init()) r2d_.reset();
        for (auto& r : ring_) r = [device_ newBufferWithLength:kRingSize options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        buildTerrainPatch();
        auto volume3D = [&](NSUInteger n) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor new];
            d.textureType = MTLTextureType3D;
            d.pixelFormat = MTLPixelFormatRGBA8Unorm;
            d.width = d.height = d.depth = n;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            d.storageMode = MTLStorageModePrivate;
            return [device_ newTextureWithDescriptor:d];
        };
        cloudShape_ = volume3D(128);
        cloudDetail_ = volume3D(32);
        MTLTextureDescriptor* cd1 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kHDRFormat width:1 height:1 mipmapped:NO];
        cd1.usage = MTLTextureUsageShaderRead;
        clearCloud_ = [device_ newTextureWithDescriptor:cd1];
        const __fp16 clear[4] = {0, 0, 0, 1};
        [clearCloud_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:clear bytesPerRow:8];
        if (pipelineCache_) {  // [shader cache] record new pipeline binaries now (off the first frame)
            if (Status cs = pipelineCache_->save(); !cs) log::warn("render", cs.error().message);
        }
        startupMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initStart).count();
        return true;
    }

    Json stats() const override {
        Json j = Json::object({{"gpuMs", std::round(gpuMs_->load() * 100.0) / 100.0},
                             {"culledDraws", static_cast<int64_t>(culled_)},
                             {"terrainNodes", static_cast<int64_t>(terrainNodesDrawn_)},
                             {"trianglesDrawn", static_cast<int64_t>(lastTriangles_ + (foliage_ ? foliage_->triangles() : 0))},
                             {"gpuFaults", static_cast<int64_t>(gpuFaults_->load())},
                             {"meshesCached", static_cast<int64_t>(meshes_.size())},
                             {"texturesCached", static_cast<int64_t>(textures_.size())}});
        {  // velocity buffer: what moves this frame (object motion vectors) and the upscaler inputs
            int64_t skinnedWithHistory = 0;
            for (const auto& [key, h] : skinHistory_) skinnedWithHistory += h.motion && h.lastFrame + 1 == frameIndex_ ? 1 : 0;
            const MotionHistory::Stats& ms = motionHistory_.stats();
            j["lights"] = Json::object({{"total", static_cast<int64_t>(lightStats_[0])},
                                        {"layerMasked", static_cast<int64_t>(lightStats_[1])},
                                        {"negative", static_cast<int64_t>(lightStats_[2])},
                                        {"inverseSquare", static_cast<int64_t>(lightStats_[3])}});
            j["velocity"] = Json::object({{"movingDraws", static_cast<int64_t>(movingDraws_)},
                                          {"trackedDraws", static_cast<int64_t>(ms.tracked)},
                                          {"teleported", static_cast<int64_t>(ms.teleported)},
                                          {"maxObjectMotionM", std::round(ms.maxDistance * 1000.0) / 1000.0},
                                          {"skinnedWithPreviousPose", skinnedWithHistory},
                                          {"mipBias", std::round(mipBias_ * 1000.0) / 1000.0},
                                          {"motionBlurTilePx", static_cast<int64_t>(motionTilePx_)}});
        }
        if (foliage_) {  // [foliage] instances, impostors, bakes
            const Json f = foliage_->stats();
            for (const auto& [k, v] : f.members()) j[k] = v;
            j["instancesDrawn"] = f.get("meshInstances").asInt() + f.get("impostorInstances").asInt();
        }
        if (shadows_) j["localShadows"] = shadows_->stats();  // [local shadows]
        if (probes_) j["reflectionProbes"] = probes_->stats();  // [reflection probes]
        if (fx_) {  // [hair+vfx] frame/simulation GPU times, emitters, grooms
            const Json fx = fx_->stats();
            for (const auto& [k, v] : fx.members()) j[k] = v;
        }
        return j;
    }

    Result<Json> bakeImpostors(const std::vector<ImpostorModel>& models, bool force) override {  // [foliage]
        @autoreleasepool {
            return foliage_->bake(models, force);
        }
    }

    RendererInfo info() const override {
        RendererInfo ri{"metal", device_ ? std::string(device_.name.UTF8String) : "", Json::object()};
        auto r2 = [](double v) { return std::round(v * 100.0) / 100.0; };
        ri.shaders = Json::object({{"library", shaderLibraryOrigin_},
                                   {"precompiledAvailable", hasPrecompiledShaderLibrary()},
                                   {"libraryMs", r2(libraryMs_)},
                                   {"pipelinesMs", r2(pipelinesMs_)},
                                   {"shaderCompileMs", r2(libraryMs_ + pipelinesMs_)},
                                   {"startupMs", r2(startupMs_)},
                                   {"hotReloaded", source_ != builtinSource_}});
        if (!shaderNote_.empty()) ri.shaders["note"] = shaderNote_;
        if (pipelineCache_) ri.shaders["pipelineCache"] = pipelineCache_->stats();
        return ri;
    }

    Json passProfile() const override {  // [profiler]
        Json j = profiler_ ? profiler_->toJson() : Json::object({{"supported", false}, {"mode", "unsupported"}});
        j["frameGpuMs"] = std::round(gpuMs_->load() * 100.0) / 100.0;
        return j;
    }
    void resetPassProfile() override {
        if (profiler_) profiler_->reset();
    }

    std::string shaderSource() const override { return source_; }
    std::vector<LightItem> effectLights() const override { return fx_->effectLights(); }  // [hair+vfx]
    Json localShadowInfo() const override { return shadows_ ? shadows_->info() : Json(); }  // [local shadows]
    void invalidateLocalShadows() override {
        if (shadows_) shadows_->invalidate();
    }
    // [reflection probes]
    Json reflectionProbeInfo(const FrameData* frame) const override { return probes_ ? probes_->info(frame) : Json(); }
    void invalidateReflectionProbes(EntityId entity) override {
        if (probes_) probes_->invalidate(entity);
    }
    Result<Image> reflectionProbeImage(EntityId entity, int mip) override {
        @autoreleasepool {
            return probes_->faceImage(entity, mip);
        }
    }

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
        if (r2d_) r2d_->invalidate(key);  // 2D
    }

    Status render(const FrameData& frame) override {
        @autoreleasepool {
            SKY_PROFILE_SCOPE("render.encode");
            // Offline (movie) sub-frames are accumulated by the caller: always the still path.
            const bool accumulateFrame = std::clamp(frame.samples, 1, 256) > 1 || frame.offline.enabled;
            const float renderScale = accumulateFrame ? 1.f : std::clamp(frame.environment.renderScale, 0.33f, 1.f);
            captureCrossfade(frame);  // [scene transitions] the previous scene's last frame, before this one replaces it
            ensureTargets(frame.width, frame.height, renderScale);
            ensureMsaaStorage(frame);  // [characters] dense hair needs MSAA targets that can spill
            // Upscaling: interactive editor tiers use the GPU-only MetalFX spatial scaler after our
            // own TAA (cheap, robust under load); full-quality frames use the temporal scaler.
            const bool scaled = !accumulateFrame && renderScale < 0.999f;
            const bool spatialUpscale = scaled && frame.quality > 0 && spatialScaler() != nil;
            const bool upscale = scaled && !spatialUpscale && temporalScaler() != nil;
            ensureHdri(frame.environment);
            const Environment& env = frame.environment;
            Cascades cascades = computeCascades(frame);
            FrameUniforms base = frameUniforms(frame, cascades);
            // All lights shade surfaces through clusters; the most important few also light
            // water, particles, fluids and the volumetric fog.
            std::vector<GPULight> allLights = gpuLights(frame);
            lightStats_ = {};
            for (const LightItem& l : frame.lights) {  // light v2 usage (perf_stats "lights")
                lightStats_[0]++;
                lightStats_[1] += (l.mask & 0xFFFFFu) != 0xFFFFFu ? 1 : 0;
                lightStats_[2] += l.negative ? 1 : 0;
                lightStats_[3] += l.inverseSquare ? 1 : 0;
            }
            {  // [local shadows] which point / spot lights cast shadows this frame, and where in the atlas
                shadows_->plan(frame, accumulateFrame);
                for (size_t i = 0; i < allLights.size() && i < frame.lights.size(); ++i) {
                    auto sp = shadows_->lightParams(i);
                    allLights[i].shadow = sp[0];
                    allLights[i].shadow2 = sp[1];
                }
            }
            std::vector<GPULight> lights(allLights.begin(),
                                         allLights.begin() + static_cast<std::ptrdiff_t>(std::min(allLights.size(), FrameData::kMaxEffectLights)));
            const LightGrid grid = buildLightGrid(frame);
            probes_->plan(frame, grid, accumulateFrame);  // [reflection probes] slots, captures, shaded probes
            base.cluster = simd_make_float4(static_cast<float>(grid.tilesX), static_cast<float>(grid.tilesY),
                                            static_cast<float>(grid.slices), std::log(grid.zFar / grid.zNear));
            const Mat4 vp = frame.viewProjection();
            const bool cut = frame.resetHistory || cameraCut(frame);
            if (cut) {
                historyValid_ = false;
                motionValid_ = false;
            }
            // Velocity buffer inputs: previous transforms (object motion), the previous frame's
            // time (foliage wind, particles), and the texture LOD bias while MetalFX upscales.
            const float prevTime = !cut && prevTimeValid_ && frame.time >= prevTime_ && frame.time - prevTime_ < 1.f ? prevTime_ : frame.time;
            base.cluster2 = simd_make_float4(grid.zNear, static_cast<float>(grid.directionalCount), prevTime, 0);
            mipBias_ = (upscale || spatialUpscale) ? std::log2(renderScale) : 0.f;
            base.extra.w = mipBias_;
            motionHistory_.begin(cut);
            prevModels_.resize(frame.draws.size());
            movingDraws_ = 0;
            for (size_t i = 0; i < frame.draws.size(); ++i) {
                const DrawItem& d = frame.draws[i];
                prevModels_[i] = motionHistory_.previous(d.entity, d.mesh, d.model);
                if (transformChanged(prevModels_[i], d.model)) ++movingDraws_;
            }
            motionHistory_.end();
            velocityComposed_ = false;
            const int samples = std::clamp(frame.samples, 1, 256);
            const bool accumulate = accumulateFrame;
            const bool jittered = accumulate || env.taa;
            const uint64_t jitterBase = frame.offline.enabled ? static_cast<uint64_t>(std::max(frame.offline.sampleOffset, 0)) : 0;
            const float w = static_cast<float>(hdr_.width), h = static_cast<float>(hdr_.height);  // internal resolution

            // Bound the frames in flight so the transient ring is never overwritten in use.
            dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
            ringIndex_ = (ringIndex_ + 1) % kFramesInFlight;
            ringOffset_ = 0;
            profiler_->beginFrame();  // [profiler] every pass below samples GPU timestamps
            // Accumulated stills commit one command buffer per sub-sample: a single multi-second
            // command buffer trips the GPU watchdog ("progress timeout") and starves the window
            // server, while short ones let the system interleave its own work.
            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            cmd.label = @"Skywalker Frame";
            id<MTLCommandBuffer> firstCmd = cmd;
            encodeCrossfadeCapture(cmd);  // [scene transitions]
            lightsBuf_ = transient(allLights.data(), allLights.size() * sizeof(GPULight));
            clusterCellsBuf_ = transient(grid.cells.data(), grid.cells.size() * sizeof(uint32_t));
            clusterIndexBuf_ = transient(grid.indices.data(), grid.indices.size() * sizeof(uint32_t));
            probeBlockBuf_ = transient(&probes_->block(), sizeof(probes::GpuProbeBlock));  // [reflection probes]
            probeMaskBuf_ = transient(probes_->clusterMasks().data(), probes_->clusterMasks().size() * sizeof(uint32_t));
            {  // animation: posed vertices for every pass below, in a command buffer committed before the
               // effects' own (skinned hair and fur read this frame's pose)
                id<MTLCommandBuffer> skinCmd = [queue_ commandBuffer];
                skinCmd.label = @"Skinning";
                encodeSkinning(skinCmd, frame);
                [skinCmd commit];
            }
            ensureCloudNoise(cmd);
            encodeEnvironment(cmd, frame, base);
            lodFrame_ = &frame;
            trianglesDrawn_ = 0;
            fx_->simulate(frame, depthPrev_, gbufB_, prevViewProj_, historyValid_);  // [hair+vfx]
            chooseFoliageBudgetBias(frame);
            {  // [foliage] impostors (bake/load within budget) and the GPU cull pass
                FoliageView fv;
                fv.viewProj = vp;
                for (int c = 0; c < kCascades; ++c) fv.cascadeViewProj[c] = cascades.viewProj[c];
                fv.shadows = base.params.z > 0.5f;
                foliage_->prepare(cmd, frame, fv, frameIndex_);
            }
            encodeShadows(cmd, frame, base, cascades);
            if (shadows_->hasWork()) {  // [local shadows] faces whose casters or light changed (own, timed command buffer)
                [cmd commit];
                id<MTLCommandBuffer> sc = [queue_ commandBuffer];
                sc.label = @"Local shadows";
                shadows_->encode(sc, frame, [&](id<MTLRenderCommandEncoder> enc, const shadows::ShadowFace& face) {
                    drawLocalShadowCasters(enc, frame, face);
                });
                auto slot = shadows_->gpuMsSlot();
                auto faults = gpuFaults_;
                [sc addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
                    if (ms > 0.0) slot->store(ms);
                    if (done.status == MTLCommandBufferStatusError) faults->fetch_add(1);
                }];
                [sc commit];
                cmd = [queue_ commandBuffer];
                cmd.label = @"Skywalker Frame (lit)";
            }
            if (probes_->hasCaptures()) {  // [reflection probes] captures + filtering (own, timed command buffers; local shadows ready)
                [cmd commit];
                encodeProbeCaptures(frame, base);
                cmd = [queue_ commandBuffer];
                cmd.label = @"Skywalker Frame (after probes)";
            }
            if (r2d_) r2d_->encodeOccluders(cmd, frame);  // 2D shadow casters
            for (int i = 0; i < samples; ++i) {
                // Sub-pixel jitter (Halton 2,3): TAA spreads it over frames, stills over sub-samples.
                Vec2 j = jittered ? halton23(accumulate ? jitterBase + static_cast<uint64_t>(i) : frameIndex_) : Vec2{0, 0};
                Vec2 jn{j.x * 2.f / w, j.y * 2.f / h};
                Mat4 jvp = Mat4::translate({jn.x, jn.y, 0.f}) * vp;
                FrameUniforms fu = base;
                fu.viewProj = toSimd(jvp);
                fu.invViewProj = toSimd(jvp.inverse());
                fu.viewProjNoJitter = toSimd(vp);
                const bool reproject = accumulate ? i > 0 : historyValid_;
                fu.prevViewProj = toSimd(accumulate || !historyValid_ ? vp : prevViewProj_);
                fu.temporal = simd_make_float4(jn.x, jn.y, static_cast<float>(frameIndex_ % 4096), static_cast<float>(i));
                const uint64_t seed = frameIndex_ * 17 + static_cast<uint64_t>(i);
                encodeClouds(cmd, frame, fu, reproject, accumulate, seed);
                encodeMain(cmd, frame, fu, lights);
                encodeAO(cmd, frame);
                encodeScreenSpace(cmd, frame, fu, reproject, accumulate, seed);
                encodeResolve(cmd, frame, fu);
                encodeEffects(cmd, frame, fu, lights, i == 0);
                if (!accumulate) encodeVelocity(cmd, fu);  // TAA, MetalFX, motion blur, debug view
                encodeVolumetrics(cmd, frame, fu, lights, seed);
                int mode = accumulate ? 2 : (env.taa && historyValid_ && !upscale ? 1 : 0);
                encodeTemporal(cmd, frame, fu, mode, 1.f / static_cast<float>(i + 1));
                if (upscale) encodeUpscale(cmd, frame, j);
                id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Depth history", "temporal");
                [blit copyFromTexture:depthResolved_ toTexture:depthPrev_];
                [blit endEncoding];
                if (!accumulate) historyValid_ = true;
                if (accumulate && i + 1 < samples) {
                    [cmd commit];
                    cmd = [queue_ commandBuffer];
                    cmd.label = @"Skywalker Frame (sub-sample)";
                }
            }
            if (accumulate) historyValid_ = true;  // the converged still seeds later TAA frames
            if (accumulate && ((frame.camera.motionBlur > 0.001f && !frame.camera.orthographic) || frame.debugView == kDebugMotion)) {
                // Stills: motion since the previous rendered frame (sub-samples share one camera).
                FrameUniforms mfu = base;
                mfu.prevViewProj = toSimd(motionValid_ ? motionPrevVP_ : vp);
                mfu.temporal = simd_make_float4(0, 0, 0, 0);
                encodeVelocity(cmd, mfu);
            }
            if (spatialUpscale) encodeSpatialUpscale(cmd);
            postSource_ = upscale || spatialUpscale ? upscaled_ : taa_[taaCurrent_];
            encodePost(cmd, frame, accumulate, base);
            // impostors tints the final image and lighting_only only changes materials: no debug pass.
            if (frame.debugView == debugview::kShadowAtlas) {  // [local shadows]
                shadows_->encodeDebug(cmd, resolve_);
            } else if (frame.debugView == debugview::kReflectionProbes) {  // [reflection probes]
                probes_->encodeDebug(cmd, resolve_, depthResolved_, frame, &base, sizeof(base));
            } else if (frame.debugView > 0 && frame.debugView != debugview::kImpostors && frame.debugView != debugview::kLightingOnly &&
                       !debugViewIsOverlay(frame.debugView)) {  // [characters] overlays are drawn by the engine on the final image
                PostUniforms pu{};
                pu.params = simd_make_float4(static_cast<float>(frame.debugView), 0, 0, 0);
                pu.texel = simd_make_float4(1.f / std::max(frame.width, 1), 1.f / std::max(frame.height, 1),
                                            static_cast<float>(frame.width), static_cast<float>(frame.height));
                fullscreen(cmd, debugViewPipeline_, resolve_,
                           {gbufA_, gbufB_, giOut_, ssrOut_, aoBlurred_, depthResolved_, hdr_, velocity_}, &pu, sizeof(pu), false,
                           @"Debug view");
            }
            encodeDebugLines2D(cmd, frame);  // [2D physics] physics2d_world.debugDraw, over the world
            if (r2d_) r2d_->encodeUI(cmd, frame, resolve_, depthResolved_);  // UI at output resolution
            encodeScreenTransition(cmd, frame);  // [scene transitions] over the whole picture, UI included
            encodeOverlays(cmd, frame, base);  // editor gizmos stay visible
            fx_->trackFrame(cmd);  // [hair+vfx] GPU frame time
            foliage_->trackFrame(cmd);  // [foliage] GPU-counted instances and triangles
            {
                dispatch_semaphore_t sem = inFlight_;
                auto gpuMs = gpuMs_;
                auto faults = gpuFaults_;
                [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    double ms = (done.GPUEndTime - firstCmd.GPUStartTime) * 1000.0;  // whole frame, all sub-samples
                    if (ms > 0.0) gpuMs->store(ms);
                    if (done.status == MTLCommandBufferStatusError || firstCmd.status == MTLCommandBufferStatusError) {
                        faults->fetch_add(1);  // timeouts, page faults: the next frames run in safe mode
                    }
                    dispatch_semaphore_signal(sem);
                }];
            }
            profiler_->endFrame(cmd);  // [profiler] samples resolve when the frame completes
            [cmd commit];
            lastCommand_ = cmd;
            evictWorldCaches();
            lodFrame_ = nullptr;
            lastTriangles_ = trianglesDrawn_ / static_cast<uint64_t>(samples);
            prevViewProj_ = vp;
            motionPrevVP_ = vp;
            motionValid_ = true;
            prevTime_ = frame.time;
            prevTimeValid_ = true;
            prevEye_ = frame.camera.eye;
            prevTarget_ = frame.camera.target;
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
            if (lastCommand_ && lastCommand_.status == MTLCommandBufferStatusError) {
                return Error::make("gpu_error",
                                   std::string("the frame failed on the GPU: ") +
                                       (lastCommand_.error ? lastCommand_.error.localizedDescription.UTF8String : "unknown error"),
                                   "the scene is too heavy or a shader faulted; lower samples/resolution or check perf_stats");
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
    static float halton(uint64_t i, uint64_t base) {
        float f = 1.f, r = 0.f;
        for (uint64_t n = i; n > 0; n /= base) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(n % base);
        }
        return r;
    }
    /// Sub-pixel offset in pixels (-0.5..0.5), 16-sample Halton(2,3) sequence.
    static Vec2 halton23(uint64_t i) { return {halton(i % 16 + 1, 2) - 0.5f, halton(i % 16 + 1, 3) - 0.5f}; }

    /// A jump in the camera (a cut) invalidates temporal history.
    bool cameraCut(const FrameData& frame) const {
        Vec3 f0 = normalize(prevTarget_ - prevEye_), f1 = normalize(frame.camera.target - frame.camera.eye);
        float move = distance(prevEye_, frame.camera.eye);
        float viewDist = std::max(distance(frame.camera.eye, frame.camera.target), 1.f);
        return move > std::max(2.f, viewDist * 0.35f) || dot(f0, f1) < 0.94f;
    }

    static const std::vector<const char*>& requiredFunctions() {
        static const std::vector<const char*> kRequired = {
            "fullscreenVertex", "skyFragment", "meshVertex", "meshFragment", "shadowVertex",
            "gridVertex", "gridFragment", "presentFragment", "outlineVertex",
            "outlineFragment", "overlayFragment", "bloomPrefilter", "bloomDown", "bloomUp",
            "compositeFragment", "envSkyFragment", "envPrefilterFragment", "brdfLutFragment",
            "ssaoFragment", "aoBlurFragment", "shadowAlphaVertex", "shadowAlphaFragment",
            "waterVertex", "waterFragment", "particleVertex", "particleFragment",
            "volumeVertex", "volumeFragment", "fluidAdvect", "fluidCorrect", "fluidCombust",
            "fluidCurl", "fluidForces", "fluidDivergence", "fluidJacobi", "fluidProject", "volumetricFragment",
            "ssgiFragment", "ssrFragment", "ssTemporalFragment", "lightingResolveFragment",
            "temporalFragment", "debugViewFragment", "terrainVertex", "terrainFragment",
            "terrainShadowVertex", "cloudsFragment",
            "cloudTemporalFragment", "cloudShapeKernel", "cloudDetailKernel", "lumaFragment",
            "exposureFragment", "motionBlurFragment", "dofCocFragment", "dofBlurFragment",
            "dofCombineFragment", "motionVectorFragment", "wireframeFragment", "overdrawFragment",
            "motionTileMaxFragment", "motionNeighborMaxFragment", "fxExposureFragment",
            "shadowClearVertex", "shadowAtlasDebugFragment", "probeSkyFragment", "probeFilterKernel", "probeDebugFragment",
            "meshFragmentProbes", "ssgiProbesFragment", "lightingResolveProbesFragment", "screenFadeFragment",
            "crossfadeFragment", "debugLineVertex", "debugLineFragment"};
        return kRequired;
    }

    /// `builtin`: the engine's own library (precompiled .metallib when available, pipelines
    /// cached in the binary archive); otherwise a `shader_set` source compiled at runtime.
    Status buildPipelines(const std::string& source, bool builtin = false) {
        auto t0 = std::chrono::steady_clock::now();
        id<MTLLibrary> lib = nil;
        std::string origin = "source", note;
        if (builtin) {
            ShaderLibraryLoad load = loadBuiltinShaderLibrary(device_, source, requiredFunctions());
            lib = load.library;
            origin = load.origin;
            note = load.note;
            if (!lib) return Error::make("shader_compile_error", load.note);
        } else {
            NSError* error = nil;
            lib = compileShaderLibrary(device_, source, &error);
            if (!lib) {
                return Error::make("shader_compile_error",
                                   error ? std::string(error.localizedDescription.UTF8String) : "unknown error");
            }
        }
        const double libMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        auto fn = [&](const char* name) { return [lib newFunctionWithName:[NSString stringWithUTF8String:name]]; };
        for (const char* required : requiredFunctions()) {
            if (!fn(required)) {
                return Error::make("shader_missing_function", std::string("shader source must define ") + required);
            }
        }
        if (pipelineCache_) pipelineCache_->setBypass(!builtin);  // hot-reloaded sources are not archived
        struct BypassReset {  // every return path below re-enables the archive
            MetalPipelineCache* cache;
            ~BypassReset() {
                if (cache) cache->setBypass(false);
            }
        } bypassReset{pipelineCache_.get()};

        enum class Blend { None, Alpha, Additive, Premultiplied };
        // `mainPass` pipelines render into the scene pass: HDR color + HDR indirect light.
        auto make = [&](const char* vs, const char* fs, MTLPixelFormat color, NSUInteger samples, Blend blend,
                        bool depth, NSError** err, bool mainPass = false,
                        bool alphaToCoverage = false) -> id<MTLRenderPipelineState> {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.alphaToCoverageEnabled = alphaToCoverage;
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
                } else if (blend == Blend::Premultiplied) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                } else if (blend == Blend::Alpha) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
            };
            if (color != MTLPixelFormatInvalid) setup(d.colorAttachments[0], color);
            if (mainPass) {
                // G-buffer: opaque passes write it; blended passes leave it untouched.
                d.colorAttachments[1].pixelFormat = kGbufAFormat;
                d.colorAttachments[2].pixelFormat = kGbufBFormat;
                d.colorAttachments[3].pixelFormat = kVelocityFormat;  // object motion (velocity buffer)
                if (blend != Blend::None) {
                    d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
                    d.colorAttachments[2].writeMask = MTLColorWriteMaskNone;
                    d.colorAttachments[3].writeMask = MTLColorWriteMaskNone;
                }
            }
            if (depth) d.depthAttachmentPixelFormat = kDepthFormat;
            return newRenderPipeline(device_, d, err);  // [shader cache]
        };

        NSError* e = nil;
        id<MTLRenderPipelineState> sky = make("fullscreenVertex", "skyFragment", kHDRFormat, kSamples, Blend::None, true, &e, true);
        id<MTLRenderPipelineState> mesh = sky ? make("meshVertex", "meshFragment", kHDRFormat, kSamples, Blend::None, true, &e, true) : nil;
        // Transparent meshes have no G-buffer for the lighting resolve: they shade reflection probes themselves.
        id<MTLRenderPipelineState> meshBlend = mesh ? make("meshVertex", "meshFragmentProbes", kHDRFormat, kSamples, Blend::Alpha, true, &e, true) : nil;
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
        // Alpha-tested cutouts: alpha-to-coverage gives soft, MSAA-resolved foliage edges.
        id<MTLRenderPipelineState> cutout = aoBlur ? make("meshVertex", "meshFragment", kHDRFormat, kSamples, Blend::None, true, &e, true, true) : nil;
        id<MTLRenderPipelineState> shadowAlpha = cutout ? make("shadowAlphaVertex", "shadowAlphaFragment", MTLPixelFormatInvalid, 1, Blend::None, true, &e) : nil;
        // Water: single-sample pass over the resolved scene (refraction/SSR read copies of it).
        id<MTLRenderPipelineState> water = shadowAlpha ? make("waterVertex", "waterFragment", kHDRFormat, 1, Blend::None, true, &e) : nil;
        // Particles: one sorted stream, premultiplied alpha (additive looks output alpha 0).
        id<MTLRenderPipelineState> particles = water ? make("particleVertex", "particleFragment", kHDRFormat, 1, Blend::Premultiplied, false, &e) : nil;
        id<MTLRenderPipelineState> volume = particles ? make("volumeVertex", "volumeFragment", kHDRFormat, 1, Blend::Premultiplied, false, &e) : nil;
        id<MTLRenderPipelineState> volumetric = volume ? make("fullscreenVertex", "volumetricFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> ssgi = volumetric ? make("fullscreenVertex", "ssgiFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> ssr = ssgi ? make("fullscreenVertex", "ssrFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> ssTemporal = ssr ? make("fullscreenVertex", "ssTemporalFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> lightResolve = ssTemporal ? make("fullscreenVertex", "lightingResolveFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> temporal = lightResolve ? make("fullscreenVertex", "temporalFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> debugView = temporal ? make("fullscreenVertex", "debugViewFragment", kColorFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> terrain = debugView ? make("terrainVertex", "terrainFragment", kHDRFormat, kSamples, Blend::None, true, &e, true) : nil;
        id<MTLRenderPipelineState> terrainShadow = terrain ? make("terrainShadowVertex", nullptr, MTLPixelFormatInvalid, 1, Blend::None, true, &e) : nil;
        id<MTLRenderPipelineState> clouds = terrainShadow ? make("fullscreenVertex", "cloudsFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> cloudTemporal = clouds ? make("fullscreenVertex", "cloudTemporalFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLComputePipelineState> cloudShapeK = cloudTemporal ? newComputePipeline(device_, fn("cloudShapeKernel"), &e) : nil;
        id<MTLComputePipelineState> cloudDetailK = cloudShapeK ? newComputePipeline(device_, fn("cloudDetailKernel"), &e) : nil;
        id<MTLRenderPipelineState> luma = cloudDetailK ? make("fullscreenVertex", "lumaFragment", MTLPixelFormatRG16Float, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> exposure = luma ? make("fullscreenVertex", "exposureFragment", MTLPixelFormatR32Float, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> motionBlur = exposure ? make("fullscreenVertex", "motionBlurFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> dofCoc = motionBlur ? make("fullscreenVertex", "dofCocFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> dofBlur = dofCoc ? make("fullscreenVertex", "dofBlurFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> dofCombine = dofBlur ? make("fullscreenVertex", "dofCombineFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> motionVec = dofCombine ? make("fullscreenVertex", "motionVectorFragment", kVelocityFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> tileMax = motionVec ? make("fullscreenVertex", "motionTileMaxFragment", kVelocityFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> neighborMax = tileMax ? make("fullscreenVertex", "motionNeighborMaxFragment", kVelocityFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> fxExposure = neighborMax ? make("fullscreenVertex", "fxExposureFragment", MTLPixelFormatR32Float, 1, Blend::None, false, &e) : nil;
                // [debug views] wireframe lines and overdraw counting (main pass, G-buffer untouched)
        id<MTLRenderPipelineState> wire = fxExposure ? make("meshVertex", "wireframeFragment", kHDRFormat, kSamples, Blend::Alpha, true, &e, true) : nil;
        id<MTLRenderPipelineState> overdraw = wire ? make("meshVertex", "overdrawFragment", kHDRFormat, kSamples, Blend::Additive, true, &e, true) : nil;
        id<MTLRenderPipelineState> terrainWire = overdraw ? make("terrainVertex", "wireframeFragment", kHDRFormat, kSamples, Blend::Alpha, true, &e, true) : nil;
        id<MTLRenderPipelineState> terrainOverdraw = terrainWire ? make("terrainVertex", "overdrawFragment", kHDRFormat, kSamples, Blend::Additive, true, &e, true) : nil;
        // [reflection probes] variants with probe code: opaque meshes in probe captures (bounce light), and the
        // SSGI / resolve passes while probes shade the frame (the plain ones stay lean without probes).
        id<MTLRenderPipelineState> meshProbes = terrainOverdraw ? make("meshVertex", "meshFragmentProbes", kHDRFormat, kSamples, Blend::None, true, &e, true) : nil;
        id<MTLRenderPipelineState> ssgiProbes = meshProbes ? make("fullscreenVertex", "ssgiProbesFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        id<MTLRenderPipelineState> resolveProbes = ssgiProbes ? make("fullscreenVertex", "lightingResolveProbesFragment", kHDRFormat, 1, Blend::None, false, &e) : nil;
        // Scene transitions over the final image, UI included (game/SceneFlow.h): fade toward a color, crossfade.
        id<MTLRenderPipelineState> screenFade = resolveProbes ? make("fullscreenVertex", "screenFadeFragment", kColorFormat, 1, Blend::Alpha, false, &e) : nil;
        id<MTLRenderPipelineState> crossfade = screenFade ? make("fullscreenVertex", "crossfadeFragment", kColorFormat, 1, Blend::Alpha, false, &e) : nil;
        // [2D physics] debug lines over the final image, under the UI.
        id<MTLRenderPipelineState> debugLines = crossfade ? make("debugLineVertex", "debugLineFragment", kColorFormat, 1, Blend::Alpha, false, &e) : nil;
        if (!debugLines) volume = nil;
        if (volume) {
            for (const char* k : {"fluidAdvect", "fluidCorrect", "fluidCombust", "fluidCurl", "fluidForces", "fluidDivergence",
                                  "fluidJacobi", "fluidProject"}) {
                id<MTLFunction> f = fn(k);
                id<MTLComputePipelineState> cps = f ? newComputePipeline(device_, f, &e) : nil;
                if (!cps) {
                    volume = nil;
                    break;
                }
                fluidKernels_[k] = cps;
            }
        }
        if (!volume) {
            return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "pipeline creation failed");
        }
        // [foliage] GPU-driven foliage and impostor pipelines (same shader library).
        if (Status fs = foliage_->build(lib, FoliageFormats{kHDRFormat, kGbufAFormat, kGbufBFormat, kDepthFormat, kSamples, kVelocityFormat}); !fs) return fs;
        if (Status ls = shadows_->build(lib, kColorFormat); !ls) return ls;  // [local shadows]
        if (Status ps = probes_->build(lib, ProbeFormats{kHDRFormat, kGbufAFormat, kGbufBFormat, kVelocityFormat, kDepthFormat,
                                                         kColorFormat, kSamples});
            !ps) {
            return ps;  // [reflection probes]
        }
        skyPipeline_ = sky;
        meshPipeline_ = mesh;
        meshBlendPipeline_ = meshBlend;
        gridPipeline_ = grid;
        shadowPipeline_ = shadow;
        presentPipeline_ = present;
        outlinePipeline_ = outline;
        overlayPipeline_ = overlay;
        screenFadePipeline_ = screenFade;
        crossfadePipeline_ = crossfade;
        debugLinePipeline_ = debugLines;
        bloomPrefilterPipeline_ = prefilter;
        bloomDownPipeline_ = down;
        bloomUpPipeline_ = up;
        compositePipeline_ = composite;
        envSkyPipeline_ = envSky;
        envPrefilterPipeline_ = envPrefilter;
        brdfPipeline_ = brdf;
        ssaoPipeline_ = ssao;
        aoBlurPipeline_ = aoBlur;
        meshCutoutPipeline_ = cutout;
        shadowAlphaPipeline_ = shadowAlpha;
        waterPipeline_ = water;
        particlePipeline_ = particles;
        volumePipeline_ = volume;
        volumetricPipeline_ = volumetric;
        ssgiPipeline_ = ssgi;
        ssrPipeline_ = ssr;
        ssTemporalPipeline_ = ssTemporal;
        resolvePipeline_ = lightResolve;
        temporalPipeline_ = temporal;
        debugViewPipeline_ = debugView;
        terrainPipeline_ = terrain;
        terrainShadowPipeline_ = terrainShadow;
        cloudsPipeline_ = clouds;
        cloudTemporalPipeline_ = cloudTemporal;
        cloudShapeKernel_ = cloudShapeK;
        cloudDetailKernel_ = cloudDetailK;
        cloudNoiseReady_ = false;
        lumaPipeline_ = luma;
        exposurePipeline_ = exposure;
        motionBlurPipeline_ = motionBlur;
        dofCocPipeline_ = dofCoc;
        dofBlurPipeline_ = dofBlur;
        dofCombinePipeline_ = dofCombine;
        motionPipeline_ = motionVec;
        motionTileMaxPipeline_ = tileMax;
        motionNeighborMaxPipeline_ = neighborMax;
        fxExposurePipeline_ = fxExposure;
        wireframePipeline_ = wire;
        overdrawPipeline_ = overdraw;
        terrainWirePipeline_ = terrainWire;
        terrainOverdrawPipeline_ = terrainOverdraw;
        meshProbesPipeline_ = meshProbes;  // [reflection probes]
        ssgiProbesPipeline_ = ssgiProbes;
        resolveProbesPipeline_ = resolveProbes;
        if (fx_) fx_->build(lib, FxFormats{kHDRFormat, kGbufAFormat, kGbufBFormat, kDepthFormat, kSamples, kShadowAtlas / 2, kVelocityFormat});  // [hair+vfx]
        libraryMs_ = libMs;  // [shader cache] engine_info.shaders
        pipelinesMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        shaderLibraryOrigin_ = builtin ? origin : "source (shader_set)";
        shaderNote_ = note;
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
        // Tile memory only on Apple GPUs; dense strand geometry switches to memory-backed targets
        // (ensureMsaaStorage), which the GPU can flush to when its geometry buffer fills mid-pass.
        d.storageMode = msaaMemoryless_ ? MTLStorageModeMemoryless : MTLStorageModePrivate;
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        if (!t) {
            d.storageMode = MTLStorageModePrivate;
            t = [device_ newTextureWithDescriptor:d];
        }
        return t;
    }

    /// [characters] A pass with memoryless attachments must fit all its geometry in the GPU's tiling
    /// buffer at once; hundreds of thousands of hair strands (stills draw every one) overflow it
    /// ("too much geometry for memoryless attachments"). Above a strand-point budget the MSAA targets
    /// become memory-backed for the rest of the session (no thrashing): the pass then spills safely.
    void ensureMsaaStorage(const FrameData& frame) {
        if (!msaaMemoryless_ || !msaaColor_) return;
        constexpr size_t kMemorylessStrandPoints = 2000000;
        size_t points = 0;
        for (const GroomItem& g : frame.grooms) {
            if (g.data) points += g.data->strandCount() * std::max<size_t>(g.data->points, 1);
        }
        if (points <= kMemorylessStrandPoints) return;
        msaaMemoryless_ = false;
        const NSUInteger iw = msaaColor_.width, ih = msaaColor_.height;
        msaaColor_ = targetMSAA(kHDRFormat, iw, ih);
        msaaGbufA_ = targetMSAA(kGbufAFormat, iw, ih);
        msaaGbufB_ = targetMSAA(kGbufBFormat, iw, ih);
        msaaDepth_ = targetMSAA(kDepthFormat, iw, ih);
        msaaVelocity_ = targetMSAA(kVelocityFormat, iw, ih);
    }

    /// Scene targets render at the internal resolution (renderScale x output); post-processing
    /// and the final image use the output resolution. MetalFX upscales between the two.
    void ensureTargets(int width, int height, float scale) {
        auto w = static_cast<NSUInteger>(std::max(width, 1));
        auto h = static_cast<NSUInteger>(std::max(height, 1));
        auto iw = static_cast<NSUInteger>(std::max<long>(16, std::lround(static_cast<double>(w) * scale)));
        auto ih = static_cast<NSUInteger>(std::max<long>(16, std::lround(static_cast<double>(h) * scale)));
        if (resolve_ && resolve_.width == w && resolve_.height == h && hdr_.width == iw && hdr_.height == ih) return;
        const MTLTextureUsage rt = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        // --- output resolution ---
        resolve_ = target2D(kColorFormat, w, h, rt);
        const NSUInteger ow2 = std::max<NSUInteger>(1, w / 2), oh2 = std::max<NSUInteger>(1, h / 2);
        MTLTextureDescriptor* bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kHDRFormat width:ow2 height:oh2 mipmapped:YES];
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
        postA_ = target2D(kHDRFormat, w, h, rt);
        postB_ = target2D(kHDRFormat, w, h, rt);
        upscaled_ = target2D(kHDRFormat, w, h, rt | MTLTextureUsageShaderWrite);
        dofCoc_ = target2D(kHDRFormat, ow2, oh2, rt);
        dofBlur_ = target2D(kHDRFormat, ow2, oh2, rt);
        // Motion blur tiles (McGuire 2012): ~40 px tiles at 1080p, the largest motion per tile and
        // per 3x3 tile neighborhood.
        motionTilePx_ = std::clamp(static_cast<int>(std::lround(static_cast<double>(h) / 27.0)), 16, 64);
        const NSUInteger tw = (w + static_cast<NSUInteger>(motionTilePx_) - 1) / static_cast<NSUInteger>(motionTilePx_);
        const NSUInteger th = (h + static_cast<NSUInteger>(motionTilePx_) - 1) / static_cast<NSUInteger>(motionTilePx_);
        motionTiles_ = target2D(kVelocityFormat, tw, th, rt);
        motionNeighbors_ = target2D(kVelocityFormat, tw, th, rt);
        fxExposure_ = target2D(MTLPixelFormatR32Float, 1, 1, rt);
        {
            const NSUInteger qw = std::max<NSUInteger>(1, w / 4), qh = std::max<NSUInteger>(1, h / 4);
            MTLTextureDescriptor* ld = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:qw height:qh mipmapped:YES];
            ld.usage = rt;
            ld.storageMode = MTLStorageModePrivate;
            lum_ = [device_ newTextureWithDescriptor:ld];
            for (auto& t : exposure_) t = target2D(MTLPixelFormatR32Float, 1, 1, rt);
            exposureValid_ = false;
        }
        // --- internal (scene) resolution ---
        hdr_ = target2D(kHDRFormat, iw, ih, rt);
        lit_ = target2D(kHDRFormat, iw, ih, rt);
        gbufA_ = target2D(kGbufAFormat, iw, ih, rt);
        gbufB_ = target2D(kGbufBFormat, iw, ih, rt);
        for (auto& t : taa_) t = target2D(kHDRFormat, iw, ih, rt);
        historyValid_ = false;
        depthResolved_ = target2D(kDepthFormat, iw, ih, rt);
        velocity_ = target2D(kVelocityFormat, iw, ih, rt);       // full motion vectors (camera + objects)
        objectMotion_ = target2D(kVelocityFormat, iw, ih, rt);   // main pass color(3), resolved
        msaaVelocity_ = targetMSAA(kVelocityFormat, iw, ih);
        reactiveNone_ = target2D(MTLPixelFormatR8Unorm, iw, ih, rt);  // MetalFX reactive mask when no GPU particles drew
        reactiveNoneCleared_ = false;
        const NSUInteger hw = std::max<NSUInteger>(1, iw / 2), hh = std::max<NSUInteger>(1, ih / 2);
        aoRaw_ = target2D(kAOFormat, hw, hh, rt);
        aoBlurred_ = target2D(kAOFormat, hw, hh, rt);
        sceneCopy_ = target2D(kHDRFormat, iw, ih, MTLTextureUsageShaderRead);
        volumetric_ = target2D(kHDRFormat, hw, hh, rt);
        depthCopy_ = target2D(kDepthFormat, iw, ih, MTLTextureUsageShaderRead);
        msaaColor_ = targetMSAA(kHDRFormat, iw, ih);
        msaaGbufA_ = targetMSAA(kGbufAFormat, iw, ih);
        msaaGbufB_ = targetMSAA(kGbufBFormat, iw, ih);
        msaaDepth_ = targetMSAA(kDepthFormat, iw, ih);
        depthPrev_ = target2D(kDepthFormat, iw, ih, MTLTextureUsageShaderRead);
        giRaw_ = target2D(kHDRFormat, hw, hh, rt);
        ssrRaw_ = target2D(kHDRFormat, hw, hh, rt);
        for (auto& t : giHist_) t = target2D(kHDRFormat, hw, hh, rt);
        for (auto& t : ssrHist_) t = target2D(kHDRFormat, hw, hh, rt);
        cloudRaw_ = target2D(kHDRFormat, hw, hh, rt);
        for (auto& t : cloudHist_) t = target2D(kHDRFormat, hw, hh, rt);
        // Rebuilt for the new sizes on demand. MetalFX encodes asynchronously (on the Neural
        // Engine for the ML scaler), so the old scaler must outlive work already queued with it.
        if (scaler_) retiredScalers_.push_back({scaler_, frameIndex_});
        scaler_ = nil;
        spatialScaler_ = nil;
    }

    /// MetalFX spatial upscaler (GPU only) from the anti-aliased internal image to the output.
    id<MTLFXSpatialScaler> spatialScaler() {
        if (spatialScaler_) return spatialScaler_;
        if (![MTLFXSpatialScalerDescriptor supportsDevice:device_]) return nil;
        MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
        d.colorTextureFormat = kHDRFormat;
        d.outputTextureFormat = kHDRFormat;
        d.inputWidth = hdr_.width;
        d.inputHeight = hdr_.height;
        d.outputWidth = upscaled_.width;
        d.outputHeight = upscaled_.height;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeHDR;
        spatialScaler_ = [d newSpatialScalerWithDevice:device_];
        return spatialScaler_;
    }

    void encodeSpatialUpscale(id<MTLCommandBuffer> cmd) {
        id<MTLFXSpatialScaler> sc = spatialScaler();
        sc.colorTexture = taa_[taaCurrent_];
        sc.outputTexture = upscaled_;
        [sc encodeToCommandBuffer:cmd];
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { (void)sc; }];
    }

    /// MetalFX temporal upscaler from the internal to the output resolution (nil if unsupported).
    id<MTLFXTemporalScaler> temporalScaler() {
        if (scaler_) return scaler_;
        if (![MTLFXTemporalScalerDescriptor supportsDevice:device_]) return nil;
        MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = kHDRFormat;
        d.depthTextureFormat = kDepthFormat;
        d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.outputTextureFormat = kHDRFormat;
        d.inputWidth = hdr_.width;
        d.inputHeight = hdr_.height;
        d.outputWidth = upscaled_.width;
        d.outputHeight = upscaled_.height;
        // Exposure comes from the engine (fxExposure_: what the composite applies), so MetalFX and
        // the tonemapper agree; GPU particles feed the reactive mask (favor the current frame).
        d.autoExposureEnabled = NO;
        d.reactiveMaskTextureEnabled = YES;
        d.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
        scaler_ = [d newTemporalScalerWithDevice:device_];
        return scaler_;
    }

    GpuMesh makeMesh(const MeshData& m) {
        GpuMesh g;
        g.vertices = [device_ newBufferWithBytes:m.vertices.data()
                                          length:m.vertices.size() * sizeof(float)
                                         options:MTLResourceStorageModeShared];
        std::vector<uint32_t> all(m.indices);
        g.lodOffset[0] = 0;
        g.lodCount_[0] = static_cast<uint32_t>(m.indices.size());
        for (size_t i = 0; i < m.lods.size() && g.lodCount < GpuMesh::kMaxLods; ++i) {
            g.lodOffset[g.lodCount] = static_cast<uint32_t>(all.size());
            g.lodCount_[g.lodCount] = static_cast<uint32_t>(m.lods[i].size());
            g.lodError[g.lodCount] = i < m.lodErrors.size() ? m.lodErrors[i] : 0.f;
            all.insert(all.end(), m.lods[i].begin(), m.lods[i].end());
            ++g.lodCount;
        }
        g.indices = [device_ newBufferWithBytes:all.data() length:all.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        g.indexCount = static_cast<uint32_t>(m.indices.size());
        g.vertexCount = static_cast<uint32_t>(m.vertexCount());
        if (m.skinned() && m.skin.joints.size() >= m.vertexCount() * 4 && m.skin.bind.size() >= m.vertexCount() * 6) {
            std::vector<SkinGpuVertex> sv(m.vertexCount());  // animation: skinning input stream
            for (size_t v = 0; v < sv.size(); ++v) {
                std::memcpy(sv[v].position, &m.skin.bind[v * 6], sizeof(float) * 3);
                std::memcpy(sv[v].normal, &m.skin.bind[v * 6 + 3], sizeof(float) * 3);
                std::memcpy(sv[v].joints, &m.skin.joints[v * 4], sizeof(uint16_t) * 4);
                std::memcpy(sv[v].weights, &m.skin.weights[v * 4], sizeof(float) * 4);
            }
            g.skin = [device_ newBufferWithBytes:sv.data() length:sv.size() * sizeof(SkinGpuVertex) options:MTLResourceStorageModeShared];
        }
        Vec3 ext = m.bounds.max - m.bounds.min;
        g.radius = std::max(length(ext) * 0.5f, 1e-3f);
        return g;
    }

    /// Pixels per world unit at `distance` for the current view (perspective).
    float pixelsPerUnit(const FrameData& frame, float distance) const {
        float h = static_cast<float>(std::max(frame.height, 1));
        if (frame.camera.orthographic) return h / (2.f * std::max(frame.camera.orthoSize, 1e-3f));
        return h / (2.f * std::tan(radians(frame.camera.fovDeg) * 0.5f) * std::max(distance, 0.05f));
    }

    /// LOD for a draw item from its on-screen size (`bias` > 0 picks coarser levels, e.g. shadows).
    int lodForDraw(const GpuMesh& m, const DrawItem& d, int bias = 0) const {
        if (m.lodCount <= 1 || !lodFrame_) return 0;
        float scale = std::max({length(d.model.transformDir({1, 0, 0})), length(d.model.transformDir({0, 1, 0})),
                                length(d.model.transformDir({0, 0, 1}))});
        float dist = distance(lodFrame_->camera.eye, d.worldBounds.center());
        bias += lodFrame_->quality >= 2 ? 1 : 0;
        return std::min(m.lodFor(pixelsPerUnit(*lodFrame_, dist) * scale) + bias, m.lodCount - 1);
    }
    void drawLod(id<MTLRenderCommandEncoder> enc, const GpuMesh& m, int lod, NSUInteger instances = 1) {
        lod = std::clamp(lod, 0, m.lodCount - 1);
        trianglesDrawn_ += static_cast<uint64_t>(m.lodCount_[lod] / 3) * instances;
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:m.lodCount_[lod]
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:m.indices
                 indexBufferOffset:m.lodOffset[lod] * sizeof(uint32_t)
                     instanceCount:instances];
    }

    // --- Animation: GPU skinning -----------------------------------------------------------
    void buildSkinningPipeline() {
        NSError* error = nil;
        const auto t0 = std::chrono::steady_clock::now();
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kSkinningShaderSource] options:nil error:&error];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"skinVertices"] : nil;
        skinPipeline_ = fn ? newComputePipeline(device_, fn, &error) : nil;
        libraryMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!skinPipeline_) {
            log::warn("render", std::string("GPU skinning unavailable, characters show their rest pose: ") +
                                    (error ? error.localizedDescription.UTF8String : "no skinVertices kernel"));
        }
    }

    /// Writes each skinned draw's posed vertices into a per-instance buffer registered as a
    /// mesh under the SkinItem key, so every pass draws it like any other mesh.
    void encodeSkinning(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        id<MTLComputeCommandEncoder> enc = nil;
        std::unordered_map<const void*, id<MTLBuffer>> palettes;  // parts of one character share a pose
        for (const SkinItem& s : frame.skins) {
            skinnedKeys_[s.key] = frameIndex_;
            const GpuMesh* base = mesh(s.mesh);
            if (!base) continue;
            GpuMesh& out = meshes_[s.key];
            if (!skinPipeline_ || !base->skin || !s.palette || s.palette->empty()) {
                out = *base;  // no GPU skinning: the rest pose
                continue;
            }
            // Two posed buffers per instance, alternating each frame: the other one still holds the
            // previous pose, which the velocity buffer needs (skinned motion vectors).
            SkinHistory& hist = skinHistory_[s.key];
            const NSUInteger bytes = static_cast<NSUInteger>(base->vertexCount) * MeshData::kFloatsPerVertex * sizeof(float);
            if (!hist.buffer[0] || hist.buffer[0].length != bytes) {
                for (auto& b : hist.buffer) b = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
                hist.lastFrame = ~0ull;
            }
            hist.motion = hist.lastFrame != ~0ull && hist.lastFrame + 1 == frameIndex_;  // posed last frame too
            if (hist.lastFrame != frameIndex_) hist.current ^= 1;
            hist.lastFrame = frameIndex_;
            out.vertices = hist.buffer[hist.current];
            out.vertexCount = base->vertexCount;
            out.indices = base->indices;
            out.indexCount = base->indexCount;
            out.lodCount = base->lodCount;  // LOD index ranges are shared with the source mesh
            std::copy(std::begin(base->lodOffset), std::end(base->lodOffset), std::begin(out.lodOffset));
            std::copy(std::begin(base->lodCount_), std::end(base->lodCount_), std::begin(out.lodCount_));
            std::copy(std::begin(base->lodError), std::end(base->lodError), std::begin(out.lodError));
            out.radius = base->radius;
            out.skin = nil;
            id<MTLBuffer> pal = palettes[s.palette.get()];
            if (!pal) {
                pal = [device_ newBufferWithBytes:s.palette->data() length:s.palette->size() * sizeof(Mat4)
                                          options:MTLResourceStorageModeShared];
                palettes[s.palette.get()] = pal;
            }
            if (!enc) {
                enc = profiledCompute(cmd, "Skinning", "skinning");
                [enc setComputePipelineState:skinPipeline_];
            }
            SkinParams sp{base->vertexCount, static_cast<uint32_t>(s.palette->size()), 0, 0};
            [enc setBuffer:base->vertices offset:0 atIndex:0];
            [enc setBuffer:base->skin offset:0 atIndex:1];
            [enc setBuffer:pal offset:0 atIndex:2];
            [enc setBytes:&sp length:sizeof(sp) atIndex:3];
            [enc setBuffer:out.vertices offset:0 atIndex:4];
            NSUInteger group = std::min<NSUInteger>(skinPipeline_.maxTotalThreadsPerThreadgroup, 64);
            [enc dispatchThreads:MTLSizeMake(base->vertexCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
        }
        if (enc) [enc endEncoding];
        // Drop instances not drawn for a while (kept briefly: thumbnails render in between frames).
        for (auto it = skinnedKeys_.begin(); it != skinnedKeys_.end();) {
            if (frameIndex_ - it->second > 120) {
                meshes_.erase(it->first);
                skinHistory_.erase(it->first);
                it = skinnedKeys_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // -----------------------------------------------------------------------------------------

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

    /// Equirectangular .hdr panorama -> RGBA16F texture with mips (sky backdrop + env bake).
    void ensureHdri(const Environment& env) {
        std::string path = env.skyMode == "hdri" ? env.hdri : std::string();
        if (path == hdriPath_) return;
        hdriPath_ = path;
        hdri_ = nil;
        if (path.empty()) return;
        auto img = loadHdr(path);
        if (!img) {
            log::warn("render", "hdri '" + path + "': " + img.error().message);
            return;
        }
        const NSUInteger w = static_cast<NSUInteger>(img->width), h = static_cast<NSUInteger>(img->height);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:YES];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [device_ newTextureWithDescriptor:d];
        id<MTLBuffer> staging = [device_ newBufferWithLength:w * h * 8 options:MTLResourceStorageModeShared];
        auto* dst = static_cast<__fp16*>(staging.contents);
        const float* src = img->rgb.data();
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            for (int c = 0; c < 3; ++c) dst[i * 4 + c] = static_cast<__fp16>(std::min(src[i * 3 + c], 60000.f));
            dst[i * 4 + 3] = static_cast<__fp16>(1.f);
        }
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:w * 8
            sourceBytesPerImage:w * h * 8
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:tex
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit generateMipmapsForTexture:tex];
        [blit endEncoding];
        [cmd commit];
        hdri_ = tex;
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
        fu.params = simd_make_float4(env.exposure, static_cast<float>(std::min(frame.lights.size(), FrameData::kMaxEffectLights)),
                                     shadows ? 1.f : 0.f, 1.f / static_cast<float>(kShadowAtlas));
        float w = static_cast<float>(hdr_ ? hdr_.width : std::max(frame.width, 1)), h = static_cast<float>(hdr_ ? hdr_.height : std::max(frame.height, 1));
        fu.viewport = simd_make_float4(w, h, 1.f / w, 1.f / h);
        float mode = env.skyMode == "atmosphere" ? 1.f : 0.f;
        if (env.skyMode == "hdri") mode = hdri_ ? 2.f : 1.f;  // no panorama loaded: fall back to the atmosphere
        fu.sky = simd_make_float4(mode, env.clouds, env.stars, env.reflections);
        fu.extra = simd_make_float4(env.fogHeight, env.shadowSoftness, static_cast<float>(kEnvMips - 1), 0.f);
        // hdri: x = rotation (radians), y = intensity, z = mip level for 128 px cube faces, w = mip count
        float envLod = hdri_ ? std::max(0.f, std::log2(static_cast<float>(hdri_.width) / (4.f * kEnvSize))) : 0.f;
        fu.hdri = simd_make_float4(radians(env.hdriRotation), env.hdriIntensity, envLod,
                                   hdri_ ? static_cast<float>(hdri_.mipmapLevelCount) : 0.f);
        fu.clouds = simd_make_float4(0.f, env.cloudHeight, env.cloudThickness, env.cloudDensity);
        fu.clouds2 = simd_make_float4(env.cloudScale, env.cloudSpeed, env.cloudMode == "flat" ? 1.f : 0.f, radians(env.windDirection));
        fu.debug = simd_make_float4(debugViewOverridesSurfaces(frame.debugView) ? static_cast<float>(frame.debugView) : 0.f, 0, 0, 0);
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
            if (l.negative) g.colorIntensity.w = -g.colorIntensity.w;  // subtracts light
            g.params = simd_make_float4(l.specular, static_cast<float>(l.mask & 0xFFFFFu), l.cosInner, l.inverseSquare ? 1.f : 0.f);
            g.params2 = simd_make_float4(l.size, l.indirect, l.volumetric, 0);
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
        du.material4 = simd_make_float4(s.alphaCutoff, s.textureAlphaOnly ? 1.f : 0.f, 0, 0);
        du.prevModel = du.model;  // static unless drawMesh knows better (velocity buffer)
        du.motion = simd_make_float4(0, static_cast<float>(d.layers & 0xFFFFFu), 0, 0);  // y = render layers (light masks)
        for (int i = 0; i < 3; ++i) du.character[i] = simd_make_float4(s.model[i].x, s.model[i].y, s.model[i].z, s.model[i].w);
        du.material5 = simd_make_float4(s.clearcoatRoughness, s.flakes, s.flakeSize, 0);
        return du;
    }

    // --- Environment (image-based lighting) ------------------------------------------------
    std::string environmentKey(const FrameData& frame) const {
        // (cloud drift is excluded: reflections keep a static sky)
        const Environment& e = frame.environment;
        char buf[1024];
        Vec3 d = e.sunDirection();
        std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.4f|%s %.2f %.2f %.2f|%s %.3f %.3f",
                      d.x, d.y, d.z, e.sunColor.x, e.sunColor.y, e.sunColor.z, e.sunIntensity, e.skyTop.x, e.skyTop.y, e.skyTop.z,
                      e.skyHorizon.x, e.skyHorizon.y, e.skyHorizon.z, e.ground.x, e.ground.y, e.ground.z, e.ambient,
                      e.fogColor.x, e.fogColor.y, e.fogColor.z, e.fogDensity, e.skyMode.c_str(), e.clouds, e.stars, e.sunSize,
                      e.hdri.c_str(), e.hdriRotation, e.hdriIntensity);
        char more[256];
        std::snprintf(more, sizeof(more), "|%s %.1f %.1f %.3f %.3f", e.cloudMode.c_str(), e.cloudHeight, e.cloudThickness,
                      e.cloudDensity, e.cloudScale);
        return std::string(buf) + more;
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
            profileRenderPass(rp, "Env sky", "environment");
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Env sky";
            [enc setRenderPipelineState:envSkyPipeline_];
            EnvUniforms eu{simd_make_float4(static_cast<float>(face), 0, kEnvSize, 0)};
            [enc setFragmentBytes:&envFu length:sizeof(envFu) atIndex:0];
            [enc setFragmentBytes:&eu length:sizeof(eu) atIndex:1];
            [enc setFragmentTexture:(hdri_ ?: white_) atIndex:0];
            [enc setFragmentTexture:cloudShape_ atIndex:1];
            [enc setFragmentTexture:cloudDetail_ atIndex:2];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
        }
        id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Env mips", "environment");
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
                profileRenderPass(rp, "Env prefilter", "environment");
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
        profileRenderPass(rp, "Shadow cascades", "shadows");
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
                    id<MTLTexture> cutTex = d.surface.alphaCutoff > 0.f ? texture(d.surface.texture, true) : nil;
                    [enc setRenderPipelineState:cutTex ? shadowAlphaPipeline_ : shadowPipeline_];
                    if (cutTex) {
                        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
                        [enc setFragmentTexture:cutTex atIndex:0];
                    }
                    [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
                    [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
                    drawLod(enc, *m, lodForDraw(*m, d, 1));
                }
                drawTerrainShadows(enc, frame, fr, lvp);
                foliage_->encodeShadows(enc, frame, c, lvp);  // [foliage]
                [enc setRenderPipelineState:shadowPipeline_];
                [enc setCullMode:MTLCullModeNone];
                fx_->encodeShadowCaster(enc, frame, lvp);  // [hair+vfx] hair and mesh particles
            }
        }
        [enc endEncoding];
    }

    // [local shadows] Every caster of one point / spot shadow view: meshes (alpha-tested too) and
    // terrain inside the light's range, the camera's foliage near the light, hair and mesh particles.
    void drawLocalShadowCasters(id<MTLRenderCommandEncoder> enc, const FrameData& frame, const shadows::ShadowFace& face) {
        simd_float4x4 lvp = toSimd(face.viewProj);
        const Vec3 c = face.center;
        const float r = face.radius;
        // Culling frustum: the view itself, or the light's bounding box for paraboloids.
        const Mat4 cullVP = face.projection == shadows::Projection::DualParaboloid
                                ? Mat4::orthographic(r, 1.f, 0.f, 2.f * r) * Mat4::lookAt(c + Vec3{0, 0, r}, c, {0, 1, 0})
                                : face.viewProj;
        const Frustum fr(cullVP);
        [enc setRenderPipelineState:shadowPipeline_];
        [enc setVertexBytes:&lvp length:sizeof(lvp) atIndex:2];
        bool alphaBound = false;
        for (const DrawItem& d : frame.draws) {
            if (!shadows::castsLocalShadow(d, c, r) || !shadows::sphereTouches(c, r, d.worldBounds) || !fr.intersects(d.worldBounds)) continue;
            const GpuMesh* m = mesh(d.mesh);
            if (!m) continue;
            DrawUniforms du = drawUniforms(d);
            id<MTLTexture> cutTex = d.surface.alphaCutoff > 0.f ? texture(d.surface.texture, true) : nil;
            if ((cutTex != nil) != alphaBound) {
                [enc setRenderPipelineState:cutTex ? shadowAlphaPipeline_ : shadowPipeline_];
                alphaBound = cutTex != nil;
            }
            if (cutTex) {
                [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
                [enc setFragmentTexture:cutTex atIndex:0];
            }
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
            drawLod(enc, *m, lodForDraw(*m, d, 1));
        }
        drawTerrainShadows(enc, frame, fr, lvp);
        foliage_->encodeLocalShadows(enc, lvp, c, r);  // [foliage]
        [enc setRenderPipelineState:shadowPipeline_];
        [enc setCullMode:MTLCullModeNone];
        fx_->encodeShadowCaster(enc, frame, lvp);  // [hair+vfx] hair and mesh particles
    }

    // --- [reflection probes] -----------------------------------------------------------------
    /// Probe resources wherever surfaces are lit: the shaded probes (buffer 10), their cluster masks
    /// (buffer 11) and the cube-array atlas (texture 33).
    void bindProbes(id<MTLRenderCommandEncoder> enc) {
        if (!probeBlockBuf_.buffer || !probeMaskBuf_.buffer) return;
        [enc setFragmentBuffer:probeBlockBuf_.buffer offset:probeBlockBuf_.offset atIndex:10];
        [enc setFragmentBuffer:probeMaskBuf_.buffer offset:probeMaskBuf_.offset atIndex:11];
        [enc setFragmentTexture:probes_->atlas() atIndex:33];
    }

    /// Captures this frame's planned probe faces through the scene path (sky, meshes, terrain; no post).
    void encodeProbeCaptures(const FrameData& frame, const FrameUniforms& base) {
        const uint64_t triangles = trianglesDrawn_;  // captures do not count as the frame's geometry
        MetalProbes::Callbacks cb;
        cb.sunShadow = [&](id<MTLCommandBuffer> c, const ProbeItem& p, const Mat4& vp, id<MTLTexture> target) {
            encodeProbeSunShadow(c, frame, p, vp, target, base.params.z > 0.5f);
        };
        cb.drawFace = [&](id<MTLRenderCommandEncoder> enc, const MetalProbes::Face& face) { drawProbeFace(enc, frame, base, face); };
        probes_->capture(frame, cb, gpuFaults_);
        trianglesDrawn_ = triangles;
    }

    /// The probe's sun shadow: one orthographic view around the capture in the top-left quadrant of
    /// `target` (the probes' own map, laid out like cascade 0 of the sun's; cleared only without a sun).
    void encodeProbeSunShadow(id<MTLCommandBuffer> cmd, const FrameData& frame, const ProbeItem& p, const Mat4& vp,
                              id<MTLTexture> target, bool sun) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = target;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Probe sun shadow", "probes");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Probe sun shadow";
        if (!sun) {
            [enc endEncoding];
            return;
        }
        [enc setRenderPipelineState:shadowPipeline_];
        [enc setDepthStencilState:depthWrite_];
        [enc setCullMode:MTLCullModeNone];
        [enc setDepthBias:1.0f slopeScale:2.0f clamp:0.01f];
        const double tile = static_cast<double>(target.width / 2);
        [enc setViewport:MTLViewport{0.0, 0.0, tile, tile, 0.0, 1.0}];
        [enc setScissorRect:MTLScissorRect{0, 0, static_cast<NSUInteger>(tile), static_cast<NSUInteger>(tile)}];
        simd_float4x4 lvp = toSimd(vp);
        [enc setVertexBytes:&lvp length:sizeof(lvp) atIndex:2];
        const Frustum fr(vp);
        bool alphaBound = false;
        for (const DrawItem& d : frame.draws) {
            if (!d.castShadows || d.surface.color.w < 0.5f || d.surface.shading == Shading::Unlit || (d.layers & p.cullMask) == 0) continue;
            if (!fr.intersects(d.worldBounds)) continue;
            const GpuMesh* m = mesh(d.mesh);
            if (!m) continue;
            DrawUniforms du = drawUniforms(d);
            id<MTLTexture> cutTex = d.surface.alphaCutoff > 0.f ? texture(d.surface.texture, true) : nil;
            if ((cutTex != nil) != alphaBound) {
                [enc setRenderPipelineState:cutTex ? shadowAlphaPipeline_ : shadowPipeline_];
                alphaBound = cutTex != nil;
            }
            if (cutTex) {
                [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
                [enc setFragmentTexture:cutTex atIndex:0];
            }
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
            drawLod(enc, *m, lodForDraw(*m, d, 1));
        }
        drawTerrainShadows(enc, frame, fr, lvp);
        [enc endEncoding];
    }

    /// One probe face: the regular scene pipelines with the face's camera, the probe's sun shadow, the
    /// lights that reach the capture (one cluster), probes off (single bounce) and, for interior probes,
    /// the probe's ambient instead of the sky's light.
    void drawProbeFace(id<MTLRenderCommandEncoder> enc, const FrameData& frame, const FrameUniforms& base, const MetalProbes::Face& face) {
        const ProbeItem& p = *face.probe;
        const float size = static_cast<float>(face.size);
        FrameUniforms fu = base;
        fu.viewProj = fu.viewProjNoJitter = fu.prevViewProj = toSimd(face.viewProj);
        fu.invViewProj = toSimd(face.viewProj.inverse());
        fu.cameraPos = v4(p.capture, frame.time);
        fu.cameraForward = v4(probes::faceBasis(face.face).forward, 0.f);
        fu.viewport = simd_make_float4(size, size, 1.f / size, 1.f / size);
        for (int c = 0; c < kCascades; ++c) fu.cascadeViewProj[c] = toSimd(face.sunViewProj);
        fu.cascadeSplits = simd_make_float4(face.sunRadius, face.sunRadius, face.sunRadius, face.sunRadius);
        fu.temporal = simd_make_float4(0, 0, 0, 0);
        fu.debug = simd_make_float4(0, 0, 0, 0);
        fu.extra.w = 0.f;  // no texture mip bias
        fu.params.w = 1.f / static_cast<float>(std::max<NSUInteger>(face.sunShadow.width, 1));  // the probes' sun shadow map
        uint32_t directional = 0;
        std::vector<uint32_t> indices = probes::captureLights(frame, p, directional);
        const uint32_t cells[2] = {0u, static_cast<uint32_t>(indices.size())};
        if (indices.empty()) indices.push_back(0);  // Metal requires a bound buffer
        fu.cluster = simd_make_float4(1, 1, 1, std::log(p.farPlane / p.nearPlane));
        fu.cluster2 = simd_make_float4(p.nearPlane, static_cast<float>(directional), frame.time, 0);
        Alloc blockBuf = transient(face.block, sizeof(probes::GpuProbeBlock));
        Alloc indexBuf = transient(indices.data(), indices.size() * sizeof(uint32_t));

        // Sky (the environment cube) behind everything.
        [enc setRenderPipelineState:probes_->skyPipeline()];
        [enc setDepthStencilState:depthNone_];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [enc setFragmentTexture:skyCube_ atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setFragmentBuffer:lightsBuf_.buffer offset:lightsBuf_.offset atIndex:2];
        [enc setFragmentBytes:cells length:sizeof(cells) atIndex:3];
        [enc setFragmentBuffer:indexBuf.buffer offset:indexBuf.offset atIndex:4];
        [enc setFragmentTexture:face.sunShadow atIndex:1];
        [enc setFragmentTexture:envCube_ atIndex:5];
        [enc setFragmentTexture:brdfLut_ atIndex:6];
        [enc setFragmentTexture:cloudShape_ atIndex:7];
        [enc setFragmentTexture:shadows_->atlas() atIndex:32];
        [enc setFragmentBuffer:blockBuf.buffer offset:blockBuf.offset atIndex:10];  // bounce light: probes reaching the capture
        [enc setFragmentBytes:&face.mask length:sizeof(face.mask) atIndex:11];
        [enc setFragmentTexture:probes_->atlas() atIndex:33];
        [enc setRenderPipelineState:meshProbesPipeline_];  // bounce light: the probes' previous captures
        [enc setDepthStencilState:depthWrite_];
        const Frustum frustum(face.viewProj);
        std::vector<const DrawItem*> blended;
        bool cutoutBound = false;
        for (size_t di = 0; di < frame.draws.size(); ++di) {
            const DrawItem& d = frame.draws[di];
            if ((d.layers & p.cullMask) == 0 || !frustum.intersects(d.worldBounds)) continue;
            if (d.surface.color.w < 0.999f) {
                blended.push_back(&d);
                continue;
            }
            const bool cut = d.surface.alphaCutoff > 0.f;
            if (cut != cutoutBound) {
                [enc setRenderPipelineState:cut ? meshCutoutPipeline_ : meshProbesPipeline_];
                cutoutBound = cut;
            }
            drawMesh(enc, d, di);
        }
        const Vec3 eye = p.capture;
        drawTerrains(enc, frame, frustum, nil, &eye);
        [enc setFragmentTexture:cloudShape_ atIndex:7];  // terrain layers use slots 7+
        if (!blended.empty()) {
            std::sort(blended.begin(), blended.end(), [&](const DrawItem* a, const DrawItem* b) {
                return distance(eye, a->worldBounds.center()) > distance(eye, b->worldBounds.center());
            });
            [enc setRenderPipelineState:meshBlendPipeline_];
            [enc setDepthStencilState:depthRead_];
            for (const DrawItem* d : blended) drawMesh(enc, *d, static_cast<size_t>(d - frame.draws.data()));
        }
    }

    // --- Transient per-frame data ----------------------------------------------------------
    struct Alloc {
        id<MTLBuffer> buffer;
        NSUInteger offset;
    };
    Alloc transient(const void* data, NSUInteger size) {
        const NSUInteger aligned = (size + 255) & ~static_cast<NSUInteger>(255);
        if (ringOffset_ + aligned > kRingSize) {  // rare overflow: a dedicated buffer
            return {[device_ newBufferWithBytes:data length:size options:MTLResourceStorageModeShared], 0};
        }
        Alloc a{ring_[ringIndex_], ringOffset_};
        std::memcpy(static_cast<uint8_t*>(ring_[ringIndex_].contents) + ringOffset_, data, size);
        ringOffset_ += aligned;
        return a;
    }

    // --- Terrain (CDLOD) ---------------------------------------------------------------------
    static constexpr int kPatchCells = 32;

    void buildTerrainPatch() {
        std::vector<simd_float2> v;
        std::vector<uint16_t> idx;
        const int n = kPatchCells;
        for (int z = 0; z <= n; ++z) {
            for (int x = 0; x <= n; ++x) v.push_back(simd_make_float2(static_cast<float>(x) / n, static_cast<float>(z) / n));
        }
        for (int z = 0; z < n; ++z) {
            for (int x = 0; x < n; ++x) {
                uint16_t a = static_cast<uint16_t>(z * (n + 1) + x), b = static_cast<uint16_t>(a + 1);
                uint16_t c = static_cast<uint16_t>(a + n + 1), d = static_cast<uint16_t>(c + 1);
                // Alternate the diagonal for a more isotropic mesh.
                if ((x + z) % 2 == 0) {
                    idx.insert(idx.end(), {a, c, b, b, c, d});
                } else {
                    idx.insert(idx.end(), {a, c, d, a, d, b});
                }
            }
        }
        patchVertices_ = [device_ newBufferWithBytes:v.data() length:v.size() * sizeof(simd_float2) options:MTLResourceStorageModeShared];
        patchIndices_ = [device_ newBufferWithBytes:idx.data() length:idx.size() * sizeof(uint16_t) options:MTLResourceStorageModeShared];
        patchIndexCount_ = static_cast<uint32_t>(idx.size());
    }

    id<MTLTexture> sharedTexture(MTLPixelFormat format, int w, int h, const void* bytes, NSUInteger bytesPerRow) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:static_cast<NSUInteger>(w)
                                                                                    height:static_cast<NSUInteger>(h)
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;  // unified memory: no staging copy
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        [t replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w), static_cast<NSUInteger>(h))
             mipmapLevel:0
               withBytes:bytes
             bytesPerRow:bytesPerRow];
        return t;
    }

    /// The terrain that forms the seabed of `w` near the camera: the one under the camera,
    /// else the closest one whose waterLevel matches the water (none for open ocean scenes).
    const TerrainItem* seabedTerrain(const FrameData& frame, const WaterItem& w) const {
        const TerrainItem* best = nullptr;
        float bestDist = 1e30f;
        for (const TerrainItem& t : frame.terrains) {
            if (!t.data || t.data->resolution() < 2) continue;
            const float half = t.data->size() * 0.5f;
            const float dx = std::max(std::abs(frame.camera.eye.x - t.origin.x) - half, 0.f);
            const float dz = std::max(std::abs(frame.camera.eye.z - t.origin.z) - half, 0.f);
            float d = std::sqrt(dx * dx + dz * dz);
            if (std::abs(t.waterLevel - w.level) < 0.5f) d -= 1.f;  // prefer terrains made for this water
            if (d < bestDist) {
                bestDist = d;
                best = &t;
            }
        }
        return best;
    }

    TerrainGpu& terrainGpu(const TerrainItem& item) {
        TerrainGpu& g = terrainsGpu_[item.entity];
        g.lastUse = frameIndex_;
        const world::TerrainData& t = *item.data;
        if (g.source == item.data.get() && g.version == t.version() && g.height) return g;
        g.source = item.data.get();
        g.version = t.version();
        const int n = t.resolution();
        g.height = sharedTexture(MTLPixelFormatR32Float, n, n, t.heights().data(), static_cast<NSUInteger>(n) * 4);
        // Normals (xz, encoded 0..1) for smooth shading independent of the LOD mesh.
        std::vector<__fp16> nrm(static_cast<size_t>(n) * n * 2);
        const float cell = t.cell();
        for (int z = 0; z < n; ++z) {
            for (int x = 0; x < n; ++x) {
                float hl = t.h(std::max(x - 1, 0), z), hr = t.h(std::min(x + 1, n - 1), z);
                float hd = t.h(x, std::max(z - 1, 0)), hu = t.h(x, std::min(z + 1, n - 1));
                Vec3 nv = normalize(Vec3{hl - hr, 2.f * cell, hd - hu});
                size_t i = (static_cast<size_t>(z) * n + x) * 2;
                nrm[i] = static_cast<__fp16>(nv.x * 0.5f + 0.5f);
                nrm[i + 1] = static_cast<__fp16>(nv.z * 0.5f + 0.5f);
            }
        }
        g.normal = sharedTexture(MTLPixelFormatRG16Float, n, n, nrm.data(), static_cast<NSUInteger>(n) * 4);
        std::vector<uint8_t> w0(static_cast<size_t>(n) * n * 4), w1(static_cast<size_t>(n) * n * 4);
        const auto& ws = t.weights();
        for (size_t i = 0, cnt = static_cast<size_t>(n) * n; i < cnt; ++i) {
            std::memcpy(&w0[i * 4], &ws[i * 8], 4);
            std::memcpy(&w1[i * 4], &ws[i * 8 + 4], 4);
        }
        g.weights0 = sharedTexture(MTLPixelFormatRGBA8Unorm, n, n, w0.data(), static_cast<NSUInteger>(n) * 4);
        g.weights1 = sharedTexture(MTLPixelFormatRGBA8Unorm, n, n, w1.data(), static_cast<NSUInteger>(n) * 4);
        {
            // Smoothed seabed for wave shoaling: block average to ~16 m cells, then a 3x3 blur.
            // Hollows behind a beach berm average out above the waterline, so swells never
            // flood them, while open beaches still see waves run up the sand.
            const int k = std::max(1, static_cast<int>(std::lround(16.f / std::max(t.cell(), 1e-3f))));
            const int m = std::max(2, (n + k - 1) / k);
            std::vector<float> avg(static_cast<size_t>(m) * m), blur(avg.size());
            const auto& hs = t.heights();
            for (int bz = 0; bz < m; ++bz) {
                for (int bx = 0; bx < m; ++bx) {
                    double sum = 0;
                    int count = 0;
                    for (int z = bz * k; z < std::min(n, bz * k + k); ++z) {
                        for (int x = bx * k; x < std::min(n, bx * k + k); ++x, ++count) sum += hs[static_cast<size_t>(z) * n + x];
                    }
                    avg[static_cast<size_t>(bz) * m + bx] = count ? static_cast<float>(sum / count) : 0.f;
                }
            }
            for (int z = 0; z < m; ++z) {
                for (int x = 0; x < m; ++x) {
                    float sum = 0;
                    int count = 0;
                    for (int dz = -1; dz <= 1; ++dz) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int xx = x + dx, zz = z + dz;
                            if (xx < 0 || zz < 0 || xx >= m || zz >= m) continue;
                            sum += avg[static_cast<size_t>(zz) * m + xx];
                            ++count;
                        }
                    }
                    blur[static_cast<size_t>(z) * m + x] = sum / static_cast<float>(count);
                }
            }
            g.seabed = sharedTexture(MTLPixelFormatR32Float, m, m, blur.data(), static_cast<NSUInteger>(m) * 4);
        }
        // Min/max height pyramid over kPatchCells-sized blocks (node culling).
        const int cells = n - 1;
        const int n0 = std::max(1, (cells + kPatchCells - 1) / kPatchCells);
        g.nodesPerSide0 = n0;
        g.minMax.clear();
        std::vector<simd_float2> lvl(static_cast<size_t>(n0) * n0);
        for (int bz = 0; bz < n0; ++bz) {
            for (int bx = 0; bx < n0; ++bx) {
                float mn = 1e30f, mx = -1e30f;
                for (int z = bz * kPatchCells; z <= std::min((bz + 1) * kPatchCells, n - 1); ++z) {
                    for (int x = bx * kPatchCells; x <= std::min((bx + 1) * kPatchCells, n - 1); ++x) {
                        mn = std::min(mn, t.h(x, z));
                        mx = std::max(mx, t.h(x, z));
                    }
                }
                lvl[static_cast<size_t>(bz) * n0 + bx] = simd_make_float2(mn, mx);
            }
        }
        g.minMax.push_back(lvl);
        int side = n0;
        while (side > 1) {
            int ns = (side + 1) / 2;
            std::vector<simd_float2> up(static_cast<size_t>(ns) * ns, simd_make_float2(1e30f, -1e30f));
            for (int z = 0; z < side; ++z) {
                for (int x = 0; x < side; ++x) {
                    simd_float2 c = g.minMax.back()[static_cast<size_t>(z) * side + x];
                    simd_float2& u = up[static_cast<size_t>(z / 2) * ns + x / 2];
                    u = simd_make_float2(std::min(u.x, c.x), std::max(u.y, c.y));
                }
            }
            g.minMax.push_back(std::move(up));
            side = ns;
        }
        g.levels = static_cast<int>(g.minMax.size()) - 1;
        return g;
    }

    /// Quadtree LOD selection (CDLOD): nodes subdivide while the eye is within their range.
    void selectTerrainNodes(const TerrainItem& item, const TerrainGpu& g, Vec3 eye, const Frustum& fr, float detail,
                            std::vector<TerrainNodeGpu>& out) const {
        const world::TerrainData& t = *item.data;
        const float finest = kPatchCells * t.cell();
        const float base = std::max(3.2f, 3.5f * detail) * finest;
        const float half = t.size() * 0.5f;
        auto range = [&](int L) { return base * std::exp2(static_cast<float>(L)); };
        std::function<void(int, int, int)> visit = [&](int L, int ix, int iz) {
            const int sideL = static_cast<int>(g.minMax[static_cast<size_t>(L)].size() > 0
                                                   ? std::lround(std::sqrt(static_cast<double>(g.minMax[static_cast<size_t>(L)].size())))
                                                   : 1);
            if (ix >= sideL || iz >= sideL) return;
            const float ns = finest * std::exp2(static_cast<float>(L));
            const float x0 = -half + ix * ns, z0 = -half + iz * ns;
            if (x0 >= half || z0 >= half) return;
            simd_float2 mm = g.minMax[static_cast<size_t>(L)][static_cast<size_t>(iz) * sideL + ix];
            Aabb b;
            b.min = {item.origin.x + x0, item.origin.y + mm.x, item.origin.z + z0};
            b.max = {item.origin.x + std::min(x0 + ns, half), item.origin.y + mm.y, item.origin.z + std::min(z0 + ns, half)};
            if (!fr.intersects(b)) return;
            Vec3 c{std::clamp(eye.x, b.min.x, b.max.x), std::clamp(eye.y, b.min.y, b.max.y), std::clamp(eye.z, b.min.z, b.max.z)};
            float d = distance(eye, c);
            if (L == 0 || d > range(L - 1)) {
                float lo = L == 0 ? 0.f : range(L - 1);
                float hi = range(L);
                float morphStart = L >= g.levels ? 1e30f : lo + (hi - lo) * 0.66f;
                float morphEnd = L >= g.levels ? 1e30f : hi * 0.98f;
                TerrainNodeGpu node;
                node.node = simd_make_float4(x0, z0, ns, static_cast<float>(L));
                node.morph = simd_make_float4(morphStart, morphEnd, static_cast<float>(kPatchCells), 0);
                out.push_back(node);
                return;
            }
            for (int dz = 0; dz < 2; ++dz) {
                for (int dx = 0; dx < 2; ++dx) visit(L - 1, ix * 2 + dx, iz * 2 + dz);
            }
        };
        visit(g.levels, 0, 0);
    }

    TerrainUniformsGpu terrainUniforms(const TerrainItem& item) {
        const world::TerrainData& t = *item.data;
        TerrainUniformsGpu u{};
        u.origin = v4(item.origin, t.size());
        u.grid = simd_make_float4(static_cast<float>(t.resolution()), 1.f / t.resolution(), t.cell(),
                                  static_cast<float>(std::min<size_t>(item.layers.size(), 8)));
        u.water = simd_make_float4(item.waterLevel, item.wetBand, item.selected ? 1.f : 0.f, item.macroVariation);
        for (size_t i = 0; i < std::min<size_t>(item.layers.size(), 8); ++i) {
            const auto& l = item.layers[i];
            const Surface& s = l.surface;
            int flags = (texture(s.texture, true) ? 1 : 0) | (texture(s.normalMap, false) ? 2 : 0) | (texture(s.ormMap, false) ? 4 : 0) |
                        (l.triplanar ? 8 : 0);
            u.layerParams[i] = simd_make_float4(l.tiling, s.normalStrength, s.roughness, static_cast<float>(flags));
            u.layerColor[i] = lin(s.color.xyz(), s.metallic);
        }
        if (!item.overlay.empty() && texture(item.overlay, true)) {
            u.overlay = simd_make_float4(item.overlayOpacity, static_cast<float>(item.overlayBlend), 0, 0);
        }
        return u;
    }

    void drawTerrains(id<MTLRenderCommandEncoder> enc, const FrameData& frame, const Frustum& fr,
                      id<MTLRenderPipelineState> pso = nil, const Vec3* lodEye = nullptr) {
        if (frame.terrains.empty()) return;
        [enc setRenderPipelineState:pso ?: terrainPipeline_];
        [enc setCullMode:MTLCullModeNone];
        for (const TerrainItem& item : frame.terrains) {
            if (!item.data) continue;
            TerrainGpu& g = terrainGpu(item);
            std::vector<TerrainNodeGpu> nodes;
            selectTerrainNodes(item, g, lodEye ? *lodEye : frame.camera.eye, fr, item.detail, nodes);
            if (nodes.empty()) continue;
            TerrainUniformsGpu u = terrainUniforms(item);
            Alloc nb = transient(nodes.data(), nodes.size() * sizeof(TerrainNodeGpu));
            [enc setVertexBuffer:patchVertices_ offset:0 atIndex:0];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
            [enc setVertexBuffer:nb.buffer offset:nb.offset atIndex:3];
            [enc setVertexTexture:g.height atIndex:0];
            [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [enc setFragmentTexture:g.normal atIndex:0];
            [enc setFragmentTexture:g.weights0 atIndex:2];
            [enc setFragmentTexture:g.weights1 atIndex:3];
            [enc setFragmentTexture:cloudShape_ atIndex:4];
            for (size_t i = 0; i < 8; ++i) {
                const Surface* s = i < item.layers.size() ? &item.layers[i].surface : nullptr;
                id<MTLTexture> a = s ? texture(s->texture, true) : nil, n = s ? texture(s->normalMap, false) : nil,
                               o = s ? texture(s->ormMap, false) : nil;
                [enc setFragmentTexture:(a ?: white_) atIndex:7 + i * 3];
                [enc setFragmentTexture:(n ?: white_) atIndex:8 + i * 3];
                [enc setFragmentTexture:(o ?: white_) atIndex:9 + i * 3];
            }
            id<MTLTexture> ov = item.overlay.empty() ? nil : texture(item.overlay, true);
            [enc setFragmentTexture:(ov ?: white_) atIndex:31];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:patchIndexCount_
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:patchIndices_
                     indexBufferOffset:0
                         instanceCount:nodes.size()];
            terrainNodesDrawn_ += nodes.size();
        }
    }

    void drawTerrainShadows(id<MTLRenderCommandEncoder> enc, const FrameData& frame, const Frustum& fr, const simd_float4x4& lvp) {
        bool bound = false;
        for (const TerrainItem& item : frame.terrains) {
            if (!item.data || !item.castShadows) continue;
            TerrainGpu& g = terrainGpu(item);
            std::vector<TerrainNodeGpu> nodes;
            selectTerrainNodes(item, g, frame.camera.eye, fr, item.detail * 0.5f, nodes);
            if (nodes.empty()) continue;
            if (!bound) {
                [enc setRenderPipelineState:terrainShadowPipeline_];
                bound = true;
            }
            TerrainUniformsGpu u = terrainUniforms(item);
            Alloc nb = transient(nodes.data(), nodes.size() * sizeof(TerrainNodeGpu));
            [enc setVertexBuffer:patchVertices_ offset:0 atIndex:0];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
            [enc setVertexBytes:&lvp length:sizeof(lvp) atIndex:2];
            [enc setVertexBuffer:nb.buffer offset:nb.offset atIndex:3];
            [enc setVertexTexture:g.height atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:patchIndexCount_
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:patchIndices_
                     indexBufferOffset:0
                         instanceCount:nodes.size()];
        }
    }

    /// After a GPU fault (watchdog timeout, page fault) the renderer stays in safe mode: the
    /// foliage triangle budget drops to a quarter so frames stay far from the watchdog.
    void chooseFoliageBudgetBias(const FrameData& frame) {
        (void)frame;
        const int faults = gpuFaults_->load();
        if (faults != reportedFaults_) {
            reportedFaults_ = faults;
            log::error("render", "GPU command buffer failed (" + std::to_string(faults) +
                                     " so far): rendering in safe mode with a reduced geometry budget");
        }
        foliage_->setSafeMode(faults > 0);  // [foliage] per-instance LODs fit MetalFoliage's budget
    }

    void evictWorldCaches() {
        foliage_->evict(frameIndex_);  // [foliage]
        std::erase_if(terrainsGpu_, [&](const auto& kv) { return frameIndex_ - kv.second.lastUse > 180; });
    }

    // --- Scene ------------------------------------------------------------------------------
    void drawMesh(id<MTLRenderCommandEncoder> enc, const DrawItem& d, size_t index) {
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
        // Velocity buffer: the previous transform and, for skinned meshes, the previous pose.
        const Mat4 prev = index < prevModels_.size() ? prevModels_[index] : d.model;
        id<MTLBuffer> prevVertices = m->vertices;
        bool moving = transformChanged(prev, d.model);
        if (d.skin >= 0) {
            if (auto it = skinHistory_.find(d.mesh); it != skinHistory_.end() && it->second.motion && it->second.buffer[it->second.current] == m->vertices) {
                prevVertices = it->second.buffer[it->second.current ^ 1];
                moving = true;
            }
        }
        du.prevModel = toSimd(prev);
        du.motion.x = moving ? 1.f : 0.f;
        const int lod = lodForDraw(*m, d);
        du.material4.w = static_cast<float>(std::clamp(lod, 0, m->lodCount - 1) + 1);  // [debug views] lod
        bool twoSided = s.doubleSided || d.mesh == "plane" || d.mesh == "quad";
        [enc setCullMode:twoSided ? MTLCullModeNone : MTLCullModeBack];
        [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
        [enc setVertexBuffer:prevVertices offset:0 atIndex:3];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc setFragmentTexture:(albedo ?: white_) atIndex:0];
        [enc setFragmentTexture:(normal ?: white_) atIndex:2];
        [enc setFragmentTexture:(orm ?: white_) atIndex:3];
        [enc setFragmentTexture:(emissive ?: white_) atIndex:4];
        drawLod(enc, *m, lod);
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
        rp.colorAttachments[1].texture = msaaGbufA_;
        rp.colorAttachments[1].resolveTexture = gbufA_;
        rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[1].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[2].texture = msaaGbufB_;
        rp.colorAttachments[2].resolveTexture = gbufB_;
        rp.colorAttachments[2].loadAction = MTLLoadActionClear;
        rp.colorAttachments[2].clearColor = MTLClearColorMake(0, 0, 1, 4);
        rp.colorAttachments[2].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[3].texture = msaaVelocity_;  // object motion (0 = static)
        rp.colorAttachments[3].resolveTexture = objectMotion_;
        rp.colorAttachments[3].loadAction = MTLLoadActionClear;
        rp.colorAttachments[3].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[3].storeAction = MTLStoreActionMultisampleResolve;
        rp.depthAttachment.texture = msaaDepth_;
        rp.depthAttachment.resolveTexture = depthResolved_;
        rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;

        profileRenderPass(rp, "Main", "main");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Main";
        [enc setFrontFacingWinding:MTLWindingCounterClockwise];
        if (frame.debugView == debugview::kOverdraw) {  // [debug views] count fragments, nothing else
            encodeOverdraw(enc, frame, fu);
            [enc endEncoding];
            return;
        }

        // Sky
        [enc setRenderPipelineState:skyPipeline_];
        [enc setDepthStencilState:depthNone_];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [enc setFragmentTexture:(hdri_ ?: white_) atIndex:0];
        [enc setFragmentTexture:(cloudsActive_ ? cloudOut_ : clearCloud_) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // Opaque meshes
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setFragmentBuffer:lightsBuf_.buffer offset:lightsBuf_.offset atIndex:2];
        [enc setFragmentBuffer:clusterCellsBuf_.buffer offset:clusterCellsBuf_.offset atIndex:3];
        [enc setFragmentBuffer:clusterIndexBuf_.buffer offset:clusterIndexBuf_.offset atIndex:4];
        [enc setFragmentTexture:shadowMap_ atIndex:1];
        [enc setFragmentTexture:envCube_ atIndex:5];
        [enc setFragmentTexture:brdfLut_ atIndex:6];
        [enc setFragmentTexture:cloudShape_ atIndex:7];
        [enc setFragmentTexture:shadows_->atlas() atIndex:32];  // [local shadows] meshes, terrain, foliage, hair
        bindProbes(enc);  // [reflection probes] meshes, terrain, foliage, hair
        [enc setRenderPipelineState:meshPipeline_];
        [enc setDepthStencilState:depthWrite_];
        std::vector<const DrawItem*> blended, outlined;
        bool cutoutBound = false;
        const Frustum frustum(frame.viewProjection());
        culled_ = 0;
        for (size_t di = 0; di < frame.draws.size(); ++di) {
            const DrawItem& d = frame.draws[di];
            if (!frustum.intersects(d.worldBounds)) {
                ++culled_;
                continue;
            }
            if (d.surface.color.w < 0.999f) {
                blended.push_back(&d);
                continue;
            }
            if (d.surface.outline > 0.f) outlined.push_back(&d);
            bool cut = d.surface.alphaCutoff > 0.f;
            if (cut != cutoutBound) {
                [enc setRenderPipelineState:cut ? meshCutoutPipeline_ : meshPipeline_];
                cutoutBound = cut;
            }
            drawMesh(enc, d, di);
        }
        fx_->encodeOpaque(enc, frame);  // [hair+vfx] strand hair, lit mesh particles

        // Terrain and instanced foliage (opaque)
        terrainNodesDrawn_ = 0;
        drawTerrains(enc, frame, frustum);
        [enc setFragmentTexture:cloudShape_ atIndex:7];  // terrain layers use slots 7+; lit surfaces expect cloud noise there
        foliage_->encodeMain(enc, frame);  // [foliage] GPU-culled instances, mesh LOD bands, impostors
        [enc setRenderPipelineState:meshPipeline_];
        [enc setDepthStencilState:depthWrite_];
        if (frame.debugView == debugview::kWireframe) {  // [debug views] edges over the opaque meshes
            [enc setRenderPipelineState:wireframePipeline_];
            [enc setDepthStencilState:depthRead_];
            [enc setTriangleFillMode:MTLTriangleFillModeLines];
            [enc setDepthBias:-4.0f slopeScale:-2.0f clamp:0.f];
            for (size_t i = 0; i < frame.draws.size(); ++i) {
                const DrawItem& d = frame.draws[i];
                if (d.surface.color.w >= 0.999f && frustum.intersects(d.worldBounds)) drawMesh(enc, d, i);
            }
            drawTerrains(enc, frame, frustum, terrainWirePipeline_);
            [enc setTriangleFillMode:MTLTriangleFillModeFill];
            [enc setDepthBias:0.f slopeScale:0.f clamp:0.f];
            [enc setRenderPipelineState:meshPipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setFragmentTexture:cloudShape_ atIndex:7];
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
            for (const DrawItem* d : blended) drawMesh(enc, *d, static_cast<size_t>(d - frame.draws.data()));
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

        if (r2d_) r2d_->encodeWorld(enc, rp, frame);  // 2D: sprites, tiles, world text (depth-tested)
        [enc endEncoding];
    }

    /// [debug views] overdraw: every mesh, terrain and foliage fragment adds 1 (no depth test, no
    /// sky); the debug pass turns the counts into a heat map.
    void encodeOverdraw(id<MTLRenderCommandEncoder> enc, const FrameData& frame, const FrameUniforms& fu) {
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setRenderPipelineState:overdrawPipeline_];
        [enc setDepthStencilState:depthNone_];
        const Frustum frustum(frame.viewProjection());
        culled_ = 0;
        for (size_t i = 0; i < frame.draws.size(); ++i) {
            const DrawItem& d = frame.draws[i];
            if (frustum.intersects(d.worldBounds)) drawMesh(enc, d, i);
            else ++culled_;
        }
        terrainNodesDrawn_ = 0;
        drawTerrains(enc, frame, frustum, terrainOverdrawPipeline_);
        [enc setDepthStencilState:depthNone_];
        foliage_->encodeMain(enc, frame);  // uses its overdraw pipelines in this view
    }

    // --- Effects: FFT water and particles (single-sample, over the resolved scene) -----------
    static GridMesh makeGrid(id<MTLDevice> device, const std::vector<float>& coords, bool square) {
        // coords: 1D sample positions per axis; the grid is their cartesian product.
        const auto n = static_cast<uint32_t>(coords.size());
        std::vector<float> v;
        v.reserve(n * n * 2);
        for (uint32_t z = 0; z < n; ++z) {
            for (uint32_t x = 0; x < n; ++x) {
                v.push_back(coords[x]);
                v.push_back(coords[z]);
            }
        }
        std::vector<uint32_t> idx;
        idx.reserve((n - 1) * (n - 1) * 6);
        for (uint32_t z = 0; z + 1 < n; ++z) {
            for (uint32_t x = 0; x + 1 < n; ++x) {
                uint32_t a = z * n + x, b = a + 1, c = a + n, d = c + 1;
                idx.insert(idx.end(), {a, c, b, b, c, d});
            }
        }
        (void)square;
        GridMesh g;
        g.vertices = [device newBufferWithBytes:v.data() length:v.size() * sizeof(float) options:MTLResourceStorageModeShared];
        g.indices = [device newBufferWithBytes:idx.data() length:idx.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        g.indexCount = static_cast<uint32_t>(idx.size());
        return g;
    }

    void ensureGrids() {
        if (endlessGrid_.vertices) return;
        // Endless ocean: dense near the camera (0.3 m), cells growing ~6% per ring out past the horizon.
        std::vector<float> side{0.f};
        float step = 0.3f, at = 0.f;
        while (at < 25000.f) {
            at += step;
            side.push_back(at);
            step *= 1.061f;
        }
        std::vector<float> coords;
        for (size_t i = side.size(); i-- > 1;) coords.push_back(-side[i]);
        coords.insert(coords.end(), side.begin(), side.end());
        endlessGrid_ = makeGrid(device_, coords, false);
        std::vector<float> unit;
        for (int i = 0; i <= 160; ++i) unit.push_back(static_cast<float>(i) / 160.f - 0.5f);
        unitGrid_ = makeGrid(device_, unit, true);
    }

    OceanGpu& oceanTextures(const WaterItem& w) {
        OceanGpu& g = oceans_[w.entity];
        const OceanCascades& o = *w.ocean;
        const auto N = static_cast<NSUInteger>(o.resolution);
        if (!g.disp[0] || g.disp[0].width != N) {
            for (int c = 0; c < OceanCascades::kCascades; ++c) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                             width:N
                                                                                            height:N
                                                                                         mipmapped:YES];
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModePrivate;
                g.disp[c] = [device_ newTextureWithDescriptor:d];
                g.slope[c] = [device_ newTextureWithDescriptor:d];
            }
            g.version = 0;
        }
        if (g.version == o.version) return g;
        g.version = o.version;
        const NSUInteger texels = N * N * 4;
        id<MTLBuffer> staging = [device_ newBufferWithLength:texels * 2 * 2 * OceanCascades::kCascades
                                                     options:MTLResourceStorageModeShared];
        auto* dst = static_cast<__fp16*>(staging.contents);
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        NSUInteger offset = 0;
        for (int c = 0; c < OceanCascades::kCascades; ++c) {
            for (int which = 0; which < 2; ++which) {
                const std::vector<float>& src = which ? o.slope[c] : o.displacement[c];
                for (NSUInteger i = 0; i < texels; ++i) dst[offset / 2 + i] = static_cast<__fp16>(src[i]);
                [blit copyFromBuffer:staging
                           sourceOffset:offset
                      sourceBytesPerRow:N * 8
                    sourceBytesPerImage:N * N * 8
                             sourceSize:MTLSizeMake(N, N, 1)
                              toTexture:which ? g.slope[c] : g.disp[c]
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
                offset += texels * 2;
            }
            [blit generateMipmapsForTexture:g.disp[c]];
            [blit generateMipmapsForTexture:g.slope[c]];
        }
        [blit endEncoding];
        [cmd commit];  // same queue: completes before the frame that samples it
        return g;
    }

    // --- Volumetric fluids ---------------------------------------------------------------
    id<MTLTexture> volumeTexture(int nx, int ny, int nz, MTLPixelFormat format) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = format;
        d.width = static_cast<NSUInteger>(nx);
        d.height = static_cast<NSUInteger>(ny);
        d.depth = static_cast<NSUInteger>(nz);
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        return [device_ newTextureWithDescriptor:d];
    }

    void clearVolume(id<MTLCommandBuffer> cmd, id<MTLTexture> t) {
        // Zero a private 3D texture with a blit from a zeroed buffer.
        NSUInteger bpp = t.pixelFormat == MTLPixelFormatR32Float ? 4 : 8;
        NSUInteger row = t.width * bpp, image = row * t.height;
        id<MTLBuffer> zeros = [device_ newBufferWithLength:image * t.depth options:MTLResourceStorageModePrivate];
        id<MTLBlitCommandEncoder> b = profiledBlit(cmd, "Fluid clear", "effects");
        [b fillBuffer:zeros range:NSMakeRange(0, zeros.length) value:0];
        [b copyFromBuffer:zeros
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:image
                     sourceSize:MTLSizeMake(t.width, t.height, t.depth)
                      toTexture:t
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }

    FluidGpu& fluidFor(id<MTLCommandBuffer> cmd, const VolumeItem& v) {
        const FluidVolume& p = v.params;
        float longest = std::max({p.size.x, p.size.y, p.size.z, 0.01f});
        int res = std::clamp(p.resolution, 16, 192);
        float cell = longest / static_cast<float>(res);
        int nx = std::clamp(static_cast<int>(std::round(p.size.x / cell)), 8, 192);
        int ny = std::clamp(static_cast<int>(std::round(p.size.y / cell)), 8, 192);
        int nz = std::clamp(static_cast<int>(std::round(p.size.z / cell)), 8, 192);
        FluidGpu& g = fluids_[v.entity];
        if (g.nx != nx || g.ny != ny || g.nz != nz) {
            g = FluidGpu{};
            g.nx = nx;
            g.ny = ny;
            g.nz = nz;
            for (int i = 0; i < 2; ++i) {
                g.vel[i] = volumeTexture(nx, ny, nz, MTLPixelFormatRGBA16Float);
                g.scal[i] = volumeTexture(nx, ny, nz, MTLPixelFormatRGBA16Float);
                g.pressure[i] = volumeTexture(nx, ny, nz, MTLPixelFormatR32Float);
            }
            g.tmpA = volumeTexture(nx, ny, nz, MTLPixelFormatRGBA16Float);
            g.tmpB = volumeTexture(nx, ny, nz, MTLPixelFormatRGBA16Float);
            g.curl = volumeTexture(nx, ny, nz, MTLPixelFormatRGBA16Float);
            g.div = volumeTexture(nx, ny, nz, MTLPixelFormatR32Float);
            for (int i = 0; i < 2; ++i) {
                clearVolume(cmd, g.vel[i]);
                clearVolume(cmd, g.scal[i]);
                clearVolume(cmd, g.pressure[i]);
            }
        }
        g.cell = cell;
        return g;
    }

    void dispatch(id<MTLComputeCommandEncoder> enc, const char* kernel, const FluidGpu& g) {
        id<MTLComputePipelineState> ps = fluidKernels_[kernel];
        [enc setComputePipelineState:ps];
        MTLSize tg = MTLSizeMake(8, 8, 4);
        [enc dispatchThreadgroups:MTLSizeMake((static_cast<NSUInteger>(g.nx) + 7) / 8, (static_cast<NSUInteger>(g.ny) + 7) / 8,
                                              (static_cast<NSUInteger>(g.nz) + 3) / 4)
            threadsPerThreadgroup:tg];
    }

    void fluidStep(id<MTLCommandBuffer> cmd, FluidGpu& g, const VolumeItem& v, float dt, float time) {
        const FluidVolume& p = v.params;
        float inv = 1.f / g.cell;
        Vec3 src{p.sourceOffset.x + p.size.x * 0.5f, p.sourceOffset.y, p.sourceOffset.z + p.size.z * 0.5f};
        Vec3 windLocal = v.model.inverse().transformDir(v.wind) * inv;
        float burst = (p.burst > 0.f && g.age < p.burst) ? 6.f : 1.f;
        bool feeding = p.emitting || (p.burst > 0.f && g.age < p.burst);
        FluidParams fp{};
        fp.dims = simd_make_float4(g.nx, g.ny, g.nz, g.cell);
        fp.step = simd_make_float4(dt, time, static_cast<float>(p.seed) * 13.7f + static_cast<float>(v.entity), burst);
        fp.source = simd_make_float4(src.x * inv - 0.5f, src.y * inv - 0.5f, src.z * inv - 0.5f, p.sourceRadius * inv);
        fp.feed = simd_make_float4(p.fuel, p.heat, p.smoke, p.speed * inv);
        fp.physics = simd_make_float4(p.buoyancy, p.vorticity, p.turbulence, p.burnRate);
        fp.decay = simd_make_float4(p.cooling, p.smokeFade, feeding ? 1.f : 0.f, 0.f);
        fp.wind = simd_make_float4(windLocal.x, windLocal.y, windLocal.z, 0.f);
        float fwd = 1.f, bwd = -1.f;

        id<MTLComputeCommandEncoder> enc = profiledCompute(cmd, "Fluid step", "effects");
        [enc setBytes:&fp length:sizeof(fp) atIndex:0];
        // Advect velocity and scalars (MacCormack), each with the old velocity field.
        for (int field = 0; field < 2; ++field) {
            id<MTLTexture> srcT = field == 0 ? g.vel[0] : g.scal[0];
            id<MTLTexture> dstT = field == 0 ? g.vel[1] : g.scal[1];
            [enc setTexture:srcT atIndex:0];
            [enc setTexture:g.vel[0] atIndex:1];
            [enc setTexture:g.tmpA atIndex:2];
            [enc setBytes:&fwd length:sizeof(float) atIndex:1];
            dispatch(enc, "fluidAdvect", g);
            [enc setTexture:g.tmpA atIndex:0];
            [enc setTexture:g.tmpB atIndex:2];
            [enc setBytes:&bwd length:sizeof(float) atIndex:1];
            dispatch(enc, "fluidAdvect", g);
            [enc setTexture:srcT atIndex:0];
            [enc setTexture:g.vel[0] atIndex:1];
            [enc setTexture:g.tmpA atIndex:2];
            [enc setTexture:g.tmpB atIndex:3];
            [enc setTexture:dstT atIndex:4];
            dispatch(enc, "fluidCorrect", g);
        }
        // Combustion & sources: scal[1] -> scal[0]
        [enc setTexture:g.scal[1] atIndex:0];
        [enc setTexture:g.scal[0] atIndex:1];
        dispatch(enc, "fluidCombust", g);
        // Vorticity, forces: vel[1] -> vel[0]
        [enc setTexture:g.vel[1] atIndex:0];
        [enc setTexture:g.curl atIndex:1];
        dispatch(enc, "fluidCurl", g);
        [enc setTexture:g.vel[1] atIndex:0];
        [enc setTexture:g.curl atIndex:1];
        [enc setTexture:g.scal[0] atIndex:2];
        [enc setTexture:g.vel[0] atIndex:3];
        dispatch(enc, "fluidForces", g);
        // Pressure projection: vel[0] -> vel[1] -> swap
        [enc setTexture:g.vel[0] atIndex:0];
        [enc setTexture:g.div atIndex:1];
        dispatch(enc, "fluidDivergence", g);
        for (int it = 0; it < 32; ++it) {
            [enc setTexture:g.pressure[it & 1] atIndex:0];
            [enc setTexture:g.div atIndex:1];
            [enc setTexture:g.pressure[(it + 1) & 1] atIndex:2];
            dispatch(enc, "fluidJacobi", g);
        }
        [enc setTexture:g.vel[0] atIndex:0];
        [enc setTexture:g.pressure[0] atIndex:1];
        [enc setTexture:g.vel[1] atIndex:2];
        dispatch(enc, "fluidProject", g);
        [enc endEncoding];
        std::swap(g.vel[0], g.vel[1]);  // projected velocity is now vel[0]; scalars are in scal[0]
        g.age += dt;
    }

    void simulateFluids(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        std::vector<EntityId> live;
        for (const VolumeItem& v : frame.volumes) {
            live.push_back(v.entity);
            if (fluidKernels_.empty()) continue;
            FluidGpu& g = fluidFor(cmd, v);
            double now = frame.time;
            if (g.lastTime < 0 || now < g.lastTime - 1e-4 || now - g.lastTime > 2.0) {
                // New or time jumped (play/stop): restart and pre-simulate so the fire is already burning.
                for (int i = 0; i < 2; ++i) {
                    clearVolume(cmd, g.vel[i]);
                    clearVolume(cmd, g.scal[i]);
                    clearVolume(cmd, g.pressure[i]);
                }
                g.age = 0;
                int warm = v.params.burst > 0.f ? 0 : 75;
                for (int i = 0; i < warm; ++i) fluidStep(cmd, g, v, 1.f / 30.f, static_cast<float>(now) - (warm - i) / 30.f);
                g.lastTime = now;
                continue;
            }
            double dt = now - g.lastTime;
            if (dt <= 1e-5) continue;
            g.lastTime = now;
            int steps = std::clamp(static_cast<int>(std::ceil(dt * 30.0)), 1, 4);
            float h = static_cast<float>(std::min(dt, 4.0 / 30.0) / steps);
            for (int i = 0; i < steps; ++i) fluidStep(cmd, g, v, h, static_cast<float>(now - dt + h * (i + 1)));
        }
        std::erase_if(fluids_, [&](const auto& kv) { return std::find(live.begin(), live.end(), kv.first) == live.end(); });
    }

    void encodeVolumes(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu, const std::vector<GPULight>& lights) {
        const GpuMesh* cube = mesh("cube");
        if (!cube) return;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = lit_;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Volumes", "effects");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Volumes";
        [enc setRenderPipelineState:volumePipeline_];
        [enc setCullMode:MTLCullModeFront];  // back faces: works with the camera inside the box too
        [enc setFrontFacingWinding:MTLWindingCounterClockwise];
        [enc setVertexBuffer:cube->vertices offset:0 atIndex:0];
        [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
        [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight) atIndex:2];
        [enc setFragmentTexture:envCube_ atIndex:5];
        [enc setFragmentTexture:depthCopy_ atIndex:7];
        // Far volumes first.
        std::vector<const VolumeItem*> order;
        for (const auto& v : frame.volumes) order.push_back(&v);
        Vec3 eye = frame.camera.eye;
        std::sort(order.begin(), order.end(), [&](const VolumeItem* a, const VolumeItem* b) {
            return distance(eye, a->model.translation()) > distance(eye, b->model.translation());
        });
        for (const VolumeItem* v : order) {
            auto it = fluids_.find(v->entity);
            if (it == fluids_.end()) continue;
            const FluidVolume& p = v->params;
            VolumeUniforms vu{};
            vu.model = toSimd(v->model);
            vu.invModel = toSimd(v->model.inverse());
            vu.size = simd_make_float4(p.size.x, p.size.y, p.size.z, 0.f);
            float steps = std::clamp(static_cast<float>(std::max({it->second.nx, it->second.ny, it->second.nz})) * 1.25f, 48.f, 200.f);
            vu.flame = simd_make_float4(p.flameIntensity, p.flameTemperature, p.smokeDensity, steps);
            vu.smokeColor = lin(p.smokeColor);
            simd_float4 lc = lin(p.lightColor);
            vu.glow = simd_make_float4(lc.x * p.light, lc.y * p.light, lc.z * p.light, static_cast<float>(frameIndex_ % 64));
            [enc setVertexBytes:&vu length:sizeof(vu) atIndex:1];
            [enc setFragmentBytes:&vu length:sizeof(vu) atIndex:0];
            [enc setFragmentTexture:it->second.scal[0] atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:cube->indexCount
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:cube->indices
                     indexBufferOffset:0];
        }
        [enc endEncoding];
    }

    void encodeEffects(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu,
                       const std::vector<GPULight>& lights, bool simulate) {
        bool anyWater = false;
        for (const auto& w : frame.water) anyWater = anyWater || (w.ocean && w.ocean->resolution > 0);
        if (!anyWater && frame.particles.empty() && frame.volumes.empty() && !fx_->hasTransparent(frame)) {  // [hair+vfx]
            oceans_.clear();
            fluids_.clear();
            return;
        }
        if (simulate) simulateFluids(cmd, frame);
        // Copies of the opaque scene: water refracts/reflects them; particles fade against depth.
        id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Scene copy", "effects");
        if (anyWater) [blit copyFromTexture:lit_ toTexture:sceneCopy_];
        [blit copyFromTexture:depthResolved_ toTexture:depthCopy_];
        [blit endEncoding];

        if (anyWater) {
            ensureGrids();
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = lit_;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = depthResolved_;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            profileRenderPass(rp, "Water", "effects");
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Water";
            [enc setRenderPipelineState:waterPipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setCullMode:MTLCullModeNone];
            [enc setFrontFacingWinding:MTLWindingCounterClockwise];
            [enc setVertexBytes:&fu length:sizeof(fu) atIndex:2];
            [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight) atIndex:2];
            [enc setFragmentTexture:shadowMap_ atIndex:1];
            [enc setFragmentTexture:envCube_ atIndex:5];
            [enc setFragmentTexture:sceneCopy_ atIndex:6];
            [enc setFragmentTexture:depthCopy_ atIndex:7];
            [enc setFragmentTexture:(hdri_ ?: white_) atIndex:14];
            [enc setFragmentTexture:shadows_->atlas() atIndex:32];  // [local shadows]
            bindProbes(enc);  // [reflection probes] off-screen reflections on water
            std::vector<EntityId> live;
            for (const WaterItem& w : frame.water) {
                if (!w.ocean || w.ocean->resolution == 0) continue;
                live.push_back(w.entity);
                OceanGpu& g = oceanTextures(w);
                WaterUniforms wu{};
                bool endless = w.size <= 0.f;
                wu.levelSize = simd_make_float4(w.level, w.size, w.center.x, w.center.y);
                wu.deep = lin(w.deepColor);
                wu.shallow = lin(w.shallowColor);
                wu.params = simd_make_float4(w.clarity, w.foam, w.reflections, w.refraction);
                wu.params2 = simd_make_float4(w.roughness, 1.f / static_cast<float>(w.ocean->resolution), endless ? 1.f : 0.f, 0.f);
                wu.patch = simd_make_float4(w.ocean->patchSize[0], w.ocean->patchSize[1], w.ocean->patchSize[2], 0.f);
                const float snap = 0.6f;  // whole grid cells, so the mesh does not swim
                wu.origin = simd_make_float4(std::round(frame.camera.eye.x / snap) * snap, 0.f,
                                             std::round(frame.camera.eye.z / snap) * snap, 0.f);
                // The seabed: the terrain under the camera (or nearest it) damps waves in the shallows.
                id<MTLTexture> seabed = white_;
                if (const TerrainItem* t = seabedTerrain(frame, w)) {
                    TerrainGpu& tg = terrainGpu(*t);
                    seabed = tg.seabed ? tg.seabed : tg.height;
                    wu.shore = simd_make_float4(t->origin.x, t->origin.y, t->origin.z, t->data->size());
                    wu.shore2 = simd_make_float4(static_cast<float>(t->data->resolution()), 1.f, 3.f, 0.f);
                }
                [enc setVertexTexture:seabed atIndex:3];
                const GridMesh& grid = endless ? endlessGrid_ : unitGrid_;
                [enc setVertexBuffer:grid.vertices offset:0 atIndex:0];
                [enc setVertexBytes:&wu length:sizeof(wu) atIndex:1];
                [enc setFragmentBytes:&wu length:sizeof(wu) atIndex:0];
                for (int c = 0; c < OceanCascades::kCascades; ++c) {
                    [enc setVertexTexture:g.disp[c] atIndex:static_cast<NSUInteger>(c)];
                    [enc setFragmentTexture:g.slope[c] atIndex:static_cast<NSUInteger>(8 + c)];
                    [enc setFragmentTexture:g.disp[c] atIndex:static_cast<NSUInteger>(11 + c)];
                }
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:grid.indexCount
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:grid.indices
                         indexBufferOffset:0];
            }
            [enc endEncoding];
            std::erase_if(oceans_, [&](const auto& kv) { return std::find(live.begin(), live.end(), kv.first) == live.end(); });
            // Particles must fade against the water surface too.
            blit = profiledBlit(cmd, "Depth copy", "effects");
            [blit copyFromTexture:depthResolved_ toTexture:depthCopy_];
            [blit endEncoding];
        }

        if (!frame.volumes.empty() && volumePipeline_) encodeVolumes(cmd, frame, fu, lights);

        if (!frame.particles.empty()) {
            id<MTLBuffer> instances = [device_ newBufferWithBytes:frame.particles.data()
                                                           length:frame.particles.size() * sizeof(ParticleInstance)
                                                          options:MTLResourceStorageModeShared];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = lit_;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            profileRenderPass(rp, "Particles", "particles");
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Particles";
            [enc setRenderPipelineState:particlePipeline_];
            [enc setCullMode:MTLCullModeNone];
            [enc setVertexBuffer:instances offset:0 atIndex:0];
            [enc setVertexBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:1];
            [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight) atIndex:2];
            [enc setFragmentTexture:shadowMap_ atIndex:1];
            [enc setFragmentTexture:envCube_ atIndex:5];
            [enc setFragmentTexture:depthCopy_ atIndex:7];
            [enc setFragmentTexture:shadows_->atlas() atIndex:32];  // [local shadows]
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:6
                  instanceCount:frame.particles.size()];
            [enc endEncoding];
        }
        FxSceneInputs fxIn{shadowMap_, envCube_, depthCopy_, lit_, &fu, sizeof(fu), lights.data(), lights.size() * sizeof(GPULight)};
        fxIn.localShadows = shadows_->atlas();  // [local shadows]
        fx_->encodeTransparent(cmd, frame, fxIn);  // [hair+vfx] GPU particles
    }

    void encodeVolumetrics(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu,
                           const std::vector<GPULight>& lights, uint64_t seed) {
        volumetricActive_ = frame.environment.godRays > 0.001f && frame.environment.haze > 0.f;
        if (!volumetricActive_) return;
        VolumetricUniforms vu{};
        vu.params = simd_make_float4(frame.environment.godRays, frame.environment.haze,
                                     std::min(frame.camera.farPlane, 300.f), static_cast<float>(seed % 64));
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = volumetric_;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Volumetric light", "volumetrics");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Volumetric light";
        [enc setRenderPipelineState:volumetricPipeline_];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [enc setFragmentBytes:&vu length:sizeof(vu) atIndex:1];
        [enc setFragmentBytes:lights.data() length:lights.size() * sizeof(GPULight) atIndex:2];
        [enc setFragmentTexture:depthResolved_ atIndex:0];
        [enc setFragmentTexture:shadowMap_ atIndex:1];
        [enc setFragmentTexture:shadows_->atlas() atIndex:32];  // [local shadows] shadowed lamp cones
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    // --- Screen-space GI and reflections, lighting resolve, temporal resolve -----------------
    void fullscreenFU(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> pso, id<MTLTexture> target,
                      std::initializer_list<id<MTLTexture>> inputs, const FrameUniforms& fu, const void* uniforms,
                      size_t size, NSString* label) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, label.UTF8String, passGroup(label));
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = label;
        [enc setRenderPipelineState:pso];
        NSUInteger i = 0;
        for (id<MTLTexture> t : inputs) [enc setFragmentTexture:t atIndex:i++];
        [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];
        [enc setFragmentBytes:uniforms length:size atIndex:1];
        bindProbes(enc);  // [reflection probes] SSGI misses and the lighting resolve
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    void ensureCloudNoise(id<MTLCommandBuffer> cmd) {
        if (cloudNoiseReady_) return;
        id<MTLComputeCommandEncoder> enc = profiledCompute(cmd, "Cloud noise", "clouds");
        for (auto [k, t] : {std::pair{cloudShapeKernel_, cloudShape_}, std::pair{cloudDetailKernel_, cloudDetail_}}) {
            [enc setComputePipelineState:k];
            [enc setTexture:t atIndex:0];
            MTLSize grid = MTLSizeMake(t.width, t.height, t.depth), group = MTLSizeMake(4, 4, 4);
            [enc dispatchThreads:grid threadsPerThreadgroup:group];
        }
        [enc endEncoding];
        cloudNoiseReady_ = true;
    }

    void encodeClouds(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu, bool reproject,
                      bool accumulate, uint64_t seed) {
        const Environment& env = frame.environment;
        cloudsActive_ = env.clouds > 0.001f && env.cloudMode != "flat" && env.skyMode != "hdri";
        cloudOut_ = cloudRaw_;
        if (!cloudsActive_) return;
        simd_float4 sd = simd_make_float4(static_cast<float>(seed % 1024), accumulate ? 64.f : 48.f, 0, 0);
        fullscreenFU(cmd, cloudsPipeline_, cloudRaw_, {cloudShape_, cloudDetail_}, fu, &sd, sizeof(sd), @"Clouds");
        if (!accumulate && reproject) {
            simd_float4 p = simd_make_float4(0.88f, 0, 1.f / cloudRaw_.width, 1.f / cloudRaw_.height);
            id<MTLTexture> dst = cloudHist_[cloudCurrent_ ^ 1];
            fullscreenFU(cmd, cloudTemporalPipeline_, dst, {cloudRaw_, cloudHist_[cloudCurrent_]}, fu, &p, sizeof(p), @"Clouds temporal");
            cloudCurrent_ ^= 1;
            cloudOut_ = dst;
        }
    }

    void encodeScreenSpace(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu, bool reproject,
                           bool accumulate, uint64_t seed) {
        const Environment& env = frame.environment;
        giActive_ = env.gi > 0.001f;
        ssrActive_ = env.ssr > 0.001f;
        giOut_ = giRaw_;
        ssrOut_ = ssrRaw_;
        if (!giActive_ && !ssrActive_) return;
        // Radiance for hits: last anti-aliased frame (keeps light bouncing) or this frame's color.
        id<MTLTexture> radiance = reproject ? taa_[taaCurrent_] : hdr_;
        const bool temporal = !accumulate && reproject;
        SSUniforms u{};
        u.params = simd_make_float4(env.giDistance, 0.35f, accumulate ? 4.f : 3.f, 12.f);
        u.params2 = simd_make_float4(reproject ? 1.f : 0.f, temporal ? 0.9f : 0.f, 0.65f, static_cast<float>(seed % 1024));
        u.texel = simd_make_float4(1.f / hdr_.width, 1.f / hdr_.height, 1.f / giRaw_.width, 1.f / giRaw_.height);
        if (giActive_) {
            fullscreenFU(cmd, probes_->active() ? ssgiProbesPipeline_ : ssgiPipeline_, giRaw_, {depthResolved_, gbufB_, radiance, envCube_}, fu,
                         &u, sizeof(u), @"SSGI");
            if (temporal) {
                id<MTLTexture> dst = giHist_[giCurrent_ ^ 1];
                fullscreenFU(cmd, ssTemporalPipeline_, dst, {giRaw_, giHist_[giCurrent_], depthResolved_, depthPrev_}, fu, &u,
                             sizeof(u), @"SSGI temporal");
                giCurrent_ ^= 1;
                giOut_ = dst;
            }
        }
        if (ssrActive_) {
            fullscreenFU(cmd, ssrPipeline_, ssrRaw_, {depthResolved_, gbufB_, radiance}, fu, &u, sizeof(u), @"SSR");
            if (temporal) {
                id<MTLTexture> dst = ssrHist_[ssrCurrent_ ^ 1];
                SSUniforms ut = u;
                ut.params2.y = 0.8f;
                fullscreenFU(cmd, ssTemporalPipeline_, dst, {ssrRaw_, ssrHist_[ssrCurrent_], depthResolved_, depthPrev_}, fu, &ut,
                             sizeof(ut), @"SSR temporal");
                ssrCurrent_ ^= 1;
                ssrOut_ = dst;
            }
        }
    }

    void encodeResolve(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu) {
        const Environment& env = frame.environment;
        ResolveUniforms r{};
        r.params = simd_make_float4(std::min(env.gi, 1.f), env.ssr, env.ao, giActive_ ? 1.f : 0.f);
        r.params2 = simd_make_float4(ssrActive_ ? 1.f : 0.f, aoActive_ ? 1.f : 0.f, 0, 0);
        r.texel = simd_make_float4(1.f / hdr_.width, 1.f / hdr_.height, 1.f / giRaw_.width, 1.f / giRaw_.height);
        fullscreenFU(cmd, probes_->active() ? resolveProbesPipeline_ : resolvePipeline_, lit_,
                     {hdr_, gbufA_, gbufB_, depthResolved_, aoBlurred_, giOut_, ssrOut_, envCube_, brdfLut_}, fu, &r,
                     sizeof(r), @"Lighting resolve");
    }

    void encodeTemporal(id<MTLCommandBuffer> cmd, const FrameData& frame, const FrameUniforms& fu, int mode, float weight) {
        TemporalUniforms t{};
        t.params = simd_make_float4(static_cast<float>(mode), weight, 0.9f, volumetricActive_ ? 1.f : 0.f);
        t.texel = simd_make_float4(1.f / hdr_.width, 1.f / hdr_.height, 1.f / volumetric_.width, 1.f / volumetric_.height);
        t.clouds = simd_make_float4(cloudsActive_ ? 1.f : 0.f, 0, 0, 0);  // clouds seen in front of geometry from above
        id<MTLTexture> dst = taa_[taaCurrent_ ^ 1];
        fullscreenFU(cmd, temporalPipeline_, dst,
                     {lit_, volumetric_, taa_[taaCurrent_], depthResolved_, fx_->reactiveMask(), cloudsActive_ ? cloudOut_ : clearCloud_,
                      velocity_},
                     fu, &t, sizeof(t),  // [hair+vfx] reactive
                     mode == 1 ? @"TAA" : (mode == 2 ? @"Accumulate" : @"Scene resolve"));
        taaCurrent_ ^= 1;
        (void)frame;
    }

    /// The velocity buffer: camera reprojection of the depth buffer + the object motion the main
    /// pass wrote (resolved MSAA attachment 3). `fu.prevViewProj` decides what "previous" means.
    void encodeVelocity(id<MTLCommandBuffer> cmd, const FrameUniforms& fu) {
        fullscreenFU(cmd, motionPipeline_, velocity_, {depthResolved_, objectMotion_}, fu, &fu.temporal, sizeof(fu.temporal),
                     @"Velocity");
        velocityComposed_ = true;
    }

    /// MetalFX temporal upscaling from the velocity buffer (camera + object motion), with the
    /// engine's exposure and the GPU-particle reactive mask.
    void encodeUpscale(id<MTLCommandBuffer> cmd, const FrameData& frame, Vec2 jitterPx) {
        const Environment& env = frame.environment;
        simd_float4 ep = simd_make_float4(env.exposure * std::exp2(env.exposureCompensation),
                                          env.autoExposure && exposureValid_ ? 1.f : 0.f, 0, 0);
        fullscreen(cmd, fxExposurePipeline_, fxExposure_, {exposure_[exposureCurrent_]}, &ep, sizeof(ep), false, @"MetalFX exposure");
        if (!reactiveNoneCleared_) {  // a cleared (all "not reactive") mask for frames without GPU particles
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = reactiveNone_;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Clear reactive mask";
            [enc endEncoding];
            reactiveNoneCleared_ = true;
        }
        id<MTLTexture> reactive = fx_->reactiveMask();
        if (!reactive || reactive.width != hdr_.width || reactive.height != hdr_.height) reactive = reactiveNone_;
        id<MTLFXTemporalScaler> sc = temporalScaler();
        sc.colorTexture = taa_[taaCurrent_];
        sc.depthTexture = depthResolved_;
        sc.motionTexture = velocity_;
        sc.outputTexture = upscaled_;
        sc.exposureTexture = fxExposure_;
        sc.reactiveMaskTexture = reactive;
        sc.jitterOffsetX = -jitterPx.x;
        sc.jitterOffsetY = jitterPx.y;
        sc.motionVectorScaleX = static_cast<float>(velocity_.width);
        sc.motionVectorScaleY = static_cast<float>(velocity_.height);
        sc.reset = !historyValid_;
        sc.depthReversed = NO;
        [sc encodeToCommandBuffer:cmd];
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { (void)sc; }];  // keep it alive until the GPU is done
        std::erase_if(retiredScalers_, [&](const RetiredScaler& r) { return frameIndex_ - r.frame > 30; });
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
        profileRenderPass(rp, "Overlays", "overlays");
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
            du.prevModel = du.model;
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBuffer:m->vertices offset:0 atIndex:3];  // meshVertex: previous pose (static)
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

    // [2D physics] Frame2D::debugLines as one line list: world-space ends through the frame's (unjittered)
    // view-projection, alpha-blended into the final image, no depth test. Bounded: at most kMaxDebugLines.
    static constexpr size_t kMaxDebugLines = 32768;
    void encodeDebugLines2D(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        const auto& lines = frame.render2d.debugLines;
        if (lines.empty() || !debugLinePipeline_) return;
        const size_t n = std::min(lines.size(), kMaxDebugLines);
        std::vector<simd_float4> verts;
        verts.reserve(n * 4);
        for (size_t i = 0; i < n; ++i) {
            const DebugLine2D& l = lines[i];
            const simd_float4 c = lin(Vec4{l.color.x, l.color.y, l.color.z, std::clamp(l.color.w, 0.f, 1.f)});
            verts.push_back(simd_make_float4(l.a.x, l.a.y, l.a.z, 1.f));
            verts.push_back(c);
            verts.push_back(simd_make_float4(l.b.x, l.b.y, l.b.z, 1.f));
            verts.push_back(c);
        }
        const Alloc buf = transient(verts.data(), verts.size() * sizeof(simd_float4));
        const simd_float4x4 vp = toSimd(frame.viewProjection());
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = resolve_;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "2D debug lines", "post");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"2D debug lines";
        [enc setRenderPipelineState:debugLinePipeline_];
        [enc setVertexBuffer:buf.buffer offset:buf.offset atIndex:0];
        [enc setVertexBytes:&vp length:sizeof(vp) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:n * 2];
        [enc endEncoding];
    }

    // [scene transitions] FrameData::fade from the scene flow. A crossfade starts from the last frame shown
    // (resolve_ is what present() and readback() hand out): it is copied once when crossfade first goes
    // above 0 and released when it is back to 0. A fade blends toward the color. Both are one fullscreen
    // triangle each, only while a transition runs.
    /// Before ensureTargets: holds on to the last frame shown when a crossfade starts (a resize reallocates
    /// resolve_; the reference keeps the old picture alive until it is copied).
    void captureCrossfade(const FrameData& frame) {
        if (frame.fade.crossfade <= 0.f) {
            crossfading_ = false;
            crossfadeFrom_ = nil;
            crossfadeSource_ = nil;
            return;
        }
        if (crossfading_) return;
        crossfading_ = true;
        crossfadeSource_ = resolve_;  // nil before the first frame: the new scene cuts in
    }

    /// At the start of the frame's command buffer, before anything writes resolve_.
    void encodeCrossfadeCapture(id<MTLCommandBuffer> cmd) {
        if (!crossfadeSource_) return;
        crossfadeFrom_ = target2D(kColorFormat, crossfadeSource_.width, crossfadeSource_.height, MTLTextureUsageShaderRead);
        id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Crossfade capture", "post");
        [blit copyFromTexture:crossfadeSource_ toTexture:crossfadeFrom_];
        [blit endEncoding];
        crossfadeSource_ = nil;
    }

    void encodeScreenTransition(id<MTLCommandBuffer> cmd, const FrameData& frame) {
        const float cross = std::clamp(frame.fade.crossfade, 0.f, 1.f);
        if (cross > 0.f && crossfadeFrom_ && crossfadeFrom_.width == resolve_.width && crossfadeFrom_.height == resolve_.height) {
            const simd_float4 u = simd_make_float4(0.f, 0.f, 0.f, cross);
            fullscreen(cmd, crossfadePipeline_, resolve_, {crossfadeFrom_}, &u, sizeof(u), true, @"Crossfade");
        }
        const float alpha = std::clamp(frame.fade.alpha, 0.f, 1.f);
        if (alpha > 0.f) {
            simd_float4 u = lin(frame.fade.color);
            u.w = alpha;
            fullscreen(cmd, screenFadePipeline_, resolve_, {}, &u, sizeof(u), true, @"Screen fade");
        }
    }

    void fullscreen(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> pso, id<MTLTexture> target,
                    std::initializer_list<id<MTLTexture>> inputs, const void* uniforms, size_t size, bool load, NSString* label) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, label.UTF8String, passGroup(label));
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

    /// Look / .cube LUT as a 3D texture (rebuilt when the grading settings change).
    bool ensureLut(const Environment& env) {
        std::string key = env.look + "|" + env.lut;
        if (!env.lut.empty()) {
            std::error_code ec;
            auto t = std::filesystem::last_write_time(env.lut, ec);
            key += ec ? "" : std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        }
        if (key == lutKey_) return lut_ != nil;
        lutKey_ = key;
        lut_ = nil;
        Result<grading::Lut3D> lut = !env.lut.empty() ? grading::loadCube(env.lut)
                                     : (env.look.empty() || env.look == "none") ? Result<grading::Lut3D>(Error::make("none", ""))
                                                                                : grading::lookLut(env.look);
        if (!lut) {
            if (!env.lut.empty()) log::warn("render", "LUT '" + env.lut + "': " + lut.error().message);
            return false;
        }
        const int n = lut->size;
        std::vector<__fp16> px(static_cast<size_t>(n) * n * n * 4);
        for (size_t i = 0, cnt = static_cast<size_t>(n) * n * n; i < cnt; ++i) {
            for (int c = 0; c < 3; ++c) px[i * 4 + c] = static_cast<__fp16>(lut->rgb[i * 3 + c]);
            px[i * 4 + 3] = static_cast<__fp16>(1.f);
        }
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRGBA16Float;
        d.width = d.height = d.depth = static_cast<NSUInteger>(n);
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        lut_ = [device_ newTextureWithDescriptor:d];
        [lut_ replaceRegion:MTLRegionMake3D(0, 0, 0, n, n, n) mipmapLevel:0 slice:0 withBytes:px.data()
                bytesPerRow:static_cast<NSUInteger>(n) * 8 bytesPerImage:static_cast<NSUInteger>(n) * n * 8];
        return true;
    }

    void encodePost(id<MTLCommandBuffer> cmd, const FrameData& frame, bool accumulated, const FrameUniforms& base) {
        const Environment& env = frame.environment;
        id<MTLTexture> src = postSource_ ?: taa_[taaCurrent_];
        const ViewCamera& cam = frame.camera;
        LensUniformsGpu lu{};
        float fl = 12.f / std::tan(radians(std::clamp(cam.fovDeg, 5.f, 170.f)) * 0.5f);  // focal length (mm), 24 mm sensor
        lu.lens = simd_make_float4(cam.aperture, cam.focusDistance, fl, 24.f);
        lu.motion = simd_make_float4(cam.motionBlur, std::clamp(cam.tiltShift, 0.f, 1.f), 0, 0);
        lu.texel = simd_make_float4(1.f / src.width, 1.f / src.height, 1.f / dofCoc_.width, 1.f / dofCoc_.height);
        lu.view = simd_make_float4(static_cast<float>(src.height), 0, 0, 0);

        // Motion blur: camera and object motion since the previous rendered frame (velocity buffer),
        // reconstructed with tile-max / neighbor-max velocities (McGuire 2012).
        if (cam.motionBlur > 0.001f && motionValid_ && !cam.orthographic && velocityComposed_) {
            MotionBlurUniformsGpu mb{};
            const float tileTexels = static_cast<float>(motionTilePx_) * static_cast<float>(velocity_.width) / static_cast<float>(src.width);
            mb.params = simd_make_float4(cam.motionBlur, tileTexels, 2.f * static_cast<float>(motionTilePx_), accumulated ? 24.f : 15.f);
            mb.size = simd_make_float4(static_cast<float>(velocity_.width), static_cast<float>(velocity_.height), static_cast<float>(src.width),
                                       static_cast<float>(src.height));
            fullscreen(cmd, motionTileMaxPipeline_, motionTiles_, {velocity_}, &mb, sizeof(mb), false, @"Motion blur tile max");
            fullscreen(cmd, motionNeighborMaxPipeline_, motionNeighbors_, {motionTiles_}, &mb, sizeof(mb), false, @"Motion blur neighbor max");
            fullscreenFU(cmd, motionBlurPipeline_, postA_, {src, depthResolved_, velocity_, motionNeighbors_}, base, &mb, sizeof(mb), @"Motion blur");
            src = postA_;
        }
        // Depth of field.
        if ((cam.aperture > 0.01f || cam.tiltShift > 0.001f) && !cam.orthographic) {
            fullscreenFU(cmd, dofCocPipeline_, dofCoc_, {src, depthResolved_}, base, &lu, sizeof(lu), @"DOF CoC");
            fullscreenFU(cmd, dofBlurPipeline_, dofBlur_, {dofCoc_}, base, &lu, sizeof(lu), @"DOF gather");
            id<MTLTexture> dst = src == postA_ ? postB_ : postA_;
            fullscreenFU(cmd, dofCombinePipeline_, dst, {src, dofBlur_, depthResolved_}, base, &lu, sizeof(lu), @"DOF combine");
            src = dst;
        }
        // Auto exposure: metering -> mip average -> adaptation.
        const bool autoExp = env.autoExposure;
        if (autoExp) {
            fullscreen(cmd, lumaPipeline_, lum_, {src}, &lu, sizeof(lu), false, @"Exposure metering");
            id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Exposure mips", "post");
            [blit generateMipmapsForTexture:lum_];
            [blit endEncoding];
            // Stills converge instantly; real time and offline movie frames adapt over time (offline:
            // by the movie's frame time, re-metering at cuts) so exposure never pumps frame to frame.
            const bool offline = frame.offline.enabled;
            float dt = offline ? std::max(frame.offline.exposureDt, 0.f) : 1.f / 60.f;
            const bool converge = !exposureValid_ || (offline ? frame.resetHistory : accumulated);
            simd_float4 ep = simd_make_float4(converge ? -1.f : dt * env.adaptationSpeed * 3.f,
                                              0.f, -10.f, 10.f);
            id<MTLTexture> dst = exposure_[exposureCurrent_ ^ 1];
            fullscreen(cmd, exposurePipeline_, dst, {lum_, exposure_[exposureCurrent_]}, &ep, sizeof(ep), false, @"Exposure adapt");
            exposureCurrent_ ^= 1;
            exposureValid_ = true;
        }
        id<MTLTexture> exposureTex = exposure_[exposureCurrent_];

        PostUniforms pu{};
        pu.params = simd_make_float4(env.exposure * std::exp2(env.exposureCompensation), env.bloomIntensity, env.bloomThreshold,
                                     env.saturation);
        pu.params2 = simd_make_float4(env.contrast, env.vignette,
                                      static_cast<float>(frame.width) / static_cast<float>(std::max(frame.height, 1)),
                                      tonemapIndex(env.tonemap));
        const float sharpen = (env.taa || accumulated) ? env.sharpen * (accumulated ? 0.5f : 1.f) : 0.f;
        pu.grade = simd_make_float4(env.temperature, env.tint, sharpen, autoExp ? 1.f : 0.f);
        const size_t levels = bloomViews_.size();
        if (env.bloomIntensity > 0.001f && levels > 0) {
            pu.texel = simd_make_float4(1.f / src.width, 1.f / src.height, env.bloomClamp, 0);  // z = bloom clamp
            fullscreen(cmd, bloomPrefilterPipeline_, bloomViews_[0], {src, exposureTex}, &pu, sizeof(pu), false, @"Bloom prefilter");
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
        pu.texel = simd_make_float4(1.f / src.width, 1.f / src.height, 0.f, static_cast<float>(frameIndex_ % 64));
        GradeUniformsGpu gu{};
        bool hasLut = ensureLut(env);
        gu.params = simd_make_float4(autoExp ? 1.f : 0.f, hasLut ? env.lookStrength : 0.f, env.grain, env.chromaticAberration);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = resolve_;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Composite", "post");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Composite";
        [enc setRenderPipelineState:compositePipeline_];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentTexture:(levels ? bloomViews_[0] : src) atIndex:1];
        [enc setFragmentTexture:exposureTex atIndex:2];
        [enc setFragmentTexture:(hasLut ? lut_ : cloudDetail_) atIndex:3];  // any 3D texture when unused
        [enc setFragmentBytes:&pu length:sizeof(pu) atIndex:0];
        [enc setFragmentBytes:&gu length:sizeof(gu) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    MTKTextureLoader* textureLoader_;
    id<MTLRenderPipelineState> skyPipeline_, meshPipeline_, meshBlendPipeline_, gridPipeline_, shadowPipeline_,
        presentPipeline_, outlinePipeline_, overlayPipeline_, bloomPrefilterPipeline_, bloomDownPipeline_,
        bloomUpPipeline_, compositePipeline_, envSkyPipeline_, envPrefilterPipeline_, brdfPipeline_, ssaoPipeline_,
        aoBlurPipeline_, meshCutoutPipeline_, shadowAlphaPipeline_, waterPipeline_, particlePipeline_, volumePipeline_, volumetricPipeline_,
        ssgiPipeline_, ssrPipeline_, ssTemporalPipeline_, resolvePipeline_, temporalPipeline_, debugViewPipeline_,
        terrainPipeline_, terrainShadowPipeline_, cloudsPipeline_, cloudTemporalPipeline_, lumaPipeline_, exposurePipeline_, motionBlurPipeline_, dofCocPipeline_,
        dofBlurPipeline_, dofCombinePipeline_;
    id<MTLTexture> lum_, exposure_[2], postA_, postB_, dofCoc_, dofBlur_, lut_, upscaled_, postSource_;
    // Velocity buffer (per-object motion vectors): main pass color(3) = object motion, composed
    // with camera reprojection into velocity_ for TAA, MetalFX, motion blur and debug views.
    id<MTLTexture> msaaVelocity_, objectMotion_, velocity_, motionTiles_, motionNeighbors_, fxExposure_, reactiveNone_;
    id<MTLRenderPipelineState> motionTileMaxPipeline_, motionNeighborMaxPipeline_, fxExposurePipeline_;
    int motionTilePx_ = 40;
    bool reactiveNoneCleared_ = false, velocityComposed_ = false;
    MotionHistory motionHistory_;          // previous transforms of drawn objects
    std::vector<Mat4> prevModels_;         // per FrameData::draws entry, this frame
    size_t movingDraws_ = 0;
    std::array<size_t, 4> lightStats_{};    // lights this frame: total, layer-masked, negative, inverse square
    float mipBias_ = 0.f;                  // texture LOD bias while MetalFX upscales (log2 renderScale)
    float prevTime_ = 0.f;
    bool prevTimeValid_ = false;
    struct SkinHistory {                   // double-buffered posed vertices of one skinned instance
        id<MTLBuffer> buffer[2];
        int current = 0;
        uint64_t lastFrame = ~0ull;
        bool motion = false;               // the other buffer holds last frame's pose
    };
    std::unordered_map<std::string, SkinHistory> skinHistory_;
    std::shared_ptr<std::atomic<int>> gpuFaults_ = std::make_shared<std::atomic<int>>(0);
    int reportedFaults_ = 0;
    id<MTLFXTemporalScaler> scaler_;
    id<MTLFXSpatialScaler> spatialScaler_;
    struct RetiredScaler {
        id<MTLFXTemporalScaler> scaler;
        uint64_t frame = 0;
    };
    std::vector<RetiredScaler> retiredScalers_;
    id<MTLRenderPipelineState> motionPipeline_;
    std::vector<id<MTLTexture>> lumViews_;
    int exposureCurrent_ = 0;
    bool exposureValid_ = false;
    std::string lutKey_;
    Mat4 motionPrevVP_;
    bool motionValid_ = false;
    id<MTLComputePipelineState> cloudShapeKernel_, cloudDetailKernel_;
    id<MTLTexture> cloudShape_, cloudDetail_, cloudRaw_, cloudHist_[2], cloudOut_, clearCloud_;
    int cloudCurrent_ = 0;
    bool cloudNoiseReady_ = false, cloudsActive_ = false;
    // Transient per-frame data: a ring of shared buffers, one per frame in flight.
    static constexpr int kFramesInFlight = 3;
    static constexpr NSUInteger kRingSize = 8u << 20;
    id<MTLBuffer> ring_[kFramesInFlight];
    NSUInteger ringOffset_ = 0;
    int ringIndex_ = 0;
    dispatch_semaphore_t inFlight_ = dispatch_semaphore_create(kFramesInFlight);
    // Terrain and foliage GPU caches
    std::unordered_map<EntityId, TerrainGpu> terrainsGpu_;
    id<MTLBuffer> patchVertices_, patchIndices_;
    uint32_t patchIndexCount_ = 0;
    Alloc lightsBuf_{}, clusterCellsBuf_{}, clusterIndexBuf_{};
    // G-buffer, lighting and temporal targets
    id<MTLTexture> lit_, gbufA_, gbufB_, msaaGbufA_, msaaGbufB_, depthPrev_;
    id<MTLTexture> taa_[2], giRaw_, giHist_[2], ssrRaw_, ssrHist_[2];
    id<MTLTexture> giOut_, ssrOut_;
    int taaCurrent_ = 0, giCurrent_ = 0, ssrCurrent_ = 0;
    bool historyValid_ = false, giActive_ = false, ssrActive_ = false;
    Mat4 prevViewProj_;
    Vec3 prevEye_{0, 0, 0}, prevTarget_{0, 0, -1};
    id<MTLTexture> volumetric_;
    std::unordered_map<std::string, id<MTLComputePipelineState>> fluidKernels_;
    std::unordered_map<EntityId, FluidGpu> fluids_;
    id<MTLTexture> sceneCopy_, depthCopy_;
    std::unordered_map<EntityId, OceanGpu> oceans_;
    GridMesh endlessGrid_, unitGrid_;
    id<MTLTexture> hdr_, bloom_, depthResolved_, aoRaw_, aoBlurred_;
    std::vector<id<MTLTexture>> bloomViews_;
    id<MTLTexture> skyCube_, envCube_, brdfLut_;
    std::string envKey_;
    id<MTLDepthStencilState> depthWrite_, depthRead_, depthNone_;
    id<MTLTexture> resolve_, msaaColor_, msaaDepth_, shadowMap_, white_;
    // [scene transitions] fade and crossfade over the final image; the frame a crossfade starts from
    id<MTLRenderPipelineState> screenFadePipeline_, crossfadePipeline_;
    id<MTLRenderPipelineState> debugLinePipeline_;  // [2D physics] debug lines
    id<MTLTexture> crossfadeFrom_, crossfadeSource_;
    bool crossfading_ = false;
    bool msaaMemoryless_ = true;  // [characters] false once dense hair needed spillable MSAA targets
    id<MTLCommandBuffer> lastCommand_;
    std::unordered_map<std::string, GpuMesh> meshes_;
    std::unordered_map<std::string, id<MTLTexture>> textures_;
    std::string hdriPath_;
    id<MTLTexture> hdri_;
    std::unordered_set<std::string> warnedMeshes_;
    id<MTLComputePipelineState> skinPipeline_;      // animation: GPU skinning
    std::unordered_map<std::string, uint64_t> skinnedKeys_;  // animation: per-instance skinned meshes -> last frame drawn
    std::string source_;
    size_t culled_ = 0;
    size_t terrainNodesDrawn_ = 0;
    uint64_t trianglesDrawn_ = 0, lastTriangles_ = 0;
    const FrameData* lodFrame_ = nullptr;  // frame being encoded (LOD selection)
    std::shared_ptr<std::atomic<double>> gpuMs_ = std::make_shared<std::atomic<double>>(0.0);
    uint64_t frameIndex_ = 0;
    bool aoActive_ = false;
    bool volumetricActive_ = false;
    std::unique_ptr<MetalRenderer2D> r2d_;  // 2D world quads + UI
    std::unique_ptr<MetalFx> fx_;  // [hair+vfx]
    std::unique_ptr<MetalFoliage> foliage_;  // [foliage]
    // [profiler] per-pass GPU timing; [shader cache] library origin, compile times, pipeline archive
    std::unique_ptr<MetalPassProfiler> profiler_;
    std::shared_ptr<MetalPipelineCache> pipelineCache_;
    std::string shaderLibraryOrigin_ = "source", shaderNote_, builtinSource_;
    double libraryMs_ = 0, pipelinesMs_ = 0, startupMs_ = 0;
    // [debug views]
    id<MTLRenderPipelineState> wireframePipeline_, overdrawPipeline_, terrainWirePipeline_, terrainOverdrawPipeline_;
    std::unique_ptr<MetalShadows> shadows_;  // [local shadows]
    std::unique_ptr<MetalProbes> probes_;    // [reflection probes]
    id<MTLRenderPipelineState> meshProbesPipeline_, ssgiProbesPipeline_, resolveProbesPipeline_;  // [reflection probes]
    Alloc probeBlockBuf_{}, probeMaskBuf_{};  // [reflection probes] this frame's shaded probes and cluster masks
};

}  // namespace

std::unique_ptr<Renderer> createMetalRenderer() {
    auto r = std::make_unique<MetalRenderer>();
    if (!r->init()) return nullptr;
    return r;
}

}  // namespace sky
