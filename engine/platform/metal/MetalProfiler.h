#pragma once
// Per-pass GPU timing for the Metal backend (perf_stats {passes: true}, the editor's pass list).
//
// Apple GPUs sample timestamps at stage boundaries (MTLCounterSamplingPointAtStageBoundary): a
// render pass gets vertex/fragment start/end samples, a compute or blit pass encoder start/end.
// Every pass of a frame writes into one MTLCounterSampleBuffer from a small pool (one per frame
// in flight); the samples are resolved on the CPU when the frame's last command buffer completes
// and folded into a rolling 60-frame `prof::PassTimeline`. Tile-based GPUs overlap the vertex
// work of one pass with the fragment work of the previous one, so pass times can sum to a bit
// more than the frame's GPU time.
//
// Any Metal file can profile its passes through the free helpers below; they are plain encoders
// when no frame is being profiled (bakes, uploads, readbacks) or the device cannot sample.

#import <Metal/Metal.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Profiler.h"

namespace sky {

class MetalPassProfiler {
public:
    explicit MetalPassProfiler(id<MTLDevice> device);
    ~MetalPassProfiler();
    MetalPassProfiler(const MetalPassProfiler&) = delete;
    MetalPassProfiler& operator=(const MetalPassProfiler&) = delete;

    bool supported() const { return supported_; }
    /// Begins profiling a frame (no-op when unsupported or every sample buffer is still in flight).
    void beginFrame();
    /// Ends the frame: its samples resolve when `last` (the frame's final command buffer) completes.
    void endFrame(id<MTLCommandBuffer> last);

    /// Adds stage-boundary samples to a render pass about to be encoded.
    void attach(MTLRenderPassDescriptor* rp, const char* label, const char* group);
    /// Compute / blit encoders with encoder-boundary samples (plain encoders when not profiling).
    id<MTLComputeCommandEncoder> compute(id<MTLCommandBuffer> cmd, const char* label, const char* group,
                                         MTLDispatchType type = MTLDispatchTypeSerial);
    id<MTLBlitCommandEncoder> blit(id<MTLCommandBuffer> cmd, const char* label, const char* group);

    /// {supported, mode, frames, spanMs, sumMs, passes: [...], groups: {...}, droppedPasses}
    Json toJson() const;
    void reset();

    /// The profiler of the frame being encoded on this thread (nullptr outside a profiled frame).
    static MetalPassProfiler* active();

private:
    struct Pass {
        std::string label, group;
        uint32_t first = 0, count = 0;  // sample indices
    };
    struct Slot {
        id<MTLCounterSampleBuffer> buffer;
        std::vector<Pass> passes;
        uint32_t used = 0;
        std::atomic<bool> busy{false};
    };
    bool reserve(uint32_t samples, const char* label, const char* group, uint32_t& first);

    id<MTLDevice> device_;
    bool supported_ = false;
    std::vector<std::shared_ptr<Slot>> slots_;
    std::shared_ptr<Slot> current_;
    std::shared_ptr<prof::PassTimeline> timeline_ = std::make_shared<prof::PassTimeline>();
    std::shared_ptr<std::atomic<int>> dropped_ = std::make_shared<std::atomic<int>>(0);
    uint64_t cpu0_ = 0, gpu0_ = 0;  // calibration pair (CPU ns, GPU ticks)
};

/// Profiled pass helpers for any Metal file (see the header comment).
void profileRenderPass(MTLRenderPassDescriptor* rp, const char* label, const char* group);
id<MTLComputeCommandEncoder> profiledCompute(id<MTLCommandBuffer> cmd, const char* label, const char* group,
                                             MTLDispatchType type = MTLDispatchTypeSerial);
id<MTLBlitCommandEncoder> profiledBlit(id<MTLCommandBuffer> cmd, const char* label, const char* group);

}  // namespace sky
