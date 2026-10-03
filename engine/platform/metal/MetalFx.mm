// MetalFx: owns the GPU particle and hair subsystems and exposes the renderer hooks.

#include "MetalFx.h"

#include "MetalFxInternal.h"

namespace sky {

struct MetalFx::Timing {
    FxGpuTimer frame;
};

MetalFx::MetalFx(id<MTLDevice> device, id<MTLCommandQueue> queue, MeshLookup meshes, TextureLookup textures)
    : device_(device), queue_(queue), timing_(std::make_shared<Timing>()) {
    particles_ = std::make_unique<MetalGpuParticles>(device, meshes, textures);
    hair_ = std::make_unique<MetalHair>(device, meshes);
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:1 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    black_ = [device_ newTextureWithDescriptor:d];
    const uint8_t zero = 0;
    [black_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:1];
}

MetalFx::~MetalFx() = default;

void MetalFx::build(id<MTLLibrary> library, const FxFormats& formats) {
    particles_->build(library, formats);
    hair_->build(library, formats);
}

void MetalFx::simulate(const FrameData& frame, id<MTLTexture> prevDepth, id<MTLTexture> prevNormals, const Mat4& prevViewProj,
                       bool prevValid) {
    reactiveUsed_ = false;
    // Separate command buffers (committed before the frame's, on the same queue) so their GPU
    // time is measured on its own.
    if (particles_->ready() && !frame.gpuEmitters.empty()) {
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        cmd.label = @"GPU particles";
        particles_->simulate(cmd, frame, prevDepth, prevNormals, prevViewProj, prevValid);
        particles_->timer.track(cmd);
        [cmd commit];
    } else {
        particles_->simulate(nil, frame, prevDepth, prevNormals, prevViewProj, prevValid);  // forgets old emitters
    }
    if (hair_->ready() && !frame.grooms.empty()) {
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        cmd.label = @"Hair";
        hair_->simulate(cmd, frame);
        hair_->timer.track(cmd);
        [cmd commit];
    } else {
        hair_->simulate(nil, frame);
    }
}

void MetalFx::encodeShadowCaster(id<MTLRenderCommandEncoder> enc, const FrameData& frame, simd_float4x4 lightViewProj) {
    hair_->encodeShadowCaster(enc, frame, lightViewProj);
    particles_->encodeShadowCaster(enc);
}

void MetalFx::encodeOpaque(id<MTLRenderCommandEncoder> enc, const FrameData& frame) {
    (void)frame;
    hair_->encodeOpaque(enc);
    particles_->encodeOpaque(enc);
}

bool MetalFx::hasTransparent(const FrameData& frame) const { return !frame.gpuEmitters.empty() && particles_->hasTransparent(); }

void MetalFx::encodeTransparent(id<MTLCommandBuffer> cmd, const FrameData& frame, const FxSceneInputs& in) {
    if (!hasTransparent(frame)) return;
    particles_->encodeTransparent(cmd, frame, in);
    reactiveUsed_ = particles_->reactive() != nil;
}

id<MTLTexture> MetalFx::reactiveMask() const { return reactiveUsed_ ? particles_->reactive() : black_; }

void MetalFx::trackFrame(id<MTLCommandBuffer> cmd) { timing_->frame.track(cmd); }

std::vector<LightItem> MetalFx::effectLights() const { return particles_->lights(); }

Json MetalFx::stats() const {
    return Json::object({{"frameGpuMs", timing_->frame.value()},
                         {"particlesGpuMs", particles_->timer.value()},
                         {"hairGpuMs", hair_->timer.value()},
                         {"gpuParticles", particles_->ready()},
                         {"hair", hair_->ready()},
                         {"emitters", particles_->stats()},
                         {"grooms", hair_->stats()}});
}

}  // namespace sky
