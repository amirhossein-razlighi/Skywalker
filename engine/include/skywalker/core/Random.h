#pragma once
// Deterministic PCG32 random generator. Simulation randomness always comes from a
// seeded generator so that agents can replay and verify behaviour exactly.

#include <cstdint>

namespace sky {

class Random {
public:
    explicit Random(uint64_t seed = 0x5EEDu) { reseed(seed); }

    void reseed(uint64_t seed) {
        state_ = 0;
        inc_ = (seed << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }

    uint32_t next() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        auto xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        auto rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
    }

    /// Uniform float in [0, 1).
    float nextFloat() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }

    uint64_t state() const { return state_; }

private:
    uint64_t state_ = 0;
    uint64_t inc_ = 0;
};

}  // namespace sky
