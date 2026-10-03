#include "skywalker/render/TextureGen.h"

#include <algorithm>
#include <cmath>

namespace sky::texgen {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- small math helpers ----------------------------------------------------------------

float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }
float frac(float x) { return x - std::floor(x); }

/// Smoothstep; edges may be reversed (e0 > e1) to get a falling ramp.
float sstep(float e0, float e1, float x) {
    float d = e1 - e0;
    if (std::fabs(d) < 1e-6f) return x < e0 ? 0.f : 1.f;
    float t = clamp01((x - e0) / d);
    return t * t * (3.f - 2.f * t);
}

int wrapi(int i, int n) {
    int r = i % n;
    return r < 0 ? r + n : r;
}

struct Col {
    float r = 0, g = 0, b = 0;
};

Col fromVec(Vec4 v) { return {v.x, v.y, v.z}; }
Col mixc(Col a, Col b, float t) { return {mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)}; }
Col scalec(Col a, float s) { return {a.r * s, a.g * s, a.b * s}; }

// ---- hashing and periodic noise --------------------------------------------------------

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

/// Uniform [0,1) value for an integer id (per-brick / per-plank / per-tile randomness).
float rnd(int a, int b, uint32_t seed) { return unit(hash3(static_cast<uint32_t>(a), static_cast<uint32_t>(b), seed)); }

/// Periodic value noise in [0,1]. (x, y) are lattice coordinates; the pattern repeats every
/// px / py cells, so sampling x = u * px, y = v * py tiles seamlessly over u, v in [0,1).
float valueNoise(float x, float y, int px, int py, uint32_t seed) {
    float fx = std::floor(x), fy = std::floor(y);
    float tx = x - fx, ty = y - fy;
    int x0 = wrapi(static_cast<int>(fx), px), y0 = wrapi(static_cast<int>(fy), py);
    int x1 = x0 + 1 == px ? 0 : x0 + 1, y1 = y0 + 1 == py ? 0 : y0 + 1;
    auto at = [&](int xi, int yi) { return rnd(xi, yi, seed); };
    float sx = tx * tx * tx * (tx * (tx * 6.f - 15.f) + 10.f);
    float sy = ty * ty * ty * (ty * (ty * 6.f - 15.f) + 10.f);
    float a = mixf(at(x0, y0), at(x1, y0), sx);
    float b = mixf(at(x0, y1), at(x1, y1), sx);
    return mixf(a, b, sy);
}

/// Fractal noise over the unit tile with `fx` x `fy` base cells; each octave doubles both.
float fbm(float u, float v, int fx, int fy, int octaves, uint32_t seed, float gain = 0.5f) {
    float sum = 0.f, amp = 1.f, norm = 0.f;
    for (int o = 0; o < octaves; ++o) {
        int px = fx << o, py = fy << o;
        sum += amp * valueNoise(u * static_cast<float>(px), v * static_cast<float>(py), px, py, seed + static_cast<uint32_t>(o) * 131u);
        norm += amp;
        amp *= gain;
    }
    return sum / norm;
}

/// Ridged variant of fbm: sharp creases where the underlying noise crosses 0.5.
float ridged(float u, float v, int f, int octaves, uint32_t seed) {
    float sum = 0.f, amp = 1.f, norm = 0.f;
    for (int o = 0; o < octaves; ++o) {
        int fo = f << o;
        float n = valueNoise(u * static_cast<float>(fo), v * static_cast<float>(fo), fo, fo, seed + static_cast<uint32_t>(o) * 313u);
        float r = 1.f - std::fabs(2.f * n - 1.f);
        sum += amp * r * r;
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

struct Voronoi {
    float f1 = 9.f, f2 = 9.f;  // distances (cell units) to nearest / second nearest feature point
    int idx = 0, idy = 0;      // wrapped cell of the nearest feature point
};

/// Periodic Voronoi with n x n cells (jittered feature point per cell).
Voronoi voronoi(float u, float v, int n, uint32_t seed, float jitter = 0.85f) {
    float px = u * static_cast<float>(n), py = v * static_cast<float>(n);
    int cx = static_cast<int>(std::floor(px)), cy = static_cast<int>(std::floor(py));
    Voronoi r;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int gx = cx + dx, gy = cy + dy;
            int wx = wrapi(gx, n), wy = wrapi(gy, n);
            float fx = static_cast<float>(gx) + 0.5f + (rnd(wx, wy, seed) - 0.5f) * jitter;
            float fy = static_cast<float>(gy) + 0.5f + (rnd(wx, wy, seed + 77u) - 0.5f) * jitter;
            float d = std::sqrt((fx - px) * (fx - px) + (fy - py) * (fy - py));
            if (d < r.f1) {
                r.f2 = r.f1;
                r.f1 = d;
                r.idx = wx;
                r.idy = wy;
            } else if (d < r.f2) {
                r.f2 = d;
            }
        }
    }
    return r;
}

// ---- shared per-pixel layer ------------------------------------------------------------

struct Px {
    Col c;
    float h = 0.5f;  // height, 0..1
    float rough = 0.6f;
    float metal = 0.f;
};

struct Ctx {
    const Params& p;
    int n;
    uint32_t seed;
    int freq;  // integer feature frequency derived from p.scale
    Col c1, c2, c3;
    float var;
    std::vector<Px> px;
};

template <typename F>
void forEach(Ctx& c, F&& fn) {
    const float inv = 1.f / static_cast<float>(c.n);
    for (int y = 0; y < c.n; ++y) {
        for (int x = 0; x < c.n; ++x) {
            Px& o = c.px[static_cast<size_t>(y) * c.n + x];
            o.rough = c.p.roughness;
            o.metal = c.p.metallic;
            fn((static_cast<float>(x) + 0.5f) * inv, (static_cast<float>(y) + 0.5f) * inv, o);
        }
    }
}

int evenAtLeast2(float v) { return std::max(2, 2 * static_cast<int>(std::lround(v * 0.5f))); }

// ---- kinds -----------------------------------------------------------------------------

void genNoise(Ctx& c) {
    forEach(c, [&](float u, float v, Px& o) {
        float h = fbm(u, v, c.freq, c.freq, 6, c.seed);
        float t = sstep(0.2f, 0.8f, h);
        o.c = mixc(c.c1, c.c2, t);
        o.c = mixc(o.c, c.c3, sstep(0.38f, 0.12f, h) * c.var * 0.7f);
        o.h = h;
    });
}

void genMarble(Ctx& c) {
    int a = c.freq, b = c.freq / 2 + 1;
    forEach(c, [&](float u, float v, Px& o) {
        float warp = fbm(u, v, 2, 2, 5, c.seed + 1u);
        float t = static_cast<float>(a) * u + static_cast<float>(b) * v + (warp - 0.5f) * (1.5f + 3.f * c.var);
        float vein = std::pow(1.f - std::fabs(std::sin(kPi * t)), 14.f);
        float t2 = static_cast<float>(a * 2 + 1) * u - static_cast<float>(b * 2 + 1) * v + (warp - 0.5f) * 5.f;
        float fine = std::pow(1.f - std::fabs(std::sin(kPi * t2)), 40.f) * 0.5f;
        float cloud = fbm(u, v, c.freq, c.freq, 5, c.seed + 2u);
        o.c = mixc(c.c1, c.c2, sstep(0.3f, 0.75f, cloud) * (0.4f + 0.6f * c.var));
        float vv = clamp01(vein * (0.5f + 0.5f * c.var) + fine * c.var);
        o.c = mixc(o.c, c.c3, vv);
        o.h = 0.5f + (cloud - 0.5f) * 0.05f - vv * 0.04f;
        o.rough = clamp01(c.p.roughness + vv * 0.1f);
    });
}

void genWood(Ctx& c) {
    int gx = 32 + c.freq * 8;
    forEach(c, [&](float u, float v, Px& o) {
        float w = fbm(u, v, 2, 3, 4, c.seed + 1u);
        float t = static_cast<float>(c.freq) * u + (w - 0.5f) * (1.f + 3.f * c.var);
        float ring = std::pow(frac(t), 1.6f);
        float grain = valueNoise(u * static_cast<float>(gx), v * 3.f, gx, 3, c.seed + 2u);
        float pores = valueNoise(u * static_cast<float>(gx * 2), v * 24.f, gx * 2, 24, c.seed + 3u);
        float k = clamp01(ring * 0.65f + grain * 0.35f * (0.5f + c.var));
        o.c = mixc(c.c1, c.c2, k);
        o.c = mixc(o.c, c.c3, sstep(0.72f, 0.95f, pores) * c.var * 0.6f);
        o.h = 0.5f + (grain - 0.5f) * 0.18f + ring * 0.06f - sstep(0.75f, 0.95f, pores) * 0.08f;
        o.rough = clamp01(c.p.roughness + (grain - 0.5f) * 0.15f);
    });
}

void genPlanks(Ctx& c) {
    int rows = std::clamp(c.freq, 2, 32);
    const float fr = static_cast<float>(rows);
    forEach(c, [&](float u, float v, Px& o) {
        int row = static_cast<int>(std::floor(v * fr));
        float fv = v * fr - static_cast<float>(row);
        float off = rnd(row, 0, c.seed + 5u);  // staggered butt joints, two boards per row
        float su = u * 2.f + off;
        int seg = wrapi(static_cast<int>(std::floor(su)), 2);
        float fs = frac(su);
        int id = row * 2 + seg;
        uint32_t ps = c.seed + 100u + static_cast<uint32_t>(id) * 7919u;
        // Boards run along u; grain lines are thin in v and long in u.
        float grain = valueNoise(u * 3.f, v * fr * 20.f, 3, rows * 20, ps) * 0.6f +
                      valueNoise(u * 6.f, v * fr * 56.f, 6, rows * 56, ps + 1u) * 0.4f;
        float tint = rnd(id, 1, c.seed + 9u);
        Col base = mixc(c.c1, c.c2, clamp01(tint * c.var + grain * 0.45f));
        float e = std::min(std::min(fv, 1.f - fv), std::min(fs, 1.f - fs) * fr * 0.5f);
        float gap = 1.f - sstep(0.02f, 0.06f, e);
        o.c = mixc(base, c.c3, gap);
        o.h = 0.15f + 0.85f * sstep(0.02f, 0.14f, e) + (grain - 0.5f) * 0.06f;
        o.rough = clamp01(c.p.roughness + gap * 0.3f + (grain - 0.5f) * 0.1f);
    });
}

void genBricks(Ctx& c) {
    int rows = evenAtLeast2(static_cast<float>(c.freq));
    int cols = rows / 2;  // bricks are two row-heights wide
    const float fr = static_cast<float>(rows), fc = static_cast<float>(cols);
    forEach(c, [&](float u, float v, Px& o) {
        int row = static_cast<int>(std::floor(v * fr));
        float fv = v * fr - static_cast<float>(row);
        float bx = u * fc + ((row & 1) ? 0.5f : 0.f);
        int ci = wrapi(static_cast<int>(std::floor(bx)), cols);
        float fx = frac(bx);
        float chip = fbm(u, v, 48, 48, 3, c.seed + 11u);
        float e = std::min(std::min(fx, 1.f - fx) * 2.f, std::min(fv, 1.f - fv));  // edge distance in row heights
        float m = 0.07f + (chip - 0.5f) * 0.12f * c.var;
        float brick = sstep(m - 0.015f, m + 0.015f, e);
        float tint = rnd(ci, row, c.seed + 13u);
        float surf = fbm(u, v, 24, 24, 4, c.seed + 15u);
        Col bc = mixc(c.c1, c.c2, clamp01(tint * c.var + (surf - 0.5f) * 0.4f));
        bc = scalec(bc, 0.88f + 0.24f * surf);
        Col mc = scalec(c.c3, 0.85f + 0.3f * surf);
        o.c = mixc(mc, bc, brick);
        o.h = mixf(0.12f + 0.1f * surf, 0.65f + 0.3f * surf, brick) * (0.8f + 0.2f * sstep(m, m + 0.12f, e));
        o.rough = clamp01(c.p.roughness + (1.f - brick) * 0.1f - (surf - 0.5f) * 0.1f);
    });
}

void genTiles(Ctx& c) {
    const int n = std::max(1, c.freq);
    const float fn = static_cast<float>(n);
    forEach(c, [&](float u, float v, Px& o) {
        int iu = static_cast<int>(std::floor(u * fn)), iv = static_cast<int>(std::floor(v * fn));
        float fu = u * fn - static_cast<float>(iu), fv = v * fn - static_cast<float>(iv);
        float e = std::min(std::min(fu, 1.f - fu), std::min(fv, 1.f - fv));
        float t = sstep(0.025f, 0.04f, e);
        float tint = rnd(iu, iv, c.seed + 3u);
        float vein = fbm(u, v, n * 2, n * 2, 4, c.seed + 4u);
        Col tc = mixc(c.c1, c.c2, clamp01(tint * c.var * 1.2f + (vein - 0.5f) * 0.3f * c.var));
        o.c = mixc(c.c3, tc, t);
        o.h = 0.1f + 0.8f * sstep(0.02f, 0.08f, e) + (vein - 0.5f) * 0.02f;
        o.rough = clamp01(c.p.roughness + (1.f - t) * 0.5f);
    });
}

void genCobblestone(Ctx& c) {
    const int n = std::max(2, c.freq);
    forEach(c, [&](float u, float v, Px& o) {
        Voronoi vr = voronoi(u, v, n, c.seed + 21u);
        float edge = vr.f2 - vr.f1;
        float stone = sstep(0.03f, 0.2f, edge);
        float tint = rnd(vr.idx, vr.idy, c.seed + 23u);
        float surf = fbm(u, v, 32, 32, 4, c.seed + 25u);
        Col sc = mixc(c.c1, c.c2, tint * c.var + (surf - 0.5f) * 0.3f);
        sc = scalec(sc, 0.85f + 0.3f * surf);
        o.c = mixc(scalec(c.c3, 0.7f + 0.6f * surf), sc, sstep(0.0f, 0.1f, edge));
        float dome = 1.f - clamp01(vr.f1 / 0.8f);
        o.h = 0.1f + 0.9f * stone * (0.55f + 0.45f * std::sqrt(dome)) + (surf - 0.5f) * 0.06f * stone;
        o.rough = clamp01(c.p.roughness + (1.f - stone) * 0.1f + (surf - 0.5f) * 0.1f);
    });
}

void genGrass(Ctx& c) {
    const int gx = 48 + c.freq * 8;
    forEach(c, [&](float u, float v, Px& o) {
        // Blades are elongated along v: high frequency across u, low along v.
        float a = valueNoise(u * static_cast<float>(gx), v * 10.f, gx, 10, c.seed + 1u);
        float b = valueNoise(u * static_cast<float>(gx * 2), v * 18.f, gx * 2, 18, c.seed + 2u);
        float blade = a * 0.55f + b * 0.45f;
        float patch = fbm(u, v, c.freq, c.freq, 4, c.seed + 3u);
        float t = clamp01(blade * 0.7f + patch * 0.3f * (0.5f + c.var));
        o.c = mixc(c.c1, c.c2, t);
        o.c = mixc(o.c, c.c3, sstep(0.6f, 0.8f, patch) * c.var * 0.7f);
        o.h = blade;
        o.rough = clamp01(c.p.roughness + (blade - 0.5f) * 0.15f);
    });
}

void genDirt(Ctx& c) {
    const int pn = std::max(4, c.freq * 6);
    forEach(c, [&](float u, float v, Px& o) {
        float base = fbm(u, v, c.freq * 2, c.freq * 2, 6, c.seed + 1u);
        float fine = valueNoise(u * 128.f, v * 128.f, 128, 128, c.seed + 2u);
        Voronoi vr = voronoi(u, v, pn, c.seed + 3u);
        float present = rnd(vr.idx, vr.idy, c.seed + 4u) < 0.35f + 0.3f * c.var ? 1.f : 0.f;
        float pebble = present * sstep(0.3f, 0.12f, vr.f1);
        o.c = mixc(c.c1, c.c2, sstep(0.25f, 0.75f, base));
        o.c = scalec(o.c, 0.85f + 0.3f * fine);
        o.c = mixc(o.c, scalec(c.c3, 0.8f + 0.4f * rnd(vr.idx, vr.idy, c.seed + 6u)), pebble);
        o.h = 0.45f * base + 0.1f * fine + 0.45f * pebble * (1.f - vr.f1 * 2.f);
        o.rough = clamp01(c.p.roughness - pebble * 0.1f);
    });
}

void genSand(Ctx& c) {
    const int ia = c.freq, ib = std::max(1, c.freq / 3);
    const int gn = std::min(c.n / 2, 256);
    forEach(c, [&](float u, float v, Px& o) {
        float warp = fbm(u, v, 3, 3, 4, c.seed + 1u);
        float t = static_cast<float>(ia) * u + static_cast<float>(ib) * v + (warp - 0.5f) * (0.8f + 1.6f * c.var);
        float s = 0.5f + 0.5f * std::sin(2.f * kPi * t);
        float ripple = std::pow(s, 1.5f);  // sharper crests, broader troughs
        float grain = valueNoise(u * static_cast<float>(gn), v * static_cast<float>(gn), gn, gn, c.seed + 2u);
        o.c = mixc(c.c1, c.c2, clamp01(ripple * 0.6f + grain * 0.4f * c.var + (warp - 0.5f) * 0.3f));
        o.c = mixc(o.c, c.c3, sstep(0.93f, 1.f, grain) * c.var * 0.5f);
        o.h = ripple * 0.7f + warp * 0.2f + grain * 0.05f;
    });
}

void genRock(Ctx& c) {
    const int n = std::max(2, c.freq);
    forEach(c, [&](float u, float v, Px& o) {
        float r = ridged(u, v, std::max(1, c.freq), 6, c.seed + 1u);
        Voronoi vr = voronoi(u, v, n, c.seed + 2u);
        float crack = 1.f - sstep(0.0f, 0.07f, vr.f2 - vr.f1);
        float h = clamp01(r * 1.3f - 0.1f);
        o.c = mixc(c.c1, c.c2, sstep(0.15f, 0.75f, h));
        float speck = valueNoise(u * 128.f, v * 128.f, 128, 128, c.seed + 3u);
        o.c = scalec(o.c, 0.9f + 0.2f * speck);
        float dark = clamp01(sstep(0.35f, 0.1f, h) * 0.7f + crack * 0.8f * c.var);
        o.c = mixc(o.c, c.c3, dark);
        o.h = clamp01(h - crack * 0.25f * (0.4f + c.var));
    });
}

void genMetalBrushed(Ctx& c) {
    const int py1 = c.n / 2, py2 = c.n / 4, py3 = c.n / 8;
    forEach(c, [&](float u, float v, Px& o) {
        // Streaks run along u: very low frequency along u, high across v.
        float s = valueNoise(u * 6.f, v * static_cast<float>(py1), 6, py1, c.seed + 1u) * 0.4f +
                  valueNoise(u * 3.f, v * static_cast<float>(py2), 3, py2, c.seed + 2u) * 0.35f +
                  valueNoise(u * 2.f, v * static_cast<float>(py3), 2, py3, c.seed + 3u) * 0.25f;
        float wash = fbm(u, v, c.freq, c.freq, 3, c.seed + 4u);
        float t = clamp01(0.5f + (s - 0.5f) * 1.4f * (0.4f + c.var) + (wash - 0.5f) * 0.3f * c.var);
        o.c = mixc(c.c1, c.c2, t);
        o.c = mixc(o.c, c.c3, sstep(0.25f, 0.05f, s) * 0.4f * c.var);
        o.h = 0.5f + (s - 0.5f) * 0.12f;
        o.rough = clamp01(c.p.roughness + (s - 0.5f) * 0.3f * c.var + (wash - 0.5f) * 0.1f);
    });
}

void genRust(Ctx& c) {
    forEach(c, [&](float u, float v, Px& o) {
        float patch = fbm(u, v, c.freq, c.freq, 6, c.seed + 1u);
        float fine = fbm(u, v, c.freq * 8, c.freq * 8, 3, c.seed + 2u);
        float thr = 0.66f - 0.25f * c.var;
        float amt = sstep(thr - 0.06f, thr + 0.06f, patch + (fine - 0.5f) * 0.3f);
        float scratch = valueNoise(u * 4.f, v * 160.f, 4, 160, c.seed + 3u);
        Col metal = scalec(c.c1, 0.85f + 0.3f * scratch);
        Col rust = mixc(c.c2, c.c3, sstep(0.3f, 0.8f, fine));
        o.c = mixc(metal, rust, amt);
        o.metal = clamp01((1.f - amt) * c.p.metallic);
        o.rough = clamp01(mixf(c.p.roughness, 0.92f, amt) + (fine - 0.5f) * 0.1f);
        o.h = 0.4f + amt * 0.4f * fine + (1.f - amt) * (scratch - 0.5f) * 0.04f;
    });
}

void genFabric(Ctx& c) {
    const int n = 4 * std::max(1, c.freq);  // even thread count keeps the over/under pattern seamless
    const float fn = static_cast<float>(n);
    const int fib = std::min(c.n / 2, 256);
    forEach(c, [&](float u, float v, Px& o) {
        int i = static_cast<int>(std::floor(u * fn)), j = static_cast<int>(std::floor(v * fn));
        float fu = u * fn - static_cast<float>(i), fv = v * fn - static_cast<float>(j);
        bool horizontalOnTop = ((i + j) & 1) == 0;
        float hp = std::sin(kPi * fv), vp = std::sin(kPi * fu);
        float h, thread;
        Col tc;
        if (horizontalOnTop) {  // warp thread running along u
            h = 0.25f + 0.6f * hp * (0.82f + 0.18f * std::sin(kPi * fu));
            tc = mixc(c.c1, c.c2, rnd(j, 0, c.seed + 1u) * c.var);
            thread = valueNoise(u * 16.f, v * static_cast<float>(fib), 16, fib, c.seed + 2u);
        } else {  // weft thread running along v
            h = 0.25f + 0.6f * vp * (0.82f + 0.18f * std::sin(kPi * fv));
            tc = mixc(c.c2, c.c1, rnd(i, 1, c.seed + 3u) * c.var);
            thread = valueNoise(u * static_cast<float>(fib), v * 16.f, fib, 16, c.seed + 4u);
        }
        o.c = scalec(tc, 0.8f + 0.4f * thread);
        o.c = mixc(c.c3, o.c, sstep(0.2f, 0.5f, h));
        o.h = h + (thread - 0.5f) * 0.05f;
    });
}

void genChecker(Ctx& c) {
    const int n = evenAtLeast2(static_cast<float>(c.freq));
    const float fn = static_cast<float>(n);
    forEach(c, [&](float u, float v, Px& o) {
        int iu = static_cast<int>(std::floor(u * fn)), iv = static_cast<int>(std::floor(v * fn));
        float mottle = fbm(u, v, 8, 8, 3, c.seed + 1u);
        o.c = ((iu + iv) & 1) ? c.c2 : c.c1;
        o.c = scalec(o.c, 1.f + (mottle - 0.5f) * 0.1f * c.var);
        o.h = 0.5f;
    });
}

void genStripes(Ctx& c) {
    const int n = evenAtLeast2(static_cast<float>(c.freq));
    const float fn = static_cast<float>(n);
    forEach(c, [&](float u, float v, Px& o) {
        int iu = static_cast<int>(std::floor(u * fn));
        float fu = u * fn - static_cast<float>(iu);
        float mottle = fbm(u, v, 8, 8, 3, c.seed + 1u);
        o.c = (iu & 1) ? c.c2 : c.c1;
        o.c = scalec(o.c, 1.f + (mottle - 0.5f) * 0.1f * c.var);
        float e = std::min(fu, 1.f - fu);
        o.h = 0.4f + 0.2f * sstep(0.f, 0.12f, e);
    });
}

void genHexagons(Ctx& c) {
    const int nx = std::max(2, c.freq), ny = std::max(1, static_cast<int>(std::lround(nx / 1.7320508f)));
    const float s3 = 1.7320508f;
    forEach(c, [&](float u, float v, Px& o) {
        float px = u * static_cast<float>(nx), py = v * static_cast<float>(ny) * s3;
        // Two interleaved rectangular grids; the nearer center wins (classic hex lattice).
        float ax = px - std::floor(px) - 0.5f;
        float ay = py - std::floor(py / s3) * s3 - s3 * 0.5f;
        float bx = (px - 0.5f) - std::floor(px - 0.5f) - 0.5f;
        float by = (py - s3 * 0.5f) - std::floor((py - s3 * 0.5f) / s3) * s3 - s3 * 0.5f;
        bool useA = ax * ax + ay * ay < bx * bx + by * by;
        float gx = useA ? ax : bx, gy = useA ? ay : by;
        float cx = px - gx, cy = py - gy;  // hex center
        float d = std::max(std::fabs(gx), std::fabs(gx) * 0.5f + std::fabs(gy) * (s3 * 0.5f));
        float e = 0.5f - d;  // distance to the hex border
        int hx = wrapi(static_cast<int>(std::lround(cx * 2.f)), nx * 2);
        int hy = wrapi(static_cast<int>(std::lround(cy / (s3 * 0.5f))), ny * 2);
        float tint = rnd(hx, hy, c.seed + 5u);
        float surf = fbm(u, v, 16, 16, 3, c.seed + 6u);
        Col hc = mixc(c.c1, c.c2, clamp01(tint * c.var + (surf - 0.5f) * 0.2f));
        float t = sstep(0.025f, 0.05f, e);
        o.c = mixc(c.c3, hc, t);
        o.h = 0.1f + 0.8f * sstep(0.02f, 0.1f, e) + (surf - 0.5f) * 0.03f;
        o.rough = clamp01(c.p.roughness + (1.f - t) * 0.3f);
    });
}

void genScales(Ctx& c) {
    const int n = evenAtLeast2(static_cast<float>(c.freq));
    const float fn = static_cast<float>(n);
    const float radius = 0.68f;
    forEach(c, [&](float u, float v, Px& o) {
        float qx = u * fn, qy = v * fn;
        int row0 = static_cast<int>(std::floor(qy));
        int bestRow = -1000, bestCi = 0;
        float bestD = 0.f, bestDy = 0.f;
        // Scales in lower rows overlap those above, like fish scales.
        for (int dr = -1; dr <= 1; ++dr) {
            int j = row0 + dr;
            float ox = (j & 1) ? 0.5f : 0.f;
            int i0 = static_cast<int>(std::floor(qx - ox + 0.5f));
            for (int di = -1; di <= 1; ++di) {
                int i = i0 + di;
                float cx = static_cast<float>(i) + ox, cy = static_cast<float>(j) + 0.5f;
                float d = std::sqrt((qx - cx) * (qx - cx) + (qy - cy) * (qy - cy));
                if (d < radius && j > bestRow) {
                    bestRow = j;
                    bestCi = i;
                    bestD = d;
                    bestDy = qy - cy;
                }
            }
        }
        if (bestRow == -1000) {  // gap between scales
            o.c = scalec(c.c3, 0.7f);
            o.h = 0.f;
            o.rough = clamp01(c.p.roughness + 0.2f);
            return;
        }
        int wi = wrapi(bestCi, n), wj = wrapi(bestRow, n);
        float tint = rnd(wi, wj, c.seed + 3u);
        float dn = bestD / radius;
        Col sc = mixc(c.c1, c.c2, clamp01(tint * c.var + (bestDy / radius * 0.5f + 0.5f) * 0.6f));
        // Dark rim on the visible lower arc, soft highlight toward the middle.
        sc = mixc(sc, c.c3, sstep(0.7f, 1.f, dn) * 0.8f);
        o.c = sc;
        o.h = 0.2f + 0.8f * (1.f - dn * dn * 0.8f) * (0.6f + 0.4f * clamp01(0.5f - bestDy / radius * 0.5f + 0.5f * (1.f - dn)));
    });
}

void genStylized(Ctx& c) {
    forEach(c, [&](float u, float v, Px& o) {
        float n1 = fbm(u, v, c.freq, c.freq, 3, c.seed + 1u);
        float n2 = fbm(u, v, c.freq * 2, c.freq * 2, 3, c.seed + 2u);
        float brush = valueNoise(u * 24.f, v * 96.f, 24, 96, c.seed + 3u);  // faint brush strokes
        float a = sstep(0.38f, 0.62f, n1);
        float b = sstep(0.5f, 0.72f, n2) * (0.35f + 0.65f * c.var);
        o.c = mixc(c.c1, c.c2, a);
        o.c = mixc(o.c, c.c3, b);
        o.c = scalec(o.c, 1.f + (brush - 0.5f) * 0.08f * c.var);
        o.h = 0.5f + (n1 - 0.5f) * 0.3f + (brush - 0.5f) * 0.03f;
    });
}

// ---- kind table ------------------------------------------------------------------------

struct KindInfo {
    const char* name;
    void (*gen)(Ctx&);
    Params (*defaults)();
};

Params mk(const char* kind, float scale, Vec4 c1, Vec4 c2, Vec4 c3, float rough, float metal = 0.f, float variation = 0.5f) {
    Params p;
    p.kind = kind;
    p.scale = scale;
    p.color1 = c1;
    p.color2 = c2;
    p.color3 = c3;
    p.roughness = rough;
    p.metallic = metal;
    p.variation = variation;
    return p;
}

const KindInfo kKinds[] = {
    {"noise", genNoise, [] { return mk("noise", 4, {0.25f, 0.27f, 0.30f, 1}, {0.70f, 0.72f, 0.75f, 1}, {0.10f, 0.10f, 0.12f, 1}, 0.7f); }},
    {"marble", genMarble, [] { return mk("marble", 3, {0.92f, 0.92f, 0.90f, 1}, {0.78f, 0.78f, 0.80f, 1}, {0.25f, 0.27f, 0.32f, 1}, 0.15f); }},
    {"wood", genWood, [] { return mk("wood", 6, {0.55f, 0.36f, 0.20f, 1}, {0.36f, 0.20f, 0.10f, 1}, {0.20f, 0.10f, 0.05f, 1}, 0.55f); }},
    {"planks", genPlanks, [] { return mk("planks", 6, {0.60f, 0.42f, 0.25f, 1}, {0.45f, 0.30f, 0.17f, 1}, {0.08f, 0.05f, 0.03f, 1}, 0.6f); }},
    {"bricks", genBricks, [] { return mk("bricks", 8, {0.55f, 0.22f, 0.15f, 1}, {0.42f, 0.17f, 0.12f, 1}, {0.62f, 0.60f, 0.56f, 1}, 0.85f); }},
    {"tiles", genTiles, [] { return mk("tiles", 4, {0.85f, 0.85f, 0.82f, 1}, {0.70f, 0.74f, 0.76f, 1}, {0.35f, 0.35f, 0.35f, 1}, 0.25f); }},
    {"cobblestone", genCobblestone, [] { return mk("cobblestone", 5, {0.50f, 0.48f, 0.45f, 1}, {0.35f, 0.34f, 0.33f, 1}, {0.15f, 0.13f, 0.10f, 1}, 0.85f); }},
    {"grass", genGrass, [] { return mk("grass", 4, {0.20f, 0.45f, 0.10f, 1}, {0.40f, 0.62f, 0.18f, 1}, {0.55f, 0.50f, 0.20f, 1}, 0.8f); }},
    {"dirt", genDirt, [] { return mk("dirt", 3, {0.32f, 0.22f, 0.13f, 1}, {0.45f, 0.33f, 0.20f, 1}, {0.58f, 0.52f, 0.45f, 1}, 0.95f); }},
    {"sand", genSand, [] { return mk("sand", 6, {0.85f, 0.72f, 0.50f, 1}, {0.93f, 0.82f, 0.62f, 1}, {0.50f, 0.40f, 0.28f, 1}, 0.95f); }},
    {"rock", genRock, [] { return mk("rock", 3, {0.28f, 0.27f, 0.27f, 1}, {0.55f, 0.53f, 0.50f, 1}, {0.08f, 0.08f, 0.08f, 1}, 0.9f); }},
    {"metal_brushed", genMetalBrushed, [] { return mk("metal_brushed", 3, {0.55f, 0.56f, 0.58f, 1}, {0.75f, 0.76f, 0.78f, 1}, {0.30f, 0.30f, 0.32f, 1}, 0.35f, 1.f); }},
    {"rust", genRust, [] { return mk("rust", 3, {0.55f, 0.56f, 0.58f, 1}, {0.55f, 0.25f, 0.10f, 1}, {0.25f, 0.10f, 0.05f, 1}, 0.5f, 1.f); }},
    {"fabric", genFabric, [] { return mk("fabric", 6, {0.20f, 0.30f, 0.55f, 1}, {0.17f, 0.26f, 0.50f, 1}, {0.05f, 0.07f, 0.12f, 1}, 0.9f); }},
    {"checker", genChecker, [] { return mk("checker", 8, {0.90f, 0.90f, 0.90f, 1}, {0.15f, 0.15f, 0.17f, 1}, {0.15f, 0.15f, 0.17f, 1}, 0.5f); }},
    {"stripes", genStripes, [] { return mk("stripes", 8, {0.90f, 0.25f, 0.20f, 1}, {0.95f, 0.92f, 0.85f, 1}, {0.30f, 0.10f, 0.08f, 1}, 0.5f); }},
    {"hexagons", genHexagons, [] { return mk("hexagons", 4, {0.20f, 0.50f, 0.60f, 1}, {0.30f, 0.65f, 0.70f, 1}, {0.05f, 0.10f, 0.12f, 1}, 0.4f); }},
    {"scales", genScales, [] { return mk("scales", 8, {0.15f, 0.50f, 0.35f, 1}, {0.30f, 0.75f, 0.50f, 1}, {0.05f, 0.20f, 0.15f, 1}, 0.35f); }},
    {"stylized", genStylized, [] { return mk("stylized", 3, {0.45f, 0.70f, 0.40f, 1}, {0.70f, 0.85f, 0.45f, 1}, {0.30f, 0.50f, 0.35f, 1}, 0.8f); }},
};

const KindInfo* findKind(const std::string& name) {
    for (const auto& k : kKinds)
        if (name == k.name) return &k;
    return nullptr;
}

uint8_t toByte(float x) { return static_cast<uint8_t>(std::lround(clamp01(x) * 255.f)); }

/// Separable wrap-around box blur, used for cavity ambient occlusion.
void boxBlur(const std::vector<float>& src, std::vector<float>& tmp, std::vector<float>& dst, int n, int r) {
    const float inv = 1.f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < n; ++y) {
        const float* row = &src[static_cast<size_t>(y) * n];
        float sum = 0.f;
        for (int k = -r; k <= r; ++k) sum += row[wrapi(k, n)];
        for (int x = 0; x < n; ++x) {
            tmp[static_cast<size_t>(y) * n + x] = sum * inv;
            sum += row[wrapi(x + r + 1, n)] - row[wrapi(x - r, n)];
        }
    }
    for (int x = 0; x < n; ++x) {
        float sum = 0.f;
        for (int k = -r; k <= r; ++k) sum += tmp[static_cast<size_t>(wrapi(k, n)) * n + x];
        for (int y = 0; y < n; ++y) {
            dst[static_cast<size_t>(y) * n + x] = sum * inv;
            sum += tmp[static_cast<size_t>(wrapi(y + r + 1, n)) * n + x] - tmp[static_cast<size_t>(wrapi(y - r, n)) * n + x];
        }
    }
}

}  // namespace

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& k : kKinds) v.emplace_back(k.name);
        return v;
    }();
    return names;
}

Params defaults(const std::string& kind) {
    const KindInfo* k = findKind(kind);
    return k ? k->defaults() : kKinds[0].defaults();
}

Result<TextureSet> generate(const Params& p) {
    const KindInfo* kind = findKind(p.kind);
    if (!kind) {
        std::string list;
        for (const auto& k : kKinds) list += (list.empty() ? "" : ", ") + std::string(k.name);
        return Error::make("invalid_argument", "unknown texture kind '" + p.kind + "'", "valid kinds: " + list);
    }
    const int n = p.size;
    if (n < 32 || n > 2048 || (n & (n - 1)) != 0)
        return Error::make("invalid_argument", "size must be a power of two between 32 and 2048 (got " + std::to_string(n) + ")",
                           "use 64, 128, 256, 512, 1024 or 2048");
    if (!std::isfinite(p.scale) || p.scale <= 0.f || p.scale > 64.f)
        return Error::make("invalid_argument", "scale must be in (0, 64]", "typical values are 2..12");

    Ctx ctx{p, n, p.seed * 0x9E3779B1u + 0x1234567u, std::max(1, static_cast<int>(std::lround(p.scale))),
            fromVec(p.color1), fromVec(p.color2), fromVec(p.color3), clamp01(p.variation), {}};
    ctx.px.assign(static_cast<size_t>(n) * n, Px{});
    kind->gen(ctx);

    const size_t count = static_cast<size_t>(n) * n;
    std::vector<float> height(count);
    for (size_t i = 0; i < count; ++i) height[i] = ctx.px[i].h;

    // Cavity AO: how far each texel sits below its blurred neighborhood.
    std::vector<float> tmp(count), blurred(count);
    boxBlur(height, tmp, blurred, n, std::max(2, n / 48));

    // Roughness gets a little extra low-amplitude breakup so large flat areas do not look CG-clean.
    const uint32_t rseed = ctx.seed + 9001u;
    const int rf = std::min(n / 4, 64);

    TextureSet out;
    out.albedo = Image(n, n);
    out.normal = Image(n, n);
    out.orm = Image(n, n);

    const float k = p.bump * static_cast<float>(n) / 128.f;
    const float inv = 1.f / static_cast<float>(n);
    for (int y = 0; y < n; ++y) {
        const int ym = wrapi(y - 1, n), yp = wrapi(y + 1, n);
        for (int x = 0; x < n; ++x) {
            const int xm = wrapi(x - 1, n), xp = wrapi(x + 1, n);
            const size_t i = static_cast<size_t>(y) * n + x;
            const Px& px = ctx.px[i];

            float dhdx = (height[static_cast<size_t>(y) * n + xp] - height[static_cast<size_t>(y) * n + xm]) * 0.5f;
            float dhdrow = (height[static_cast<size_t>(yp) * n + x] - height[static_cast<size_t>(ym) * n + x]) * 0.5f;
            float nx = -dhdx * k, ny = dhdrow * k, nz = 1.f;  // +Y up: image rows grow downward
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len;
            ny /= len;
            nz /= len;

            float ao = 1.f - clamp01((blurred[i] - height[i]) * 2.2f) * 0.75f;
            float breakup = valueNoise((static_cast<float>(x) + 0.5f) * inv * static_cast<float>(rf),
                                       (static_cast<float>(y) + 0.5f) * inv * static_cast<float>(rf), rf, rf, rseed);
            float rough = clamp01(px.rough + (breakup - 0.5f) * 0.25f * ctx.var);
            Col col = scalec(px.c, 0.8f + 0.2f * ao);

            uint8_t* a = out.albedo.at(x, y);
            a[0] = toByte(col.r);
            a[1] = toByte(col.g);
            a[2] = toByte(col.b);
            a[3] = 255;
            uint8_t* nm = out.normal.at(x, y);
            nm[0] = toByte(nx * 0.5f + 0.5f);
            nm[1] = toByte(ny * 0.5f + 0.5f);
            nm[2] = toByte(nz * 0.5f + 0.5f);
            nm[3] = 255;
            uint8_t* orm = out.orm.at(x, y);
            orm[0] = toByte(ao);
            orm[1] = toByte(rough);
            orm[2] = toByte(px.metal);
            orm[3] = 255;
        }
    }
    return out;
}

}  // namespace sky::texgen
