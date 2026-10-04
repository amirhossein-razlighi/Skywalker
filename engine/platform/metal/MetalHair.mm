// Strand hair and fur: GPU guide simulation, child interpolation, deep opacity maps and
// strand / card rendering (Hair.metal).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MetalFxInternal.h"
#include "MetalProfiler.h"     // [profiler] per-pass GPU timing
#include "MetalShaderCache.h"  // [shader cache] pipelines through the binary archive
#include "skywalker/core/Log.h"
#include "skywalker/fx/Groom.h"

namespace sky {

namespace {

simd_float4x4 simdMat(const Mat4& m) {
    simd_float4x4 r;
    std::memcpy(&r, m.m, sizeof(float) * 16);
    return r;
}

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
simd_float4 lin3(Vec4 c, float w) { return simd_make_float4(toLinear(c.x), toLinear(c.y), toLinear(c.z), w); }

float modelScale(const Mat4& m) {
    return (length(m.transformDir({1, 0, 0})) + length(m.transformDir({0, 1, 0})) + length(m.transformDir({0, 0, 1}))) / 3.f;
}

bool sameMatrix(const Mat4& a, const Mat4& b) { return std::memcmp(a.m, b.m, sizeof(a.m)) == 0; }

}  // namespace

MetalHair::MetalHair(id<MTLDevice> device, MetalFx::MeshLookup meshes) : device_(device), meshes_(std::move(meshes)) {}

bool MetalHair::build(id<MTLLibrary> lib, const FxFormats& fmt) {
    ready_ = false;
    kernels_.clear();
    shadowTile_ = fmt.shadowTileTexels;
    NSError* err = nil;
    for (const char* k : {"hairReset", "hairSimulate", "hairInterpolate"}) {
        id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:k]];
        id<MTLComputePipelineState> ps = f ? newComputePipeline(device_, f, &err) : nil;
        if (!ps) {
            log::warn("render", std::string("hair disabled: missing or invalid kernel ") + k);
            return false;
        }
        kernels_[k] = ps;
    }
    auto fn = [&](const char* n) -> id<MTLFunction> { return n ? [lib newFunctionWithName:[NSString stringWithUTF8String:n]] : nil; };
    auto mainPass = [&](const char* vs, const char* fs) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn(vs);
        d.fragmentFunction = fn(fs);
        if (!d.vertexFunction || !d.fragmentFunction) return nil;
        d.rasterSampleCount = fmt.samples;
        d.colorAttachments[0].pixelFormat = fmt.hdr;
        d.colorAttachments[1].pixelFormat = fmt.gbufA;
        d.colorAttachments[2].pixelFormat = fmt.gbufB;
        d.colorAttachments[3].pixelFormat = fmt.velocity;
        d.depthAttachmentPixelFormat = fmt.depth;
        return newRenderPipeline(device_, d, &err);
    };
    auto depthOnly = [&](const char* vs, const char* fs) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn(vs);
        d.fragmentFunction = fn(fs);
        if (!d.vertexFunction || (fs && !d.fragmentFunction)) return nil;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        return newRenderPipeline(device_, d, &err);
    };
    strandPipeline_ = mainPass("hairVertex", "hairFragment");

    cardPipeline_ = mainPass("hairCardVertex", "hairCardFragment");
    shadowPipeline_ = depthOnly("hairShadowVertex", "hairShadowFragment");
    domDepthPipeline_ = depthOnly("hairDomVertex", "hairDomDepthFragment");
    domMeshPipeline_ = depthOnly("hairDomMeshVertex", nullptr);
    MTLRenderPipelineDescriptor* dd = [MTLRenderPipelineDescriptor new];
    dd.vertexFunction = fn("hairDomVertex");
    dd.fragmentFunction = fn("hairDomDensityFragment");
    dd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    dd.colorAttachments[0].blendingEnabled = YES;
    dd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    dd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    dd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    dd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    domDensityPipeline_ = dd.vertexFunction && dd.fragmentFunction ? newRenderPipeline(device_, dd, &err) : nil;
    if (!strandPipeline_ || !cardPipeline_ || !shadowPipeline_ || !domDepthPipeline_ || !domMeshPipeline_ || !domDensityPipeline_) {
        log::warn("render", "hair disabled: " + (err ? std::string(err.localizedDescription.UTF8String) : std::string("missing shaders")));
        return false;
    }
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    depthWrite_ = [device_ newDepthStencilStateWithDescriptor:ds];

    ready_ = true;
    return true;
}

void MetalHair::upload(GroomGpu& g, const GroomItem& item) {
    const GroomData& d = *item.data;
    g.hash = d.hash;
    g.P = d.points;
    g.G = static_cast<uint32_t>(d.guideCount());
    g.N = static_cast<uint32_t>(d.strandCount());
    std::vector<simd_float4> rest(d.guideRest.size());
    for (size_t i = 0; i < rest.size(); ++i) rest[i] = simd_make_float4(d.guideRest[i].x, d.guideRest[i].y, d.guideRest[i].z, 0.f);
    g.rest = [device_ newBufferWithBytes:rest.data() length:rest.size() * 16 options:MTLResourceStorageModeShared];
    const NSUInteger guideBytes = static_cast<NSUInteger>(g.G) * g.P * 16;
    g.pos = [device_ newBufferWithLength:guideBytes options:MTLResourceStorageModePrivate];
    g.prev = [device_ newBufferWithLength:guideBytes options:MTLResourceStorageModePrivate];
    std::vector<HairChildGpu> ch(d.children.size());
    for (size_t i = 0; i < ch.size(); ++i) {
        const auto& c = d.children[i];
        ch[i].root = simd_make_float4(c.root.x, c.root.y, c.root.z, c.lengthScale);
        ch[i].weights = simd_make_float4(c.weight[0], c.weight[1], c.weight[2], c.random);
        uint32_t wbits;
        std::memcpy(&wbits, &c.width, 4);
        ch[i].guides = simd_make_uint4(c.guide[0], c.guide[1], c.guide[2], wbits);
    }
    g.children = [device_ newBufferWithBytes:ch.data() length:ch.size() * sizeof(HairChildGpu) options:MTLResourceStorageModeShared];
    std::vector<__fp16> off(d.offsets.size() * 4);
    for (size_t i = 0; i < d.offsets.size(); ++i) {
        off[i * 4 + 0] = static_cast<__fp16>(d.offsets[i].x);
        off[i * 4 + 1] = static_cast<__fp16>(d.offsets[i].y);
        off[i * 4 + 2] = static_cast<__fp16>(d.offsets[i].z);
        off[i * 4 + 3] = static_cast<__fp16>(0.f);
    }
    g.offsets = [device_ newBufferWithBytes:off.data() length:off.size() * 2 options:MTLResourceStorageModeShared];
    g.render = [device_ newBufferWithLength:static_cast<NSUInteger>(g.N) * g.P * 16 options:MTLResourceStorageModePrivate];
    g.renderPrev = [device_ newBufferWithLength:static_cast<NSUInteger>(g.N) * g.P * 16 options:MTLResourceStorageModePrivate];
    g.motion = false;
    if (!g.domDepth) {
        auto depthTex = [&]() {
            MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:kDomSize
                                                                                        height:kDomSize
                                                                                     mipmapped:NO];
            t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            t.storageMode = MTLStorageModePrivate;
            return [device_ newTextureWithDescriptor:t];
        };
        g.domDepth = depthTex();
        g.domOpaque = depthTex();
        MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:kDomSize
                                                                                    height:kDomSize
                                                                                 mipmapped:NO];
        t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        t.storageMode = MTLStorageModePrivate;
        g.domDensity = [device_ newTextureWithDescriptor:t];
    }
    g.posValid = false;
    g.interpolated = false;
}

void MetalHair::simulate(id<MTLCommandBuffer> cmd, const FrameData& frame) {
    std::erase_if(grooms_, [&](const auto& kv) {
        return std::none_of(frame.grooms.begin(), frame.grooms.end(), [&](const GroomItem& it) { return it.entity == kv.first; });
    });
    order_.clear();
    if (!ready_ || frame.grooms.empty()) return;
    const ViewCamera& cam = frame.camera;
    const float H = static_cast<float>(std::max(frame.height, 1));
    const bool ortho = cam.orthographic;
    const float pixelAt1m = ortho ? cam.orthoSize * 2.f / H : 2.f * std::tan(radians(cam.fovDeg) * 0.5f) / H;
    id<MTLComputeCommandEncoder> enc = profiledCompute(cmd, "Hair simulation", "hair");
    for (const GroomItem& item : frame.grooms) {
        if (!item.data || item.data->children.empty() || item.data->points < 3) continue;
        GroomGpu& g = grooms_[item.entity];
        if (g.hash != item.data->hash || !g.rest) upload(g, item);
        const Groom& p = item.params;
        const GroomData& d = *item.data;
        const float scale = modelScale(item.model);
        // Bounds (world) with room for motion.
        g.center = item.model.transformPoint(d.bounds.center());
        g.radius = length(d.bounds.extents()) * scale + p.length * 0.25f * scale + 0.02f;
        // Level of detail.
        float dist = std::max(distance(cam.eye, g.center), 0.01f);
        float screenPx = 2.f * g.radius / (pixelAt1m * (ortho ? 1.f : dist));
        g.cards = p.lod == "cards" || (p.lod == "auto" && screenPx < p.cardsBelow);
        float widthM = p.widthRoot * 0.001f * scale;
        float ratio = widthM / (pixelAt1m * (ortho ? 1.f : dist));
        float fraction = 1.f;
        if (frame.samples <= 1 && !frame.offline.enabled && g.N > 20000) {
            // Real time: thin strands far away merge into the coverage of fewer, more opaque
            // ones, and the strand count follows the groom's size on screen (geometry is the cost
            // on tile-based GPUs). Stills (accumulated sub-samples) always draw every strand.
            fraction = std::clamp(12.f * ratio, 0.3f, 1.f);
            float budget = std::clamp(screenPx * 36.f, 6000.f, static_cast<float>(g.N));
            fraction = std::min(fraction, budget / static_cast<float>(g.N));
        }
        if (g.cards) fraction = std::min(fraction, 0.3f);
        g.drawn = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<float>(g.N) * fraction));
        g.castShadows = p.castShadows;
        HairParamsUniforms& u = g.params;
        u.model = simdMat(item.model);
        u.dims = simd_make_float4(static_cast<float>(g.P), static_cast<float>(g.drawn), static_cast<float>(g.G), scale);
        u.width = simd_make_float4(p.widthRoot * 0.001f, p.widthTip * 0.001f, p.density,
                                   static_cast<float>(g.N) / static_cast<float>(g.drawn));
        Vec3 sigma = fx::hairAbsorption(p.melanin, p.redness);
        u.sigma = simd_make_float4(sigma.x, sigma.y, sigma.z, p.colorVariation);
        u.dye = lin3(p.dye, p.specular);
        u.rootColor = lin3(p.rootColor, std::clamp(p.roughness, 0.02f, 1.f));
        u.tipColor = lin3(p.tipColor, std::clamp(p.radialRoughness, 0.05f, 1.f));
        u.shade = simd_make_float4(p.scatter, radians(p.cuticleTilt), 1.f, 0.f);
        u.view = simd_make_float4(pixelAt1m, ortho ? 1.f : 0.f, 0.f, 0.f);
        u.wind = simd_make_float4(item.wind.x, item.wind.y, item.wind.z, 0.35f);
        u.cards = simd_make_float4(d.spacing * scale * 2.4f, 28.f, 0.f, 0.f);
        int nc = 0;
        for (const FxCollider& c : item.colliders) {
            if (nc >= 8) break;
            u.colliders[nc * 2] = simd_make_float4(c.a.x, c.a.y, c.a.z, static_cast<float>(c.kind));
            u.colliders[nc * 2 + 1] = simd_make_float4(c.b.x, c.b.y, c.b.z, c.radius);
            ++nc;
        }
        u.sim2 = simd_make_float4(p.rootStiffness, 9.81f, static_cast<float>(nc), 2.2f * p.wind);

        // Simulation.
        const double now = frame.time;
        const bool moved = !sameMatrix(g.lastModel, item.model);
        bool changed = false;
        [enc setBytes:&u length:sizeof(u) atIndex:0];
        [enc setBuffer:g.rest offset:0 atIndex:1];
        [enc setBuffer:g.pos offset:0 atIndex:2];
        [enc setBuffer:g.prev offset:0 atIndex:3];
        const NSUInteger guidePoints = static_cast<NSUInteger>(g.G) * g.P;
        bool reset = !g.posValid || g.lastTime < 0 || now < g.lastTime - 1e-4 || now - g.lastTime > 2.0;
        if (reset || (!p.simulate && moved)) {
            u.sim = simd_make_float4(0.f, static_cast<float>(now), p.damping, p.stiffness);
            [enc setBytes:&u length:sizeof(u) atIndex:0];
            [enc setComputePipelineState:kernels_["hairReset"]];
            [enc dispatchThreads:MTLSizeMake(guidePoints, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            g.posValid = true;
            changed = true;
            if (reset && p.simulate) {
                // Settle: let gravity and collisions relax the rest pose before the first frame.
                [enc setComputePipelineState:kernels_["hairSimulate"]];
                for (int i = 0; i < 45; ++i) {
                    u.sim = simd_make_float4(1.f / 60.f, static_cast<float>(now) - (45 - i) / 60.f, std::max(p.damping, 0.35f), p.stiffness);
                    [enc setBytes:&u length:sizeof(u) atIndex:0];
                    [enc dispatchThreads:MTLSizeMake(g.G, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                }
            }
        } else if (p.simulate && now - g.lastTime > 1e-6) {
            double dt = now - g.lastTime;
            int sub = std::clamp(static_cast<int>(std::ceil(dt * 120.0)), 1, 6);
            float h = static_cast<float>(std::min(dt, 0.1) / sub);
            [enc setComputePipelineState:kernels_["hairSimulate"]];
            for (int i = 0; i < sub; ++i) {
                u.sim = simd_make_float4(h, static_cast<float>(now - dt + h * (i + 1)), p.damping, p.stiffness);
                [enc setBytes:&u length:sizeof(u) atIndex:0];
                [enc dispatchThreads:MTLSizeMake(g.G, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            }
            changed = true;
        }
        g.lastTime = now;
        g.lastModel = item.model;
        // Rebuild the rendered strands from the guides.
        g.motion = false;
        if (changed || !g.interpolated || g.drawn != g.lastDrawn) {
            // Velocity buffer: keep last frame's strands so moving hair gets motion vectors.
            const bool keepPrev = g.interpolated && g.drawn == g.lastDrawn && !reset;
            if (keepPrev) std::swap(g.render, g.renderPrev);
            g.motion = keepPrev;
            [enc setComputePipelineState:kernels_["hairInterpolate"]];
            [enc setBytes:&u length:sizeof(u) atIndex:0];
            [enc setBuffer:g.children offset:0 atIndex:1];
            [enc setBuffer:g.offsets offset:0 atIndex:2];
            [enc setBuffer:g.pos offset:0 atIndex:3];
            [enc setBuffer:g.render offset:0 atIndex:4];
            [enc dispatchThreads:MTLSizeMake(g.drawn, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            g.interpolated = true;
            g.lastDrawn = g.drawn;
        }
        order_.push_back(item.entity);
    }
    [enc endEncoding];
}

void MetalHair::renderShadowMaps(id<MTLCommandBuffer> cmd, const FrameData& frame) {
    if (!ready_) return;
    for (const GroomItem& item : frame.grooms) {
        auto it = grooms_.find(item.entity);
        if (it != grooms_.end() && it->second.posValid) renderDom(cmd, it->second, item, frame);
    }
}

void MetalHair::renderDom(id<MTLCommandBuffer> cmd, GroomGpu& g, const GroomItem& item, const FrameData& frame) {
    HairParamsUniforms& u = g.params;
    const Environment& env = frame.environment;
    if (env.sunElevation < -5.f || env.sunIntensity <= 0.f) {
        u.dom = simd_make_float4(0, 0, 0, 0);
        return;
    }
    Vec3 L = env.sunDirection();  // direction light travels
    const float r = g.radius;
    Vec3 eye = g.center - L * (r * 2.5f);
    Mat4 view = Mat4::lookAt(eye, g.center, std::fabs(L.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    const float nearP = r * 0.5f, farP = r * 4.5f;
    Mat4 vp = Mat4::orthographic(r, 1.f, nearP, farP) * view;
    u.domViewProj = simdMat(vp);
    const float texel = 2.f * r / static_cast<float>(kDomSize);
    u.dom = simd_make_float4(std::clamp(r * 0.012f, 0.0015f, 0.03f), farP - nearP, texel, 1.f);
    simd_float4 lightInfo = simd_make_float4(L.x, L.y, L.z, texel);
    const MTLViewport vpt{0, 0, static_cast<double>(kDomSize), static_cast<double>(kDomSize), 0, 1};
    // A uniform subset of ~16k strands (with proportionally more coverage) is plenty for the map.
    HairParamsUniforms du = u;
    const uint32_t domStride = std::max<uint32_t>(1, g.drawn / 8000);
    du.cards.z = static_cast<float>(domStride);
    const uint32_t domCount = std::max<uint32_t>(1, g.drawn / domStride);

    // 1. Opaque meshes around the groom (head, shoulders): their depth from the sun.
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = g.domOpaque;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Hair DOM opaque", "hair");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Hair DOM opaque";
        [enc setViewport:vpt];
        [enc setRenderPipelineState:domMeshPipeline_];
        [enc setDepthStencilState:depthWrite_];
        [enc setCullMode:MTLCullModeNone];
        const float reach = r * 1.6f;
        for (const DrawItem& dr : frame.draws) {
            if (!dr.castShadows || dr.surface.color.w < 0.5f) continue;
            Vec3 c = dr.worldBounds.center();
            float br = length(dr.worldBounds.extents());
            if (distance(c, g.center) > reach + br) continue;
            FxMesh m = meshes_ ? meshes_(dr.mesh) : FxMesh{};
            if (!m.vertices) continue;
            simd_float4x4 mvp = simdMat(vp * dr.model);
            [enc setVertexBuffer:m.vertices offset:0 atIndex:0];
            [enc setVertexBytes:&mvp length:sizeof(mvp) atIndex:1];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:m.indexCount
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:m.indices
                     indexBufferOffset:0];
        }
        [enc endEncoding];
    }
    // 2. Nearest hair depth.
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = g.domDepth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Hair DOM depth", "hair");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Hair DOM depth";
        [enc setViewport:vpt];
        [enc setRenderPipelineState:domDepthPipeline_];
        [enc setDepthStencilState:depthWrite_];
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:3];
        [enc setVertexBuffer:g.render offset:0 atIndex:4];
        [enc setVertexBuffer:g.children offset:0 atIndex:5];
        [enc setVertexBytes:&lightInfo length:sizeof(lightInfo) atIndex:6];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:g.P * 2 instanceCount:domCount];
        [enc endEncoding];
    }
    // 3. Cumulative density in 4 layers behind the nearest hair.
    {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g.domDensity;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        profileRenderPass(rp, "Hair DOM density", "hair");
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        enc.label = @"Hair DOM density";
        [enc setViewport:vpt];
        [enc setRenderPipelineState:domDensityPipeline_];
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBytes:&du length:sizeof(du) atIndex:3];
        [enc setVertexBuffer:g.render offset:0 atIndex:4];
        [enc setVertexBuffer:g.children offset:0 atIndex:5];
        [enc setVertexBytes:&lightInfo length:sizeof(lightInfo) atIndex:6];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:3];
        [enc setFragmentTexture:g.domDepth atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:g.P * 2 instanceCount:domCount];
        [enc endEncoding];
    }
}

void MetalHair::encodeShadowCaster(id<MTLRenderCommandEncoder> enc, const FrameData& frame, simd_float4x4 lvp) {
    if (!ready_) return;
    Vec3 L = frame.environment.sunDirection();
    // Orthographic cascade: |row 0| of the rotation part = 1 / half width.
    float row0 = std::sqrt(lvp.columns[0][0] * lvp.columns[0][0] + lvp.columns[1][0] * lvp.columns[1][0] +
                           lvp.columns[2][0] * lvp.columns[2][0]);
    float halfWidth = row0 > 1e-8f ? 1.f / row0 : 1.f;
    float texel = 2.f * halfWidth / static_cast<float>(shadowTile_);
    simd_float4 lightInfo = simd_make_float4(L.x, L.y, L.z, texel);
    bool bound = false;
    for (EntityId id : order_) {
        GroomGpu& g = grooms_[id];
        if (!g.castShadows || !g.interpolated) continue;
        simd_float4 c = simd_mul(lvp, simd_make_float4(g.center.x, g.center.y, g.center.z, 1.f));
        float rr = g.radius / halfWidth;
        if (c.x < -1.f - rr || c.x > 1.f + rr || c.y < -1.f - rr || c.y > 1.f + rr) continue;
        if (!bound) {
            [enc setRenderPipelineState:shadowPipeline_];
            bound = true;
        }
        HairParamsUniforms u = g.params;
        // Every 2nd/3rd strand (with that much more coverage) is plenty for the sun's shadow.
        const uint32_t stride = g.drawn > 60000 ? 3 : (g.drawn > 20000 ? 2 : 1);
        u.cards.z = static_cast<float>(stride);
        const uint32_t count = std::max<uint32_t>(1, g.drawn / stride);
        [enc setVertexBytes:&u length:sizeof(u) atIndex:3];
        [enc setVertexBuffer:g.render offset:0 atIndex:4];
        [enc setVertexBuffer:g.children offset:0 atIndex:5];
        [enc setVertexBytes:&lightInfo length:sizeof(lightInfo) atIndex:6];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:g.P * 2 instanceCount:count];
    }
}

void MetalHair::encodeOpaque(id<MTLRenderCommandEncoder> enc) {
    if (!ready_ || order_.empty()) return;
    [enc setDepthStencilState:depthWrite_];
    [enc setCullMode:MTLCullModeNone];
    for (EntityId id : order_) {
        GroomGpu& g = grooms_[id];
        if (!g.interpolated) continue;
        [enc setVertexBytes:&g.params length:sizeof(g.params) atIndex:3];
        [enc setFragmentBytes:&g.params length:sizeof(g.params) atIndex:5];  // 2-4: lights + clusters (renderer)
        [enc setFragmentTexture:g.domDepth atIndex:9];
        [enc setFragmentTexture:g.domOpaque atIndex:10];
        [enc setFragmentTexture:g.domDensity atIndex:11];
        if (g.cards) {
            [enc setRenderPipelineState:cardPipeline_];
            [enc setVertexBuffer:g.pos offset:0 atIndex:4];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:g.P * 2 instanceCount:g.G];
        } else {
            [enc setVertexBuffer:g.render offset:0 atIndex:4];
            [enc setVertexBuffer:g.children offset:0 atIndex:5];
            [enc setVertexBuffer:(g.motion ? g.renderPrev : g.render) offset:0 atIndex:6];  // previous strands
            [enc setRenderPipelineState:strandPipeline_];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:g.P * 2 instanceCount:g.drawn];
        }
    }
}

Json MetalHair::stats() const {
    Json arr = Json::array();
    for (const auto& [id, g] : grooms_) {
        double gpuMB = (static_cast<double>(g.G) * g.P * 16 * 3 + static_cast<double>(g.N) * (48 + g.P * 8.0 + g.P * 32.0) +
                        kDomSize * kDomSize * 16.0) / (1024.0 * 1024.0);
        arr.push(Json::object({{"entity", id},
                               {"strands", g.N},
                               {"drawn", g.drawn},
                               {"guides", g.G},
                               {"pointsPerStrand", g.P},
                               {"cards", g.cards},
                               {"gpuMemoryMB", gpuMB}}));
    }
    return arr;
}

}  // namespace sky
