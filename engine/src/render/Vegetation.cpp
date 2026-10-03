// Procedural vegetation and ground-detail meshes for foliage scattering: grass clumps,
// tall grass, ferns, flowers, pebbles, shells and rocks. Vertex colors carry ambient
// occlusion (dark roots) and color variation; the foliage shader bends vertices by their
// height above the mesh origin, so every mesh is built standing on y = 0.

#include <algorithm>
#include <cmath>

#include "skywalker/render/MeshData.h"

namespace sky::mesh {

namespace {

uint32_t hashU(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
float rnd(uint32_t& s) {
    s = hashU(s + 0x9E3779B9u);
    return static_cast<float>(s >> 8) * (1.f / 16777216.f);
}

void addTri(MeshData& m, uint32_t a, uint32_t b, uint32_t c) {
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
}

uint32_t vcount(const MeshData& m) { return static_cast<uint32_t>(m.vertexCount()); }

/// A tapered, curved blade (two-sided via the renderer's double-sided flag).
void blade(MeshData& m, Vec3 base, float yaw, float height, float width, float bend, Vec4 rootC, Vec4 tipC, int segs) {
    Vec3 dir{std::cos(yaw), 0, std::sin(yaw)};      // bend direction
    Vec3 side{-dir.z, 0, dir.x};                     // blade width axis
    uint32_t first = vcount(m);
    for (int i = 0; i <= segs; ++i) {
        float t = static_cast<float>(i) / segs;
        float y = height * t;
        Vec3 c = base + dir * (bend * t * t * height) + Vec3{0, y * (1.f - 0.25f * bend * t), 0};
        float w = width * (1.f - t) * (1.f - t * 0.15f) + 0.0015f;
        // Normal: the blade face, tilted toward the sky for soft grass shading.
        Vec3 tangent = normalize(dir * (2.f * bend * t * height) + Vec3{0, height, 0});
        Vec3 n = normalize(cross(side, tangent) * 0.55f + Vec3{0, 1, 0} * 0.45f);
        Vec4 col{rootC.x + (tipC.x - rootC.x) * t, rootC.y + (tipC.y - rootC.y) * t, rootC.z + (tipC.z - rootC.z) * t, 1.f};
        if (i == segs) {
            m.addVertex(c, n, {0.5f, t}, col);
        } else {
            m.addVertex(c - side * w, n, {0.f, t}, col);
            m.addVertex(c + side * w, n, {1.f, t}, col);
        }
    }
    for (int i = 0; i < segs; ++i) {
        uint32_t a = first + static_cast<uint32_t>(i) * 2, b = a + 1;
        if (i == segs - 1) {
            uint32_t tip = a + 2;
            addTri(m, a, b, tip);
        } else {
            addTri(m, a, b, a + 3);
            addTri(m, a, a + 3, a + 2);
        }
    }
}

MeshData grassClump(uint32_t seed, int blades, float hMin, float hMax, float spread, float width) {
    MeshData m;
    uint32_t s = seed;
    for (int i = 0; i < blades; ++i) {
        float a = rnd(s) * 6.2831853f, r = std::sqrt(rnd(s)) * spread;
        float h = hMin + (hMax - hMin) * rnd(s);
        float shade = 0.85f + 0.3f * rnd(s);
        Vec4 root{0.35f * shade, 0.38f * shade, 0.30f * shade, 1};
        Vec4 tip{1.05f * shade, 1.0f * shade, 0.82f * shade, 1};
        blade(m, {std::cos(a) * r, 0, std::sin(a) * r}, rnd(s) * 6.2831853f, h, width * (0.8f + 0.4f * rnd(s)),
              0.15f + 0.35f * rnd(s), root, tip, 4);
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

/// Low-poly ellipsoid (pebbles, shell bodies).
void ellipsoid(MeshData& m, Vec3 c, Vec3 r, Vec4 col, int seg, int rings, uint32_t seed, float bumpy) {
    uint32_t first = vcount(m);
    uint32_t s = seed;
    std::vector<float> bump(static_cast<size_t>((rings + 1) * (seg + 1)));
    for (auto& b : bump) b = 1.f + (rnd(s) - 0.5f) * bumpy;
    for (int j = 0; j <= rings; ++j) {
        float v = static_cast<float>(j) / rings, th = v * 3.14159265f;
        for (int i = 0; i <= seg; ++i) {
            float u = static_cast<float>(i) / seg, ph = u * 6.2831853f;
            float k = bump[static_cast<size_t>(j * (seg + 1) + (i % seg))];
            if (j == 0 || j == rings) k = 1.f;
            Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            Vec3 p = c + Vec3{n.x * r.x * k, n.y * r.y * k, n.z * r.z * k};
            float ao = 0.55f + 0.45f * std::clamp(n.y * 0.5f + 0.6f, 0.f, 1.f);
            m.addVertex(p, normalize(Vec3{n.x / r.x, n.y / r.y, n.z / r.z}), {u, v}, {col.x * ao, col.y * ao, col.z * ao, 1});
        }
    }
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < seg; ++i) {
            uint32_t a = first + static_cast<uint32_t>(j * (seg + 1) + i), b = a + static_cast<uint32_t>(seg + 1);
            addTri(m, a, a + 1, b);
            addTri(m, a + 1, b + 1, b);
        }
    }
}

}  // namespace

MeshData grass() { return grassClump(11, 9, 0.18f, 0.42f, 0.09f, 0.012f); }
MeshData grassTall() { return grassClump(23, 11, 0.55f, 1.1f, 0.14f, 0.009f); }

MeshData pebbles() {
    MeshData m;
    uint32_t s = 77;
    int n = 5;
    for (int i = 0; i < n; ++i) {
        float a = rnd(s) * 6.2831853f, r = rnd(s) * 0.09f;
        float size = 0.018f + rnd(s) * 0.035f;
        float tone = 0.55f + rnd(s) * 0.6f;
        Vec4 col{tone * (0.95f + rnd(s) * 0.1f), tone * (0.93f + rnd(s) * 0.06f), tone * (0.88f + rnd(s) * 0.1f), 1};
        ellipsoid(m, {std::cos(a) * r, size * 0.35f, std::sin(a) * r}, {size * (1.f + rnd(s) * 0.6f), size * 0.55f, size},
                  col, 10, 6, s, 0.18f);
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

MeshData shell() {
    // A ribbed scallop: a cupped fan with radial ridges and pale banding.
    MeshData m;
    const int ribs = 18, rings = 8;
    const float radius = 0.035f;
    uint32_t first = vcount(m);
    for (int j = 0; j <= rings; ++j) {
        float t = static_cast<float>(j) / rings;
        for (int i = 0; i <= ribs; ++i) {
            float u = static_cast<float>(i) / ribs;
            float ang = (u - 0.5f) * 2.3f;
            float ridge = std::fabs(std::sin(u * ribs * 3.14159265f));
            float r = radius * t;
            float y = (std::sqrt(std::max(0.f, 1.f - t * t)) * 0.35f + ridge * 0.06f * t) * radius;
            Vec3 p{std::sin(ang) * r, y + 0.002f, std::cos(ang) * r - radius * 0.4f};
            Vec3 n = normalize(Vec3{std::sin(ang) * t * 0.6f, 1.f, std::cos(ang) * t * 0.6f});
            float band = 0.82f + 0.18f * std::sin(t * 22.f);
            float dark = 0.75f + 0.25f * ridge;
            m.addVertex(p, n, {u, t}, {band * dark, band * 0.94f * dark, band * 0.86f * dark, 1});
        }
    }
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < ribs; ++i) {
            uint32_t a = first + static_cast<uint32_t>(j * (ribs + 1) + i), b = a + static_cast<uint32_t>(ribs + 1);
            addTri(m, a, a + 1, b);
            addTri(m, a + 1, b + 1, b);
        }
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

MeshData rock() {
    // A weathered boulder: a sphere displaced by smooth 3D fractal noise (no faceting),
    // flattened underneath and slightly squashed; sits on y = 0 with its base buried.
    MeshData m;
    const int seg = 40, rings = 24;
    auto hash = [](int x, int y, int z) {
        uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u;
        return static_cast<float>(hashU(h) >> 8) * (1.f / 16777216.f);
    };
    auto noise3 = [&](Vec3 p) {
        int xi = static_cast<int>(std::floor(p.x)), yi = static_cast<int>(std::floor(p.y)), zi = static_cast<int>(std::floor(p.z));
        Vec3 f{p.x - xi, p.y - yi, p.z - zi};
        Vec3 u{f.x * f.x * (3 - 2 * f.x), f.y * f.y * (3 - 2 * f.y), f.z * f.z * (3 - 2 * f.z)};
        auto l = [](float a, float b, float t) { return a + (b - a) * t; };
        return l(l(l(hash(xi, yi, zi), hash(xi + 1, yi, zi), u.x), l(hash(xi, yi + 1, zi), hash(xi + 1, yi + 1, zi), u.x), u.y),
                 l(l(hash(xi, yi, zi + 1), hash(xi + 1, yi, zi + 1), u.x), l(hash(xi, yi + 1, zi + 1), hash(xi + 1, yi + 1, zi + 1), u.x), u.y),
                 u.z);
    };
    auto shape = [&](Vec3 n) {
        float r = 1.f, amp = 0.32f, fr = 1.6f;
        for (int o = 0; o < 5; ++o) {
            r += (noise3(n * fr + Vec3{3.1f * o, 1.7f, 5.3f}) - 0.5f) * amp;
            amp *= 0.5f;
            fr *= 2.1f;
        }
        // Strata: flat-ish facets like weathered stone.
        r = r * 0.85f + std::round(r * 6.f) / 6.f * 0.15f;
        return r;
    };
    auto pos = [&](float th, float ph) {
        Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
        float r = shape(n);
        Vec3 p{n.x * r * 0.55f, n.y * r * 0.42f + 0.28f, n.z * r * 0.5f};
        p.y = std::max(p.y, 0.02f + p.y * 0.1f);  // flat base
        return p;
    };
    for (int j = 0; j <= rings; ++j) {
        float th = static_cast<float>(j) / rings * 3.14159265f;
        for (int i = 0; i <= seg; ++i) {
            float ph = static_cast<float>(i % seg) / seg * 6.2831853f;
            Vec3 p = pos(th, ph);
            const float e = 0.01f;
            Vec3 a = pos(th + e, ph) - pos(th - e, ph);
            Vec3 b = pos(th, ph + e) - pos(th, ph - e);
            Vec3 n = cross(b, a);
            if (j == 0 || j == rings || length(n) < 1e-6f) n = Vec3{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            n = normalize(n);
            if (dot(n, p - Vec3{0, 0.28f, 0}) < 0) n = -n;
            float ao = 0.6f + 0.4f * std::clamp(p.y / 0.5f, 0.f, 1.f);
            m.addVertex(p, n, {static_cast<float>(i) / seg, static_cast<float>(j) / rings}, {ao, ao, ao, 1});
        }
    }
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < seg; ++i) {
            uint32_t a = static_cast<uint32_t>(j * (seg + 1) + i), b = a + static_cast<uint32_t>(seg + 1);
            addTri(m, a, a + 1, b);
            addTri(m, a + 1, b + 1, b);
        }
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

MeshData fern() {
    MeshData m;
    uint32_t s = 91;
    const int fronds = 7;
    for (int f = 0; f < fronds; ++f) {
        float yaw = static_cast<float>(f) / fronds * 6.2831853f + rnd(s) * 0.5f;
        float len = 0.55f + rnd(s) * 0.35f;
        Vec3 dir{std::cos(yaw), 0, std::sin(yaw)}, side{-dir.z, 0, dir.x};
        const int segs = 10;
        for (int i = 0; i < segs; ++i) {
            float t0 = static_cast<float>(i) / segs, t1 = static_cast<float>(i + 1) / segs;
            auto spine = [&](float t) { return dir * (len * t) + Vec3{0, len * (0.9f * t - 0.75f * t * t), 0}; };
            Vec3 p0 = spine(t0), p1 = spine(t1);
            float lw = 0.11f * std::sin(3.14159f * std::min(1.f, t0 * 1.15f + 0.05f)) * (1.f - t0 * 0.3f);
            Vec3 n = normalize(Vec3{0, 1, 0} + dir * 0.2f);
            Vec4 c{0.55f + 0.45f * t0, 0.6f + 0.4f * t0, 0.45f + 0.35f * t0, 1};
            for (float sgn : {-1.f, 1.f}) {
                uint32_t a = vcount(m);
                Vec3 leafTip = (p0 + p1) * 0.5f + side * (lw * sgn) + Vec3{0, -0.02f, 0};
                m.addVertex(p0, n, {0, t0}, c);
                m.addVertex(p1, n, {0, t1}, c);
                m.addVertex(leafTip, n, {1, t0}, {c.x * 1.05f, c.y * 1.05f, c.z, 1});
                addTri(m, a, a + 1, a + 2);
            }
        }
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

MeshData flowers() {
    MeshData m;
    uint32_t s = 5;
    const Vec4 palette[4] = {{1.0f, 0.97f, 0.92f, 1}, {1.0f, 0.86f, 0.25f, 1}, {0.72f, 0.5f, 0.95f, 1}, {0.98f, 0.45f, 0.4f, 1}};
    for (int k = 0; k < 4; ++k) {
        float a = rnd(s) * 6.2831853f, r = rnd(s) * 0.08f;
        Vec3 base{std::cos(a) * r, 0, std::sin(a) * r};
        float h = 0.22f + rnd(s) * 0.2f;
        blade(m, base, rnd(s) * 6.28f, h, 0.004f, 0.15f, {0.3f, 0.45f, 0.2f, 1}, {0.45f, 0.65f, 0.3f, 1}, 3);
        Vec3 top = base + Vec3{0, h * 0.97f, 0};
        Vec4 pc = palette[k % 4];
        const int petals = 6;
        uint32_t center = vcount(m);
        m.addVertex(top + Vec3{0, 0.004f, 0}, {0, 1, 0}, {0.5f, 0.5f}, {0.95f, 0.8f, 0.25f, 1});
        for (int p = 0; p <= petals * 2; ++p) {
            float pa = static_cast<float>(p) / (petals * 2) * 6.2831853f;
            float pr = (p % 2 == 0 ? 0.028f : 0.012f) * (0.8f + 0.4f * rnd(s));
            m.addVertex(top + Vec3{std::cos(pa) * pr, -0.004f * (p % 2 == 0), std::sin(pa) * pr}, normalize(Vec3{0, 1, 0}),
                        {0.5f, 0.5f}, pc);
        }
        for (int p = 0; p < petals * 2; ++p) addTri(m, center, center + 1 + static_cast<uint32_t>(p) + 1, center + 1 + static_cast<uint32_t>(p));
    }
    m.hasVertexColors = true;
    m.computeBounds();
    return m;
}

}  // namespace sky::mesh
