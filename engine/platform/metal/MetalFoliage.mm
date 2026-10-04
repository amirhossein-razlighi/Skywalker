// GPU-driven instanced foliage and octahedral impostors (see MetalFoliage.h).

#include "MetalFoliage.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "skywalker/core/Log.h"
#include "MetalProfiler.h"     // [profiler] per-pass GPU timing
#include "MetalShaderCache.h"  // [shader cache] pipelines through the binary archive
#include "skywalker/render/DebugViews.h"
#include "skywalker/render/Impostor.h"

namespace sky {

namespace {

// Mirrors of Terrain.metal (layouts must match field by field).
struct FoliageUniformsGpu {
    simd_float4 wind;    // xy = direction, z = speed, w = bend strength
    simd_float4 params;  // x = cull distance, y = mesh height, z = debug tint, w = time
    simd_float4 fade;    // x = impostor transition distance, y = crossfade width
    simd_float4x4 part;
};

constexpr int kMaxParts = 8;
constexpr int kViews = 5;  // camera + 4 sun cascades
constexpr int kBands = 4;  // mesh distance bands (the 5th bin is the impostor)

struct FoliageCullParamsGpu {
    simd_float4 planes[kViews * 6];
    simd_float4 eye, params, center, bands;
    uint32_t counts[4];
    uint32_t lods[2 * kMaxParts * kBands][2];
};
static_assert(sizeof(FoliageCullParamsGpu) == kViews * 6 * 16 + 5 * 16 + 2 * kMaxParts * kBands * 8);

struct ImpostorUniformsGpu {
    simd_float4 center, grid, surface, light;
};

struct ImpostorBakeUniformsGpu {
    simd_float4 center, right, up, forward;
};

constexpr NSUInteger kCullThreads = 128;
constexpr float kFoliageLodScale = 0.8f / 3.f;  // ~3 px of LOD error instead of GpuMesh::lodFor's 0.8 px

// Indirect argument layout per chunk and view (written by foliageCullKernel): for each part and
// mesh band an MTLDrawIndexedPrimitivesIndirectArguments (5 uints), then one
// MTLDrawPrimitivesIndirectArguments (4 uints) for the impostor cards.
constexpr NSUInteger kIndexedArgsBytes = 5 * sizeof(uint32_t);
constexpr NSUInteger meshArgsOffset(uint32_t part, int band) { return (part * kBands + static_cast<NSUInteger>(band)) * kIndexedArgsBytes; }
constexpr NSUInteger impostorArgsOffset(uint32_t parts) { return parts * kBands * kIndexedArgsBytes; }
constexpr NSUInteger viewArgsBytes(uint32_t parts) { return impostorArgsOffset(parts) + 4 * sizeof(uint32_t); }
static_assert(sizeof(MTLDrawIndexedPrimitivesIndirectArguments) == kIndexedArgsBytes);
static_assert(sizeof(MTLDrawPrimitivesIndirectArguments) == 4 * sizeof(uint32_t));
constexpr uint64_t kInstanceBufferIdleFrames = 180, kImpostorIdleFrames = 900;

simd_float4x4 toSimd(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}

/// Normalized frustum planes of a view-projection matrix (Gribb/Hartmann, depth [0, 1]).
void frustumPlanes(const Mat4& m, simd_float4* out) {
    auto row = [&](int r) { return simd_make_float4(m.at(0, r), m.at(1, r), m.at(2, r), m.at(3, r)); };
    const simd_float4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const simd_float4 planes[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (int i = 0; i < 6; ++i) {
        float len = simd_length(planes[i].xyz);
        out[i] = len > 1e-12f ? planes[i] / len : planes[i];
    }
}

bool outside(const simd_float4* planes, const Aabb& b) {
    for (int i = 0; i < 6; ++i) {
        const simd_float4& p = planes[i];
        Vec3 v{p.x >= 0 ? b.max.x : b.min.x, p.y >= 0 ? b.max.y : b.min.y, p.z >= 0 ? b.max.z : b.min.z};
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0) return true;
    }
    return false;
}

float nearestDistance(Vec3 p, const Aabb& b) {
    Vec3 c{std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y), std::clamp(p.z, b.min.z, b.max.z)};
    return distance(p, c);
}

float farthestDistance(Vec3 p, const Aabb& b) {
    Vec3 c{std::fabs(p.x - b.min.x) > std::fabs(p.x - b.max.x) ? b.min.x : b.max.x,
           std::fabs(p.y - b.min.y) > std::fabs(p.y - b.max.y) ? b.min.y : b.max.y,
           std::fabs(p.z - b.min.z) > std::fabs(p.z - b.max.z) ? b.min.z : b.max.z};
    return distance(p, c);
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

/// A finished atlas on the CPU, ready to upload.
struct ImpostorAtlasData {
    impostor::Atlas atlas;
    Vec3 center;
    float radius = 1.f;
    float roughness = 0.7f;
    bool hemi = true;
};

struct MetalFoliage::Impostor {
    id<MTLTexture> albedo, normal;
    int frames = 0, tile = 0, size = 0;
    Vec3 center;
    float radius = 1.f;
    float roughness = 0.7f;
    bool hemi = true;
    bool failed = false;
    bool fromCache = false;
    double bakeMs = 0.0;
    uint64_t lastUse = 0;
    std::string label, path;
    size_t bytes() const { return static_cast<size_t>(size) * size * 8 * 4 / 3; }
};

struct MetalFoliage::Chunk {
    const InstanceBatch* batch = nullptr;
    id<MTLBuffer> instances;
    NSUInteger listOffset = 0, listStride = 0;  // bytes: view 0 region, size of one view region
    NSUInteger argsOffset = 0, viewArgs = 0;    // bytes
    uint32_t parts = 0, views = 1;  // views processed by the cull pass (1 = camera only)
    uint32_t viewMask = 0;  // bit v: view v may draw (chunk bounds vs frustum)
    float dmin = 0.f, dmax = 0.f;
    float D = 0.f, W = 0.f;
    float cull = 0.f;  // effective cull distance this frame
    float bandFar[3] = {};
    float bandFrac[4] = {};  // share of the footprint in each mesh band (triangle budget estimate)
    uint8_t bandLod[kMaxParts][kBands] = {};  // camera mesh LOD per part and band (lod debug view)
    int firstImpostorCascade = 4;
    Impostor* imp = nullptr;
    FoliageUniformsGpu uniforms{};
    ImpostorUniformsGpu impUniforms{};

    bool bandOverlaps(int b) const {
        float lo = b == 0 ? 0.f : bandFar[b - 1], hi = b < 3 ? bandFar[b] : 1e30f;
        return hi > dmin && lo <= dmax;
    }
};

struct MetalFoliage::Counters {
    std::atomic<uint64_t> meshInstances{0}, impostors{0}, kiloTris{0}, shadowKiloTris{0}, shadowImpostors{0};
    std::atomic<bool> gpuInvalid{false};  // a frame's indirect arguments failed validation
};

MetalFoliage::MetalFoliage(id<MTLDevice> device, id<MTLCommandQueue> queue, MeshLookup meshes, TextureLookup textures,
                           SurfaceUniforms surfaceUniforms)
    : device_(device),
      queue_(queue),
      meshes_(std::move(meshes)),
      textures_(std::move(textures)),
      surfaceUniforms_(std::move(surfaceUniforms)),
      counters_(std::make_shared<Counters>()) {
    if (const char* env = std::getenv("SKY_GPU_CULL"); env && std::string(env) == "0") gpuCull_ = false;
    // Developer switch for profiling: SKY_FOLIAGE_DEBUG=nomesh,noimpostors,noshadows,noimpostorshadows,nomeshshadows.
    if (const char* env = std::getenv("SKY_FOLIAGE_DEBUG")) {
        const std::string d = env;
        debugSkip_ = (d.find("nomesh") != std::string::npos ? 1u : 0u) | (d.find("noimpostors") != std::string::npos ? 2u : 0u) |
                     (d.find("noshadows") != std::string::npos ? 4u : 0u) | (d.find("noimpostorshadows") != std::string::npos ? 8u : 0u) |
                     (d.find("nomeshshadows") != std::string::npos ? 16u : 0u);
    }
    MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    white_ = [device_ newTextureWithDescriptor:wd];
    const uint8_t px[4] = {255, 255, 255, 255};
    [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    bakeDepth_ = [device_ newDepthStencilStateWithDescriptor:ds];
}

MetalFoliage::~MetalFoliage() = default;

Status MetalFoliage::build(id<MTLLibrary> lib, const FoliageFormats& fmt) {
    auto fn = [&](const char* name) { return [lib newFunctionWithName:[NSString stringWithUTF8String:name]]; };
    for (const char* required : {"foliageVertex", "foliageShadowVertex", "foliageShadowAlphaVertex", "foliageCullKernel", "impostorVertex",
                                 "impostorFragment", "impostorShadowVertex", "impostorShadowFragment", "impostorBakeVertex",
                                 "impostorBakeFragment", "meshFragment", "shadowAlphaFragment"}) {
        if (!fn(required)) return Error::make("shader_missing_function", std::string("shader source must define ") + required);
    }
    NSError* e = nil;
    auto make = [&](const char* vs, const char* fs, bool mainPass, bool alphaToCoverage) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn(vs);
        d.fragmentFunction = fs ? fn(fs) : nil;
        d.alphaToCoverageEnabled = alphaToCoverage;
        d.depthAttachmentPixelFormat = fmt.depth;
        if (mainPass) {
            d.rasterSampleCount = fmt.samples;
            d.colorAttachments[0].pixelFormat = fmt.hdr;
            d.colorAttachments[1].pixelFormat = fmt.gbufA;
            d.colorAttachments[2].pixelFormat = fmt.gbufB;
            d.colorAttachments[3].pixelFormat = fmt.velocity;
        }
        return newRenderPipeline(device_, d, &e);
    };
    id<MTLComputePipelineState> cull = newComputePipeline(device_, fn("foliageCullKernel"), &e);
    id<MTLRenderPipelineState> mesh = cull ? make("foliageVertex", "meshFragment", true, false) : nil;
    id<MTLRenderPipelineState> cutout = mesh ? make("foliageVertex", "meshFragment", true, true) : nil;
    id<MTLRenderPipelineState> shadow = cutout ? make("foliageShadowVertex", nullptr, false, false) : nil;
    id<MTLRenderPipelineState> shadowAlpha = shadow ? make("foliageShadowAlphaVertex", "shadowAlphaFragment", false, false) : nil;
    id<MTLRenderPipelineState> imp = shadowAlpha ? make("impostorVertex", "impostorFragment", true, true) : nil;
    id<MTLRenderPipelineState> impShadow = imp ? make("impostorShadowVertex", "impostorShadowFragment", false, false) : nil;
    id<MTLRenderPipelineState> bake = nil;
    if (impShadow) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn("impostorBakeVertex");
        d.fragmentFunction = fn("impostorBakeFragment");
        d.rasterSampleCount = 4;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm_sRGB;
        d.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.colorAttachments[2].pixelFormat = MTLPixelFormatR8Unorm;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        bake = newRenderPipeline(device_, d, &e);
    }
    if (!bake) {
        return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "foliage pipeline creation failed");
    }
    cull_ = cull;
    mesh_ = mesh;
    meshCutout_ = cutout;
    {  // [debug views] overdraw: every fragment adds 1 (additive, G-buffer untouched); optional
        auto overdraw = [&](const char* vs) -> id<MTLRenderPipelineState> {
            id<MTLFunction> f = fn("overdrawFragment");
            if (!f) return nil;
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = fn(vs);
            d.fragmentFunction = f;
            d.depthAttachmentPixelFormat = fmt.depth;
            d.rasterSampleCount = fmt.samples;
            d.colorAttachments[0].pixelFormat = fmt.hdr;
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[0].sourceAlphaBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[1].pixelFormat = fmt.gbufA;
            d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
            d.colorAttachments[2].pixelFormat = fmt.gbufB;
            d.colorAttachments[2].writeMask = MTLColorWriteMaskNone;
            NSError* oe = nil;
            return newRenderPipeline(device_, d, &oe);
        };
        overdrawMesh_ = overdraw("foliageVertex");
        overdrawImpostor_ = overdrawMesh_ ? overdraw("impostorVertex") : nil;
    }
    meshShadow_ = shadow;
    meshShadowAlpha_ = shadowAlpha;
    impostor_ = imp;
    impostorShadow_ = impShadow;
    bake_ = bake;
    return {};
}

// --- Per-frame preparation: impostors, then the cull pass ---------------------------------------

id<MTLBuffer> MetalFoliage::instanceBuffer(const InstanceBatch& b) {
    InstanceGpu& g = instanceBuffers_[b.id];
    g.lastUse = frameIndex_;
    if (!g.buffer) {
        g.buffer = [device_ newBufferWithBytes:b.instances->data()
                                        length:b.instances->size() * sizeof(world::FoliageInstance)
                                       options:MTLResourceStorageModeShared];
    }
    return g.buffer;
}

namespace {

// --- CPU reference of foliageCullKernel (Terrain.metal): the fallback path and its spec ------

uint32_t binsFor(const FoliageCullParamsGpu& p, uint32_t view, float d, Vec3 center, float radius) {
    for (int k = 0; k < 6; ++k) {
        const simd_float4& pl = p.planes[view * 6 + static_cast<uint32_t>(k)];
        if (pl.x * center.x + pl.y * center.y + pl.z * center.z + pl.w < -radius) return 0u;
    }
    const float D = p.eye.w;
    const uint32_t band = d < p.bands.x ? 0u : (d < p.bands.y ? 1u : (d < p.bands.z ? 2u : 3u));
    if (view == 0) {
        if (D <= 0.f) return 1u << band;
        const float t = std::clamp((d - (D - p.params.x)) / std::max(p.params.x, 1e-3f), 0.f, 1.f);
        return (t < 1.f ? (1u << band) : 0u) | (t > 0.f ? 16u : 0u);
    }
    if (d > p.params.w) return 0u;
    if (D > 0.f && (d >= D || static_cast<float>(view - 1) >= p.center.w)) return 16u;
    return 1u << band;
}

/// The CPU path (SKY_GPU_CULL=0, or after a GPU validation failure): same lists and arguments.
void cpuCull(const std::vector<world::FoliageInstance>& instances, const FoliageCullParamsGpu& p, uint8_t* listsOut,
                           uint8_t* argsOut, uint32_t* stats) {
    const uint32_t n = p.counts[0], views = std::min<uint32_t>(p.counts[2], kViews), stride = p.counts[3];
    const uint32_t parts = std::min<uint32_t>(p.counts[1], kMaxParts);
    std::vector<uint32_t> bins(static_cast<size_t>(n) * kViews, 0);
    uint32_t counts[kViews][5] = {};
    const Vec3 eye{p.eye.x, p.eye.y, p.eye.z}, mc{p.center.x, p.center.y, p.center.z};
    for (uint32_t i = 0; i < n; ++i) {
        const world::FoliageInstance& in = instances[i];
        const Vec3 T{in.row0[3], in.row1[3], in.row2[3]};
        const float d = distance(T, eye);
        if (d > p.params.y * (0.72f + 0.28f * in.fade)) continue;
        const Vec3 C{in.row0[0] * mc.x + in.row0[1] * mc.y + in.row0[2] * mc.z + T.x, in.row1[0] * mc.x + in.row1[1] * mc.y + in.row1[2] * mc.z + T.y,
                     in.row2[0] * mc.x + in.row2[1] * mc.y + in.row2[2] * mc.z + T.z};
        const float r = p.params.z * length(Vec3{in.row0[0], in.row1[0], in.row2[0]});
        for (uint32_t v = 0; v < views; ++v) {
            const uint32_t mask = binsFor(p, v, d, C, r);
            bins[static_cast<size_t>(i) * kViews + v] = mask;
            for (uint32_t b = 0; b < 5; ++b) counts[v][b] += (mask >> b) & 1u;
        }
    }
    auto* lists = reinterpret_cast<uint32_t*>(listsOut);
    auto* args = reinterpret_cast<uint32_t*>(argsOut);
    const NSUInteger viewArgs = viewArgsBytes(parts) / sizeof(uint32_t);
    double kiloTris = 0, shadowKiloTris = 0;
    for (uint32_t v = 0; v < views; ++v) {
        uint32_t offsets[5], off = 0;
        for (uint32_t b = 0; b < 5; ++b) offsets[b] = off, off += counts[v][b];
        uint32_t cursor[5] = {};
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t mask = bins[static_cast<size_t>(i) * kViews + v];
            for (uint32_t b = 0; b < 5; ++b) {
                if (!((mask >> b) & 1u)) continue;
                const uint32_t slot = offsets[b] + cursor[b]++;
                if (slot < stride) lists[v * stride + slot] = i;
            }
        }
        const uint32_t table = v == 0 ? 0u : 1u;
        uint32_t* a = args + v * viewArgs;
        for (uint32_t part = 0; part < parts; ++part) {
            for (uint32_t b = 0; b < 4; ++b) {
                const uint32_t* lod = p.lods[table * kMaxParts * kBands + part * kBands + b];
                uint32_t* x = a + (meshArgsOffset(part, static_cast<int>(b)) / sizeof(uint32_t));
                x[0] = lod[1], x[1] = counts[v][b], x[2] = lod[0], x[3] = 0, x[4] = offsets[b];
                (v == 0 ? kiloTris : shadowKiloTris) += static_cast<double>(counts[v][b]) * (lod[1] / 3) / 1024.0;
            }
        }
        uint32_t* x = a + impostorArgsOffset(parts) / sizeof(uint32_t);
        x[0] = 4, x[1] = counts[v][4], x[2] = 0, x[3] = offsets[4];
        if (v == 0) {
            stats[0] += counts[0][0] + counts[0][1] + counts[0][2] + counts[0][3];
            stats[1] += counts[0][4];
        } else {
            stats[4] += counts[v][4];
        }
    }
    stats[2] += static_cast<uint32_t>(kiloTris + 0.5);
    stats[3] += static_cast<uint32_t>(shadowKiloTris + 0.5);
}

}  // namespace

void MetalFoliage::prepare(id<MTLCommandBuffer> cmd, const FrameData& frame, const FoliageView& view, uint64_t frameIndex) {
    frameIndex_ = frameIndex;
    ring_ = static_cast<int>(frameIndex % kRing);
    chunks_.clear();
    frameStats_ = nil;
    frameImpostors_.assign(frame.impostors.size(), nullptr);
    if (counters_->gpuInvalid.exchange(false) && gpuCull_) {
        gpuCull_ = false;
        log::error("render", "GPU foliage culling produced invalid draw arguments; using the CPU path");
    }
    if (frame.instances.empty() || !mesh_) return;

    // Impostors: ready ones are used; missing ones are baked (or loaded from the project cache)
    // within the frame's budget. Until then their layers draw meshes out to the cull distance.
    // Real time: ~100 ms of baking/loading per frame at most (cache loads are a few tens of ms;
    // a bake can take longer and runs alone). Stills do everything now.
    const double budgetMs = frame.samples > 1 ? 1e30 : 100.0;
    const auto workStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < frame.impostors.size(); ++i) {
        Impostor* imp = impostor(frame.impostors[i], msSince(workStart) < budgetMs, false);
        frameImpostors_[i] = imp && !imp->failed ? imp : nullptr;
    }

    simd_float4 planes[kViews * 6];
    frustumPlanes(view.viewProj, planes);
    for (int c = 0; c < 4; ++c) frustumPlanes(view.cascadeViewProj[c], planes + (c + 1) * 6);
    const Vec3 eye = frame.camera.eye;
    const Environment& env = frame.environment;
    const float windAngle = radians(env.windDirection);
    const bool debug = frame.debugView == 10;
    const float viewH = static_cast<float>(std::max(frame.height, 1));
    auto pixelsPerUnit = [&](float dist) {
        if (frame.camera.orthographic) return viewH / (2.f * std::max(frame.camera.orthoSize, 1e-3f));
        return viewH / (2.f * std::tan(radians(frame.camera.fovDeg) * 0.5f) * std::max(dist, 0.05f));
    };

    NSUInteger listBytes = 0, argBytes = 0;
    chunks_.reserve(frame.instances.size());
    for (const InstanceBatch& b : frame.instances) {
        if (!b.instances || b.instances->empty() || !b.parts || b.parts->empty()) continue;
        Chunk c;
        c.batch = &b;
        c.dmin = nearestDistance(eye, b.bounds);
        c.dmax = farthestDistance(eye, b.bounds);
        c.imp = b.impostor >= 0 && static_cast<size_t>(b.impostor) < frameImpostors_.size() ? frameImpostors_[static_cast<size_t>(b.impostor)] : nullptr;
        c.D = c.imp ? b.impostorDistance : 0.f;
        // An impostor still loading: draw only the mesh range meanwhile (a few frames of pop-in),
        // never every instance as a mesh out to the cull distance (that is what impostors avoid).
        c.cull = b.impostor >= 0 && !c.imp && b.impostorDistance > 0.f ? std::min(b.cullDistance, b.impostorDistance) : b.cullDistance;
        if (c.dmin > c.cull) continue;
        c.W = c.D > 0.f ? impostor::crossfadeWidth(c.D) : 0.f;
        const bool shadows = view.shadows && b.castShadows && !(frame.quality >= 2 && c.dmin > 60.f);
        if (!outside(planes, b.bounds)) c.viewMask |= 1u;
        if (shadows) {
            for (int v = 1; v < kViews; ++v) {
                if (!outside(planes + v * 6, b.bounds)) c.viewMask |= 1u << v;
            }
        }
        if (!c.viewMask) continue;
        // Far cascades take impostors only (lower tiers from the second cascade on).
        c.firstImpostorCascade = c.D > 0.f ? (frame.quality >= 1 ? 1 : 2) : 4;
        const float meshRange = c.D > 0.f ? c.D : c.cull;
        // Geometric bands: LOD error shrinks with 1/distance, so near bands are narrow.
        c.bandFar[0] = meshRange * 0.125f, c.bandFar[1] = meshRange * 0.25f, c.bandFar[2] = meshRange * 0.5f;
        {  // share of the chunk's footprint in each mesh band (budget estimate)
            const float midY = (b.bounds.min.y + b.bounds.max.y) * 0.5f, meshEnd = c.D > 0.f ? c.D : c.cull;
            for (int sy = 0; sy < 8; ++sy) {
                for (int sx = 0; sx < 8; ++sx) {
                    Vec3 q{b.bounds.min.x + (b.bounds.max.x - b.bounds.min.x) * (static_cast<float>(sx) + 0.5f) / 8.f, midY,
                           b.bounds.min.z + (b.bounds.max.z - b.bounds.min.z) * (static_cast<float>(sy) + 0.5f) / 8.f};
                    const float d = distance(eye, q);
                    if (d >= meshEnd) continue;
                    const int band = d < c.bandFar[0] ? 0 : d < c.bandFar[1] ? 1 : d < c.bandFar[2] ? 2 : 3;
                    c.bandFrac[band] += 1.f / 64.f;
                }
            }
        }
        c.parts = static_cast<uint32_t>(std::min<size_t>(b.parts->size(), kMaxParts));
        const auto n = static_cast<NSUInteger>(b.instances->size());
        const NSUInteger views = shadows ? kViews : 1;
        c.views = static_cast<uint32_t>(views);
        c.listStride = 2 * n * sizeof(uint32_t);
        c.listOffset = listBytes;
        listBytes += (views * c.listStride + 255) & ~NSUInteger{255};
        c.viewArgs = viewArgsBytes(c.parts);
        c.argsOffset = argBytes;
        argBytes += (views * c.viewArgs + 255) & ~NSUInteger{255};
        c.instances = instanceBuffer(b);

        FoliageUniformsGpu& u = c.uniforms;
        u.wind = simd_make_float4(std::sin(windAngle), std::cos(windAngle), env.windSpeed, b.wind);
        u.params = simd_make_float4(c.cull, b.meshHeight, debug ? 1.f : 0.f, frame.time);
        u.fade = simd_make_float4(c.D, c.W, 0.f, 0.f);
        if (c.imp) {
            ImpostorUniformsGpu& iu = c.impUniforms;
            iu.center = simd_make_float4(c.imp->center.x, c.imp->center.y, c.imp->center.z, c.imp->radius);
            iu.grid = simd_make_float4(static_cast<float>(c.imp->frames), 1.f / static_cast<float>(c.imp->frames), c.imp->hemi ? 1.f : 0.f,
                                       1.f / static_cast<float>(std::max(c.imp->tile, 1)));
            iu.surface = simd_make_float4(c.imp->roughness, debug ? 1.f : 0.f, 0.05f * c.imp->radius * b.maxScale, 0.f);
            Vec3 sun = env.sunDirection();
            iu.light = simd_make_float4(sun.x, sun.y, sun.z, 0.f);
        }
        chunks_.push_back(c);
    }
    if (chunks_.empty()) return;

    // Mesh LOD of a part in a distance band, chosen at the band's near edge so the on-screen
    // error stays under a pixel; `budgetBias_` coarsens everything when the frame would exceed
    // the foliage triangle budget.
    auto lodFor = [&](const Chunk& c, uint32_t part, int band, int table) {
        const InstancePart& ip = (*c.batch->parts)[part];
        const GpuMesh* m = meshes_(ip.mesh);
        if (!m) return 0;
        const float nearEdge = band == 0 ? 0.f : c.bandFar[band - 1];
        // Shadows: two levels coarser (a cascade texel is much larger than a screen pixel).
        const int bias = table * 2 + (frame.quality >= 2 ? 1 : 0) + budgetBias_;
        // Foliage tolerates a few pixels of simplification error (leaves flutter, TAA resolves it):
        // photoscanned trees of millions of triangles are unaffordable at the 1 px bar of props.
        int lod = m->lodFor(pixelsPerUnit(nearEdge) * c.batch->maxScale * kFoliageLodScale) + bias;
        // Simplified leaf cards thin out: cap them (impostors take over the distance) unless the
        // triangle budget forbids it.
        if (ip.surface.alphaCutoff > 0.f && frame.quality < 2 && budgetBias_ == 0) lod = std::min(lod, (c.D > 0.f ? 2 : 1) + bias);
        return std::clamp(lod, 0, m->lodCount - 1);
    };
    // Triangle budget (camera view): a hard safety net against frames long enough to trip the GPU
    // watchdog. Estimated from each chunk's footprint split into the distance bands.
    auto chunkTriangles = [&](const Chunk& c) {
        double total = 0;
        if (!(c.viewMask & 1u)) return total;
        const double n = static_cast<double>(c.batch->instances->size());
        for (int band = 0; band < kBands; ++band) {
            if (c.bandFrac[band] <= 0.f) continue;
            double tris = 0;
            for (uint32_t part = 0; part < c.parts; ++part) {
                if (const GpuMesh* m = meshes_((*c.batch->parts)[part].mesh)) tris += m->lodCount_[lodFor(c, part, band, 0)] / 3;
            }
            total += n * c.bandFrac[band] * tris;
        }
        return total;
    };
    budgetBias_ = 0;
    for (; budgetBias_ <= 4; ++budgetBias_) {
        double total = 0;
        for (const Chunk& c : chunks_) total += chunkTriangles(c);
        lastEstimate_ = static_cast<uint64_t>(total);
        const uint64_t budget = safeMode_ ? kTriangleBudget / 4 : kTriangleBudget;
        if (total <= static_cast<double>(budget) || budgetBias_ == 4) break;
    }
    // Per-model breakdown (perf_stats "foliageModels"): where the mesh triangles go.
    layerEstimate_.clear();
    for (const Chunk& c : chunks_) {
        const std::string& key = (*c.batch->parts)[0].mesh;
        auto& e = layerEstimate_[key];
        e.triangles += chunkTriangles(c);
        e.transition = c.D;
        e.cull = c.cull;
    }

    // Lists and arguments live in shared memory (unified on Apple silicon): the CPU fallback
    // writes them directly, and every frame's arguments are checked when the GPU is done.
    auto ensure = [&](id<MTLBuffer> __strong& buf, NSUInteger bytes) {
        if (buf && buf.length >= bytes) return;
        buf = [device_ newBufferWithLength:std::max<NSUInteger>(bytes + bytes / 2, 1 << 16) options:MTLResourceStorageModeShared];
    };
    ensure(lists_[ring_], listBytes);
    ensure(args_[ring_], argBytes);
    frameStats_ = [device_ newBufferWithLength:8 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    std::memset(frameStats_.contents, 0, frameStats_.length);
    frameArgBytes_ = argBytes;
    frameChecks_.clear();

    const bool gpu = gpuCull_ && cull_;
    id<MTLComputeCommandEncoder> enc = nil;
    if (gpu) {
        // Arguments the kernel does not write (views a chunk does not process) draw nothing.
        id<MTLBlitCommandEncoder> blit = profiledBlit(cmd, "Foliage args clear", "foliage");
        [blit fillBuffer:args_[ring_] range:NSMakeRange(0, argBytes) value:0];
        [blit endEncoding];
        enc = profiledCompute(cmd, "Foliage cull", "foliage", MTLDispatchTypeConcurrent);
        [enc setComputePipelineState:cull_];
        [enc setBuffer:frameStats_ offset:0 atIndex:4];
    } else {
        std::memset(args_[ring_].contents, 0, argBytes);
    }
    FoliageCullParamsGpu p{};
    std::memcpy(p.planes, planes, sizeof(planes));
    for (Chunk& c : chunks_) {
        const InstanceBatch& b = *c.batch;
        const Vec3 center = b.modelBounds.center();
        p.eye = simd_make_float4(eye.x, eye.y, eye.z, c.D);
        // Shadow casters: everything in a cascade (fast editing tier: only near the camera).
        p.params = simd_make_float4(c.W, c.cull, std::max(length(b.modelBounds.extents()), 1e-3f),
                                    c.views > 1 ? (frame.quality >= 2 ? 60.f : 1e9f) : 0.f);
        p.center = simd_make_float4(center.x, center.y, center.z, static_cast<float>(c.firstImpostorCascade));
        p.bands = simd_make_float4(c.bandFar[0], c.bandFar[1], c.bandFar[2], 0.f);
        p.counts[0] = static_cast<uint32_t>(b.instances->size());
        p.counts[1] = c.parts;
        p.counts[2] = c.views;
        p.counts[3] = static_cast<uint32_t>(c.listStride / sizeof(uint32_t));
        uint32_t maxIndices = 0;
        // Mesh LOD per part and distance band (camera; shadows one level coarser).
        for (int table = 0; table < 2; ++table) {
            for (uint32_t part = 0; part < kMaxParts; ++part) {
                const GpuMesh* m = part < c.parts ? meshes_((*b.parts)[part].mesh) : nullptr;
                for (int band = 0; band < kBands; ++band) {
                    uint32_t* slot = p.lods[table * kMaxParts * kBands + part * kBands + band];
                    if (!m) {
                        slot[0] = slot[1] = 0;
                        continue;
                    }
                    const int lod = lodFor(c, part, band, table);
                    if (table == 0) c.bandLod[part][band] = static_cast<uint8_t>(lod);
                    slot[0] = m->lodOffset[lod];
                    slot[1] = m->lodCount_[lod];
                    maxIndices = std::max(maxIndices, slot[1]);
                }
            }
        }
        frameChecks_.push_back({c.argsOffset, c.viewArgs, c.views, c.parts, p.counts[0], p.counts[3], maxIndices});
        if (gpu) {
            [enc setBuffer:c.instances offset:0 atIndex:0];
            [enc setBytes:&p length:sizeof(p) atIndex:1];
            [enc setBuffer:lists_[ring_] offset:c.listOffset atIndex:2];
            [enc setBuffer:args_[ring_] offset:c.argsOffset atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kCullThreads, 1, 1)];
        } else {
            cpuCull(*b.instances, p, static_cast<uint8_t*>(lists_[ring_].contents) + c.listOffset,
                    static_cast<uint8_t*>(args_[ring_].contents) + c.argsOffset, static_cast<uint32_t*>(frameStats_.contents));
        }
    }
    if (enc) [enc endEncoding];
}


// --- Drawing -------------------------------------------------------------------------------------

void MetalFoliage::bindPart(id<MTLRenderCommandEncoder> enc, const InstancePart& part, bool shadow, bool& cutout) {
    const Surface& s = part.surface;
    id<MTLTexture> albedo = textures_(s.texture, true);
    cutout = s.alphaCutoff > 0.f && (!shadow || albedo);
    if (shadow) {
        if (!cutout) return;
        FxDrawUniforms du = surfaceUniforms_(s, simd_make_float4(1, 0, 0, 0));
        [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc setFragmentTexture:albedo atIndex:0];
        return;
    }
    id<MTLTexture> normal = textures_(s.normalMap, false), orm = textures_(s.ormMap, false), emissive = textures_(s.emissiveMap, true);
    FxDrawUniforms du = surfaceUniforms_(s, simd_make_float4(albedo ? 1 : 0, normal ? 1 : 0, orm ? 1.f + s.occlusionStrength : 0.f,
                                                             emissive ? 1 : 0));
    [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
    [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
    [enc setFragmentTexture:(albedo ?: white_) atIndex:0];
    [enc setFragmentTexture:(normal ?: white_) atIndex:2];
    [enc setFragmentTexture:(orm ?: white_) atIndex:3];
    [enc setFragmentTexture:(emissive ?: white_) atIndex:4];
}

void MetalFoliage::encodeShadows(id<MTLRenderCommandEncoder> enc, const FrameData& frame, int cascade, simd_float4x4 lvp) {
    (void)frame;
    const uint32_t v = static_cast<uint32_t>(cascade + 1);
    if (debugSkip_ & 4u) return;
    bool any = false;
    for (Chunk& c : chunks_) {
        if (!(c.viewMask & (1u << v))) continue;
        const bool impostorOnly = c.D > 0.f && (cascade >= c.firstImpostorCascade || c.dmin >= c.D);
        const bool impostors = c.D > 0.f && (cascade >= c.firstImpostorCascade || c.dmax >= c.D);
        if (!any) {
            [enc setCullMode:MTLCullModeNone];
            [enc setVertexBytes:&lvp length:sizeof(lvp) atIndex:2];
            any = true;
        }
        const NSUInteger list = c.listOffset + v * c.listStride, args = c.argsOffset + v * c.viewArgs;
        if (!impostorOnly && !(debugSkip_ & 16u)) {
            for (uint32_t pi = 0; pi < c.parts; ++pi) {
                const InstancePart& part = (*c.batch->parts)[pi];
                const GpuMesh* m = meshes_(part.mesh);
                if (!m) continue;
                bool cut = false;
                bindPart(enc, part, true, cut);
                [enc setRenderPipelineState:cut ? meshShadowAlpha_ : meshShadow_];
                c.uniforms.part = toSimd(part.local);
                [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
                [enc setVertexBuffer:c.instances offset:0 atIndex:3];
                [enc setVertexBytes:&c.uniforms length:sizeof(c.uniforms) atIndex:4];
                [enc setVertexBuffer:lists_[ring_] offset:list atIndex:5];
                for (int band = 0; band < kBands; ++band) {
                    if (!c.bandOverlaps(band) || (c.D > 0.f && (band == 0 ? 0.f : c.bandFar[band - 1]) >= c.D)) continue;
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:m->indices
                             indexBufferOffset:0
                                indirectBuffer:args_[ring_]
                          indirectBufferOffset:args + meshArgsOffset(pi, band)];
                }
            }
        }
        if (impostors && c.imp && !(debugSkip_ & 8u)) {
            [enc setRenderPipelineState:impostorShadow_];
            [enc setVertexBuffer:c.instances offset:0 atIndex:3];
            [enc setVertexBuffer:lists_[ring_] offset:list atIndex:5];
            [enc setVertexBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:6];
            [enc setFragmentBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:0];
            [enc setFragmentBytes:&lvp length:sizeof(lvp) atIndex:1];
            [enc setFragmentBuffer:c.instances offset:0 atIndex:5];
            [enc setFragmentTexture:c.imp->albedo atIndex:0];
            [enc setFragmentTexture:c.imp->normal atIndex:2];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:args_[ring_] indirectBufferOffset:args + impostorArgsOffset(c.parts)];
        }
    }
}

void MetalFoliage::encodeMain(id<MTLRenderCommandEncoder> enc, const FrameData& frame) {
    const bool lodDebug = frame.debugView == debugview::kLod;
    const bool overdraw = frame.debugView == debugview::kOverdraw && overdrawMesh_ && overdrawImpostor_;
    bool cutBound = false, first = true;
    for (Chunk& c : chunks_) {
        if (debugSkip_ & 1u) break;
        if (!(c.viewMask & 1u) || (c.D > 0.f && c.dmin >= c.D)) continue;  // impostors only
        for (uint32_t pi = 0; pi < c.parts; ++pi) {
            const InstancePart& part = (*c.batch->parts)[pi];
            const GpuMesh* m = meshes_(part.mesh);
            if (!m) continue;
            bool cut = false;
            bindPart(enc, part, false, cut);
            if (first || cut != cutBound) {
                [enc setRenderPipelineState:overdraw ? overdrawMesh_ : (cut ? meshCutout_ : mesh_)];
                cutBound = cut;
                first = false;
            }
            c.uniforms.part = toSimd(part.local);
            [enc setCullMode:part.surface.doubleSided ? MTLCullModeNone : MTLCullModeBack];
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBuffer:c.instances offset:0 atIndex:3];
            [enc setVertexBytes:&c.uniforms length:sizeof(c.uniforms) atIndex:4];
            [enc setVertexBuffer:lists_[ring_] offset:c.listOffset atIndex:5];
            FxDrawUniforms lodUniforms{};
            if (lodDebug) {  // [debug views] the lod view tints each band by its mesh LOD
                const Surface& s = part.surface;
                lodUniforms = surfaceUniforms_(s, simd_make_float4(textures_(s.texture, true) ? 1 : 0, textures_(s.normalMap, false) ? 1 : 0,
                                                                   textures_(s.ormMap, false) ? 1.f + s.occlusionStrength : 0.f,
                                                                   textures_(s.emissiveMap, true) ? 1 : 0));
            }
            for (int band = 0; band < kBands; ++band) {
                if (!c.bandOverlaps(band)) continue;
                if (lodDebug) {
                    lodUniforms.material4.w = 1.f + static_cast<float>(c.bandLod[pi][band]);
                    [enc setFragmentBytes:&lodUniforms length:sizeof(lodUniforms) atIndex:0];
                }
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:m->indices
                         indexBufferOffset:0
                            indirectBuffer:args_[ring_]
                      indirectBufferOffset:c.argsOffset + meshArgsOffset(pi, band)];
            }
        }
    }
    bool impBound = false;
    if (debugSkip_ & 2u) return;
    for (Chunk& c : chunks_) {
        if (!(c.viewMask & 1u) || !c.imp || c.D <= 0.f || c.dmax <= c.D - c.W) continue;
        if (!impBound) {
            [enc setRenderPipelineState:overdraw ? overdrawImpostor_ : impostor_];
            [enc setCullMode:MTLCullModeNone];
            impBound = true;
        }
        [enc setVertexBuffer:c.instances offset:0 atIndex:3];
        [enc setVertexBytes:&c.uniforms length:sizeof(c.uniforms) atIndex:4];
        [enc setVertexBuffer:lists_[ring_] offset:c.listOffset atIndex:5];
        [enc setVertexBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:6];
        [enc setFragmentBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:0];
        [enc setFragmentBuffer:c.instances offset:0 atIndex:5];
        [enc setFragmentTexture:c.imp->albedo atIndex:0];
        [enc setFragmentTexture:c.imp->normal atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:args_[ring_] indirectBufferOffset:c.argsOffset + impostorArgsOffset(c.parts)];
    }
}

void MetalFoliage::trackFrame(id<MTLCommandBuffer> cmd) {
    if (!frameStats_) {
        counters_->meshInstances = counters_->impostors = counters_->kiloTris = counters_->shadowKiloTris = counters_->shadowImpostors = 0;
        return;
    }
    id<MTLBuffer> stats = frameStats_, args = args_[ring_];
    std::shared_ptr<Counters> counters = counters_;
    // Every frame's indirect arguments are checked against what they may contain; a violation
    // switches culling to the CPU path for the following frames.
    auto checks = std::make_shared<std::vector<ArgCheck>>(frameChecks_);  // (the CPU path is checked too)
    const char* dumpEnv = std::getenv("SKY_FOLIAGE_DUMP");
    const bool dump = dumpEnv && *dumpEnv == '1';
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        const auto* s = static_cast<const uint32_t*>(stats.contents);
        counters->meshInstances = s[0];
        counters->impostors = s[1];
        counters->kiloTris = s[2];
        counters->shadowKiloTris = s[3];
        counters->shadowImpostors = s[4];
        const auto* base = static_cast<const uint8_t*>(args.contents);
        for (const ArgCheck& c : *checks) {
            for (uint32_t v = 0; v < c.views; ++v) {
                const uint8_t* view = base + c.argsOffset + v * c.viewArgs;
                bool ok = true;
                for (uint32_t part = 0; part < c.parts && ok; ++part) {
                    for (int band = 0; band < kBands && ok; ++band) {
                        const auto* x = reinterpret_cast<const uint32_t*>(view + meshArgsOffset(part, band));
                        ok = x[0] <= c.maxIndices && x[0] % 3 == 0 && x[1] <= c.instances && x[3] == 0 &&
                             static_cast<uint64_t>(x[4]) + x[1] <= c.listStride;
                    }
                }
                const auto* x = reinterpret_cast<const uint32_t*>(view + impostorArgsOffset(c.parts));
                ok = ok && (x[0] == 4 || x[0] == 0) && x[1] <= c.instances && x[2] == 0 && static_cast<uint64_t>(x[3]) + x[1] <= c.listStride;
                if (!ok) {
                    counters->gpuInvalid = true;
                    std::fprintf(stderr, "[foliage] invalid indirect arguments (chunk args @%lu, view %u)\n",
                                 static_cast<unsigned long>(c.argsOffset), v);
                    return;
                }
            }
        }
        if (dump) {  // SKY_FOLIAGE_DUMP=1: summary of the validated arguments
            uint32_t maxInst = 0, maxIdx = 0, draws = 0;
            for (const ArgCheck& c : *checks) {
                for (uint32_t v = 0; v < c.views; ++v) {
                    const uint8_t* view = base + c.argsOffset + v * c.viewArgs;
                    for (uint32_t part = 0; part < c.parts; ++part) {
                        for (int band = 0; band < kBands; ++band) {
                            const auto* x = reinterpret_cast<const uint32_t*>(view + meshArgsOffset(part, band));
                            maxInst = std::max(maxInst, x[1]), maxIdx = std::max(maxIdx, x[0]), draws += x[1] ? 1 : 0;
                        }
                    }
                    const auto* x = reinterpret_cast<const uint32_t*>(view + impostorArgsOffset(c.parts));
                    maxInst = std::max(maxInst, x[1]), draws += x[1] ? 1 : 0;
                }
            }
            std::fprintf(stderr, "[foliage] args ok: %zu chunks, %u non-empty draws, max instanceCount %u, max indexCount %u; stats %u %u %u %u %u\n",
                         checks->size(), draws, maxInst, maxIdx, s[0], s[1], s[2], s[3], s[4]);
        }
    }];
}

void MetalFoliage::evict(uint64_t frameIndex) {
    std::erase_if(instanceBuffers_, [&](const auto& kv) { return frameIndex - kv.second.lastUse > kInstanceBufferIdleFrames; });
    std::erase_if(impostors_, [&](const auto& kv) { return frameIndex - kv.second->lastUse > kImpostorIdleFrames; });
}

uint64_t MetalFoliage::triangles() const {
    // The impostor cards are 2 triangles each.
    return (counters_->kiloTris.load() + counters_->shadowKiloTris.load()) * 1024 +
           (counters_->impostors.load() + counters_->shadowImpostors.load()) * 2;
}

Json MetalFoliage::stats() const {
    size_t bytes = 0, ready = 0;
    for (const auto& [k, imp] : impostors_) {
        if (imp->failed) continue;
        bytes += imp->bytes();
        ++ready;
    }
    auto round2 = [](double v) { return std::round(v * 100.0) / 100.0; };
    Json models = Json::array();
    for (const auto& [mesh, e] : layerEstimate_) {
        models.push(Json::object({{"mesh", mesh},
                                  {"meshTriangles", static_cast<int64_t>(e.triangles)},
                                  {"impostorDistance", std::round(e.transition)},
                                  {"cullDistance", e.cull}}));
    }
    return Json::object({{"foliageModels", models},{"meshInstances", static_cast<int64_t>(counters_->meshInstances.load())},
                         {"impostorInstances", static_cast<int64_t>(counters_->impostors.load())},
                         {"shadowImpostorInstances", static_cast<int64_t>(counters_->shadowImpostors.load())},
                         {"foliageTriangles", static_cast<int64_t>(counters_->kiloTris.load() * 1024)},
                         {"foliageShadowTriangles", static_cast<int64_t>(counters_->shadowKiloTris.load() * 1024)},
                         {"foliageChunksDrawn", static_cast<int64_t>(chunks_.size())},
                         {"foliageBudgetBias", budgetBias_},
                         {"foliageGpuCulling", gpuCull_},
                         {"foliageTriangleEstimate", static_cast<int64_t>(lastEstimate_)},
                         {"impostorsResident", static_cast<int64_t>(ready)},
                         {"impostorsBaked", static_cast<int64_t>(bakes_)},
                         {"impostorsLoaded", static_cast<int64_t>(cacheLoads_)},
                         {"impostorBakeMs", round2(bakeMs_)},
                         {"impostorMemoryMB", round2(static_cast<double>(bytes) / (1024.0 * 1024.0))}});
}

// --- Impostors: bake, cache, upload --------------------------------------------------------------

MetalFoliage::Impostor* MetalFoliage::impostor(const ImpostorModel& model, bool allowWork, bool force, bool* worked) {
    auto it = impostors_.find(model.key);
    if (it != impostors_.end() && !force) {
        it->second->lastUse = frameIndex_;
        return it->second.get();
    }
    if (!allowWork) return nullptr;
    if (worked) *worked = true;
    std::unique_ptr<Impostor> imp;
    if (!force && !model.cachePath.empty()) {
        auto loaded = loadModel(model);
        if (loaded) {
            imp = std::move(loaded.value());
            ++cacheLoads_;
        } else if (loaded.error().code != "not_found") {
            log::warn("render", "impostor cache for " + model.label + " ignored (rebaking): " + loaded.error().message);
        }
    }
    if (!imp) {
        auto baked = bakeModel(model);
        if (baked) {
            imp = std::move(baked.value());
            ++bakes_;
            bakeMs_ += imp->bakeMs;
        } else {
            log::warn("render", "impostor bake failed for " + model.label + ": " + baked.error().message);
            imp = std::make_unique<Impostor>();
            imp->failed = true;
        }
    }
    imp->lastUse = frameIndex_;
    imp->label = model.label;
    imp->path = model.cachePath;
    Impostor* raw = imp.get();
    impostors_[model.key] = std::move(imp);
    return raw;
}

std::unique_ptr<MetalFoliage::Impostor> MetalFoliage::upload(const ImpostorAtlasData& data) {
    std::vector<impostor::Atlas> levels = impostor::buildMips(data.atlas);
    const auto size = static_cast<NSUInteger>(data.atlas.size);
    auto makeTexture = [&](MTLPixelFormat format) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:size height:size mipmapped:YES];
        d.mipmapLevelCount = levels.size();
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        return [device_ newTextureWithDescriptor:d];
    };
    auto imp = std::make_unique<Impostor>();
    imp->albedo = makeTexture(MTLPixelFormatRGBA8Unorm_sRGB);
    imp->normal = makeTexture(MTLPixelFormatRGBA8Unorm);
    NSUInteger total = 0;
    for (const auto& l : levels) total += static_cast<NSUInteger>(l.size) * l.size * 8;
    id<MTLBuffer> staging = [device_ newBufferWithLength:total options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    cmd.label = @"Impostor upload";
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    NSUInteger offset = 0;
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto& l = levels[i];
        const auto n = static_cast<NSUInteger>(l.size);
        for (int plane = 0; plane < 2; ++plane) {
            const std::vector<uint8_t>& src = plane == 0 ? l.albedo : l.normal;
            std::memcpy(static_cast<uint8_t*>(staging.contents) + offset, src.data(), src.size());
            [blit copyFromBuffer:staging
                       sourceOffset:offset
                  sourceBytesPerRow:n * 4
                sourceBytesPerImage:n * n * 4
                         sourceSize:MTLSizeMake(n, n, 1)
                          toTexture:(plane == 0 ? imp->albedo : imp->normal)
                   destinationSlice:0
                   destinationLevel:i
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            offset += n * n * 4;
        }
    }
    [blit endEncoding];
    [cmd commit];  // the frame's command buffer (same queue) runs after it
    imp->frames = data.atlas.frames;
    imp->tile = data.atlas.tile();
    imp->size = data.atlas.size;
    imp->center = data.center;
    imp->radius = data.radius;
    imp->roughness = data.roughness;
    imp->hemi = data.hemi;
    return imp;
}

Result<std::unique_ptr<MetalFoliage::Impostor>> MetalFoliage::loadModel(const ImpostorModel& model) {
    auto t0 = std::chrono::steady_clock::now();
    auto loaded = impostor::load(model.cachePath);
    if (!loaded) return loaded.error();
    const Json& meta = loaded->meta;
    if (meta.get("key").asString() != model.key || meta.get("version").asInt(0) != impostor::kBakeVersion) {
        return Error::make("stale_cache", "the cache was baked from different inputs");
    }
    ImpostorAtlasData data;
    data.atlas = std::move(loaded->atlas);
    const Json& c = meta.get("center");
    data.center = {c[size_t{0}].asFloat(), c[size_t{1}].asFloat(), c[size_t{2}].asFloat()};
    data.radius = meta.get("radius").asFloat(1.f);
    data.roughness = meta.get("roughness").asFloat(0.7f);
    data.hemi = meta.get("hemi").asBool(true);
    auto imp = upload(data);
    imp->fromCache = true;
    imp->bakeMs = msSince(t0);
    return imp;
}

Result<std::unique_ptr<MetalFoliage::Impostor>> MetalFoliage::bakeModel(const ImpostorModel& model) {
    if (!bake_) return Error::make("unsupported", "the impostor bake pipeline is unavailable");
    if (!model.parts || model.parts->empty()) return Error::make("invalid_model", "the model has no parts");
    auto t0 = std::chrono::steady_clock::now();
    const int frames = std::clamp(model.frames, impostor::kMinFrames, impostor::kMaxFrames);
    const int tile = impostor::tileSize(model.resolution, frames);
    const int size = frames * tile;
    const Vec3 center = model.bounds.center();
    const float radius = std::max(length(model.bounds.extents()), 1e-3f) * 1.01f;
    std::vector<const GpuMesh*> meshes;
    for (const auto& p : *model.parts) {
        const GpuMesh* m = meshes_(p.mesh);
        if (!m) return Error::make("not_ready", "mesh " + p.mesh + " is not loaded");
        meshes.push_back(m);
    }
    // One render pass per row of frames into a (size x tile) band: bounded memory, and heavy
    // models may overflow the tiler's parameter buffer (so the MSAA targets are not memoryless).
    const auto n = static_cast<NSUInteger>(size), rowH = static_cast<NSUInteger>(tile);
    auto target = [&](MTLPixelFormat format, bool msaa) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:n height:rowH mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        if (msaa) {
            d.textureType = MTLTextureType2DMultisample;
            d.sampleCount = 4;
        }
        return [device_ newTextureWithDescriptor:d];
    };
    const MTLPixelFormat formats[3] = {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm, MTLPixelFormatR8Unorm};
    const NSUInteger bpp[3] = {4, 4, 1};
    id<MTLTexture> resolved[3], msaa[3];
    for (int i = 0; i < 3; ++i) {
        resolved[i] = target(formats[i], false);
        msaa[i] = target(formats[i], true);
    }
    id<MTLTexture> depth = target(MTLPixelFormatDepth32Float, true);
    id<MTLBuffer> reads[3];
    for (int i = 0; i < 3; ++i) reads[i] = [device_ newBufferWithLength:n * n * bpp[i] options:MTLResourceStorageModeShared];
    if (!resolved[0] || !resolved[1] || !resolved[2] || !msaa[0] || !msaa[1] || !msaa[2] || !depth || !reads[0] || !reads[1] || !reads[2]) {
        return Error::make("out_of_memory", "could not allocate a " + std::to_string(size) + " px impostor atlas");
    }
    // Detail that the frame resolution can show: the LOD whose error stays under a texel (leaf
    // cards at most one level down, sloppy simplification thins canopies).
    std::vector<int> lods(meshes.size(), 0);
    uint64_t trianglesPerFrame = 0;
    for (size_t pi = 0; pi < meshes.size(); ++pi) {
        const GpuMesh& m = *meshes[pi];
        int lod = m.lodFor(static_cast<float>(tile) / (2.f * radius));
        if ((*model.parts)[pi].surface.alphaCutoff > 0.f) lod = std::min(lod, 1);
        lods[pi] = std::clamp(lod, 0, m.lodCount - 1);
        trianglesPerFrame += m.lodCount_[lods[pi]] / 3;
    }
    // Bounded GPU work: frames are baked in batches of at most kBakeTrianglesPerBatch triangles,
    // each batch its own short command buffer (a multi-second command buffer trips the GPU
    // watchdog and stalls the desktop). A row of frames is one render target band; batches inside
    // a row keep the MSAA contents between passes.
    constexpr uint64_t kBakeTrianglesPerBatch = 12'000'000;
    const int framesPerBatch = static_cast<int>(std::clamp<uint64_t>(kBakeTrianglesPerBatch / std::max<uint64_t>(trianglesPerFrame, 1), 1,
                                                                     static_cast<uint64_t>(frames)));
    id<MTLCommandBuffer> last = nil;
    for (int fy = 0; fy < frames; ++fy) {
        for (int fx0 = 0; fx0 < frames; fx0 += framesPerBatch) {
            const int fx1 = std::min(frames, fx0 + framesPerBatch);
            const bool firstPass = fx0 == 0, lastPass = fx1 == frames;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            for (int i = 0; i < 3; ++i) {
                rp.colorAttachments[i].texture = msaa[i];
                rp.colorAttachments[i].resolveTexture = lastPass ? resolved[i] : nil;
                rp.colorAttachments[i].loadAction = firstPass ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[i].storeAction = lastPass ? MTLStoreActionMultisampleResolve : MTLStoreActionStore;
            }
            rp.depthAttachment.texture = depth;
            rp.depthAttachment.loadAction = firstPass ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = 1.0;
            rp.depthAttachment.storeAction = lastPass ? MTLStoreActionDontCare : MTLStoreActionStore;
            id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
            cmd.label = @"Impostor bake";
            id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
            enc.label = @"Impostor frames";
            [enc setRenderPipelineState:bake_];
            [enc setDepthStencilState:bakeDepth_];
            [enc setFrontFacingWinding:MTLWindingCounterClockwise];
            for (int fx = fx0; fx < fx1; ++fx) {
                const impostor::Basis b = impostor::frameBasis(impostor::frameDirection(fx, fy, frames, model.hemi));
                ImpostorBakeUniformsGpu bu{simd_make_float4(center.x, center.y, center.z, radius),
                                           simd_make_float4(b.right.x, b.right.y, b.right.z, 0), simd_make_float4(b.up.x, b.up.y, b.up.z, 0),
                                           simd_make_float4(b.forward.x, b.forward.y, b.forward.z, 0)};
                const double t = static_cast<double>(tile);
                [enc setViewport:MTLViewport{fx * t, 0.0, t, t, 0.0, 1.0}];
                [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(fx * tile), 0, rowH, rowH}];
                [enc setVertexBytes:&bu length:sizeof(bu) atIndex:2];
                [enc setFragmentBytes:&bu length:sizeof(bu) atIndex:2];
                for (size_t pi = 0; pi < meshes.size(); ++pi) {
                    const InstancePart& part = (*model.parts)[pi];
                    const Surface& s = part.surface;
                    id<MTLTexture> albedo = textures_(s.texture, true), normal = textures_(s.normalMap, false),
                                   orm = textures_(s.ormMap, false), emissive = textures_(s.emissiveMap, true);
                    FxDrawUniforms du = surfaceUniforms_(s, simd_make_float4(albedo ? 1 : 0, normal ? 1 : 0,
                                                                             orm ? 1.f + s.occlusionStrength : 0.f, emissive ? 1 : 0));
                    du.model = toSimd(part.local);
                    du.normalMatrix = toSimd(part.local.inverse().transposed());
                    [enc setCullMode:s.doubleSided ? MTLCullModeNone : MTLCullModeBack];
                    [enc setVertexBuffer:meshes[pi]->vertices offset:0 atIndex:0];
                    [enc setVertexBytes:&du length:sizeof(du) atIndex:1];
                    [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
                    [enc setFragmentTexture:(albedo ?: white_) atIndex:0];
                    [enc setFragmentTexture:(normal ?: white_) atIndex:2];
                    [enc setFragmentTexture:(orm ?: white_) atIndex:3];
                    [enc setFragmentTexture:(emissive ?: white_) atIndex:4];
                    const int lod = lods[pi];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:meshes[pi]->lodCount_[lod]
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:meshes[pi]->indices
                             indexBufferOffset:meshes[pi]->lodOffset[lod] * sizeof(uint32_t)];
                }
            }
            [enc endEncoding];
            if (lastPass) {  // read the resolved row back for the CPU post-process and the cache
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                for (int i = 0; i < 3; ++i) {
                    [blit copyFromTexture:resolved[i]
                                     sourceSlice:0
                                     sourceLevel:0
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:MTLSizeMake(n, rowH, 1)
                                        toBuffer:reads[i]
                               destinationOffset:static_cast<NSUInteger>(fy) * rowH * n * bpp[i]
                          destinationBytesPerRow:n * bpp[i]
                        destinationBytesPerImage:n * rowH * bpp[i]];
                }
                [blit endEncoding];
            }
            [cmd commit];
            last = cmd;
        }
    }
    [last waitUntilCompleted];  // same queue: every earlier batch has completed too
    if (last.status == MTLCommandBufferStatusError) {
        return Error::make("gpu_error", last.error ? last.error.localizedDescription.UTF8String : "the impostor bake failed");
    }
    const NSUInteger rgbaBytes = n * n * 4;
    id<MTLBuffer> readA = reads[0], readB = reads[1], readR = reads[2];
    ImpostorAtlasData data;
    data.atlas.size = size;
    data.atlas.frames = frames;
    data.atlas.albedo.assign(static_cast<const uint8_t*>(readA.contents), static_cast<const uint8_t*>(readA.contents) + rgbaBytes);
    data.atlas.normal.assign(static_cast<const uint8_t*>(readB.contents), static_cast<const uint8_t*>(readB.contents) + rgbaBytes);
    // Model roughness: the coverage-weighted mean (resolved roughness is premultiplied by coverage).
    {
        const auto* r = static_cast<const uint8_t*>(readR.contents);
        double sumR = 0, sumA = 0;
        for (size_t i = 0, cnt = static_cast<size_t>(n) * n; i < cnt; ++i) {
            sumR += r[i];
            sumA += data.atlas.albedo[i * 4 + 3];
        }
        data.roughness = sumA > 0 ? static_cast<float>(std::clamp(sumR / sumA, 0.05, 1.0)) : 0.7f;
    }
    impostor::finalize(data.atlas);
    data.center = center;
    data.radius = radius;
    data.hemi = model.hemi;
    const double ms = msSince(t0);
    if (!model.cachePath.empty()) {
        Json meta = Json::object({{"format", "skywalker.impostor"},
                                  {"version", impostor::kBakeVersion},
                                  {"key", model.key},
                                  {"label", model.label},
                                  {"frames", frames},
                                  {"tile", tile},
                                  {"size", size},
                                  {"center", Json::array({center.x, center.y, center.z})},
                                  {"radius", radius},
                                  {"roughness", data.roughness},
                                  {"hemi", model.hemi},
                                  {"bakeMs", std::round(ms * 10.0) / 10.0}});
        if (Status s = impostor::save(model.cachePath, data.atlas, meta); !s) log::warn("render", "impostor cache not written: " + s.error().message);
    }
    auto imp = upload(data);
    imp->bakeMs = msSince(t0);
    log::info("render", "baked impostor " + model.label + " (" + std::to_string(size) + " px, " + std::to_string(frames) + "x" +
                            std::to_string(frames) + " views) in " + std::to_string(static_cast<int>(imp->bakeMs)) + " ms");
    return imp;
}

Result<Json> MetalFoliage::bake(const std::vector<ImpostorModel>& models, bool force) {
    Json out = Json::array();
    for (const ImpostorModel& m : models) {
        auto t0 = std::chrono::steady_clock::now();
        Impostor* imp = impostor(m, true, force);
        Json e = Json::object({{"key", m.key}, {"label", m.label}, {"path", m.cachePath}});
        if (!imp || imp->failed) {
            e["ok"] = false;
        } else {
            e["ok"] = true;
            e["atlas"] = imp->size;
            e["frames"] = imp->frames;
            e["tile"] = imp->tile;
            e["roughness"] = std::round(imp->roughness * 1000.0) / 1000.0;
            e["memoryMB"] = std::round(static_cast<double>(imp->bytes()) / (1024.0 * 1024.0) * 100.0) / 100.0;
            e["fromCache"] = imp->fromCache;
            e["ms"] = std::round(msSince(t0) * 10.0) / 10.0;
        }
        out.push(e);
    }
    return out;
}

}  // namespace sky
