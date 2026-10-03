#include "skywalker/fx/Groom.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>
#include <unordered_map>

#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"
#include "skywalker/scene/Scene.h"

namespace sky {

// ---------------------------------------------------------------------------------------
// Component reflection
// ---------------------------------------------------------------------------------------

const TypeInfo& Groom::type() {
    static const TypeInfo info{
        "groom",
        "Strand hair and fur grown on the entity's mesh (or `target`'s), or loaded from a .hair/.groom.json/.skygroom "
        "file. Rendered as real strands (Marschner shading, deep opacity self-shadowing, multiple scattering) and "
        "simulated on the GPU (gravity, wind, collisions). Easiest start: groom_create with a preset (hair_straight, "
        "hair_wavy, hair_curly, hair_ponytail, hair_short, fur_short, fur_long), then tweak fields.",
        {
            SKY_FIELD(Groom, preset, String, "Preset it was made from (informational)"),
            SKY_FIELD(Groom, visible, Bool, "Draw the groom"),
            SKY_FIELD(Groom, source, String,
                      "Groom file (.hair = Cem Yuksel's format, .groom.json, .skygroom), project-relative; \"\" = grow "
                      "procedurally on the mesh"),
            SKY_FIELD(Groom, target, String, "Entity whose mesh grows the hair (\"\" = this entity)"),
            SKY_FIELD_RANGE(Groom, importScale, Float, "Imported files: scale to meters (0.01 for centimeters)", 1e-5f, 1000.f),
            SKY_FIELD(Groom, importZUp, Bool, "Imported files: the source is Z-up"),
            SKY_FIELD_RANGE(Groom, strands, Int, "Rendered strands (head of hair 50k-150k; fur 100k+). Imported files: cap",
                            1.f, 1000000.f),
            SKY_FIELD_RANGE(Groom, guides, Int, "Simulated guide strands (0 = automatic, ~4*sqrt(strands))", 0.f, 8192.f),
            SKY_FIELD_RANGE(Groom, segments, Int, "Segments per strand (smoothness of curls; 4 for fur, 16-31 for long hair)",
                            2.f, 31.f),
            SKY_FIELD_RANGE(Groom, length, Float, "Strand length in meters", 0.001f, 5.f),
            SKY_FIELD_RANGE(Groom, lengthVariation, Float, "Random length variation (fraction)", 0.f, 0.95f),
            SKY_FIELD_RANGE(Groom, widthRoot, Float, "Strand width at the root in millimeters (human hair 0.05-0.1)", 0.001f, 10.f),
            SKY_FIELD_RANGE(Groom, widthTip, Float, "Strand width at the tip in millimeters", 0.f, 10.f),
            SKY_FIELD(Groom, direction, Vec3, "Comb direction in the mesh's space (e.g. [0,-1,-0.4] = down and back)"),
            SKY_FIELD_RANGE(Groom, directionBlend, Float, "0 = grow along the surface normal, 1 = along `direction`", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, gravity, Float, "Droop: 0 = stands up (fur, crew cut) .. 1 = hangs down", 0.f, 2.f),
            SKY_FIELD_RANGE(Groom, curlRadius, Float, "Curl (helix) radius in meters (0.005-0.02 for curly hair)", 0.f, 0.2f),
            SKY_FIELD_RANGE(Groom, curlFrequency, Float, "Curl turns per meter of strand", 0.f, 200.f),
            SKY_FIELD_RANGE(Groom, wave, Float, "Wave amplitude in meters", 0.f, 0.2f),
            SKY_FIELD_RANGE(Groom, waveFrequency, Float, "Waves per meter of strand", 0.f, 100.f),
            SKY_FIELD_RANGE(Groom, clumps, Int, "Clumps strands gather into (0 = none; hundreds for a head)", 0.f, 100000.f),
            SKY_FIELD_RANGE(Groom, clumpStrength, Float, "How far strands pull into their clump", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, clumpShape, Float, "Clump profile: > 1 tips clump more than roots, < 1 clump early", 0.1f, 8.f),
            SKY_FIELD_RANGE(Groom, frizz, Float, "Noise / flyaways in meters", 0.f, 0.1f),
            SKY_FIELD_RANGE(Groom, frizzScale, Float, "Frizz noise frequency (per meter)", 0.1f, 1000.f),
            SKY_FIELD_ENUM(Groom, maskChannel, "Vertex-color channel that controls density (scalp painting)", "none", "r", "g",
                           "b", "a"),
            SKY_FIELD(Groom, maskDirection, Vec3, "Grow only where the surface faces this way (mesh space; with maskAngle)"),
            SKY_FIELD_RANGE(Groom, maskAngle, Float, "Degrees around maskDirection that grow hair (180 = everywhere)", 0.f, 180.f),
            SKY_FIELD_RANGE(Groom, maskSoftness, Float, "Soft hairline width in degrees", 0.f, 90.f),
            SKY_FIELD_RANGE(Groom, melanin, Float,
                            "Pigment (physically based absorption): 0 white, 0.1 platinum, 0.2 blond, 0.35 dark blond, 0.5 light "
                            "brown, 0.7 brown, 0.85 dark brown, 0.95+ black",
                            0.f, 1.f),
            SKY_FIELD_RANGE(Groom, redness, Float, "Red pigment (pheomelanin) fraction: 0.5 auburn, 0.9 ginger", 0.f, 1.f),
            SKY_FIELD(Groom, dye, Color, "Dye tint (white = natural)"),
            SKY_FIELD(Groom, rootColor, Color, "Color multiplier at the roots (root-to-tip gradient, e.g. dark roots)"),
            SKY_FIELD(Groom, tipColor, Color, "Color multiplier at the tips (sun-bleached or dyed tips)"),
            SKY_FIELD_RANGE(Groom, colorVariation, Float, "Per-strand pigment variation", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, roughness, Float, "Longitudinal roughness: highlight width (0.2 silky .. 0.6 dull)", 0.02f, 1.f),
            SKY_FIELD_RANGE(Groom, radialRoughness, Float, "Azimuthal roughness: softness of light through the strand", 0.05f, 1.f),
            SKY_FIELD_RANGE(Groom, specular, Float, "Primary (white) highlight strength", 0.f, 4.f),
            SKY_FIELD_RANGE(Groom, scatter, Float, "Multiple scattering: light hair glows, dark hair stays rich", 0.f, 4.f),
            SKY_FIELD_RANGE(Groom, cuticleTilt, Float, "Cuticle tilt in degrees (separates the two highlights)", 0.f, 10.f),
            SKY_FIELD_RANGE(Groom, density, Float,
                            "Coverage per strand (2 = a full head of hair at typical strand counts; lower for wispy, "
                            "higher for thick)", 0.05f, 8.f),
            SKY_FIELD(Groom, simulate, Bool, "Simulate on the GPU (gravity, wind, collisions)"),
            SKY_FIELD_RANGE(Groom, stiffness, Float, "How strongly strands keep their groomed shape", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, rootStiffness, Float, "Stiffness near the roots", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, damping, Float, "Velocity damping", 0.f, 1.f),
            SKY_FIELD_RANGE(Groom, wind, Float, "Environment wind influence", 0.f, 10.f),
            SKY_FIELD(Groom, collide, Bool, "Collide with the mesh (sphere/capsule proxy) and `colliders`"),
            SKY_FIELD(Groom, colliders, String, "Comma-separated entity names that hair collides with (shoulders, hands)"),
            SKY_FIELD_ENUM(Groom, lod, "auto = strands up close, cards far away; strands; cards (cheapest)", "auto", "strands",
                           "cards"),
            SKY_FIELD_RANGE(Groom, cardsBelow, Float, "auto LOD: draw cards when the groom is smaller than this (pixels)", 0.f,
                            4096.f),
            SKY_FIELD(Groom, castShadows, Bool, "Cast sun shadows on the scene"),
            SKY_FIELD(Groom, seed, Int, "Random seed (a different head of the same style)"),
        }};
    return info;
}

const std::vector<std::string>& Groom::presets() {
    static const std::vector<std::string> n{"hair_straight", "hair_wavy", "hair_curly", "hair_ponytail", "hair_short",
                                            "fur_short", "fur_long"};
    return n;
}

static_assert(std::is_standard_layout_v<Groom>);

size_t GroomData::memoryBytes() const {
    return guideRest.size() * sizeof(Vec3) + children.size() * sizeof(Child) + offsets.size() * sizeof(Vec3);
}

namespace fx {

namespace {

constexpr float kPi = 3.14159265358979f;

// --- little helpers ----------------------------------------------------------------------

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
template <typename T>
uint64_t fnvv(uint64_t h, const T& v) {
    return fnv(h, &v, sizeof(T));
}
uint64_t fnvs(uint64_t h, const std::string& s) { return fnv(fnvv(h, s.size()), s.data(), s.size()); }

float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

Vec3 safeNormalize(Vec3 v, Vec3 fallback) {
    float l = length(v);
    return l > 1e-12f ? v / l : fallback;
}

// Value noise (3D) for frizz: deterministic, smooth.
float hash3(int x, int y, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffff) / 16777215.f;
}
float valueNoise3(Vec3 p) {
    int xi = static_cast<int>(std::floor(p.x)), yi = static_cast<int>(std::floor(p.y)), zi = static_cast<int>(std::floor(p.z));
    float fx = p.x - static_cast<float>(xi), fy = p.y - static_cast<float>(yi), fz = p.z - static_cast<float>(zi);
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    fz = fz * fz * (3 - 2 * fz);
    float r = 0;
    for (int c = 0; c < 8; ++c) {
        int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
        r += w * hash3(xi + dx, yi + dy, zi + dz);
    }
    return r * 2.f - 1.f;
}

/// Parallel-transport frames of a polyline (mirrored by the GPU in Hair.metal).
/// `ref` / `alt` are the mesh X / Z axes in the curve's space (rotate with the groom).
void transportFrames(const Vec3* b, int P, Vec3 ref, Vec3* T, Vec3* N, Vec3 alt = Vec3{0, 0, 1}) {
    Vec3 prevT{0, 1, 0};
    for (int k = 0; k < P; ++k) {
        Vec3 d = k + 1 < P ? b[k + 1] - b[k] : b[k] - b[k - 1];
        T[k] = safeNormalize(d, prevT);
        prevT = T[k];
    }
    Vec3 n0 = ref - T[0] * dot(ref, T[0]);
    if (length(n0) < 0.1f) n0 = alt - T[0] * dot(alt, T[0]);
    N[0] = safeNormalize(n0, Vec3{1, 0, 0});
    for (int k = 1; k < P; ++k) {
        Vec3 n = N[k - 1] - T[k] * dot(N[k - 1], T[k]);
        N[k] = safeNormalize(n, N[k - 1]);
    }
}

// --- surface sampling --------------------------------------------------------------------

struct Surface {
    const MeshData* mesh = nullptr;
    std::vector<uint32_t> tris;  // triangle index into mesh->indices / 3
    std::vector<float> cdf;
    std::vector<float> triMax;   // upper bound of the mask per triangle
    float area = 0;               // masked area estimate
};

Vec3 vpos(const MeshData& m, uint32_t i) {
    const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
    return {v[0], v[1], v[2]};
}
Vec3 vnorm(const MeshData& m, uint32_t i) {
    const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
    return {v[3], v[4], v[5]};
}
Vec4 vcolor(const MeshData& m, uint32_t i) {
    const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
    return {v[8], v[9], v[10], v[11]};
}

float maskWeight(const Groom& g, Vec3 n, Vec4 color) {
    float w = 1.f;
    if (g.maskAngle < 179.99f) {
        Vec3 dir = safeNormalize(g.maskDirection, Vec3{0, 1, 0});
        float ang = degrees(std::acos(std::clamp(dot(safeNormalize(n, Vec3{0, 1, 0}), dir), -1.f, 1.f)));
        float soft = std::max(g.maskSoftness, 0.01f);
        w *= std::clamp((g.maskAngle - ang) / soft + 0.5f, 0.f, 1.f);
    }
    const std::string& ch = g.maskChannel;
    if (ch == "r") w *= std::clamp(color.x, 0.f, 1.f);
    else if (ch == "g") w *= std::clamp(color.y, 0.f, 1.f);
    else if (ch == "b") w *= std::clamp(color.z, 0.f, 1.f);
    else if (ch == "a") w *= std::clamp(color.w, 0.f, 1.f);
    return w;
}

Surface buildSurface(const MeshData& m, const Groom& g) {
    Surface s;
    s.mesh = &m;
    const size_t triCount = m.indices.size() / 3;
    double acc = 0, masked = 0;
    for (size_t t = 0; t < triCount; ++t) {
        uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        Vec3 a = vpos(m, i0), b = vpos(m, i1), c = vpos(m, i2);
        float area = 0.5f * length(cross(b - a, c - a));
        if (!(area > 1e-12f)) continue;
        // Conservative mask bound: vertices plus the face normal (a cap can cut through a triangle).
        Vec3 fn = safeNormalize(cross(b - a, c - a), Vec3{0, 1, 0});
        float mx = std::max({maskWeight(g, vnorm(m, i0), vcolor(m, i0)), maskWeight(g, vnorm(m, i1), vcolor(m, i1)),
                             maskWeight(g, vnorm(m, i2), vcolor(m, i2)), maskWeight(g, fn, (vcolor(m, i0) + vcolor(m, i1) + vcolor(m, i2)) * (1.f / 3.f))});
        if (mx <= 0.f) continue;
        mx = std::min(1.f, mx * 1.25f + 0.02f);
        acc += area * mx;
        float avgMask = (maskWeight(g, vnorm(m, i0), vcolor(m, i0)) + maskWeight(g, vnorm(m, i1), vcolor(m, i1)) +
                         maskWeight(g, vnorm(m, i2), vcolor(m, i2))) / 3.f;
        masked += area * std::max(avgMask, 0.f);
        s.tris.push_back(static_cast<uint32_t>(t));
        s.cdf.push_back(static_cast<float>(acc));
        s.triMax.push_back(mx);
    }
    if (acc > 0) {
        for (float& c : s.cdf) c = static_cast<float>(c / acc);
        s.cdf.back() = 1.f;
    }
    s.area = static_cast<float>(std::max(masked, acc * 0.05));
    return s;
}

struct Root {
    Vec3 p, n;
};

/// Area-weighted random points where the mask allows hair (rejection sampling).
bool sampleRoot(const Surface& s, const Groom& g, Random& rng, Root& out) {
    if (s.cdf.empty()) return false;
    const MeshData& m = *s.mesh;
    for (int attempt = 0; attempt < 64; ++attempt) {
        float u = rng.nextFloat();
        size_t ti = static_cast<size_t>(std::lower_bound(s.cdf.begin(), s.cdf.end(), u) - s.cdf.begin());
        ti = std::min(ti, s.cdf.size() - 1);
        size_t t = s.tris[ti];
        uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        float r1 = rng.nextFloat(), r2 = rng.nextFloat();
        float sq = std::sqrt(r1);
        float b0 = 1.f - sq, b1 = sq * (1.f - r2), b2 = sq * r2;
        Vec3 p = vpos(m, i0) * b0 + vpos(m, i1) * b1 + vpos(m, i2) * b2;
        Vec3 n = safeNormalize(vnorm(m, i0) * b0 + vnorm(m, i1) * b1 + vnorm(m, i2) * b2,
                               safeNormalize(cross(vpos(m, i1) - vpos(m, i0), vpos(m, i2) - vpos(m, i0)), Vec3{0, 1, 0}));
        Vec4 c = vcolor(m, i0) * b0 + vcolor(m, i1) * b1 + vcolor(m, i2) * b2;
        float accept = maskWeight(g, n, c) / s.triMax[ti];
        if (rng.nextFloat() < accept) {
            out = {p, n};
            return true;
        }
    }
    return false;
}

// --- parallel loops (deterministic: every item is computed independently) -----------------

template <typename F>
void parallelFor(size_t n, F&& f) {
    unsigned hw = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (n < 2048 || hw == 1) {
        for (size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::vector<std::thread> threads;
    size_t chunk = (n + hw - 1) / hw;
    for (unsigned t = 0; t < hw; ++t) {
        size_t a = t * chunk, b = std::min(n, a + chunk);
        if (a >= b) break;
        threads.emplace_back([a, b, &f]() {
            for (size_t i = a; i < b; ++i) f(i);
        });
    }
    for (auto& th : threads) th.join();
}

// --- spatial grid for nearest-root queries ------------------------------------------------

class RootGrid {
public:
    RootGrid(const std::vector<Vec3>& pts, float cell) : pts_(pts), cell_(std::max(cell, 1e-5f)) {
        for (uint32_t i = 0; i < pts.size(); ++i) cells_[key(cellOf(pts[i]))].push_back(i);
    }
    /// Up to k nearest points (ascending distance).
    int nearest(Vec3 p, int k, uint32_t* idx, float* dist) const {
        k = std::clamp(k, 1, 8);
        float bd[8];
        uint32_t bi[8];
        int n = 0;
        int ring = 1;
        const int maxRing = 64;
        const int want = std::min<int>(k, static_cast<int>(pts_.size()));
        while (true) {
            n = 0;
            auto c = cellOf(p);
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dy = -ring; dy <= ring; ++dy) {
                    for (int dx = -ring; dx <= ring; ++dx) {
                        auto it = cells_.find(key({c[0] + dx, c[1] + dy, c[2] + dz}));
                        if (it == cells_.end()) continue;
                        for (uint32_t i : it->second) {
                            float d = distance(p, pts_[i]);
                            if (n == k && d >= bd[k - 1]) continue;
                            int j = n < k ? n++ : k - 1;  // insertion into the small sorted list
                            while (j > 0 && (bd[j - 1] > d || (bd[j - 1] == d && bi[j - 1] > i))) {
                                bd[j] = bd[j - 1];
                                bi[j] = bi[j - 1];
                                --j;
                            }
                            bd[j] = d;
                            bi[j] = i;
                        }
                    }
                }
            }
            // Everything outside the searched block is at least ring * cell away.
            if ((n >= want && (n == 0 || bd[n - 1] <= static_cast<float>(ring) * cell_)) || ring >= maxRing) break;
            ring *= 2;
        }
        for (int i = 0; i < n; ++i) {
            idx[i] = bi[i];
            dist[i] = bd[i];
        }
        return n;
    }

private:
    std::array<int, 3> cellOf(Vec3 p) const {
        return {static_cast<int>(std::floor(p.x / cell_)), static_cast<int>(std::floor(p.y / cell_)),
                static_cast<int>(std::floor(p.z / cell_))};
    }
    static uint64_t key(std::array<int, 3> c) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(c[0]) & 0x1fffff) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(c[1]) & 0x1fffff) << 21) |
               static_cast<uint64_t>(static_cast<uint32_t>(c[2]) & 0x1fffff);
    }
    const std::vector<Vec3>& pts_;
    float cell_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

// --- collision proxy -----------------------------------------------------------------------

FxCollider proxyFromBounds(const Aabb& b) {
    Vec3 c = b.center(), e = b.extents();
    float ex[3] = {e.x, e.y, e.z};
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int bb) { return ex[a] > ex[bb]; });
    FxCollider p;
    if (ex[order[0]] > ex[order[1]] * 1.3f) {
        float r = (ex[order[1]] + ex[order[2]]) * 0.5f;
        Vec3 axis{order[0] == 0 ? 1.f : 0.f, order[0] == 1 ? 1.f : 0.f, order[0] == 2 ? 1.f : 0.f};
        float half = std::max(ex[order[0]] - r, 0.f);
        p.kind = FxCollider::Kind::Capsule;
        p.a = c - axis * half;
        p.b = c + axis * half;
        p.radius = r;
    } else {
        p.kind = FxCollider::Kind::Sphere;
        p.a = p.b = c;
        p.radius = (e.x + e.y + e.z) / 3.f;
    }
    return p;
}

Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = b - a;
    float t = dot(ab, ab) > 1e-12f ? std::clamp(dot(p - a, ab) / dot(ab, ab), 0.f, 1.f) : 0.f;
    return a + ab * t;
}

/// Pushes p out of the proxy (with a small margin). Returns true if moved.
bool pushOut(const FxCollider& c, Vec3& p, float margin) {
    if (c.kind == FxCollider::Kind::Plane) {
        float d = dot(p - c.a, c.b);
        if (d < margin) {
            p += c.b * (margin - d);
            return true;
        }
        return false;
    }
    Vec3 q = c.kind == FxCollider::Kind::Sphere ? c.a : closestOnSegment(p, c.a, c.b);
    Vec3 d = p - q;
    float l = length(d);
    float r = c.radius + margin;
    if (l < r) {
        p = q + safeNormalize(d, Vec3{0, 1, 0}) * r;
        return true;
    }
    return false;
}

// --- child construction -----------------------------------------------------------------

/// Base curve of a child: its root plus the weighted displacement of its guides, sampled at
/// u = k/(P-1) * lengthScale.
void baseCurve(const GroomData& d, const GroomData::Child& c, const Vec3* guides, Vec3 rootPos, Vec3* out) {
    const int P = static_cast<int>(d.points);
    for (int k = 0; k < P; ++k) {
        float u = static_cast<float>(k) / static_cast<float>(P - 1) * c.lengthScale * static_cast<float>(P - 1);
        int i0 = std::min(static_cast<int>(u), P - 2);
        float f = u - static_cast<float>(i0);
        Vec3 p = rootPos;
        for (int j = 0; j < 3; ++j) {
            if (c.weight[j] == 0.f) continue;
            const Vec3* g = guides + static_cast<size_t>(c.guide[j]) * static_cast<size_t>(P);
            p += (lerp(g[i0], g[i0 + 1], f) - g[0]) * c.weight[j];
        }
        out[k] = p;
    }
}

void assignGuides(GroomData& d, const std::vector<Vec3>& guideRoots, const RootGrid& grid, float spacing) {
    parallelFor(d.children.size(), [&](size_t ci) {
        GroomData::Child& c = d.children[ci];
        uint32_t idx[3];
        float dist[3];
        int n = grid.nearest(c.root, 3, idx, dist);
        float wsum = 0;
        for (int j = 0; j < 3; ++j) {
            c.guide[j] = j < n ? idx[j] : (n ? idx[0] : 0);
            float w = j < n ? 1.f / std::pow(dist[j] + spacing * 0.35f, 2.f) : 0.f;
            c.weight[j] = w;
            wsum += w;
        }
        for (float& w : c.weight) w = wsum > 0 ? w / wsum : 0.f;
        if (wsum <= 0) c.weight[0] = 1.f;
    });
    (void)guideRoots;
}

void computeBounds(GroomData& d) {
    std::vector<Vec3> pts;
    reconstructStrands(d, d.guideRest, Mat4{}, pts);
    Aabb b{Vec3(1e30f), Vec3(-1e30f)};
    for (const Vec3& p : pts) {
        b.min = vmin(b.min, p);
        b.max = vmax(b.max, p);
    }
    for (const Vec3& p : d.guideRest) {
        b.min = vmin(b.min, p);
        b.max = vmax(b.max, p);
    }
    if (pts.empty() && d.guideRest.empty()) b = {Vec3(0.f), Vec3(0.f)};
    d.bounds = b;
}

int pointsFor(const Groom& g) { return std::clamp(g.segments, 2, 31) + 1; }

int guideCountFor(const Groom& g, int strands) {
    int n = g.guides > 0 ? g.guides : static_cast<int>(std::lround(std::sqrt(static_cast<double>(strands)) * 4.0));
    return std::clamp(n, 1, std::max(1, std::min(strands, 8192)));
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------------------

Result<GroomData> generateGroom(const Groom& g, const MeshData* mesh) {
    if (!mesh || mesh->indices.size() < 3) {
        return Error::make("no_mesh", "the groom needs a mesh to grow on", "add a mesh component or set `target`");
    }
    const int P = pointsFor(g);
    const int N = std::clamp(g.strands, 1, 1000000);
    const int G = guideCountFor(g, N);
    Surface surf = buildSurface(*mesh, g);
    if (surf.cdf.empty()) {
        return Error::make("empty_mask", "the mask leaves no surface to grow hair on",
                           "widen maskAngle, check maskDirection or the vertex-color channel");
    }
    Random rng(0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(g.seed)) * 2654435761ull));

    GroomData d;
    d.points = static_cast<uint32_t>(P);
    Aabb mb = mesh->bounds;
    if (!(mb.max.x >= mb.min.x)) {
        mb = {Vec3(1e30f), Vec3(-1e30f)};
        for (size_t i = 0; i < mesh->vertexCount(); ++i) {
            mb.min = vmin(mb.min, vpos(*mesh, static_cast<uint32_t>(i)));
            mb.max = vmax(mb.max, vpos(*mesh, static_cast<uint32_t>(i)));
        }
    }
    d.proxy = proxyFromBounds(mb);
    d.proxy.radius *= 0.97f;  // slightly inside the surface: hair lies on it, not above it
    d.hasProxy = true;

    // Guide roots and rest shapes.
    std::vector<Root> guideRoots;
    guideRoots.reserve(static_cast<size_t>(G));
    for (int i = 0; i < G; ++i) {
        Root r;
        if (sampleRoot(surf, g, rng, r)) guideRoots.push_back(r);
    }
    if (guideRoots.empty()) return Error::make("empty_mask", "could not place any strand roots", "widen the mask");
    const float spacing = std::sqrt(surf.area / static_cast<float>(guideRoots.size()));
    d.spacing = spacing;
    const Vec3 comb = length(g.direction) > 1e-6f ? normalize(g.direction) : Vec3{0, 0, 0};
    const Vec3 down{0, -1, 0};
    d.guideRest.resize(guideRoots.size() * static_cast<size_t>(P));
    const float margin = std::max(0.0015f, g.length * 0.004f);
    for (size_t gi = 0; gi < guideRoots.size(); ++gi) {
        const Root& r = guideRoots[gi];
        Vec3* out = &d.guideRest[gi * static_cast<size_t>(P)];
        // Combed hair lies along the scalp: blend from the normal toward the comb direction
        // projected onto the tangent plane (falls back to "downhill" where they are parallel).
        Vec3 combT = comb - r.n * dot(comb, r.n);
        if (length(combT) < 0.15f) combT = down - r.n * dot(down, r.n);
        combT = safeNormalize(combT, r.n);
        Vec3 dir = safeNormalize(lerp(r.n, combT, length(comb) > 0 ? g.directionBlend : 0.f), r.n);
        float segLen = g.length / static_cast<float>(P - 1);
        Vec3 pos = r.p;
        out[0] = pos;
        for (int k = 1; k < P; ++k) {
            // Droop: bend toward gravity a little more each segment.
            dir = safeNormalize(dir + down * (g.gravity * 3.2f / static_cast<float>(P - 1)), dir);
            Vec3 next = pos + dir * segLen;
            // Lie on the scalp instead of growing through it (long hair drapes over the head).
            if (pushOut(d.proxy, next, margin * (1.f + static_cast<float>(k) * 0.6f))) {
                next = pos + safeNormalize(next - pos, dir) * segLen;
                pushOut(d.proxy, next, margin * (1.f + static_cast<float>(k) * 0.6f));
                dir = safeNormalize(next - pos, dir);
            }
            pos = next;
            out[k] = pos;
        }
    }

    // Children: roots, guides, length, variation.
    d.children.resize(static_cast<size_t>(N));
    size_t placed = 0;
    for (int i = 0; i < N; ++i) {
        Root r;
        if (!sampleRoot(surf, g, rng, r)) continue;
        GroomData::Child& c = d.children[placed++];
        c.root = r.p;
        c.lengthScale = 1.f - std::clamp(g.lengthVariation, 0.f, 0.95f) * rng.nextFloat();
        c.random = rng.nextFloat();
        c.width = 0.8f + 0.4f * rng.nextFloat();
    }
    d.children.resize(placed);
    if (placed == 0) return Error::make("empty_mask", "could not place any strand roots", "widen the mask");

    std::vector<Vec3> groots;
    for (const Root& r : guideRoots) groots.push_back(r.p);
    RootGrid grid(groots, spacing * 2.f);
    assignGuides(d, groots, grid, spacing);

    // Clumps: the first `clumps` children (already in random order) are clump centers.
    const size_t C = std::min<size_t>(static_cast<size_t>(std::max(g.clumps, 0)), d.children.size());
    std::vector<uint32_t> clumpOf(d.children.size(), UINT32_MAX);
    if (C > 0 && g.clumpStrength > 0.f) {
        std::vector<Vec3> croots(C);
        for (size_t i = 0; i < C; ++i) croots[i] = d.children[i].root;
        RootGrid cgrid(croots, 2.f * std::sqrt(surf.area / static_cast<float>(C)));
        parallelFor(d.children.size(), [&](size_t i) {
            uint32_t idx;
            float dist;
            if (cgrid.nearest(d.children[i].root, 1, &idx, &dist)) clumpOf[i] = idx;
        });
    }

    const Vec3 ref{1, 0, 0};
    auto curled = [&](const Vec3* b, const Vec3* t, const Vec3* n, float phase, float rand, Vec3* out) {
        float s = 0;
        for (int k = 0; k < P; ++k) {
            if (k > 0) s += distance(b[k], b[k - 1]);
            float tk = static_cast<float>(k) / static_cast<float>(P - 1);
            Vec3 bn = cross(t[k], n[k]);
            Vec3 o{0, 0, 0};
            float ramp = smoothstep(0.f, 0.18f, tk);
            if (g.curlRadius > 0.f && g.curlFrequency > 0.f) {
                float th = 2.f * kPi * (g.curlFrequency * s + phase);
                float rad = g.curlRadius * (0.85f + 0.3f * rand) * ramp;
                o += (n[k] * std::cos(th) + bn * std::sin(th)) * rad;
            }
            if (g.wave > 0.f && g.waveFrequency > 0.f) {
                float th = 2.f * kPi * (g.waveFrequency * s + phase * 0.37f);
                o += n[k] * (std::sin(th) * g.wave * ramp);
            }
            out[k] = b[k] + o;
        }
    };
    // Each clump's (curled) target curve, computed once.
    const size_t Pz = static_cast<size_t>(P);
    std::vector<Vec3> clumpCurves(C * Pz);
    parallelFor(C, [&](size_t ci) {
        std::array<Vec3, 32> b, t, n;
        const GroomData::Child& cc = d.children[ci];
        baseCurve(d, cc, d.guideRest.data(), cc.root, b.data());
        transportFrames(b.data(), P, ref, t.data(), n.data());
        curled(b.data(), t.data(), n.data(), cc.random * 7.31f, cc.random, &clumpCurves[ci * Pz]);
    });

    // Final shapes -> offsets in the base curve's frames (children are independent: parallel).
    d.offsets.resize(d.children.size() * Pz);
    parallelFor(d.children.size(), [&](size_t i) {
        std::array<Vec3, 32> base, T, Nn, own;
        const GroomData::Child& c = d.children[i];
        baseCurve(d, c, d.guideRest.data(), c.root, base.data());
        transportFrames(base.data(), P, ref, T.data(), Nn.data());
        float ownPhase = c.random * 7.31f;
        bool clumped = clumpOf[i] != UINT32_MAX && clumpOf[i] != i;
        float phase = ownPhase;
        if (clumped) {
            float clumpPhase = d.children[clumpOf[i]].random * 7.31f;
            phase = clumpPhase + (ownPhase - clumpPhase) * 0.15f * (1.f - g.clumpStrength);
        }
        curled(base.data(), T.data(), Nn.data(), phase, c.random, own.data());
        if (clumped) {
            const Vec3* cc = &clumpCurves[clumpOf[i] * Pz];
            for (int k = 0; k < P; ++k) {
                float tk = static_cast<float>(k) / static_cast<float>(P - 1);
                float w = g.clumpStrength * std::pow(tk, g.clumpShape);
                own[k] = lerp(own[k], cc[k], w);
            }
        }
        if (g.frizz > 0.f) {
            float amp = g.frizz * (0.3f + 1.7f * std::pow(c.random, 4.f));
            Vec3 seedOff{c.random * 131.f, c.random * 71.f + 17.f, c.random * 37.f + 5.f};
            float s = 0;
            for (int k = 1; k < P; ++k) {
                s += distance(base[k], base[k - 1]);
                float tk = static_cast<float>(k) / static_cast<float>(P - 1);
                Vec3 q = seedOff + Vec3{s * g.frizzScale, 0, 0};
                Vec3 nz{valueNoise3(q), valueNoise3(q + Vec3{0, 19.1f, 0}), valueNoise3(q + Vec3{0, 0, 41.7f})};
                own[k] += nz * (amp * std::pow(tk, 1.5f));
            }
        }
        own[0] = base[0];
        for (int k = 1; k < P; ++k) pushOut(d.proxy, own[k], margin);
        for (int k = 0; k < P; ++k) {
            Vec3 dv = own[k] - base[k];
            Vec3 bn = cross(T[k], Nn[k]);
            d.offsets[i * Pz + static_cast<size_t>(k)] = {dot(dv, T[k]), dot(dv, Nn[k]), dot(dv, bn)};
        }
    });
    computeBounds(d);
    return d;
}

namespace {

/// Resamples a polyline to `P` points evenly spaced along its arc length.
void resample(const Vec3* pts, uint32_t count, int P, Vec3* out) {
    if (count == 0) {
        for (int k = 0; k < P; ++k) out[k] = Vec3{0, 0, 0};
        return;
    }
    if (count == 1) {
        for (int k = 0; k < P; ++k) out[k] = pts[0];
        return;
    }
    std::vector<float> acc(count, 0.f);
    for (uint32_t i = 1; i < count; ++i) acc[i] = acc[i - 1] + distance(pts[i], pts[i - 1]);
    float total = acc.back();
    uint32_t seg = 0;
    for (int k = 0; k < P; ++k) {
        float s = total * static_cast<float>(k) / static_cast<float>(P - 1);
        while (seg + 2 < count && acc[seg + 1] < s) ++seg;
        float len = acc[seg + 1] - acc[seg];
        float f = len > 1e-12f ? std::clamp((s - acc[seg]) / len, 0.f, 1.f) : 0.f;
        out[k] = lerp(pts[seg], pts[seg + 1], f);
    }
}

}  // namespace

Result<GroomData> groomFromStrands(const Groom& g, const StrandSet& strands, const MeshData* mesh) {
    const size_t total = strands.strandCount();
    if (total == 0) return Error::make("empty_groom", "the groom file has no strands");
    const int P = pointsFor(g);
    // Deterministic shuffle so any prefix is a uniform subset (strand LOD) and caps are fair.
    std::vector<uint32_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    Random rng(0xC0FFEEull ^ static_cast<uint64_t>(static_cast<uint32_t>(g.seed)));
    for (size_t i = total; i > 1; --i) std::swap(order[i - 1], order[rng.next() % i]);
    const size_t N = std::min<size_t>(total, static_cast<size_t>(std::clamp(g.strands, 1, 1000000)));
    order.resize(N);
    std::vector<size_t> starts(total, 0);
    for (size_t i = 1; i < total; ++i) starts[i] = starts[i - 1] + strands.counts[i - 1];

    std::vector<Vec3> shapes(N * static_cast<size_t>(P));
    for (size_t i = 0; i < N; ++i) {
        uint32_t s = order[i];
        resample(&strands.points[starts[s]], strands.counts[s], P, &shapes[i * static_cast<size_t>(P)]);
    }
    GroomData d;
    d.points = static_cast<uint32_t>(P);
    const int G = guideCountFor(g, static_cast<int>(N));
    d.guideRest.assign(shapes.begin(), shapes.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(G) * P));
    std::vector<Vec3> groots(static_cast<size_t>(G));
    Aabb rb{Vec3(1e30f), Vec3(-1e30f)};
    for (int i = 0; i < G; ++i) groots[static_cast<size_t>(i)] = d.guideRest[static_cast<size_t>(i) * P];
    for (size_t i = 0; i < N; ++i) {
        rb.min = vmin(rb.min, shapes[i * P]);
        rb.max = vmax(rb.max, shapes[i * P]);
    }
    float diag = length(rb.max - rb.min);
    float spacing = std::max(diag / std::sqrt(static_cast<float>(G)), 1e-4f);
    d.spacing = spacing;
    d.children.resize(N);
    for (size_t i = 0; i < N; ++i) {
        auto& c = d.children[i];
        c.root = shapes[i * P];
        c.lengthScale = 1.f;
        c.random = rng.nextFloat();
        c.width = 1.f;
        uint32_t s = order[i];
        if (!strands.widths.empty() && s < strands.widths.size() && g.widthRoot > 0.f) {
            c.width = std::clamp(strands.widths[s] / g.widthRoot, 0.05f, 20.f);
        }
    }
    RootGrid grid(groots, spacing * 2.f);
    assignGuides(d, groots, grid, spacing);
    // Offsets: the strand's own shape relative to its interpolated base curve.
    d.offsets.resize(N * static_cast<size_t>(P));
    parallelFor(N, [&](size_t i) {
        std::array<Vec3, 32> base, T, Nn;
        baseCurve(d, d.children[i], d.guideRest.data(), d.children[i].root, base.data());
        transportFrames(base.data(), P, Vec3{1, 0, 0}, T.data(), Nn.data());
        for (int k = 0; k < P; ++k) {
            Vec3 dv = shapes[i * P + static_cast<size_t>(k)] - base[k];
            Vec3 bn = cross(T[k], Nn[k]);
            d.offsets[i * P + static_cast<size_t>(k)] = {dot(dv, T[k]), dot(dv, Nn[k]), dot(dv, bn)};
        }
    });
    if (mesh && mesh->vertexCount() > 0) {
        d.proxy = proxyFromBounds(mesh->bounds);
        d.proxy.radius *= 0.97f;
        d.hasProxy = true;
    } else {
        // A sphere through the roots approximates the scalp.
        Vec3 c{0, 0, 0};
        for (const Vec3& p : groots) c += p;
        c = c / static_cast<float>(groots.size());
        float r = 0;
        for (const Vec3& p : groots) r += distance(p, c);
        r /= static_cast<float>(groots.size());
        // The roots cover a cap, so their centroid sits above the scalp center: push it down.
        d.proxy.kind = FxCollider::Kind::Sphere;
        d.proxy.a = d.proxy.b = c - Vec3{0, r * 0.35f, 0};
        d.proxy.radius = r * 0.9f;
        d.hasProxy = groots.size() >= 8;
    }
    computeBounds(d);
    return d;
}

void reconstructStrands(const GroomData& d, const std::vector<Vec3>& guides, const Mat4& model, std::vector<Vec3>& out) {
    const int P = static_cast<int>(d.points);
    out.resize(d.children.size() * static_cast<size_t>(P));
    if (P < 2) return;
    Vec3 ax = model.transformDir({1, 0, 0}), ay = model.transformDir({0, 1, 0}), az = model.transformDir({0, 0, 1});
    float scale = (length(ax) + length(ay) + length(az)) / 3.f;
    Vec3 ref = safeNormalize(ax, Vec3{1, 0, 0});
    Vec3 alt = safeNormalize(az, Vec3{0, 0, 1});
    parallelFor(d.children.size(), [&](size_t i) {
        std::array<Vec3, 32> base, T, Nn;
        const auto& c = d.children[i];
        baseCurve(d, c, guides.data(), model.transformPoint(c.root), base.data());
        transportFrames(base.data(), P, ref, T.data(), Nn.data(), alt);
        for (int k = 0; k < P; ++k) {
            Vec3 o = d.offsets[i * P + static_cast<size_t>(k)];
            Vec3 bn = cross(T[k], Nn[k]);
            out[i * P + static_cast<size_t>(k)] = base[k] + (T[k] * o.x + Nn[k] * o.y + bn * o.z) * scale;
        }
    });
}

StrandSet restStrands(const GroomData& d) {
    StrandSet s;
    reconstructStrands(d, d.guideRest, Mat4{}, s.points);
    s.counts.assign(d.children.size(), d.points);
    return s;
}

uint64_t groomHash(const Groom& g, const std::string& meshKey, const MeshData* mesh, int64_t sourceStamp) {
    uint64_t h = 1469598103934665603ull;
    h = fnvs(h, g.source);
    h = fnvv(h, g.importScale);
    h = fnvv(h, g.importZUp);
    for (int v : {g.strands, g.guides, g.segments, g.clumps, g.seed}) h = fnvv(h, v);
    for (float v : {g.length, g.lengthVariation, g.directionBlend, g.gravity, g.curlRadius, g.curlFrequency, g.wave,
                    g.waveFrequency, g.clumpStrength, g.clumpShape, g.frizz, g.frizzScale, g.maskAngle, g.maskSoftness,
                    g.direction.x, g.direction.y, g.direction.z, g.maskDirection.x, g.maskDirection.y, g.maskDirection.z,
                    g.widthRoot}) {
        h = fnvv(h, v);
    }
    h = fnvs(h, g.maskChannel);
    h = fnvs(h, meshKey);
    h = fnvv(h, sourceStamp);
    if (mesh) {
        h = fnvv(h, mesh->vertices.size());
        h = fnvv(h, mesh->indices.size());
        if (!mesh->vertices.empty()) {
            h = fnv(h, mesh->vertices.data(), std::min<size_t>(mesh->vertices.size(), 64) * sizeof(float));
            h = fnv(h, mesh->vertices.data() + mesh->vertices.size() - std::min<size_t>(mesh->vertices.size(), 64),
                    std::min<size_t>(mesh->vertices.size(), 64) * sizeof(float));
        }
    }
    return h;
}

// ---------------------------------------------------------------------------------------
// File formats
// ---------------------------------------------------------------------------------------

namespace {

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

template <typename T>
bool readPod(const std::vector<uint8_t>& b, size_t& off, T& v) {
    if (off + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}
template <typename T>
void writePod(std::vector<uint8_t>& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

}  // namespace

Result<StrandSet> parseHairFile(const std::vector<uint8_t>& b) {
    // Header (128 bytes): "HAIR", u32 hairs, u32 points, u32 flags, u32 default segments,
    // f32 default thickness, f32 default transparency, f32[3] default color, char[88] info.
    if (b.size() < 128 || std::memcmp(b.data(), "HAIR", 4) != 0) {
        return Error::make("invalid_hair", "not a .hair file (missing HAIR signature)");
    }
    size_t off = 4;
    uint32_t hairs = 0, points = 0, flags = 0, defSegments = 0;
    float defThickness = 0, defTransparency = 0, defColor[3];
    readPod(b, off, hairs);
    readPod(b, off, points);
    readPod(b, off, flags);
    readPod(b, off, defSegments);
    readPod(b, off, defThickness);
    readPod(b, off, defTransparency);
    for (float& c : defColor) readPod(b, off, c);
    off = 128;
    if (hairs == 0 || hairs > 10000000 || points > 200000000) return Error::make("invalid_hair", "implausible strand or point count");
    const bool hasSegments = flags & 1u, hasPoints = flags & 2u, hasThickness = flags & 4u, hasTransparency = flags & 8u,
               hasColor = flags & 16u;
    if (!hasPoints) return Error::make("invalid_hair", ".hair file has no point array");
    StrandSet s;
    s.counts.resize(hairs);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < hairs; ++i) {
        uint16_t seg = static_cast<uint16_t>(defSegments);
        if (hasSegments && !readPod(b, off, seg)) return Error::make("invalid_hair", "truncated segments array");
        s.counts[i] = static_cast<uint32_t>(seg) + 1;
        sum += s.counts[i];
    }
    if (sum != points) {
        return Error::make("invalid_hair", "segment counts describe " + std::to_string(sum) + " points but the header says " +
                                               std::to_string(points));
    }
    s.points.resize(points);
    for (uint32_t i = 0; i < points; ++i) {
        float xyz[3];
        for (float& v : xyz) {
            if (!readPod(b, off, v)) return Error::make("invalid_hair", "truncated point array");
        }
        s.points[i] = {xyz[0], xyz[1], xyz[2]};
    }
    if (hasThickness) {
        s.widths.resize(hairs);
        size_t p = 0;
        for (uint32_t i = 0; i < hairs; ++i) {
            float first = defThickness;
            for (uint32_t k = 0; k < s.counts[i]; ++k, ++p) {
                float t;
                if (!readPod(b, off, t)) return Error::make("invalid_hair", "truncated thickness array");
                if (k == 0) first = t;
            }
            s.widths[i] = first;
        }
    }
    (void)hasTransparency;
    (void)hasColor;  // transparency and color arrays are not used (shading is physically based)
    return s;
}

std::vector<uint8_t> writeHairFile(const StrandSet& s) {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'H', 'A', 'I', 'R'});
    writePod(b, static_cast<uint32_t>(s.counts.size()));
    writePod(b, static_cast<uint32_t>(s.points.size()));
    uint32_t flags = 1u | 2u | (s.widths.empty() ? 0u : 4u);
    writePod(b, flags);
    writePod(b, uint32_t{0});
    writePod(b, 0.1f);
    writePod(b, 0.f);
    for (int i = 0; i < 3; ++i) writePod(b, 1.f);
    std::string info = "Skywalker groom export";
    info.resize(88, '\0');
    b.insert(b.end(), info.begin(), info.end());
    for (uint32_t c : s.counts) writePod(b, static_cast<uint16_t>(std::max<uint32_t>(c, 1) - 1));
    for (const Vec3& p : s.points) {
        writePod(b, p.x);
        writePod(b, p.y);
        writePod(b, p.z);
    }
    if (!s.widths.empty()) {
        for (size_t i = 0; i < s.counts.size(); ++i) {
            for (uint32_t k = 0; k < s.counts[i]; ++k) writePod(b, s.widths[i]);
        }
    }
    return b;
}

Result<StrandSet> parseGroomJson(const Json& doc) {
    if (doc.get("format").asString() != "skywalker-groom") {
        return Error::make("invalid_groom", "not a groom document (\"format\" must be \"skywalker-groom\")");
    }
    StrandSet s;
    for (const auto& strand : doc.get("strands").elements()) {
        const auto& v = strand.elements();
        if (v.size() < 6 || v.size() % 3 != 0) return Error::make("invalid_groom", "each strand needs >= 2 points as [x,y,z,...]");
        s.counts.push_back(static_cast<uint32_t>(v.size() / 3));
        for (size_t i = 0; i < v.size(); i += 3) s.points.push_back({v[i].asFloat(), v[i + 1].asFloat(), v[i + 2].asFloat()});
    }
    for (const auto& w : doc.get("widths").elements()) s.widths.push_back(w.asFloat());
    if (!s.widths.empty() && s.widths.size() != s.counts.size()) {
        return Error::make("invalid_groom", "\"widths\" must have one entry per strand");
    }
    if (s.counts.empty()) return Error::make("empty_groom", "the groom has no strands");
    return s;
}

Json writeGroomJson(const StrandSet& s) {
    Json strands = Json::array();
    size_t p = 0;
    for (uint32_t c : s.counts) {
        Json arr = Json::array();
        for (uint32_t k = 0; k < c; ++k, ++p) {
            arr.push(s.points[p].x);
            arr.push(s.points[p].y);
            arr.push(s.points[p].z);
        }
        strands.push(std::move(arr));
    }
    Json doc = Json::object({{"format", "skywalker-groom"}, {"version", 1}, {"strands", strands}});
    if (!s.widths.empty()) {
        Json w = Json::array();
        for (float v : s.widths) w.push(v);
        doc["widths"] = w;
    }
    return doc;
}

Result<StrandSet> parseSkyGroom(const std::vector<uint8_t>& b) {
    if (b.size() < 24 || std::memcmp(b.data(), "SKYGROOM", 8) != 0) return Error::make("invalid_groom", "not a .skygroom file");
    size_t off = 8;
    uint32_t version = 0, strands = 0, points = 0, flags = 0;
    readPod(b, off, version);
    readPod(b, off, strands);
    readPod(b, off, points);
    readPod(b, off, flags);
    if (version != 1) return Error::make("invalid_groom", "unsupported .skygroom version " + std::to_string(version));
    if (points < 2 || strands == 0 || static_cast<uint64_t>(strands) * points > 100000000ull) {
        return Error::make("invalid_groom", "implausible .skygroom dimensions");
    }
    StrandSet s;
    s.counts.assign(strands, points);
    s.points.resize(static_cast<size_t>(strands) * points);
    for (Vec3& p : s.points) {
        if (!readPod(b, off, p.x) || !readPod(b, off, p.y) || !readPod(b, off, p.z)) {
            return Error::make("invalid_groom", "truncated .skygroom point data");
        }
    }
    if (flags & 1u) {
        s.widths.resize(strands);
        for (float& w : s.widths) {
            if (!readPod(b, off, w)) return Error::make("invalid_groom", "truncated .skygroom widths");
        }
    }
    return s;
}

std::vector<uint8_t> writeSkyGroom(const StrandSet& s) {
    // Uniform point counts only: resample to the longest strand's count.
    uint32_t P = 2;
    for (uint32_t c : s.counts) P = std::max(P, c);
    std::vector<uint8_t> b;
    b.insert(b.end(), {'S', 'K', 'Y', 'G', 'R', 'O', 'O', 'M'});
    writePod(b, uint32_t{1});
    writePod(b, static_cast<uint32_t>(s.counts.size()));
    writePod(b, P);
    writePod(b, s.widths.empty() ? 0u : 1u);
    std::vector<Vec3> tmp(P);
    size_t start = 0;
    for (uint32_t c : s.counts) {
        resample(&s.points[start], c, static_cast<int>(P), tmp.data());
        start += c;
        for (const Vec3& p : tmp) {
            writePod(b, p.x);
            writePod(b, p.y);
            writePod(b, p.z);
        }
    }
    for (float w : s.widths) writePod(b, w);
    return b;
}

Result<StrandSet> loadStrands(const std::string& path, float scale, bool zUp) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) return Error::make("io_error", "cannot read " + path);
    std::string lowerPath = str::lower(path);
    Result<StrandSet> r = Error::make("unknown_format", "unknown groom format: " + path, "use .hair, .groom.json or .skygroom");
    auto ends = [&](const char* ext) {
        std::string e(ext);
        return lowerPath.size() >= e.size() && lowerPath.compare(lowerPath.size() - e.size(), e.size(), e) == 0;
    };
    if (ends(".hair")) {
        r = parseHairFile(bytes);
    } else if (ends(".skygroom")) {
        r = parseSkyGroom(bytes);
    } else if (ends(".json")) {
        auto j = Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!j) return j.error();
        r = parseGroomJson(j.value());
    }
    if (!r) return r;
    for (Vec3& p : r->points) {
        if (zUp) p = Vec3{p.x, p.z, -p.y};
        p = p * scale;
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------------------

Vec3 hairAbsorption(float melanin, float redness) {
    // Concentration from a perceptual 0..1 control, then the eumelanin / pheomelanin absorption
    // spectra (as in Blender's Principled Hair and Unreal's hair shading).
    float m = std::clamp(melanin, 0.f, 0.9995f);
    float qty = -std::log(std::max(1.f - m, 1e-4f));
    float eu = qty * (1.f - std::clamp(redness, 0.f, 1.f)), pheo = qty * std::clamp(redness, 0.f, 1.f);
    return Vec3{0.506f, 0.841f, 1.653f} * eu + Vec3{0.343f, 0.733f, 1.924f} * pheo;
}

Vec3 hairColorFromAbsorption(Vec3 s) {
    // Chiang et al. 2016 inverse mapping (azimuthal roughness 0.3).
    const float D = 5.889f;
    return {std::exp(-std::sqrt(std::max(s.x, 0.f)) * D), std::exp(-std::sqrt(std::max(s.y, 0.f)) * D),
            std::exp(-std::sqrt(std::max(s.z, 0.f)) * D)};
}

// ---------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------

Json groomPreset(const std::string& name) {
    static const std::pair<const char*, const char*> kPresets[] = {
        {"hair_straight", R"({"strands":110000,"segments":20,"length":0.34,"lengthVariation":0.12,"widthRoot":0.075,
            "widthTip":0.04,"direction":[0,-0.35,-1],"directionBlend":0.9,"gravity":0.95,"clumps":500,"clumpStrength":0.3,
            "clumpShape":2.2,"frizz":0.0015,"frizzScale":25,"maskDirection":[0,0.8,-0.6],"maskAngle":95,"maskSoftness":10,
            "melanin":0.82,"redness":0.25,"roughness":0.28,"radialRoughness":0.65,"stiffness":0.35})"},
        {"hair_wavy", R"({"strands":120000,"segments":24,"length":0.38,"lengthVariation":0.18,"widthRoot":0.08,
            "widthTip":0.04,"direction":[0,-0.35,-1],"directionBlend":0.9,"gravity":0.85,"wave":0.014,"waveFrequency":6.5,
            "clumps":700,"clumpStrength":0.55,"clumpShape":1.4,"frizz":0.0025,"frizzScale":30,"maskDirection":[0,0.8,-0.6],
            "maskAngle":95,"maskSoftness":10,"melanin":0.55,"redness":0.35,"roughness":0.33,"radialRoughness":0.7,
            "stiffness":0.4})"},
        {"hair_curly", R"({"strands":90000,"segments":31,"length":0.24,"lengthVariation":0.25,"widthRoot":0.09,
            "widthTip":0.05,"direction":[0,0.2,-1],"directionBlend":0.6,"gravity":0.35,"curlRadius":0.011,"curlFrequency":16,
            "clumps":1100,"clumpStrength":0.8,"clumpShape":0.7,"frizz":0.0035,"frizzScale":40,"maskDirection":[0,0.8,-0.6],
            "maskAngle":95,"maskSoftness":10,"melanin":0.93,"redness":0.15,"roughness":0.42,"radialRoughness":0.8,
            "stiffness":0.6})"},
        {"hair_ponytail", R"({"strands":90000,"segments":24,"length":0.42,"lengthVariation":0.1,"widthRoot":0.075,
            "widthTip":0.04,"direction":[0,0.15,-1],"directionBlend":0.95,"gravity":1.1,"clumps":1,"clumpStrength":0.92,
            "clumpShape":0.35,"frizz":0.001,"maskDirection":[0,0.8,-0.6],"maskAngle":95,"maskSoftness":10,"melanin":0.35,
            "redness":0.4,"roughness":0.3,"stiffness":0.5})"},
        {"hair_short", R"({"strands":90000,"segments":5,"length":0.035,"lengthVariation":0.3,"widthRoot":0.07,
            "widthTip":0.03,"direction":[0,0.1,-1],"directionBlend":0.85,"gravity":0.2,"clumps":2500,"clumpStrength":0.25,
            "frizz":0.0012,"maskDirection":[0,0.8,-0.6],"maskAngle":95,"maskSoftness":10,"melanin":0.88,"redness":0.1,
            "roughness":0.4,"stiffness":0.9,"simulate":false})"},
        {"fur_short", R"({"strands":160000,"segments":4,"length":0.014,"lengthVariation":0.4,"widthRoot":0.035,
            "widthTip":0.006,"direction":[0,-0.3,-1],"directionBlend":0.8,"gravity":0.15,"clumps":0,"frizz":0.0006,
            "frizzScale":120,"maskAngle":180,"melanin":0.5,"redness":0.65,"colorVariation":0.25,"roughness":0.5,
            "radialRoughness":0.85,"scatter":1.2,"stiffness":0.9,"rootStiffness":1,"cardsBelow":60,"simulate":false})"},
        {"fur_long", R"({"strands":90000,"segments":8,"length":0.07,"lengthVariation":0.35,"widthRoot":0.05,
            "widthTip":0.01,"direction":[0,-0.6,-1],"directionBlend":0.75,"gravity":0.45,"clumps":3000,"clumpStrength":0.45,
            "clumpShape":1.6,"frizz":0.0025,"frizzScale":60,"maskAngle":180,"melanin":0.25,"redness":0.3,
            "colorVariation":0.3,"roughness":0.45,"radialRoughness":0.85,"scatter":1.3,"stiffness":0.6})"},
    };
    for (const auto& [n, json] : kPresets) {
        if (name == n) {
            auto j = Json::parse(json);
            if (j) {
                Json out = j.value();
                out["preset"] = name;
                return out;
            }
        }
    }
    return Json::object();
}

// ---------------------------------------------------------------------------------------
// Scene helpers and the groom cache
// ---------------------------------------------------------------------------------------

EntityId findNear(const Scene& scene, EntityId self, const std::string& name) {
    if (name.empty()) return kNoEntity;
    auto search = [&](EntityId root) -> EntityId {
        std::vector<EntityId> stack{root};
        while (!stack.empty()) {
            EntityId e = stack.back();
            stack.pop_back();
            const EntityRecord* r = scene.record(e);
            if (r && e != self && str::lower(r->name) == str::lower(name)) return e;
            for (EntityId c : scene.children(e)) stack.push_back(c);
        }
        return kNoEntity;
    };
    if (scene.exists(self)) {
        if (EntityId e = search(self)) return e;
        const EntityRecord* r = scene.record(self);
        if (r && r->parent) {
            if (EntityId e = search(r->parent)) return e;
        }
    }
    return scene.find(name);
}

EntityId groomMeshEntity(const Scene& scene, EntityId e) {
    const Groom* g = scene.get<Groom>(e);
    if (g && !g->target.empty()) {
        if (EntityId t = findNear(scene, e, g->target)) return t;
    }
    return e;
}

std::vector<FxCollider> collidersFromNames(const Scene& scene, EntityId self, const std::string& names) {
    std::vector<FxCollider> out;
    for (const std::string& raw : str::split(names, ',')) {
        std::string name = str::trim(raw);
        if (name.empty()) continue;
        EntityId e = findNear(scene, self, name);
        if (!e || !scene.isActive(e)) continue;
        Mat4 w = scene.worldMatrix(e);
        const MeshRenderer* m = scene.get<MeshRenderer>(e);
        std::string mesh = m ? m->mesh : "";
        FxCollider c;
        if (mesh == "plane" || mesh == "quad") {
            c.kind = FxCollider::Kind::Plane;
            c.a = w.translation();
            c.b = safeNormalize(w.transformDir(mesh == "plane" ? Vec3{0, 1, 0} : Vec3{0, 0, 1}), Vec3{0, 1, 0});
        } else if (mesh == "sphere") {
            c.kind = FxCollider::Kind::Sphere;
            c.a = c.b = w.translation();
            c.radius = 0.5f * std::max({length(w.transformDir({1, 0, 0})), length(w.transformDir({0, 1, 0})),
                                        length(w.transformDir({0, 0, 1}))});
        } else {
            FxCollider local = proxyFromBounds(scene.localBounds(e));
            float s = (length(w.transformDir({1, 0, 0})) + length(w.transformDir({0, 1, 0})) + length(w.transformDir({0, 0, 1}))) / 3.f;
            c = local;
            c.a = w.transformPoint(local.a);
            c.b = w.transformPoint(local.b);
            c.radius = local.radius * s;
        }
        out.push_back(c);
    }
    return out;
}

namespace {

Vec3 axisScale(const Mat4& m) {
    return {length(m.transformDir({1, 0, 0})), length(m.transformDir({0, 1, 0})), length(m.transformDir({0, 0, 1}))};
}

/// The entity's world transform without scale: grooms are generated in meters on the scaled
/// mesh, so lengths and widths are physical whatever the entity's scale.
Mat4 rigidPart(const Mat4& m) {
    Vec3 s = axisScale(m);
    return m * Mat4::scale({1.f / std::max(s.x, 1e-8f), 1.f / std::max(s.y, 1e-8f), 1.f / std::max(s.z, 1e-8f)});
}

MeshData scaledMesh(const MeshData& m, Vec3 s) {
    MeshData out = m;
    const size_t n = out.vertexCount();
    for (size_t i = 0; i < n; ++i) {
        float* v = &out.vertices[i * MeshData::kFloatsPerVertex];
        v[0] *= s.x, v[1] *= s.y, v[2] *= s.z;
        Vec3 nn = safeNormalize(Vec3{v[3] / std::max(s.x, 1e-8f), v[4] / std::max(s.y, 1e-8f), v[5] / std::max(s.z, 1e-8f)},
                                Vec3{0, 1, 0});
        v[3] = nn.x, v[4] = nn.y, v[5] = nn.z;
    }
    out.bounds = {m.bounds.min * s, m.bounds.max * s};
    return out;
}

}  // namespace

std::shared_ptr<const GroomData> GroomSystem::groomFor(const Scene& scene, EntityId e, const MeshProvider& meshes,
                                                       const PathResolver& resolve, std::string* error) {
    const Groom* g = scene.get<Groom>(e);
    if (!g) {
        if (error) *error = "no groom component";
        return nullptr;
    }
    EntityId meshEnt = groomMeshEntity(scene, e);
    const MeshRenderer* mr = scene.get<MeshRenderer>(meshEnt);
    std::string meshKey = mr ? mr->mesh : "";
    const MeshData* mesh = meshKey.empty() || !meshes ? nullptr : meshes(meshKey);
    const Vec3 scale = axisScale(scene.worldMatrix(meshEnt));
    std::string sourcePath;
    int64_t stamp = 0;
    if (!g->source.empty()) {
        sourcePath = resolve ? resolve(g->source) : g->source;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(sourcePath, ec);
        if (!ec) stamp = static_cast<int64_t>(t.time_since_epoch().count());
    }
    uint64_t h = groomHash(*g, meshKey, mesh, stamp);
    for (float v : {scale.x, scale.y, scale.z}) h = fnvv(h, std::round(v * 1e4f));
    Entry& entry = cache_[e];
    if (entry.hash == h && (entry.data || !entry.error.empty())) {
        if (error) *error = entry.error;
        return entry.data;
    }
    auto start = std::chrono::steady_clock::now();
    MeshData scaled;
    if (mesh) {
        scaled = scaledMesh(*mesh, scale);
        mesh = &scaled;
    }
    Result<GroomData> r = Error::make("no_mesh", "no mesh");
    if (!g->source.empty()) {
        auto strands = loadStrands(sourcePath, g->importScale, g->importZUp);
        if (strands) {
            r = groomFromStrands(*g, strands.value(), mesh);
        } else {
            r = strands.error();
        }
    } else {
        r = generateGroom(*g, mesh);
    }
    lastGenerateMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    entry.hash = h;
    if (r) {
        r->hash = h;
        entry.data = std::make_shared<const GroomData>(std::move(r.value()));
        entry.error.clear();
    } else {
        entry.data = nullptr;
        entry.error = r.error().message;
    }
    if (error) *error = entry.error;
    return entry.data;
}

void GroomSystem::gather(const Scene& scene, const MeshProvider& meshes, const PathResolver& resolve,
                         std::vector<GroomItem>& out) {
    std::vector<EntityId> live;
    const Environment& env = scene.environment();
    float wa = radians(env.windDirection);
    Vec3 wind = Vec3{std::sin(wa), 0.f, std::cos(wa)} * env.windSpeed;
    for (EntityId e : scene.entities()) {
        const Groom* g = scene.get<Groom>(e);
        if (!g) continue;
        live.push_back(e);
        if (!g->visible || !scene.isActive(e)) continue;
        auto data = groomFor(scene, e, meshes, resolve);
        if (!data || data->children.empty()) continue;
        GroomItem item;
        item.entity = e;
        item.model = rigidPart(scene.worldMatrix(groomMeshEntity(scene, e)));
        item.data = data;
        item.params = *g;
        item.wind = wind * g->wind;
        if (g->collide) {
            if (data->hasProxy) {
                const Mat4& m = item.model;
                float s = (length(m.transformDir({1, 0, 0})) + length(m.transformDir({0, 1, 0})) + length(m.transformDir({0, 0, 1}))) / 3.f;
                FxCollider c = data->proxy;
                c.a = m.transformPoint(c.a);
                c.b = m.transformPoint(c.b);
                c.radius *= s;
                item.colliders.push_back(c);
            }
            for (const auto& c : collidersFromNames(scene, e, g->colliders)) item.colliders.push_back(c);
        }
        out.push_back(std::move(item));
    }
    std::erase_if(cache_, [&](const auto& kv) { return std::find(live.begin(), live.end(), kv.first) == live.end(); });
}

}  // namespace fx
}  // namespace sky
