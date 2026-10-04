// GPU particles: compute simulation, GPU sorting and indirect drawing (GpuParticles.metal).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MetalFxInternal.h"
#include "skywalker/core/Log.h"
#include "skywalker/render/MeshData.h"

namespace sky {

namespace {

constexpr uint32_t kRequestCap = 65536;  // sub-emitter events per emitter per frame
constexpr uint32_t kMaxCapacity = 4000000;
constexpr NSUInteger kArgsDrawOffset = 32, kArgsIndexedOffset = 48, kArgsSimOffset = 16;

simd_float4x4 simdMat(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}

uint32_t nextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

bool blendedLook(int look) {
    // smoke, rain, snow, mist, splash and lit sprites blend over each other (order matters);
    // glow, flame, spark and emissive sprites are additive.
    return look == 2 || look == 4 || look == 5 || look == 6 || look == 7 || look == fx::kGpuLookSprite;
}

uint32_t hash32(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return static_cast<uint32_t>(v);
}

}  // namespace

void FxGpuTimer::track(id<MTLCommandBuffer> cmd) {
    std::shared_ptr<std::atomic<double>> slot = ms;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double t = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
        if (t > 0.0 && t < 10000.0) {
            double prev = slot->load();
            slot->store(prev > 0.0 ? prev * 0.8 + t * 0.2 : t);
        }
    }];
}

MetalGpuParticles::MetalGpuParticles(id<MTLDevice> device, MetalFx::MeshLookup meshes, MetalFx::TextureLookup textures)
    : device_(device), meshes_(std::move(meshes)), textures_(std::move(textures)) {
    dummy_ = [device_ newBufferWithLength:256 options:MTLResourceStorageModeShared];
    std::memset(dummy_.contents, 0, 256);
    MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    white_ = [device_ newTextureWithDescriptor:wd];
    const uint8_t px[4] = {255, 255, 255, 255};
    [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
    MTLTextureDescriptor* fd = [MTLTextureDescriptor new];
    fd.textureType = MTLTextureType3D;
    fd.pixelFormat = MTLPixelFormatRGBA32Float;
    fd.width = fd.height = fd.depth = 1;
    fd.storageMode = MTLStorageModeShared;
    dummyField_ = [device_ newTextureWithDescriptor:fd];
    const float zero[4] = {0, 0, 0, 0};
    [dummyField_ replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:zero bytesPerRow:16 bytesPerImage:16];
}

bool MetalGpuParticles::build(id<MTLLibrary> lib, const FxFormats& fmt) {
    ready_ = false;
    kernels_.clear();
    NSError* err = nil;
    for (const char* k : {"gpuInit", "gpuEmitArgs", "gpuEmit", "gpuSimArgs", "gpuSimulate", "gpuFinalize", "gpuSortKeys",
                          "gpuBitonicLocal", "gpuBitonicGlobal", "gpuLightReduce"}) {
        id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:k]];
        id<MTLComputePipelineState> ps = f ? [device_ newComputePipelineStateWithFunction:f error:&err] : nil;
        if (!ps) {
            log::warn("render", std::string("GPU particles disabled: missing or invalid kernel ") + k +
                                    (err ? ": " + std::string(err.localizedDescription.UTF8String) : ""));
            return false;
        }
        kernels_[k] = ps;
    }
    auto fn = [&](const char* n) { return [lib newFunctionWithName:[NSString stringWithUTF8String:n]]; };
    auto effect = [&](const char* vs) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn(vs);
        d.fragmentFunction = fn("gpuParticleFragment");
        if (!d.vertexFunction || !d.fragmentFunction) return nil;
        auto* c0 = d.colorAttachments[0];
        c0.pixelFormat = fmt.hdr;
        c0.blendingEnabled = YES;
        c0.sourceRGBBlendFactor = MTLBlendFactorOne;
        c0.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c0.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c0.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        auto* c1 = d.colorAttachments[1];
        c1.pixelFormat = MTLPixelFormatR8Unorm;
        c1.blendingEnabled = YES;
        c1.sourceRGBBlendFactor = MTLBlendFactorOne;
        c1.destinationRGBBlendFactor = MTLBlendFactorOne;
        c1.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c1.destinationAlphaBlendFactor = MTLBlendFactorOne;
        return [device_ newRenderPipelineStateWithDescriptor:d error:&err];
    };
    quadPipeline_ = effect("gpuParticleVertex");
    ribbonPipeline_ = effect("gpuRibbonVertex");
    MTLRenderPipelineDescriptor* md = [MTLRenderPipelineDescriptor new];
    md.vertexFunction = fn("gpuMeshParticleVertex");
    md.fragmentFunction = fn("meshFragment");
    md.rasterSampleCount = fmt.samples;
    md.colorAttachments[0].pixelFormat = fmt.hdr;
    md.colorAttachments[1].pixelFormat = fmt.gbufA;
    md.colorAttachments[2].pixelFormat = fmt.gbufB;
    md.depthAttachmentPixelFormat = fmt.depth;
    meshPipeline_ = md.vertexFunction && md.fragmentFunction ? [device_ newRenderPipelineStateWithDescriptor:md error:&err] : nil;
    MTLRenderPipelineDescriptor* sd = [MTLRenderPipelineDescriptor new];
    sd.vertexFunction = fn("gpuMeshParticleShadowVertex");
    sd.depthAttachmentPixelFormat = fmt.depth;
    meshShadowPipeline_ = sd.vertexFunction ? [device_ newRenderPipelineStateWithDescriptor:sd error:&err] : nil;
    if (!quadPipeline_ || !ribbonPipeline_ || !meshPipeline_ || !meshShadowPipeline_) {
        log::warn("render", "GPU particles disabled: " + (err ? std::string(err.localizedDescription.UTF8String) : std::string("missing shaders")));
        return false;
    }
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    depthWrite_ = [device_ newDepthStencilStateWithDescriptor:ds];
    ready_ = true;
    return true;
}

void MetalGpuParticles::ensure(Emitter& e, const GpuEmitterItem& item, id<MTLCommandBuffer> cmd) {
    (void)cmd;
    const ParticleEmitter& em = item.params;
    e.entity = item.entity;
    uint32_t capacity = static_cast<uint32_t>(std::clamp<int64_t>(em.maxParticles, 1, kMaxCapacity));
    e.facing = fx::gpuFacingId(em.facing);
    uint32_t trail = e.facing == static_cast<int>(fx::GpuFacing::Ribbon) ? static_cast<uint32_t>(std::clamp(em.trailSegments, 2, 32)) : 0u;
    int look = fx::gpuLookId(em.look);
    bool litSprite = look == fx::kGpuLookSprite && em.intensity <= 1.001f;
    bool sorted = em.sort && (blendedLook(look) && (look != fx::kGpuLookSprite || litSprite)) &&
                  e.facing != static_cast<int>(fx::GpuFacing::Mesh) && e.facing != static_cast<int>(fx::GpuFacing::Ribbon);
    uint32_t sortCount = sorted ? nextPow2(std::max<uint32_t>(capacity, 1024)) : 0u;
    if (!e.particles || e.capacity != capacity || e.trailSlots != trail || e.sortCount != sortCount) {
        e.capacity = capacity;
        e.trailSlots = trail;
        e.sortCount = sortCount;
        const MTLResourceOptions priv = MTLResourceStorageModePrivate;
        e.particles = [device_ newBufferWithLength:static_cast<NSUInteger>(capacity) * 64 options:priv];
        e.dead = [device_ newBufferWithLength:static_cast<NSUInteger>(capacity) * 4 options:priv];
        e.alive[0] = [device_ newBufferWithLength:static_cast<NSUInteger>(capacity) * 4 options:priv];
        e.alive[1] = [device_ newBufferWithLength:static_cast<NSUInteger>(capacity) * 4 options:priv];
        e.counters = [device_ newBufferWithLength:64 options:MTLResourceStorageModeShared];
        std::memset(e.counters.contents, 0, 64);
        e.args = [device_ newBufferWithLength:128 options:MTLResourceStorageModeShared];
        std::memset(e.args.contents, 0, 128);
        e.history = trail ? [device_ newBufferWithLength:static_cast<NSUInteger>(capacity) * trail * 16 options:priv] : nil;
        e.keys = sortCount ? [device_ newBufferWithLength:static_cast<NSUInteger>(sortCount) * 8 options:priv] : nil;
        e.lightOut = [device_ newBufferWithLength:5 * 16 options:MTLResourceStorageModeShared];
        std::memset(e.lightOut.contents, 0, 5 * 16);
        e.initialized = false;
        e.lastTime = -1;
        e.cur = 0;
    }
    if (item.isSubEmitter && !e.requests) {
        e.requests = [device_ newBufferWithLength:static_cast<NSUInteger>(kRequestCap) * 32 options:MTLResourceStorageModePrivate];
    }
    e.sorted = sorted;
    // Mesh surface for shape "mesh".
    const MeshSurface* src = item.surface.get();
    if (src != e.surfaceSource) {
        e.surfaceSource = src;
        e.triangles = 0;
        e.surfacePos = e.surfaceNrm = e.surfaceCdf = nil;
        if (src && src->triangles() > 0) {
            std::vector<simd_float4> pos(src->positions.size()), nrm(src->normals.size());
            for (size_t i = 0; i < pos.size(); ++i) {
                pos[i] = simd_make_float4(src->positions[i].x, src->positions[i].y, src->positions[i].z, 0);
                nrm[i] = simd_make_float4(src->normals[i].x, src->normals[i].y, src->normals[i].z, 0);
            }
            e.surfacePos = [device_ newBufferWithBytes:pos.data() length:pos.size() * 16 options:MTLResourceStorageModeShared];
            e.surfaceNrm = [device_ newBufferWithBytes:nrm.data() length:nrm.size() * 16 options:MTLResourceStorageModeShared];
            e.surfaceCdf = [device_ newBufferWithBytes:src->cdf.data() length:src->cdf.size() * 4 options:MTLResourceStorageModeShared];
            e.triangles = static_cast<uint32_t>(src->triangles());
        }
    }
    // Vector field texture.
    if (item.field && item.field->version != e.fieldVersion) {
        const VectorField& vf = *item.field;
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRGBA32Float;
        d.width = static_cast<NSUInteger>(vf.nx);
        d.height = static_cast<NSUInteger>(vf.ny);
        d.depth = static_cast<NSUInteger>(vf.nz);
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead;
        e.field = [device_ newTextureWithDescriptor:d];
        [e.field replaceRegion:MTLRegionMake3D(0, 0, 0, d.width, d.height, d.depth)
                   mipmapLevel:0
                         slice:0
                     withBytes:vf.data.data()
                   bytesPerRow:d.width * 16
                 bytesPerImage:d.width * d.height * 16];
        e.fieldVersion = vf.version;
    } else if (!item.field) {
        e.field = nil;
        e.fieldVersion = 0;
    }
    e.sheet = item.texture.empty() || !textures_ ? nil : textures_(item.texture, true);
    // Particle mesh.
    e.mesh = FxMesh{};
    if (e.facing == static_cast<int>(fx::GpuFacing::Mesh) && !item.particleMesh.empty()) {
        if (item.particleMesh.rfind("fx:", 0) == 0) {
            auto it = builtinMeshes_.find(item.particleMesh);
            if (it == builtinMeshes_.end()) {
                MeshData md;
                FxMesh m;
                if (fx::builtinParticleMesh(item.particleMesh, md)) {
                    m.vertices = [device_ newBufferWithBytes:md.vertices.data() length:md.vertices.size() * 4 options:MTLResourceStorageModeShared];
                    m.indices = [device_ newBufferWithBytes:md.indices.data() length:md.indices.size() * 4 options:MTLResourceStorageModeShared];
                    m.indexCount = static_cast<uint32_t>(md.indices.size());
                }
                it = builtinMeshes_.emplace(item.particleMesh, m).first;
            }
            e.mesh = it->second;
        } else if (meshes_) {
            e.mesh = meshes_(item.particleMesh);
        }
    }
    e.drawable = e.facing != static_cast<int>(fx::GpuFacing::Mesh) || (e.mesh.vertices && e.mesh.indexCount > 0);
    e.position = item.world.translation();
    e.light = em.light;
    e.lightRange = em.lightRange;
    e.lightShadows = em.lightShadows;
    e.lightColor = em.lightColor.xyz();
    e.expected = em.rate > 0.f ? std::max(1.f, em.rate * em.lifetime * 0.6f) : 300.f;
}

void MetalGpuParticles::step(id<MTLComputeCommandEncoder> enc, Emitter& e, const GpuEmitterItem& item, Emitter* sub, float dt,
                             float time, uint32_t spawn, bool writeTrail, const GpuStepUniforms& base, bool last) {
    (void)item;
    GpuStepUniforms st = base;
    st.capacity = e.capacity;
    st.cur = e.cur;
    st.spawn = std::min(spawn, e.capacity);
    st.seed = hash32((static_cast<uint64_t>(e.entity) << 32) ^ (e.steps * 0x9E3779B97F4A7C15ull) ^ frame_);
    st.requestCap = kRequestCap;
    st.triangles = e.triangles;
    st.writeTrail = writeTrail ? 1u : 0u;
    st.trailSlots = e.trailSlots;
    st.indexCount = e.mesh.indexCount;
    st.sortCount = e.sortCount;
    e.params.frame[0] = time;
    e.params.frame[1] = dt;
    e.params.frame[2] = static_cast<float>(st.spawn);
    e.params.frame[3] = static_cast<float>(e.steps);
    e.params.trail[2] = static_cast<float>(e.trailHead);
    [enc setBytes:&e.params length:sizeof(e.params) atIndex:0];
    [enc setBytes:&st length:sizeof(st) atIndex:14];
    [enc setBuffer:e.particles offset:0 atIndex:1];
    [enc setBuffer:e.dead offset:0 atIndex:2];
    [enc setBuffer:e.alive[e.cur] offset:0 atIndex:3];
    [enc setBuffer:e.alive[1 - e.cur] offset:0 atIndex:4];
    [enc setBuffer:e.counters offset:0 atIndex:5];
    [enc setBuffer:e.args offset:0 atIndex:6];
    [enc setBuffer:(e.requests ?: dummy_) offset:0 atIndex:7];
    [enc setBuffer:(sub ? sub->counters : dummy_) offset:0 atIndex:8];
    [enc setBuffer:(sub && sub->requests ? sub->requests : dummy_) offset:0 atIndex:9];
    [enc setBuffer:(e.history ?: dummy_) offset:0 atIndex:10];
    [enc setBuffer:(e.surfacePos ?: dummy_) offset:0 atIndex:11];
    [enc setBuffer:(e.surfaceNrm ?: dummy_) offset:0 atIndex:12];
    [enc setBuffer:(e.surfaceCdf ?: dummy_) offset:0 atIndex:13];
    [enc setTexture:(e.field ?: dummyField_) atIndex:2];
    const MTLSize one = MTLSizeMake(1, 1, 1);
    [enc setComputePipelineState:kernels_["gpuEmitArgs"]];
    [enc dispatchThreads:one threadsPerThreadgroup:one];
    [enc setComputePipelineState:kernels_["gpuEmit"]];
    [enc dispatchThreadgroupsWithIndirectBuffer:e.args indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc setComputePipelineState:kernels_["gpuSimArgs"]];
    [enc dispatchThreads:one threadsPerThreadgroup:one];
    [enc setComputePipelineState:kernels_["gpuSimulate"]];
    [enc dispatchThreadgroupsWithIndirectBuffer:e.args indirectBufferOffset:kArgsSimOffset threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [enc setComputePipelineState:kernels_["gpuFinalize"]];
    [enc dispatchThreads:one threadsPerThreadgroup:one];
    e.cur = 1 - e.cur;
    ++e.steps;
    if (!last) return;
    // The new alive list is now e.alive[e.cur]; keys and lights read it at index 4.
    [enc setBuffer:e.alive[e.cur] offset:0 atIndex:4];
    if (e.sorted && e.keys) {
        GpuStepUniforms ss = st;
        ss.cur = 1 - e.cur;  // gpuSortKeys reads the count of the list just written
        [enc setBytes:&ss length:sizeof(ss) atIndex:14];
        [enc setBuffer:e.keys offset:0 atIndex:15];
        [enc setComputePipelineState:kernels_["gpuSortKeys"]];
        [enc dispatchThreads:MTLSizeMake(e.sortCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        const uint32_t n = e.sortCount;
        simd_uint2 km = simd_make_uint2(0, 0);
        [enc setComputePipelineState:kernels_["gpuBitonicLocal"]];
        [enc setBytes:&km length:sizeof(km) atIndex:16];
        [enc dispatchThreadgroups:MTLSizeMake(n / 1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(512, 1, 1)];
        for (uint32_t k = 2048; k <= n; k <<= 1) {
            [enc setComputePipelineState:kernels_["gpuBitonicGlobal"]];
            for (uint32_t j = k >> 1; j >= 1024; j >>= 1) {
                simd_uint2 kj = simd_make_uint2(k, j);
                [enc setBytes:&kj length:sizeof(kj) atIndex:16];
                [enc dispatchThreads:MTLSizeMake(n / 2, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            }
            simd_uint2 merge = simd_make_uint2(k, 1);
            [enc setComputePipelineState:kernels_["gpuBitonicLocal"]];
            [enc setBytes:&merge length:sizeof(merge) atIndex:16];
            [enc dispatchThreadgroups:MTLSizeMake(n / 1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(512, 1, 1)];
        }
    }
    if (e.light > 0.f) {
        [enc setBuffer:e.lightOut offset:0 atIndex:17];
        [enc setComputePipelineState:kernels_["gpuLightReduce"]];
        [enc dispatchThreadgroups:one threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }
}

void MetalGpuParticles::simulate(id<MTLCommandBuffer> cmd, const FrameData& frame, id<MTLTexture> prevDepth,
                                 id<MTLTexture> prevNormals, const Mat4& prevViewProj, bool prevValid) {
    ++frame_;
    // Forget emitters that are gone.
    std::erase_if(emitters_, [&](const auto& kv) {
        return std::none_of(frame.gpuEmitters.begin(), frame.gpuEmitters.end(),
                            [&](const GpuEmitterItem& it) { return it.entity == kv.first; });
    });
    drawOrder_.clear();
    if (!ready_ || frame.gpuEmitters.empty()) return;
    // Parents before their sub-emitters (requests are consumed in the same frame).
    std::vector<const GpuEmitterItem*> items;
    for (const auto& it : frame.gpuEmitters) items.push_back(&it);
    std::unordered_map<EntityId, int> depth;
    for (int pass = 0; pass < 4; ++pass) {
        for (const auto* it : items) {
            if (it->subEmitter) depth[it->subEmitter] = std::max(depth[it->subEmitter], depth[it->entity] + 1);
        }
    }
    std::stable_sort(items.begin(), items.end(), [&](const GpuEmitterItem* a, const GpuEmitterItem* b) {
        return depth[a->entity] < depth[b->entity];
    });
    for (const auto* it : items) ensure(emitters_[it->entity], *it, cmd);
    for (auto& [id, e] : emitters_) e.requestsPerEvent = 1;
    for (const auto* it : items) {
        if (!it->subEmitter) continue;
        auto s = emitters_.find(it->subEmitter);
        if (s != emitters_.end()) s->second.requestsPerEvent = std::max<uint32_t>(s->second.requestsPerEvent,
                                                                               static_cast<uint32_t>(std::clamp(it->params.subEmitCount, 1, 1024)));
    }

    GpuStepUniforms base{};
    base.prevViewProj = simdMat(prevViewProj);
    base.prevInvViewProj = simdMat(prevViewProj.inverse());
    base.eye = simd_make_float4(frame.camera.eye.x, frame.camera.eye.y, frame.camera.eye.z, prevValid && prevDepth ? 1.f : 0.f);

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    enc.label = @"GPU particles";
    [enc setTexture:prevDepth atIndex:0];
    [enc setTexture:prevNormals atIndex:1];
    const double now = frame.time;
    for (const auto* itp : items) {
        const GpuEmitterItem& item = *itp;
        Emitter& e = emitters_[item.entity];
        Emitter* sub = nullptr;
        if (item.subEmitter) {
            auto s = emitters_.find(item.subEmitter);
            if (s != emitters_.end() && s->second.requests) sub = &s->second;
        }
        const ParticleEmitter& em = item.params;
        e.params = fx::packEmitter(item);
        e.params.extra[2] = static_cast<float>(e.requestsPerEvent);
        bool reset = !e.initialized || e.lastTime < 0 || now < e.lastTime - 1e-4 || now - e.lastTime > 2.0;
        if (reset) {
            [enc setComputePipelineState:kernels_["gpuInit"]];
            GpuStepUniforms st = base;
            st.capacity = e.capacity;
            st.trailSlots = e.trailSlots;
            [enc setBytes:&st length:sizeof(st) atIndex:14];
            [enc setBuffer:e.dead offset:0 atIndex:2];
            [enc setBuffer:e.counters offset:0 atIndex:5];
            [enc setBuffer:(e.history ?: dummy_) offset:0 atIndex:10];
            [enc dispatchThreads:MTLSizeMake(e.capacity, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            e.initialized = true;
            e.cur = 0;
            e.spawnCarry = 0;
            e.started = false;
            e.burstSeen = item.burstSerial;
            e.trailClock = 0;
            e.trailHead = 0;
            e.prevWorld = item.world;
            e.lastTime = now;
            std::memset(e.args.contents, 0, 128);
            if (em.prewarm && em.rate > 0.f && em.emitting) {
                // Start fully developed: simulate the emitter's lifetime at 30 Hz.
                float warm = std::min(em.lifetime * (1.f + em.lifetimeJitter), 8.f);
                int n = std::max(1, static_cast<int>(std::ceil(warm * 30.f)));
                for (int i = 0; i < n; ++i) {
                    float h = 1.f / 30.f;
                    e.spawnCarry += em.rate * h;
                    auto spawn = static_cast<uint32_t>(std::min(e.spawnCarry, 4.0e6f));
                    e.spawnCarry -= static_cast<float>(spawn);
                    bool trailStep = false;
                    if (e.trailSlots) {
                        e.trailClock += h;
                        if (e.trailClock >= e.params.trail[1]) {
                            e.trailClock = std::fmod(e.trailClock, std::max(e.params.trail[1], 1e-4f));
                            e.trailHead = (e.trailHead + 1) % e.trailSlots;
                            trailStep = true;
                        }
                    }
                    step(enc, e, item, sub, h, static_cast<float>(now) - static_cast<float>(n - i) * h, spawn, trailStep, base,
                         i == n - 1);
                }
                e.started = true;
                continue;  // fully developed at `now`
            }
            // Otherwise fall through: a zero-length first step emits the start burst right now.
        }
        double dt = now - e.lastTime;
        if (dt <= 1e-6 && e.started) {
            e.prevWorld = item.world;
            continue;
        }
        e.lastTime = now;
        int sub_steps = std::clamp(static_cast<int>(std::ceil(dt * 60.0)), 1, 4);
        float h = static_cast<float>(std::min(dt, 0.1) / sub_steps);
        Mat4 delta = item.world * e.prevWorld.inverse();
        for (int i = 0; i < sub_steps; ++i) {
            uint32_t spawn = 0;
            if (em.emitting) {
                e.spawnCarry += std::max(em.rate, 0.f) * h;
                spawn = static_cast<uint32_t>(std::min(e.spawnCarry, 4.0e6f));
                e.spawnCarry -= static_cast<float>(spawn);
                if (!e.started) spawn += static_cast<uint32_t>(std::max(em.burst, 0));
            }
            e.started = true;
            if (i == 0 && item.burstSerial > e.burstSeen) {
                spawn += static_cast<uint32_t>(std::min<uint64_t>(item.burstSerial - e.burstSeen, kMaxCapacity));
                e.burstSeen = item.burstSerial;
            }
            bool trailStep = false;
            if (e.trailSlots) {
                e.trailClock += h;
                if (e.trailClock >= e.params.trail[1]) {
                    e.trailClock = std::fmod(e.trailClock, std::max(e.params.trail[1], 1e-4f));
                    e.trailHead = (e.trailHead + 1) % e.trailSlots;
                    trailStep = true;
                }
            }
            std::memcpy(e.params.delta, (i == 0 ? delta : Mat4{}).m, sizeof(e.params.delta));
            step(enc, e, item, sub, h, static_cast<float>(now - dt + h * (i + 1)), spawn, trailStep, base, i == sub_steps - 1);
        }
        std::memcpy(e.params.delta, Mat4{}.m, sizeof(e.params.delta));
        e.prevWorld = item.world;
    }
    [enc endEncoding];
    // Draw order: far emitters first.
    for (const auto* it : items) drawOrder_.push_back(it->entity);
    Vec3 eye = frame.camera.eye;
    std::stable_sort(drawOrder_.begin(), drawOrder_.end(), [&](EntityId a, EntityId b) {
        return distance(eye, emitters_[a].position) > distance(eye, emitters_[b].position);
    });
}

void MetalGpuParticles::encodeShadowCaster(id<MTLRenderCommandEncoder> enc) {
    if (!ready_) return;
    bool bound = false;
    for (EntityId id : drawOrder_) {
        Emitter& e = emitters_[id];
        if (e.facing != static_cast<int>(fx::GpuFacing::Mesh) || !e.drawable || e.steps == 0) continue;
        if (!bound) {
            [enc setRenderPipelineState:meshShadowPipeline_];
            bound = true;
        }
        [enc setVertexBuffer:e.mesh.vertices offset:0 atIndex:0];
        [enc setVertexBytes:&e.params length:sizeof(e.params) atIndex:3];
        [enc setVertexBuffer:e.particles offset:0 atIndex:4];
        [enc setVertexBuffer:e.alive[e.cur] offset:0 atIndex:5];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:e.mesh.indices
                 indexBufferOffset:0
                    indirectBuffer:e.args
              indirectBufferOffset:kArgsIndexedOffset];
    }
}

void MetalGpuParticles::encodeOpaque(id<MTLRenderCommandEncoder> enc) {
    if (!ready_) return;
    bool bound = false;
    for (EntityId id : drawOrder_) {
        Emitter& e = emitters_[id];
        if (e.facing != static_cast<int>(fx::GpuFacing::Mesh) || !e.drawable || e.steps == 0) continue;
        if (!bound) {
            [enc setRenderPipelineState:meshPipeline_];
            [enc setDepthStencilState:depthWrite_];
            [enc setCullMode:MTLCullModeNone];
            bound = true;
        }
        FxDrawUniforms du{};
        du.model = matrix_identity_float4x4;
        du.normalMatrix = matrix_identity_float4x4;
        du.color = simd_make_float4(1, 1, 1, 1);
        du.emissive = simd_make_float4(0, 0, 0, 0);
        du.material = simd_make_float4(e.params.material[1], e.params.material[0], 0, 0);
        du.material2 = simd_make_float4(1, 1, 1, 0);
        du.material3 = simd_make_float4(0, 0.45f, 0, 0);  // thin, translucent things (leaves, petals)
        [enc setVertexBuffer:e.mesh.vertices offset:0 atIndex:0];
        [enc setVertexBytes:&e.params length:sizeof(e.params) atIndex:3];
        [enc setVertexBuffer:e.particles offset:0 atIndex:4];
        [enc setVertexBuffer:e.alive[e.cur] offset:0 atIndex:5];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        for (NSUInteger t : {0u, 2u, 3u, 4u}) [enc setFragmentTexture:white_ atIndex:t];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:e.mesh.indices
                 indexBufferOffset:0
                    indirectBuffer:e.args
              indirectBufferOffset:kArgsIndexedOffset];
    }
}

bool MetalGpuParticles::hasTransparent() const {
    if (!ready_) return false;
    for (EntityId id : drawOrder_) {
        auto it = emitters_.find(id);
        if (it != emitters_.end() && it->second.facing != static_cast<int>(fx::GpuFacing::Mesh) && it->second.drawable) return true;
    }
    return false;
}

void MetalGpuParticles::encodeTransparent(id<MTLCommandBuffer> cmd, const FrameData& frame, const FxSceneInputs& in) {
    if (!ready_ || !in.target) return;
    (void)frame;
    const NSUInteger w = in.target.width, h = in.target.height;
    if (!reactive_ || reactive_.width != w || reactive_.height != h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        reactive_ = [device_ newTextureWithDescriptor:d];
    }
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = in.target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].texture = reactive_;
    rp.colorAttachments[1].loadAction = MTLLoadActionClear;
    rp.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    enc.label = @"GPU particles";
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBytes:in.frameUniforms length:in.frameUniformsSize atIndex:4];
    [enc setFragmentBytes:in.frameUniforms length:in.frameUniformsSize atIndex:1];
    [enc setFragmentBytes:in.lights length:in.lightsSize atIndex:2];
    [enc setFragmentTexture:in.shadowAtlas atIndex:1];
    [enc setFragmentTexture:in.envCube atIndex:5];
    [enc setFragmentTexture:in.sceneDepth atIndex:7];
    [enc setFragmentTexture:in.localShadows atIndex:32];
    for (EntityId id : drawOrder_) {
        Emitter& e = emitters_[id];
        if (e.facing == static_cast<int>(fx::GpuFacing::Mesh) || !e.drawable || e.steps == 0) continue;
        bool ribbon = e.facing == static_cast<int>(fx::GpuFacing::Ribbon);
        [enc setRenderPipelineState:ribbon ? ribbonPipeline_ : quadPipeline_];
        GpuDrawUniforms dr{e.sorted ? 2u : 1u, e.sorted ? 1u : 0u, e.trailSlots, 0u};
        [enc setVertexBytes:&e.params length:sizeof(e.params) atIndex:0];
        [enc setVertexBuffer:e.particles offset:0 atIndex:1];
        [enc setVertexBuffer:(e.sorted ? e.keys : e.alive[e.cur]) offset:0 atIndex:2];
        [enc setVertexBytes:&dr length:sizeof(dr) atIndex:3];
        [enc setVertexBuffer:(e.history ?: dummy_) offset:0 atIndex:5];
        [enc setFragmentBytes:&e.params length:sizeof(e.params) atIndex:0];
        [enc setFragmentTexture:(e.sheet ?: white_) atIndex:8];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:e.args indirectBufferOffset:kArgsDrawOffset];
    }
    [enc endEncoding];
}

std::vector<LightItem> MetalGpuParticles::lights() const {
    std::vector<LightItem> out;
    for (const auto& [id, e] : emitters_) {
        if (e.light <= 0.f || !e.lightOut || e.steps == 0) continue;
        const auto* v = static_cast<const simd_float4*>(e.lightOut.contents);
        float alive = v[4].x, total = 0;
        for (int k = 0; k < 4; ++k) total += v[k].w;
        if (alive < 1.f || total <= 0.f) continue;
        float strength = e.light * std::min(1.5f, std::sqrt(alive / e.expected));
        for (int k = 0; k < 4; ++k) {
            if (v[k].w < total * 0.04f) continue;
            LightItem li;
            li.kind = LightItem::Kind::Point;
            li.position = {v[k].x, v[k].y, v[k].z};
            li.color = e.lightColor;
            li.intensity = strength * v[k].w / total;
            li.range = e.lightRange;
            li.id = lightId(id, 3) | (static_cast<uint64_t>(k) << 48);
            li.shadows = e.lightShadows;
            out.push_back(li);
        }
    }
    return out;
}

Json MetalGpuParticles::stats() const {
    Json arr = Json::array();
    for (const auto& [id, e] : emitters_) {
        uint32_t alive = e.counters ? static_cast<const uint32_t*>(e.counters.contents)[6] : 0;
        arr.push(Json::object({{"entity", id},
                               {"alive", alive},
                               {"capacity", e.capacity},
                               {"sorted", e.sorted},
                               {"memoryMB", (static_cast<double>(e.capacity) * (64 + 12 + 16.0 * e.trailSlots) +
                                             static_cast<double>(e.sortCount) * 8) / (1024.0 * 1024.0)}}));
    }
    return arr;
}

}  // namespace sky
