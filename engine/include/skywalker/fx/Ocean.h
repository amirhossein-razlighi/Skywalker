#pragma once
// Spectral ocean (Tessendorf): a JONSWAP wind-wave spectrum with directional spreading is
// evolved with the deep/finite-depth dispersion relation and transformed with inverse FFTs
// into height, choppy horizontal displacement, slopes and the displacement Jacobian
// (folding crests -> whitecaps). Three cascades (long swell, mid waves, ripples) cover
// wavelengths from hundreds of meters down to ~20 cm without visible tiling.
//
// Everything is a pure function of (parameters, seed, time), so play sessions replay
// exactly and gameplay can query the real surface (boats, buoyancy, splashes).

#include <memory>

#include "skywalker/ecs/Components.h"
#include "skywalker/render/Renderer.h"

namespace sky::fx {

class Ocean {
public:
    static constexpr int kN = 256;

    /// Rebuilds the spectrum when the wave parameters change. Cheap when unchanged.
    void configure(const Water& water);
    /// Evolves the wave field to `time` (seconds). Cached: repeated calls are free.
    void evaluate(double time);

    /// Surface height above the water level at world-relative (x, z) — accounts for the
    /// choppy horizontal displacement, so it matches what is rendered.
    float height(float x, float z) const;
    /// Surface normal at (x, z).
    Vec3 normal(float x, float z) const;

    std::shared_ptr<const OceanCascades> cascades() const { return data_; }
    double time() const { return time_; }

private:
    struct Complex {
        float re = 0, im = 0;
    };
    struct Spectrum {
        std::vector<Complex> h0;       // h0(k)
        std::vector<Complex> h0mkConj; // conj(h0(-k))
        std::vector<float> omega;      // dispersion
        std::vector<float> kx, kz;
    };
    Vec4 sample(const std::vector<float>& tex, float tileSize, float x, float z) const;  // bilinear, tiled

    Spectrum spectra_[OceanCascades::kCascades];
    std::string key_;
    double time_ = -1e9;
    uint64_t version_ = 0;
    float choppiness_ = 1.f;
    std::vector<float> foam_[OceanCascades::kCascades];  // persistent whitecaps (decay over seconds)
    std::shared_ptr<OceanCascades> data_;
};

/// In-place radix-2 complex FFT (inverse when `inverse`), n a power of two. Exposed for tests.
void fft1d(float* re, float* im, int n, bool inverse);

}  // namespace sky::fx
