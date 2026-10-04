// GPU-driven instanced foliage and octahedral impostors (see MetalFoliage.h).

#include "MetalFoliage.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

#include "skywalker/core/Log.h"
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
    float bandFar[3] = {};
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
};

MetalFoliage::MetalFoliage(id<MTLDevice> device, id<MTLCommandQueue> queue, MeshLookup meshes, TextureLookup textures,
                           SurfaceUniforms surfaceUniforms)
    : device_(device),
      queue_(queue),
      meshes_(std::move(meshes)),
      textures_(std::move(textures)),
      surfaceUniforms_(std::move(surfaceUniforms)),
      counters_(std::make_shared<Counters>()) {
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
        }
        return [device_ newRenderPipelineStateWithDescriptor:d error:&e];
    };
    id<MTLComputePipelineState> cull = [device_ newComputePipelineStateWithFunction:fn("foliageCullKernel") error:&e];
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
        bake = [device_ newRenderPipelineStateWithDescriptor:d error:&e];
    }
    if (!bake) {
        return Error::make("pipeline_error", e ? std::string(e.localizedDescription.UTF8String) : "foliage pipeline creation failed");
    }
    cull_ = cull;
    mesh_ = mesh;
    meshCutout_ = cutout;
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

void MetalFoliage::prepare(id<MTLCommandBuffer> cmd, const FrameData& frame, const FoliageView& view, uint64_t frameIndex) {
    frameIndex_ = frameIndex;
    ring_ = static_cast<int>(frameIndex % kRing);
    chunks_.clear();
    frameStats_ = nil;
    frameImpostors_.assign(frame.impostors.size(), nullptr);
    if (frame.instances.empty() || !cull_) return;

    // Impostors: ready ones are used; missing ones are baked (or loaded from the project cache)
    // within the frame's budget. Until then their layers draw meshes out to the cull distance.
    int budget = frame.samples > 1 ? INT_MAX : 1;
    for (size_t i = 0; i < frame.impostors.size(); ++i) {
        bool worked = false;
        Impostor* imp = impostor(frame.impostors[i], budget > 0, false, &worked);
        if (worked) --budget;
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
        if (c.dmin > b.cullDistance) continue;
        c.imp = b.impostor >= 0 && static_cast<size_t>(b.impostor) < frameImpostors_.size() ? frameImpostors_[static_cast<size_t>(b.impostor)] : nullptr;
        c.D = c.imp ? b.impostorDistance : 0.f;
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
        const float meshRange = c.D > 0.f ? c.D : b.cullDistance;
        c.bandFar[0] = meshRange * 0.25f, c.bandFar[1] = meshRange * 0.5f, c.bandFar[2] = meshRange * 0.75f;
        c.parts = static_cast<uint32_t>(std::min<size_t>(b.parts->size(), kMaxParts));
        const auto n = static_cast<NSUInteger>(b.instances->size());
        const NSUInteger views = shadows ? kViews : 1;
        c.views = static_cast<uint32_t>(views);
        c.listStride = 2 * n * sizeof(uint32_t);
        c.listOffset = listBytes;
        listBytes += (views * c.listStride + 255) & ~NSUInteger{255};
        c.viewArgs = (c.parts * 20 + 4) * sizeof(uint32_t);
        c.argsOffset = argBytes;
        argBytes += (views * c.viewArgs + 255) & ~NSUInteger{255};
        c.instances = instanceBuffer(b);

        FoliageUniformsGpu& u = c.uniforms;
        u.wind = simd_make_float4(std::sin(windAngle), std::cos(windAngle), env.windSpeed, b.wind);
        u.params = simd_make_float4(b.cullDistance, b.meshHeight, debug ? 1.f : 0.f, frame.time);
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

    auto ensure = [&](id<MTLBuffer> __strong& buf, NSUInteger bytes) {
        if (buf && buf.length >= bytes) return;
        buf = [device_ newBufferWithLength:std::max<NSUInteger>(bytes + bytes / 2, 1 << 16) options:MTLResourceStorageModePrivate];
    };
    ensure(lists_[ring_], listBytes);
    ensure(args_[ring_], argBytes);
    frameStats_ = [device_ newBufferWithLength:8 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    std::memset(frameStats_.contents, 0, frameStats_.length);

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    enc.label = @"Foliage cull";
    [enc setComputePipelineState:cull_];
    [enc setBuffer:frameStats_ offset:0 atIndex:4];
    FoliageCullParamsGpu p{};
    std::memcpy(p.planes, planes, sizeof(planes));
    for (const Chunk& c : chunks_) {
        const InstanceBatch& b = *c.batch;
        const Vec3 center = b.modelBounds.center();
        p.eye = simd_make_float4(eye.x, eye.y, eye.z, c.D);
        // Shadow casters: everything in a cascade (fast editing tier: only near the camera).
        p.params = simd_make_float4(c.W, b.cullDistance, std::max(length(b.modelBounds.extents()), 1e-3f),
                                    c.views > 1 ? (frame.quality >= 2 ? 60.f : 1e9f) : 0.f);
        p.center = simd_make_float4(center.x, center.y, center.z, static_cast<float>(c.firstImpostorCascade));
        p.bands = simd_make_float4(c.bandFar[0], c.bandFar[1], c.bandFar[2], 0.f);
        p.counts[0] = static_cast<uint32_t>(b.instances->size());
        p.counts[1] = c.parts;
        p.counts[2] = c.views;
        p.counts[3] = static_cast<uint32_t>(c.listStride / sizeof(uint32_t));
        // Mesh LOD per part and distance band (camera; shadows one level coarser), chosen at the
        // band's near edge so the on-screen error stays under a pixel.
        for (int table = 0; table < 2; ++table) {
            for (uint32_t part = 0; part < kMaxParts; ++part) {
                const GpuMesh* m = part < c.parts ? meshes_((*b.parts)[part].mesh) : nullptr;
                for (int band = 0; band < kBands; ++band) {
                    uint32_t* slot = p.lods[table * kMaxParts * kBands + part * kBands + band];
                    if (!m) {
                        slot[0] = slot[1] = 0;
                        continue;
                    }
                    const float nearEdge = band == 0 ? 0.f : c.bandFar[band - 1];
                    int bias = table + (frame.quality >= 2 ? 1 : 0);
                    int lod = m->lodFor(pixelsPerUnit(nearEdge) * b.maxScale) + bias;
                    // Simplified leaf cards thin out: cap them (impostors take over the distance).
                    if ((*b.parts)[part].surface.alphaCutoff > 0.f && frame.quality < 2) lod = std::min(lod, (c.D > 0.f ? 2 : 1) + bias);
                    lod = std::clamp(lod, 0, m->lodCount - 1);
                    slot[0] = m->lodOffset[lod];
                    slot[1] = m->lodCount_[lod];
                }
            }
        }
        [enc setBuffer:c.instances offset:0 atIndex:0];
        [enc setBytes:&p length:sizeof(p) atIndex:1];
        [enc setBuffer:lists_[ring_] offset:c.listOffset atIndex:2];
        [enc setBuffer:args_[ring_] offset:c.argsOffset atIndex:3];
        [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kCullThreads, 1, 1)];
    }
    [enc endEncoding];
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
        if (!impostorOnly) {
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
                          indirectBufferOffset:args + (pi * kBands + static_cast<uint32_t>(band)) * 20];
                }
            }
        }
        if (impostors && c.imp) {
            [enc setRenderPipelineState:impostorShadow_];
            [enc setVertexBuffer:c.instances offset:0 atIndex:3];
            [enc setVertexBuffer:lists_[ring_] offset:list atIndex:5];
            [enc setVertexBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:6];
            [enc setFragmentBytes:&c.impUniforms length:sizeof(c.impUniforms) atIndex:0];
            [enc setFragmentBytes:&lvp length:sizeof(lvp) atIndex:1];
            [enc setFragmentBuffer:c.instances offset:0 atIndex:5];
            [enc setFragmentTexture:c.imp->albedo atIndex:0];
            [enc setFragmentTexture:c.imp->normal atIndex:2];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:args_[ring_] indirectBufferOffset:args + c.parts * 20];
        }
    }
}

void MetalFoliage::encodeMain(id<MTLRenderCommandEncoder> enc, const FrameData& frame) {
    (void)frame;
    bool cutBound = false, first = true;
    for (Chunk& c : chunks_) {
        if (!(c.viewMask & 1u) || (c.D > 0.f && c.dmin >= c.D)) continue;  // impostors only
        for (uint32_t pi = 0; pi < c.parts; ++pi) {
            const InstancePart& part = (*c.batch->parts)[pi];
            const GpuMesh* m = meshes_(part.mesh);
            if (!m) continue;
            bool cut = false;
            bindPart(enc, part, false, cut);
            if (first || cut != cutBound) {
                [enc setRenderPipelineState:cut ? meshCutout_ : mesh_];
                cutBound = cut;
                first = false;
            }
            c.uniforms.part = toSimd(part.local);
            [enc setCullMode:part.surface.doubleSided ? MTLCullModeNone : MTLCullModeBack];
            [enc setVertexBuffer:m->vertices offset:0 atIndex:0];
            [enc setVertexBuffer:c.instances offset:0 atIndex:3];
            [enc setVertexBytes:&c.uniforms length:sizeof(c.uniforms) atIndex:4];
            [enc setVertexBuffer:lists_[ring_] offset:c.listOffset atIndex:5];
            for (int band = 0; band < kBands; ++band) {
                if (!c.bandOverlaps(band)) continue;
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:m->indices
                         indexBufferOffset:0
                            indirectBuffer:args_[ring_]
                      indirectBufferOffset:c.argsOffset + (pi * kBands + static_cast<uint32_t>(band)) * 20];
            }
        }
    }
    bool impBound = false;
    for (Chunk& c : chunks_) {
        if (!(c.viewMask & 1u) || !c.imp || c.D <= 0.f || c.dmax <= c.D - c.W) continue;
        if (!impBound) {
            [enc setRenderPipelineState:impostor_];
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
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:args_[ring_] indirectBufferOffset:c.argsOffset + c.parts * 20];
    }
}

void MetalFoliage::trackFrame(id<MTLCommandBuffer> cmd) {
    if (!frameStats_) {
        counters_->meshInstances = counters_->impostors = counters_->kiloTris = counters_->shadowKiloTris = counters_->shadowImpostors = 0;
        return;
    }
    id<MTLBuffer> stats = frameStats_;
    std::shared_ptr<Counters> counters = counters_;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        const auto* s = static_cast<const uint32_t*>(stats.contents);
        counters->meshInstances = s[0];
        counters->impostors = s[1];
        counters->kiloTris = s[2];
        counters->shadowKiloTris = s[3];
        counters->shadowImpostors = s[4];
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
    return Json::object({{"meshInstances", static_cast<int64_t>(counters_->meshInstances.load())},
                         {"impostorInstances", static_cast<int64_t>(counters_->impostors.load())},
                         {"shadowImpostorInstances", static_cast<int64_t>(counters_->shadowImpostors.load())},
                         {"foliageTriangles", static_cast<int64_t>(counters_->kiloTris.load() * 1024)},
                         {"foliageShadowTriangles", static_cast<int64_t>(counters_->shadowKiloTris.load() * 1024)},
                         {"foliageChunksDrawn", static_cast<int64_t>(chunks_.size())},
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
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 3; ++i) {
        rp.colorAttachments[i].texture = msaa[i];
        rp.colorAttachments[i].resolveTexture = resolved[i];
        rp.colorAttachments[i].loadAction = MTLLoadActionClear;
        rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[i].storeAction = MTLStoreActionMultisampleResolve;
    }
    rp.depthAttachment.texture = depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;

    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    cmd.label = @"Impostor bake";
    for (int fy = 0; fy < frames; ++fy) {
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Impostor frames";
        [enc setRenderPipelineState:bake_];
        [enc setDepthStencilState:bakeDepth_];
        [enc setFrontFacingWinding:MTLWindingCounterClockwise];
        for (int fx = 0; fx < frames; ++fx) {
            const impostor::Basis b = impostor::frameBasis(impostor::frameDirection(fx, fy, frames, model.hemi));
            ImpostorBakeUniformsGpu bu{simd_make_float4(center.x, center.y, center.z, radius), simd_make_float4(b.right.x, b.right.y, b.right.z, 0),
                                       simd_make_float4(b.up.x, b.up.y, b.up.z, 0), simd_make_float4(b.forward.x, b.forward.y, b.forward.z, 0)};
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
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:meshes[pi]->lodCount_[0]
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:meshes[pi]->indices
                         indexBufferOffset:0];
            }
        }
        [enc endEncoding];
        // Read the resolved row back for the CPU post-process and the project cache.
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
    [cmd waitUntilCompleted];
    if (cmd.status == MTLCommandBufferStatusError) {
        return Error::make("gpu_error", cmd.error ? cmd.error.localizedDescription.UTF8String : "the impostor bake failed");
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
