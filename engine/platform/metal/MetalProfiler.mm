// Per-pass GPU timing (see MetalProfiler.h).

#include "MetalProfiler.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace sky {

namespace {

constexpr uint32_t kSamplesPerBuffer = 4096;  // 32 KB of timestamps: the per-buffer limit
constexpr int kSlots = 4;                     // frames in flight + 1
thread_local MetalPassProfiler* tActive = nullptr;

NSString* ns(const char* s) { return [NSString stringWithUTF8String:s ? s : ""]; }

}  // namespace

MetalPassProfiler::MetalPassProfiler(id<MTLDevice> device) : device_(device) {
    const char* env = std::getenv("SKY_GPU_PROFILER");
    if (env && std::string(env) == "0") return;
    if (!device_ || ![device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return;
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device_.counterSets) {
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
    }
    if (!timestamps) return;
    MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
    d.counterSet = timestamps;
    d.storageMode = MTLStorageModeShared;
    d.sampleCount = kSamplesPerBuffer;
    for (int i = 0; i < kSlots; ++i) {
        NSError* err = nil;
        id<MTLCounterSampleBuffer> b = [device_ newCounterSampleBufferWithDescriptor:d error:&err];
        if (!b) return;
        auto slot = std::make_shared<Slot>();
        slot->buffer = b;
        slots_.push_back(std::move(slot));
    }
    MTLTimestamp cpu = 0, gpu = 0;
    [device_ sampleTimestamps:&cpu gpuTimestamp:&gpu];
    cpu0_ = cpu, gpu0_ = gpu;
    supported_ = true;
}

MetalPassProfiler::~MetalPassProfiler() {
    if (tActive == this) tActive = nullptr;
}

MetalPassProfiler* MetalPassProfiler::active() { return tActive; }

void MetalPassProfiler::beginFrame() {
    current_.reset();
    tActive = nullptr;
    if (!supported_) return;
    for (auto& s : slots_) {
        bool expected = false;
        if (s->busy.compare_exchange_strong(expected, true)) {
            s->passes.clear();
            s->used = 0;
            current_ = s;
            tActive = this;
            return;
        }
    }
}

bool MetalPassProfiler::reserve(uint32_t samples, const char* label, const char* group, uint32_t& first) {
    if (!current_) return false;
    if (current_->used + samples > kSamplesPerBuffer) {
        dropped_->fetch_add(1);
        return false;
    }
    first = current_->used;
    current_->used += samples;
    current_->passes.push_back(Pass{label ? label : "", group ? group : "", first, samples});
    return true;
}

void MetalPassProfiler::attach(MTLRenderPassDescriptor* rp, const char* label, const char* group) {
    if (!rp) return;
    uint32_t first = 0;
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    if (!reserve(4, label, group, first)) {
        a.sampleBuffer = nil;  // a reused descriptor must not write into a recycled buffer
        return;
    }
    a.sampleBuffer = current_->buffer;
    a.startOfVertexSampleIndex = first;
    a.endOfVertexSampleIndex = first + 1;
    a.startOfFragmentSampleIndex = first + 2;
    a.endOfFragmentSampleIndex = first + 3;
}

id<MTLComputeCommandEncoder> MetalPassProfiler::compute(id<MTLCommandBuffer> cmd, const char* label, const char* group,
                                                        MTLDispatchType type) {
    id<MTLComputeCommandEncoder> enc = nil;
    uint32_t first = 0;
    if (reserve(2, label, group, first)) {
        MTLComputePassDescriptor* d = [MTLComputePassDescriptor computePassDescriptor];
        d.dispatchType = type;
        d.sampleBufferAttachments[0].sampleBuffer = current_->buffer;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        enc = [cmd computeCommandEncoderWithDescriptor:d];
    } else {
        enc = [cmd computeCommandEncoderWithDispatchType:type];
    }
    enc.label = ns(label);
    return enc;
}

id<MTLBlitCommandEncoder> MetalPassProfiler::blit(id<MTLCommandBuffer> cmd, const char* label, const char* group) {
    id<MTLBlitCommandEncoder> enc = nil;
    uint32_t first = 0;
    if (reserve(2, label, group, first)) {
        MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
        d.sampleBufferAttachments[0].sampleBuffer = current_->buffer;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
        enc = [cmd blitCommandEncoderWithDescriptor:d];
    } else {
        enc = [cmd blitCommandEncoder];
    }
    enc.label = ns(label);
    return enc;
}

void MetalPassProfiler::endFrame(id<MTLCommandBuffer> last) {
    tActive = nullptr;
    std::shared_ptr<Slot> slot = std::move(current_);
    current_.reset();
    if (!slot) return;
    if (slot->passes.empty() || !last) {
        slot->busy.store(false);
        return;
    }
    // Calibrate GPU ticks -> nanoseconds (Apple silicon reports nanoseconds; others may not).
    MTLTimestamp cpu = 0, gpu = 0;
    [device_ sampleTimestamps:&cpu gpuTimestamp:&gpu];
    double nsPerTick = 1.0;
    if (gpu > gpu0_ + 1000000 && cpu > cpu0_) nsPerTick = static_cast<double>(cpu - cpu0_) / static_cast<double>(gpu - gpu0_);
    auto timeline = timeline_;
    [last addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusCompleted && slot->used > 0) {
            NSData* data = [slot->buffer resolveCounterRange:NSMakeRange(0, slot->used)];
            const auto* ts = data ? static_cast<const MTLCounterResultTimestamp*>(data.bytes) : nullptr;
            const size_t n = data ? data.length / sizeof(MTLCounterResultTimestamp) : 0;
            if (ts && n >= slot->used) {
                std::vector<prof::PassSample> samples;
                samples.reserve(slot->passes.size());
                uint64_t origin = std::numeric_limits<uint64_t>::max();
                for (uint32_t i = 0; i < slot->used; ++i) {
                    const uint64_t t = ts[i].timestamp;
                    if (t != 0 && t != MTLCounterErrorValue) origin = std::min(origin, t);
                }
                const double toMs = nsPerTick * 1e-6;
                auto span = [&](uint32_t a, uint32_t b, double& s0, double& s1) {  // samples a..b (inclusive)
                    uint64_t lo = std::numeric_limits<uint64_t>::max(), hi = 0;
                    for (uint32_t k = a; k <= b; ++k) {
                        const uint64_t t = ts[k].timestamp;
                        if (t == 0 || t == MTLCounterErrorValue || t < origin) continue;  // the stage had no work
                        lo = std::min(lo, t);
                        hi = std::max(hi, t);
                    }
                    if (hi < lo) return false;
                    s0 = static_cast<double>(lo - origin) * toMs;
                    s1 = static_cast<double>(hi - origin) * toMs;
                    return true;
                };
                for (const Pass& p : slot->passes) {
                    prof::PassSample out{p.label, p.group, 0, 0, -1};
                    if (p.count == 4) {
                        // Render pass: the GPU runs the vertex work of later passes early and
                        // overlapped, so the pass time is its fragment span (sequential across
                        // passes); the vertex span is reported separately.
                        double v0 = 0, v1 = 0;
                        const bool vertex = span(p.first, p.first + 1, v0, v1);
                        if (span(p.first + 2, p.first + 3, out.startMs, out.endMs)) {
                            if (vertex) out.vertexMs = v1 - v0;
                        } else if (vertex) {
                            out.startMs = v0, out.endMs = v1;
                        } else {
                            continue;
                        }
                    } else if (!span(p.first, p.first + p.count - 1, out.startMs, out.endMs)) {
                        continue;
                    }
                    samples.push_back(std::move(out));
                }
                timeline->addFrame(samples);
            }
        }
        slot->busy.store(false);
    }];
}

Json MetalPassProfiler::toJson() const {
    Json j = timeline_->toJson();
    j["supported"] = supported_;
    j["mode"] = supported_ ? "stage_boundary" : "unsupported";
    j["droppedPasses"] = dropped_->load();
    return j;
}

void MetalPassProfiler::reset() {
    timeline_->reset();
    dropped_->store(0);
}

// --- Free helpers --------------------------------------------------------------------------

void profileRenderPass(MTLRenderPassDescriptor* rp, const char* label, const char* group) {
    if (MetalPassProfiler* p = MetalPassProfiler::active()) p->attach(rp, label, group);
}

id<MTLComputeCommandEncoder> profiledCompute(id<MTLCommandBuffer> cmd, const char* label, const char* group, MTLDispatchType type) {
    if (MetalPassProfiler* p = MetalPassProfiler::active()) return p->compute(cmd, label, group, type);
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoderWithDispatchType:type];
    enc.label = ns(label);
    return enc;
}

id<MTLBlitCommandEncoder> profiledBlit(id<MTLCommandBuffer> cmd, const char* label, const char* group) {
    if (MetalPassProfiler* p = MetalPassProfiler::active()) return p->blit(cmd, label, group);
    id<MTLBlitCommandEncoder> enc = [cmd blitCommandEncoder];
    enc.label = ns(label);
    return enc;
}

}  // namespace sky
