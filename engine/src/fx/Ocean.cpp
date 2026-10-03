#include "skywalker/fx/Ocean.h"

#include <cmath>
#include <cstdio>
#include <future>

#include "skywalker/core/Random.h"

namespace sky::fx {

namespace {

constexpr float kG = 9.81f;
constexpr float kPi = 3.14159265358979f;
// Each cascade covers a band of wavelengths; tiles are patchSize / these factors.
constexpr float kCascadeDivisor[OceanCascades::kCascades] = {1.f, 4.73f, 19.1f};

float gaussian(Random& r) {
    float u1 = std::max(r.nextFloat(), 1e-7f), u2 = r.nextFloat();
    return std::sqrt(-2.f * std::log(u1)) * std::cos(2.f * kPi * u2);
}

/// JONSWAP frequency spectrum S(omega) for wind speed U (m/s) over fetch F (m).
float jonswap(float omega, float U, float F) {
    if (omega <= 1e-4f) return 0.f;
    float alpha = 0.076f * std::pow(U * U / (F * kG), 0.22f);
    float wp = 22.f * std::pow(kG * kG / (U * F), 1.f / 3.f);
    float sigma = omega <= wp ? 0.07f : 0.09f;
    float r = std::exp(-(omega - wp) * (omega - wp) / (2.f * sigma * sigma * wp * wp));
    return alpha * kG * kG / std::pow(omega, 5.f) * std::exp(-1.25f * std::pow(wp / omega, 4.f)) * std::pow(3.3f, r);
}

void fft2d(std::vector<float>& re, std::vector<float>& im, int n, std::vector<float>& tr, std::vector<float>& ti) {
    for (int y = 0; y < n; ++y) fft1d(re.data() + y * n, im.data() + y * n, n, true);
    tr.resize(static_cast<size_t>(n));
    ti.resize(static_cast<size_t>(n));
    for (int x = 0; x < n; ++x) {
        for (int y = 0; y < n; ++y) {
            tr[static_cast<size_t>(y)] = re[static_cast<size_t>(y * n + x)];
            ti[static_cast<size_t>(y)] = im[static_cast<size_t>(y * n + x)];
        }
        fft1d(tr.data(), ti.data(), n, true);
        for (int y = 0; y < n; ++y) {
            re[static_cast<size_t>(y * n + x)] = tr[static_cast<size_t>(y)];
            im[static_cast<size_t>(y * n + x)] = ti[static_cast<size_t>(y)];
        }
    }
}

}  // namespace

void fft1d(float* re, float* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {  // bit reversal
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * 3.14159265358979323846 / len * (inverse ? 1 : -1);
        float wr = static_cast<float>(std::cos(ang)), wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void Ocean::configure(const Water& w) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.4f %.4f %d", w.windSpeed, w.windDirection, w.choppiness,
                  w.waveScale, w.patchSize, w.depth, w.seed);
    if (key_ == buf) return;
    key_ = buf;
    time_ = -1e9;  // force re-evaluation
    const int N = kN;
    float U = std::max(w.windSpeed, 0.3f);
    const float fetch = 300000.f;
    float wa = radians(w.windDirection);
    float thetaW = std::atan2(std::cos(wa), std::sin(wa));  // wind vector (sin a, cos a) as atan2(z, x)
    float L0 = std::max(w.patchSize, 10.f);
    float bounds[OceanCascades::kCascades + 1] = {0.f, 2.f * kPi / (L0 / kCascadeDivisor[1]) * 6.f,
                                                   2.f * kPi / (L0 / kCascadeDivisor[2]) * 6.f, 1e9f};
    for (int c = 0; c < OceanCascades::kCascades; ++c) {
        float L = L0 / kCascadeDivisor[c];
        float dk = 2.f * kPi / L;
        Spectrum& sp = spectra_[c];
        size_t cells = static_cast<size_t>(N * N);
        sp.h0.assign(cells, {});
        sp.h0mkConj.assign(cells, {});
        sp.omega.assign(cells, 0.f);
        sp.kx.assign(cells, 0.f);
        sp.kz.assign(cells, 0.f);
        Random rng(static_cast<uint64_t>(w.seed) * 7349u + static_cast<uint64_t>(c) * 131u + 5u);
        for (int m = 0; m < N; ++m) {
            for (int n = 0; n < N; ++n) {
                size_t i = static_cast<size_t>(m * N + n);
                float kx = dk * static_cast<float>(n - N / 2), kz = dk * static_cast<float>(m - N / 2);
                float k = std::sqrt(kx * kx + kz * kz);
                sp.kx[i] = kx;
                sp.kz[i] = kz;
                float gr = gaussian(rng), gi = gaussian(rng);  // always draw: stable noise per cell
                if (k < 1e-6f || k < bounds[c] || k >= bounds[c + 1]) continue;
                float omega = std::sqrt(kG * k * std::tanh(std::min(k * std::max(w.depth, 0.5f), 20.f)));
                sp.omega[i] = omega;
                float dOmegaDk = kG / (2.f * std::max(omega, 1e-4f));
                float delta = std::atan2(kz, kx) - thetaW;
                float cd = std::cos(delta);
                float spread = (2.f / kPi) * cd * cd * (cd > 0.f ? 1.f : 0.05f);
                float S = jonswap(omega, U, fetch) * dOmegaDk / k * spread;
                S *= std::exp(-k * k * 0.0004f);  // fade the smallest ripples
                float amp = std::sqrt(std::max(S, 0.f) * dk * dk * 0.5f) * w.waveScale;
                sp.h0[i] = {gr * amp, gi * amp};
            }
        }
        for (int m = 0; m < N; ++m) {
            for (int n = 0; n < N; ++n) {
                size_t i = static_cast<size_t>(m * N + n);
                size_t j = static_cast<size_t>(((N - m) % N) * N + (N - n) % N);
                sp.h0mkConj[i] = {sp.h0[j].re, -sp.h0[j].im};
            }
        }
    }
    auto d = std::make_shared<OceanCascades>();
    d->resolution = N;
    for (int c = 0; c < OceanCascades::kCascades; ++c) {
        d->patchSize[c] = L0 / kCascadeDivisor[c];
        d->displacement[c].assign(static_cast<size_t>(N * N * 4), 0.f);
        d->slope[c].assign(static_cast<size_t>(N * N * 4), 0.f);
    }
    data_ = d;
    choppiness_ = w.choppiness;
}

void Ocean::evaluate(double time) {
    if (!data_ || time == time_) return;
    double prev = time_;
    time_ = time;
    const int N = kN;
    const size_t cells = static_cast<size_t>(N * N);
    auto d = std::make_shared<OceanCascades>(*data_);
    const float lambda = choppiness_;
    // Per cascade: 4 packed complex spectra (8 real fields) -> 4 inverse 2D FFTs. All 12
    // transforms are independent, so they run in parallel.
    std::vector<float> re[OceanCascades::kCascades][4], im[OceanCascades::kCascades][4];
    for (int c = 0; c < OceanCascades::kCascades; ++c) {
        for (int q = 0; q < 4; ++q) {
            re[c][q].assign(cells, 0.f);
            im[c][q].assign(cells, 0.f);
        }
        const Spectrum& sp = spectra_[c];
        for (size_t i = 0; i < cells; ++i) {
            float om = sp.omega[i];
            if (om == 0.f) continue;
            float cs = static_cast<float>(std::cos(om * time)), sn = static_cast<float>(std::sin(om * time));
            const Complex& a = sp.h0[i];
            const Complex& b = sp.h0mkConj[i];
            // h = h0 e^{iwt} + conj(h0(-k)) e^{-iwt}
            float hr = a.re * cs - a.im * sn + b.re * cs + b.im * sn;
            float hi = a.re * sn + a.im * cs - b.re * sn + b.im * cs;
            float kx = sp.kx[i], kz = sp.kz[i];
            float ik = 1.f / std::sqrt(kx * kx + kz * kz);
            float dxr = lambda * kx * ik * hi, dxi = -lambda * kx * ik * hr;  // -i (kx/k) h
            float dzr = lambda * kz * ik * hi, dzi = -lambda * kz * ik * hr;
            float sxr = -kx * hi, sxi = kx * hr;  // i kx h
            float szr = -kz * hi, szi = kz * hr;
            float dxxr = lambda * kx * kx * ik * hr, dxxi = lambda * kx * kx * ik * hi;
            float dzzr = lambda * kz * kz * ik * hr, dzzi = lambda * kz * kz * ik * hi;
            float dxzr = lambda * kx * kz * ik * hr, dxzi = lambda * kx * kz * ik * hi;
            re[c][0][i] = hr - dxi;
            im[c][0][i] = hi + dxr;
            re[c][1][i] = dzr - sxi;
            im[c][1][i] = dzi + sxr;
            re[c][2][i] = szr - dxxi;
            im[c][2][i] = szi + dxxr;
            re[c][3][i] = dzzr - dxzi;
            im[c][3][i] = dzzi + dxzr;
        }
    }
    {
        std::vector<std::future<void>> jobs;
        for (int c = 0; c < OceanCascades::kCascades; ++c) {
            for (int q = 0; q < 4; ++q) {
                jobs.push_back(std::async(std::launch::async, [&, c, q] {
                    std::vector<float> tr, ti;
                    fft2d(re[c][q], im[c][q], N, tr, ti);
                }));
            }
        }
        for (auto& j : jobs) j.get();
    }
    float dt = static_cast<float>(time - prev);
    bool continuous = dt > 0.f && dt < 1.f;
    float keep = continuous ? std::exp(-dt / 2.2f) : 0.f;  // whitecaps linger ~2 s
    for (int c = 0; c < OceanCascades::kCascades; ++c) {
        float* disp = d->displacement[c].data();
        float* slope = d->slope[c].data();
        std::vector<float>& foam = foam_[c];
        if (foam.size() != cells) foam.assign(cells, 0.f);
        for (int z = 0; z < N; ++z) {
            for (int x = 0; x < N; ++x) {
                size_t i = static_cast<size_t>(z * N + x);
                float sign = ((x + z) & 1) ? -1.f : 1.f;  // centered wave numbers
                float h = re[c][0][i] * sign, dx = im[c][0][i] * sign;
                float dz = re[c][1][i] * sign, sx = im[c][1][i] * sign;
                float sz = re[c][2][i] * sign, dxx = im[c][2][i] * sign;
                float dzz = re[c][3][i] * sign, dxz = im[c][3][i] * sign;
                float jac = (1.f + dxx) * (1.f + dzz) - dxz * dxz;
                float fresh = std::clamp((0.6f - jac) * 2.2f, 0.f, 1.f);
                foam[i] = std::max(foam[i] * keep, fresh);
                disp[i * 4 + 0] = dx;
                disp[i * 4 + 1] = h;
                disp[i * 4 + 2] = dz;
                disp[i * 4 + 3] = jac;
                slope[i * 4 + 0] = sx;
                slope[i * 4 + 1] = sz;
                slope[i * 4 + 2] = foam[i];
                slope[i * 4 + 3] = 0.f;
            }
        }
    }
    d->version = ++version_;
    data_ = d;
}

Vec4 Ocean::sample(const std::vector<float>& tex, float L, float x, float z) const {
    const int N = kN;
    float u = x / L * N, v = z / L * N;
    float fu = std::floor(u), fv = std::floor(v);
    float tu = u - fu, tv = v - fv;
    auto at = [&](int ix, int iz) {
        ix = ((ix % N) + N) % N;
        iz = ((iz % N) + N) % N;
        const float* p = &tex[static_cast<size_t>((iz * N + ix) * 4)];
        return Vec4{p[0], p[1], p[2], p[3]};
    };
    int ix = static_cast<int>(fu), iz = static_cast<int>(fv);
    Vec4 a = at(ix, iz), b = at(ix + 1, iz), c = at(ix, iz + 1), e = at(ix + 1, iz + 1);
    auto mix = [](Vec4 p, Vec4 q, float t) {
        return Vec4{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t};
    };
    return mix(mix(a, b, tu), mix(c, e, tu), tv);
}

float Ocean::height(float x, float z) const {
    if (!data_) return 0.f;
    // Find the undisplaced point whose choppy displacement lands on (x, z).
    float px = x, pz = z;
    for (int it = 0; it < 4; ++it) {
        float dx = 0, dz = 0;
        for (int c = 0; c < OceanCascades::kCascades; ++c) {
            Vec4 s = sample(data_->displacement[c], data_->patchSize[c], px, pz);
            dx += s.x;
            dz += s.z;
        }
        px = x - dx;
        pz = z - dz;
    }
    float h = 0;
    for (int c = 0; c < OceanCascades::kCascades; ++c) h += sample(data_->displacement[c], data_->patchSize[c], px, pz).y;
    return h;
}

Vec3 Ocean::normal(float x, float z) const {
    if (!data_) return {0, 1, 0};
    float sx = 0, sz = 0;
    for (int c = 0; c < OceanCascades::kCascades; ++c) {
        Vec4 s = sample(data_->slope[c], data_->patchSize[c], x, z);
        sx += s.x;
        sz += s.y;
    }
    return normalize(Vec3{-sx, 1.f, -sz});
}

}  // namespace sky::fx
