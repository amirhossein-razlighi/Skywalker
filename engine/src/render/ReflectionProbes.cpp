// Reflection probes: CPU math, the per-frame plan and the GPU contract. See render/ReflectionProbes.h.

#include "skywalker/render/ReflectionProbes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

#include "skywalker/ecs/Components.h"
#include "skywalker/render/LightClusters.h"
#include "skywalker/render/RenderLayers.h"
#include "skywalker/render/Renderer.h"

namespace sky::probes {

namespace {

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void pod(const T& v) {
        bytes(&v, sizeof(T));
    }
    void str(const std::string& s) { bytes(s.data(), s.size()), pod(s.size()); }
};

/// Normalized frustum planes (Gribb/Hartmann, depth 0..1) for sphere tests.
struct Planes {
    Vec4 p[6];
    explicit Planes(const Mat4& m) {
        auto row = [&](int r) { return Vec4{m.at(0, r), m.at(1, r), m.at(2, r), m.at(3, r)}; };
        Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        p[0] = r3 + r0;
        p[1] = r3 + r0 * -1.f;
        p[2] = r3 + r1;
        p[3] = r3 + r1 * -1.f;
        p[4] = r2;
        p[5] = r3 + r2 * -1.f;
        for (Vec4& v : p) {
            float l = length(v.xyz());
            if (l > 1e-8f) v = v * (1.f / l);
        }
    }
    bool sphere(Vec3 c, float r) const {
        for (const Vec4& v : p) {
            if (v.x * c.x + v.y * c.y + v.z * c.z + v.w < -r) return false;
        }
        return true;
    }
};

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

bool sphereTouchesBox(Vec3 c, float r, const Aabb& b) {
    Vec3 q{std::clamp(c.x, b.min.x, b.max.x), std::clamp(c.y, b.min.y, b.max.y), std::clamp(c.z, b.min.z, b.max.z)};
    return distance(q, c) <= r;
}

bool boxesOverlap(const Aabb& a, const Aabb& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z &&
           a.max.z >= b.min.z;
}

std::string probeRef(EntityId e) { return "probe #" + std::to_string(e); }

int leadingDirectional(const FrameData& f) {
    int n = 0;
    while (static_cast<size_t>(n) < f.lights.size() && f.lights[static_cast<size_t>(n)].kind == LightItem::Kind::Directional) ++n;
    return n;
}

}  // namespace

const char* updateName(int update) {
    switch (update) {
        case 1: return "on_change";
        case 2: return "realtime";
        default: return "once";
    }
}

const char* ambientName(int ambient) {
    switch (ambient) {
        case 1: return "sky";
        case 2: return "color";
        default: return "probe";
    }
}

double slotBytes(int resolution) {
    return static_cast<double>(resolution) * resolution * 8.0 /* RGBA16F */ * kFaces * (4.0 / 3.0) /* mips */;
}

int sanitizeResolution(int px) {
    int r = kMinResolution;
    while (r < kMaxResolution && r * 3 / 2 < px) r *= 2;  // nearest power of two (rounds 192 -> 256)
    return r;
}

ProbeItem makeItem(EntityId entity, const Mat4& world, const ReflectionProbe& p) {
    ProbeItem it;
    it.entity = entity;
    const Vec3 ax{world.at(0, 0), world.at(0, 1), world.at(0, 2)};
    const Vec3 ay{world.at(1, 0), world.at(1, 1), world.at(1, 2)};
    const Vec3 az{world.at(2, 0), world.at(2, 1), world.at(2, 2)};
    const Vec3 scale{std::max(length(ax), 1e-6f), std::max(length(ay), 1e-6f), std::max(length(az), 1e-6f)};
    Mat4 r;
    const Vec3 nx = ax / scale.x, ny = ay / scale.y, nz = az / scale.z;
    r.at(0, 0) = nx.x, r.at(0, 1) = nx.y, r.at(0, 2) = nx.z;
    r.at(1, 0) = ny.x, r.at(1, 1) = ny.y, r.at(1, 2) = ny.z;
    r.at(2, 0) = nz.x, r.at(2, 1) = nz.y, r.at(2, 2) = nz.z;
    it.rotation = r;
    it.center = world.translation();
    it.sphere = p.shape == "sphere";
    if (it.sphere) {
        const float rad = std::max(p.radius, 0.05f) * std::max({scale.x, scale.y, scale.z});
        it.halfExtents = {rad, rad, rad};
    } else {
        it.halfExtents = {std::max(p.size.x * scale.x * 0.5f, 0.05f), std::max(p.size.y * scale.y * 0.5f, 0.05f),
                          std::max(p.size.z * scale.z * 0.5f, 0.05f)};
    }
    it.capture = it.center + r.transformDir(p.captureOffset);
    const float minHalf = std::min({it.halfExtents.x, it.halfExtents.y, it.halfExtents.z});
    it.blendDistance = std::clamp(p.blendDistance, 0.f, minHalf);
    it.boxProjection = p.boxProjection;
    if (p.projectionSize.x > 0.f && p.projectionSize.y > 0.f && p.projectionSize.z > 0.f) {
        it.projectionBox = true;
        it.projectionHalf = {p.projectionSize.x * scale.x * 0.5f, p.projectionSize.y * scale.y * 0.5f, p.projectionSize.z * scale.z * 0.5f};
        it.projectionCenter = p.projectionOffset;
    }
    it.intensity = std::max(p.intensity, 0.f);
    it.interior = p.interior;
    it.ambientMode = p.ambient == "sky" ? 1 : p.ambient == "color" ? 2 : 0;
    if (it.interior && it.ambientMode == 1) it.ambientMode = 0;  // an interior has no sky to fall back to
    const float energy = std::max(p.ambientEnergy, 0.f);
    it.ambientColor = Vec3{srgbToLinear(p.ambientColor.x), srgbToLinear(p.ambientColor.y), srgbToLinear(p.ambientColor.z)} * energy;
    it.update = p.update == "realtime" ? 2 : p.update == "on_change" ? 1 : 0;
    it.interval = std::clamp(p.interval, 1, 600);
    it.resolution = sanitizeResolution(p.resolution);
    it.cullMask = static_cast<uint32_t>(p.cullMask) & render::kAllLayers;
    it.priority = p.priority;
    it.nearPlane = 0.05f;
    const float autoFar = std::max(3.f * boundingRadius(it), 40.f);
    it.farPlane = std::clamp(p.maxDistance > 0.f ? p.maxDistance : autoFar, 1.f, 20000.f);
    Hasher h;
    h.pod(it.center), h.bytes(it.rotation.m, sizeof(it.rotation.m)), h.pod(it.halfExtents), h.pod(it.sphere), h.pod(it.capture);
    h.pod(it.resolution), h.pod(it.cullMask), h.pod(it.interior), h.pod(it.ambientColor), h.pod(it.farPlane);
    it.captureKey = h.h;
    return it;
}

// ---------------------------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------------------------

FaceBasis faceBasis(int face) {
    switch (face) {
        case 0: return {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
        case 1: return {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};
        case 2: return {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
        case 3: return {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
        case 4: return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        default: return {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    }
}

Vec3 cubeDir(int face, Vec2 uv) {
    const float px = uv.x * 2.f - 1.f, py = uv.y * 2.f - 1.f;
    Vec3 d;
    switch (face) {
        case 0: d = {1.f, -py, -px}; break;
        case 1: d = {-1.f, -py, px}; break;
        case 2: d = {px, 1.f, py}; break;
        case 3: d = {px, -1.f, -py}; break;
        case 4: d = {px, -py, 1.f}; break;
        default: d = {-px, -py, -1.f}; break;
    }
    return normalize(d);
}

Mat4 faceViewProj(Vec3 eye, int face, float nearPlane, float farPlane) {
    const FaceBasis b = faceBasis(face);
    const float n = std::max(nearPlane, 1e-4f), f = std::max(farPlane, n * 2.f);
    const float A = f / (f - n), B = n * f / (n - f);
    Mat4 m;
    auto row = [&](int r, Vec3 v, float w) {
        m.at(0, r) = v.x;
        m.at(1, r) = v.y;
        m.at(2, r) = v.z;
        m.at(3, r) = w;
    };
    row(0, b.right, -dot(b.right, eye));
    row(1, b.up, -dot(b.up, eye));
    row(2, b.forward * A, -A * dot(b.forward, eye) + B);
    row(3, b.forward, -dot(b.forward, eye));
    return m;
}

Vec3 toLocal(const ProbeItem& p, Vec3 world) {
    const Vec3 d = world - p.center;
    // rotation is orthonormal: its inverse is its transpose.
    return {dot(d, {p.rotation.at(0, 0), p.rotation.at(0, 1), p.rotation.at(0, 2)}),
            dot(d, {p.rotation.at(1, 0), p.rotation.at(1, 1), p.rotation.at(1, 2)}),
            dot(d, {p.rotation.at(2, 0), p.rotation.at(2, 1), p.rotation.at(2, 2)})};
}

float edgeDistance(const ProbeItem& p, Vec3 world) {
    const Vec3 l = toLocal(p, world);
    if (p.sphere) return p.halfExtents.x - length(l);
    return std::min({p.halfExtents.x - std::fabs(l.x), p.halfExtents.y - std::fabs(l.y), p.halfExtents.z - std::fabs(l.z)});
}

float influence(const ProbeItem& p, Vec3 world) {
    const float d = edgeDistance(p, world);
    if (p.interior) {
        // Walls sit on (or just outside) an interior volume's faces: they still belong to the room.
        if (d <= -kInteriorMargin) return 0.f;
        return std::clamp(d / std::max(p.blendDistance, 1e-3f), kInteriorMinWeight, 1.f);
    }
    if (d <= 0.f) return 0.f;
    return std::clamp(d / std::max(p.blendDistance, 1e-3f), 0.f, 1.f);
}

Vec3 lookupDir(const ProbeItem& p, Vec3 world, Vec3 R) {
    if (!p.boxProjection) return normalize(R);
    Vec3 l = toLocal(p, world);
    const Vec3 rl = toLocal(p, p.center + R);  // rotation only
    Vec3 half = p.halfExtents;
    if (p.projectionBox) {
        l = l - p.projectionCenter;
        half = p.projectionHalf;
    }
    float t;
    if (p.sphere && !p.projectionBox) {
        const float b = dot(l, rl), c = dot(l, l) - p.halfExtents.x * p.halfExtents.x, rr = dot(rl, rl);
        t = (-b + std::sqrt(std::max(b * b - rr * c, 0.f))) / std::max(rr, 1e-8f);
    } else {
        auto exitT = [](float o, float d, float e) {
            if (std::fabs(d) < 1e-6f) return std::numeric_limits<float>::max();
            return std::max((e - o) / d, (-e - o) / d);
        };
        t = std::min({exitT(l.x, rl.x, half.x), exitT(l.y, rl.y, half.y), exitT(l.z, rl.z, half.z)});
    }
    t = std::max(t, 0.f);
    return normalize(world + R * t - p.capture);
}

float volume(const ProbeItem& p) {
    if (p.sphere) return 4.18879f * p.halfExtents.x * p.halfExtents.x * p.halfExtents.x;
    return 8.f * p.halfExtents.x * p.halfExtents.y * p.halfExtents.z;
}

float boundingRadius(const ProbeItem& p) { return p.sphere ? p.halfExtents.x : length(p.halfExtents); }

Aabb worldBounds(const ProbeItem& p) {
    if (p.sphere) return {p.center - Vec3(p.halfExtents.x), p.center + Vec3(p.halfExtents.x)};
    Aabb local{p.halfExtents * -1.f, p.halfExtents};
    return local.transformed(Mat4::translate(p.center) * p.rotation);
}

bool shadesBefore(const ProbeItem& a, const ProbeItem& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    const float va = volume(a), vb = volume(b);
    if (va != vb) return va < vb;
    return a.entity < b.entity;
}

Blend blend(const std::vector<const ProbeItem*>& ordered, Vec3 world) {
    Blend b;
    float acc = 0.f;
    for (size_t i = 0; i < ordered.size() && b.count < kMaxPerPixel; ++i) {
        const float w = influence(*ordered[i], world);
        if (w <= 0.f) continue;
        if (ordered[i]->interior) b.interior = true;
        const float a = w * (1.f - acc);
        b.probe[static_cast<size_t>(b.count)] = static_cast<int>(i);
        b.weight[static_cast<size_t>(b.count)] = a;
        ++b.count;
        acc += a;
        if (acc >= 0.999f) break;
    }
    if (b.interior && acc > 0.f) {
        for (int k = 0; k < b.count; ++k) b.weight[static_cast<size_t>(k)] /= acc;
        b.sky = 0.f;
    } else {
        b.sky = 1.f - acc;
    }
    return b;
}

Mat4 sunViewProj(Vec3 center, float radius, Vec3 sunDir) {
    const Vec3 dir = normalize(sunDir);
    const float reach = 120.f;  // casters between the sun and the capture (roofs, the street's far side)
    const float r = std::max(radius, 1.f);
    Mat4 view = Mat4::lookAt(center - dir * (r + reach), center, std::fabs(dir.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    Mat4 proj = Mat4::orthographic(r, 1.f, 0.1f, r * 2.f + reach * 2.f);
    return proj * view;
}

// ---------------------------------------------------------------------------------------------
// GPU contract
// ---------------------------------------------------------------------------------------------

GpuProbe gpuProbe(const ProbeItem& p, int slot, int baseMip, int colorIndex) {
    GpuProbe g{};
    const Mat4 w2l = p.rotation.transposed() * Mat4::translate(p.center * -1.f);
    std::memcpy(g.worldToLocal, w2l.m, sizeof(g.worldToLocal));
    g.extents[0] = p.halfExtents.x, g.extents[1] = p.halfExtents.y, g.extents[2] = p.halfExtents.z, g.extents[3] = p.blendDistance;
    g.capture[0] = p.capture.x, g.capture[1] = p.capture.y, g.capture[2] = p.capture.z, g.capture[3] = p.intensity;
    const int flags = (p.boxProjection ? 1 : 0) | (p.interior ? 2 : 0) | (p.sphere ? 4 : 0);
    g.params[0] = static_cast<float>(slot), g.params[1] = static_cast<float>(baseMip), g.params[2] = static_cast<float>(flags);
    g.params[3] = static_cast<float>(p.ambientMode);
    g.ambient[0] = p.ambientColor.x, g.ambient[1] = p.ambientColor.y, g.ambient[2] = p.ambientColor.z;
    g.ambient[3] = static_cast<float>(colorIndex);
    if (p.projectionBox) {
        g.projection[0] = p.projectionCenter.x, g.projection[1] = p.projectionCenter.y, g.projection[2] = p.projectionCenter.z;
        g.projection[3] = 1.f;
        g.projectionHalf[0] = p.projectionHalf.x, g.projectionHalf[1] = p.projectionHalf.y, g.projectionHalf[2] = p.projectionHalf.z;
    }
    return g;
}

Vec3 debugColor(int colorIndex) {
    static const Vec3 kPalette[12] = {{0.95f, 0.30f, 0.25f}, {0.25f, 0.65f, 0.98f}, {0.35f, 0.90f, 0.35f}, {0.98f, 0.80f, 0.20f},
                                      {0.85f, 0.35f, 0.95f}, {0.20f, 0.90f, 0.85f}, {0.98f, 0.55f, 0.15f}, {0.60f, 0.45f, 0.98f},
                                      {0.95f, 0.45f, 0.65f}, {0.70f, 0.95f, 0.25f}, {0.40f, 0.55f, 0.75f}, {0.85f, 0.70f, 0.50f}};
    const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette[((colorIndex % n) + n) % n];
}

GpuProbeBlock gpuBlock(const FrameData& frame, const Plan& plan) {
    GpuProbeBlock b{};
    b.info[0] = static_cast<float>(std::min<size_t>(plan.shaded.size(), kMaxProbes));
    b.info[1] = static_cast<float>(kMips - 1);
    for (size_t i = 0; i < plan.shaded.size() && i < static_cast<size_t>(kMaxProbes); ++i) {
        const int idx = plan.shaded[i];
        const ProbeState& s = plan.probes[static_cast<size_t>(idx)];
        b.probes[i] = gpuProbe(frame.probes[static_cast<size_t>(idx)], s.slot, s.baseMip, s.slot);
    }
    return b;
}

std::vector<uint32_t> clusterMasks(const FrameData& frame, const LightGrid& grid, const Plan& plan) {
    std::vector<uint32_t> masks(grid.clusterCount(), 0u);
    for (size_t i = 0; i < plan.shaded.size() && i < static_cast<size_t>(kMaxProbes); ++i) {
        const ProbeItem& p = frame.probes[static_cast<size_t>(plan.shaded[i])];
        ClusterRange r;
        if (!sphereClusters(grid, frame, p.center, boundingRadius(p), r)) continue;
        const uint32_t bit = 1u << i;
        for (int s = r.s0; s <= r.s1; ++s) {
            for (int ty = r.ty0; ty <= r.ty1; ++ty) {
                for (int tx = r.tx0; tx <= r.tx1; ++tx) masks[static_cast<size_t>((s * grid.tilesY + ty) * grid.tilesX + tx)] |= bit;
            }
        }
    }
    return masks;
}

std::vector<uint32_t> captureLights(const FrameData& frame, const ProbeItem& p, uint32_t& directionalCount) {
    const int dir = leadingDirectional(frame);
    directionalCount = static_cast<uint32_t>(dir);
    std::vector<std::pair<float, uint32_t>> near;
    const size_t limit = std::min(frame.lights.size(), FrameData::kMaxLights);
    for (size_t i = static_cast<size_t>(dir); i < limit; ++i) {
        const LightItem& l = frame.lights[i];
        if (l.kind == LightItem::Kind::Directional) continue;
        const float d = distance(l.position, p.capture);
        if (d > l.range + p.farPlane) continue;
        near.emplace_back(d, static_cast<uint32_t>(i));
    }
    std::sort(near.begin(), near.end());
    std::vector<uint32_t> out;
    for (const auto& [d, i] : near) {
        if (out.size() >= static_cast<size_t>(kMaxCaptureLights)) break;
        out.push_back(i);
    }
    if (out.empty()) out.push_back(0);  // GPU buffers must not be empty (the cluster lists none)
    return out;
}

uint64_t contentHash(const FrameData& f, const ProbeItem& p) {
    Hasher h;
    h.pod(p.captureKey);
    const Environment& e = f.environment;
    h.pod(e.sunDirection()), h.pod(e.sunColor), h.pod(e.sunIntensity), h.pod(e.skyTop), h.pod(e.skyHorizon), h.pod(e.ground);
    h.pod(e.ambient), h.pod(e.fogColor), h.pod(e.fogDensity), h.pod(e.clouds), h.pod(e.reflections), h.pod(e.hdriRotation);
    h.pod(e.hdriIntensity), h.str(e.skyMode), h.str(e.hdri);
    const float reach = p.farPlane;
    for (const DrawItem& d : f.draws) {
        if ((d.layers & p.cullMask) == 0 || !sphereTouchesBox(p.capture, reach, d.worldBounds)) continue;
        h.pod(d.entity);
        h.str(d.mesh);
        h.bytes(d.model.m, sizeof(d.model.m));
        const Surface& s = d.surface;
        h.pod(s.color), h.pod(s.emissive), h.pod(s.metallic), h.pod(s.roughness), h.pod(s.shading);
        h.str(s.texture), h.str(s.emissiveMap);
        if (d.skin >= 0 && static_cast<size_t>(d.skin) < f.skins.size() && f.skins[static_cast<size_t>(d.skin)].palette) {
            const auto& pal = *f.skins[static_cast<size_t>(d.skin)].palette;
            h.bytes(pal.data(), pal.size() * sizeof(Mat4));
        }
    }
    for (const TerrainItem& t : f.terrains) {
        if (!t.data) continue;
        h.pod(t.entity);
        const void* data = t.data.get();
        h.pod(data);
        h.pod(t.data->version());
        h.pod(t.origin);
    }
    for (const LightItem& l : f.lights) {
        if (l.kind != LightItem::Kind::Directional && distance(l.position, p.capture) > l.range + reach) continue;
        h.pod(l.position), h.pod(l.direction), h.pod(l.color), h.pod(l.intensity), h.pod(l.range), h.pod(l.kind), h.pod(l.cosCone);
    }
    return h.h;
}

std::vector<std::string> sceneWarnings(const FrameData& frame) {
    std::vector<std::string> out;
    const auto& ps = frame.probes;
    for (size_t i = 0; i < ps.size(); ++i) {
        const ProbeItem& p = ps[i];
        const Aabb box = worldBounds(p);
        for (size_t j = i + 1; j < ps.size(); ++j) {
            const ProbeItem& q = ps[j];
            if (p.priority != q.priority || !boxesOverlap(box, worldBounds(q))) continue;
            if (distance(p.center, q.center) >= boundingRadius(p) + boundingRadius(q)) continue;
            // Overlapping, same priority: the smaller volume wins inside the overlap (often intended for a
            // detail probe, ambiguous for two similar volumes).
            const float va = volume(p), vb = volume(q);
            if (std::min(va, vb) > 0.5f * std::max(va, vb)) {
                out.push_back(probeRef(p.entity) + " and " + probeRef(q.entity) + " overlap with equal priority " +
                              std::to_string(p.priority) + " and similar sizes: which one wins inside the overlap depends on a "
                              "small size difference. Give the probe that should win a higher priority, or shrink one to its room");
            }
        }
        bool geometry = !frame.terrains.empty();
        const ProbeItem* solid = nullptr;
        EntityId solidEntity = 0;
        for (const DrawItem& d : frame.draws) {
            if ((d.layers & p.cullMask) == 0) continue;
            if (!geometry && boxesOverlap(box, d.worldBounds)) geometry = true;
            if (solidEntity == 0 && d.surface.color.w >= 0.999f && (d.mesh == "cube" || d.mesh == "sphere")) {
                const Vec3 l = d.model.inverse().transformPoint(p.capture);
                const bool inside = d.mesh == "cube" ? std::fabs(l.x) < 0.499f && std::fabs(l.y) < 0.499f && std::fabs(l.z) < 0.499f
                                                     : length(l) < 0.499f;
                if (inside) {
                    solid = &p;
                    solidEntity = d.entity;
                }
            }
        }
        if (!geometry) {
            out.push_back(probeRef(p.entity) + " has no geometry inside its volume: nothing reflects it. Move it over the "
                          "floor / between the walls, or resize it (probe_add {size: \"auto\"} fits a room)");
        }
        if (solid) {
            out.push_back(probeRef(p.entity) + " captures from inside the solid mesh #" + std::to_string(solidEntity) +
                          ": its cubemap sees only the inside of that mesh. Move the probe or set captureOffset into open space");
        }
        if (!p.interior) {
            // The floor under the capture point: on (or within blendDistance of) the bottom face it gets
            // little of the probe, so wet streets and polished floors keep reflecting the sky.
            float floorY = -1e30f;
            for (const DrawItem& d : frame.draws) {
                const Aabb& b = d.worldBounds;
                if ((d.layers & p.cullMask) == 0 || p.capture.x < b.min.x || p.capture.x > b.max.x || p.capture.z < b.min.z ||
                    p.capture.z > b.max.z || b.max.y > p.capture.y + 0.01f) {
                    continue;
                }
                floorY = std::max(floorY, b.max.y);
            }
            const float w = floorY > -1e29f ? influence(p, {p.capture.x, floorY + 0.005f, p.capture.z}) : 1.f;
            if (w < 0.5f) {
                char y[32];
                std::snprintf(y, sizeof(y), "%.2f", floorY);
                out.push_back(probeRef(p.entity) + ": the floor under it (y = " + y + ") gets only " + std::to_string(static_cast<int>(w * 100.f)) +
                              "% of the probe because it lies within blendDistance of the volume's bottom face. Extend the "
                              "volume below the floor (keep reflections aligned with projectionSize), or lower blendDistance");
            }
        }
        if (edgeDistance(p, p.capture) <= 0.f) {
            out.push_back(probeRef(p.entity) + ": the capture point lies outside the influence volume; box projection "
                          "assumes it is inside (set captureOffset back inside)");
        }
        const float minHalf = std::min({p.halfExtents.x, p.halfExtents.y, p.halfExtents.z});
        if (p.blendDistance >= minHalf * 0.999f && minHalf > 0.f) {
            out.push_back(probeRef(p.entity) + ": blendDistance reaches the middle of the volume, so the probe never shows "
                          "at full strength; lower blendDistance");
        }
        if (p.farPlane < boundingRadius(p)) {
            out.push_back(probeRef(p.entity) + ": maxDistance (" + std::to_string(static_cast<int>(p.farPlane)) +
                          " m) is shorter than the volume; walls beyond it are missing from the capture");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------------------------

Settings settingsFor(const Environment& env, int quality, bool still) {
    Settings s;
    s.budget = std::clamp(env.probeBudget, 0, kMaxProbes);
    s.facesPerFrame = std::clamp(env.probeUpdates, 1, kFaces * kMaxProbes);
    if (quality >= 2) s.facesPerFrame = std::max(1, s.facesPerFrame / 2);
    s.unlimited = still;
    return s;
}

void Planner::invalidate(EntityId entity) {
    for (auto& [e, r] : records_) {
        if (entity == 0 || e == entity) r.forced = true;
    }
    if (entity != 0 && !records_.count(entity)) records_[entity].forced = true;
}

void Planner::atlasLost() {
    for (auto& [e, r] : records_) {
        r.ready = false;
        r.facesDone = 0;
    }
    inProgress_ = 0;
    atlasResolution_ = 0;
}

void Planner::reportCaptureMs(EntityId entity, double ms) {
    auto it = records_.find(entity);
    if (it != records_.end()) it->second.lastCaptureMs = ms;
}

Plan Planner::plan(const FrameData& frame, const Settings& settings) {
    ++frame_;
    Plan p;
    p.settings = settings;
    const size_t n = frame.probes.size();
    p.probes.resize(n);
    const Planes view(frame.viewProjection());
    const Vec3 eye = frame.camera.eye;
    for (size_t i = 0; i < n; ++i) {
        const ProbeItem& it = frame.probes[i];
        ProbeState& s = p.probes[i];
        s.entity = it.entity;
        s.inView = view.sphere(it.center, boundingRadius(it));
        const float dist = std::max(edgeDistance(it, eye) > 0.f ? 0.f : distance(eye, it.center) - boundingRadius(it), 0.f);
        // In view first, then priority, then nearer.
        s.importance = (s.inView ? 1e6f : 0.f) + static_cast<float>(it.priority) * 1e3f + 1e3f / (1.f + dist);
        records_[it.entity].lastSeen = frame_;
    }
    // Forget probes that are gone (their slots return to the pool).
    for (auto it = records_.begin(); it != records_.end();) {
        if (frame_ - it->second.lastSeen > 600) {
            for (EntityId& o : slots_) {
                if (o == it->first) o = 0;
            }
            if (inProgress_ == it->first) inProgress_ = 0;
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
    if (settings.budget <= 0 || n == 0) {
        for (auto& s : p.probes) s.reason = "disabled";
        if (settings.budget <= 0 && n > 0) p.warnings.push_back("reflection probes are off (Environment.probeBudget = 0)");
        p.atlasResolution = atlasResolution_;
        p.atlasSlots = static_cast<int>(slots_.size());
        last_ = p;
        return last_;
    }

    // --- Slots: the most important `budget` probes own a slot; others keep theirs while unused. ---
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return p.probes[static_cast<size_t>(a)].importance > p.probes[static_cast<size_t>(b)].importance;
    });
    // GPU memory stays bounded: the atlas never exceeds kMaxAtlasMB (512 px probes fit 16 slots).
    int topRes = kMinResolution;
    for (size_t k = 0; k < order.size() && k < static_cast<size_t>(settings.budget); ++k) {
        topRes = std::max(topRes, frame.probes[static_cast<size_t>(order[k])].resolution);
    }
    const size_t memoryCap = std::max<size_t>(1, static_cast<size_t>(kMaxAtlasMB * 1048576.0 / slotBytes(topRes)));
    const size_t budget = std::min(static_cast<size_t>(settings.budget), memoryCap);
    if (budget < static_cast<size_t>(settings.budget) && n > budget) {
        p.warnings.push_back("the probe atlas is capped at " + std::to_string(static_cast<int>(kMaxAtlasMB)) + " MB: " +
                             std::to_string(budget) + " probes of " + std::to_string(topRes) +
                             " px fit; lower the largest probes' resolution to fit more");
    }
    if (slots_.size() > budget) {  // the budget shrank: drop the slots beyond it
        for (size_t k = budget; k < slots_.size(); ++k) {
            if (auto r = records_.find(slots_[k]); r != records_.end()) r->second.slot = -1, r->second.ready = false;
        }
        slots_.resize(budget);
        slotUse_.resize(budget);
    }
    std::vector<int> wanted;
    for (int idx : order) {
        if (wanted.size() >= budget) {
            ProbeState& s = p.probes[static_cast<size_t>(idx)];
            Record& r = records_[s.entity];
            if (r.slot >= 0 && static_cast<size_t>(r.slot) < slots_.size() && slots_[static_cast<size_t>(r.slot)] == s.entity) {
                slots_[static_cast<size_t>(r.slot)] = 0;
            }
            r.slot = -1;
            r.ready = false;
            r.facesDone = 0;
            if (inProgress_ == s.entity) inProgress_ = 0;
            s.reason = "over_budget";
            ++p.overBudget;
            continue;
        }
        wanted.push_back(idx);
    }
    auto isWanted = [&](EntityId e) {
        for (int idx : wanted) {
            if (frame.probes[static_cast<size_t>(idx)].entity == e) return true;
        }
        return false;
    };
    // Atlas resolution: the largest wanted probe (changing it re-creates the atlas).
    int res = kMinResolution;
    for (int idx : wanted) res = std::max(res, frame.probes[static_cast<size_t>(idx)].resolution);
    if (res != atlasResolution_) {
        if (atlasResolution_ != 0) p.atlasReset = true;
        for (auto& [e, r] : records_) r.ready = false, r.facesDone = 0;
        inProgress_ = 0;
        atlasResolution_ = res;
    }
    for (int idx : wanted) {
        const ProbeItem& it = frame.probes[static_cast<size_t>(idx)];
        Record& r = records_[it.entity];
        if (r.slot >= 0 && static_cast<size_t>(r.slot) < slots_.size() && slots_[static_cast<size_t>(r.slot)] == it.entity) {
            slotUse_[static_cast<size_t>(r.slot)] = frame_;
            continue;
        }
        // A free slot, else a new one (the atlas grows in steps of 4 cubes; the backend copies the old
        // slots over), else the least recently used slot of a probe that is not wanted this frame.
        int best = -1;
        for (size_t k = 0; k < slots_.size() && best < 0; ++k) {
            if (slots_[k] == 0) best = static_cast<int>(k);
        }
        if (best < 0 && slots_.size() < budget) {
            best = static_cast<int>(slots_.size());
            const size_t grown = std::min(budget, slots_.size() + 4);
            slots_.resize(grown, 0);
            slotUse_.resize(grown, 0);
        }
        if (best < 0) {
            for (size_t k = 0; k < slots_.size(); ++k) {
                if (isWanted(slots_[k])) continue;
                if (best < 0 || slotUse_[k] < slotUse_[static_cast<size_t>(best)]) best = static_cast<int>(k);
            }
        }
        if (best < 0) continue;  // cannot happen: wanted.size() <= budget
        if (EntityId old = slots_[static_cast<size_t>(best)]; old != 0) {
            if (auto o = records_.find(old); o != records_.end()) o->second.slot = -1, o->second.ready = false, o->second.facesDone = 0;
            if (inProgress_ == old) inProgress_ = 0;
        }
        slots_[static_cast<size_t>(best)] = it.entity;
        slotUse_[static_cast<size_t>(best)] = frame_;
        r.slot = best;
        r.ready = false;
        r.facesDone = 0;
    }
    p.atlasResolution = atlasResolution_;
    p.atlasSlots = static_cast<int>(slots_.size());

    // --- Captures within the face budget. ---
    struct Need {
        int idx;
        float rank;
    };
    std::vector<Need> needs;
    for (int idx : wanted) {
        const ProbeItem& it = frame.probes[static_cast<size_t>(idx)];
        ProbeState& s = p.probes[static_cast<size_t>(idx)];
        Record& r = records_[it.entity];
        s.slot = r.slot;
        s.hadCapture = r.ready;
        s.baseMip = 0;
        for (int sz = atlasResolution_; sz > it.resolution && s.baseMip < kMips - 2; sz /= 2) ++s.baseMip;
        if (r.captureKey != it.captureKey || r.resolution != it.resolution) {
            // The probe moved or changed: start over (a ready probe keeps shading its old capture meanwhile).
            r.facesDone = 0;
            if (inProgress_ == it.entity) inProgress_ = 0;
        }
        bool need = !r.ready || r.forced || r.captureKey != it.captureKey || r.resolution != it.resolution;
        if (!need && it.update == static_cast<int>(Update::OnChange)) need = contentHash(frame, it) != r.contentHash;
        if (!need && it.update == static_cast<int>(Update::Realtime)) need = frame_ - r.lastCapture >= static_cast<uint64_t>(it.interval);
        if (!need && inProgress_ != it.entity) continue;
        if (r.firstRequest == 0) r.firstRequest = frame_;
        // In progress first, then first captures, then in view, priority, nearness; realtime by age.
        float rank = s.importance + (r.ready ? 0.f : 1e8f) + (inProgress_ == it.entity ? 1e9f : 0.f) +
                     static_cast<float>(std::min<uint64_t>(frame_ - r.firstRequest, 1000)) * 10.f;
        needs.push_back({idx, rank});
    }
    std::stable_sort(needs.begin(), needs.end(), [](const Need& a, const Need& b) { return a.rank > b.rank; });
    int faceBudget = settings.unlimited ? std::numeric_limits<int>::max() : settings.facesPerFrame;
    for (const Need& nd : needs) {
        const ProbeItem& it = frame.probes[static_cast<size_t>(nd.idx)];
        ProbeState& s = p.probes[static_cast<size_t>(nd.idx)];
        Record& r = records_[it.entity];
        const int remaining = kFaces - r.facesDone;
        // One capture may span frames at a time (it owns the scratch cube): others wait for it.
        const bool blocked = inProgress_ != 0 && inProgress_ != it.entity;
        if (faceBudget <= 0 || blocked) {
            p.facesDeferred += remaining;
            continue;
        }
        const int count = std::min(remaining, faceBudget);
        CaptureJob job;
        job.probe = nd.idx;
        job.firstFace = r.facesDone;
        job.faceCount = count;
        job.completes = r.facesDone + count >= kFaces;
        if (r.facesDone == 0) r.contentHash = contentHash(frame, it);
        faceBudget -= count;
        // A first capture only sees direct light; capturing again right away, lit by the first capture,
        // adds the room's bounce light (walls lit by walls). Without the budget for it, it comes later.
        const bool first = job.completes && !s.hadCapture;
        job.bounce = first && job.firstFace == 0 && faceBudget >= kFaces;
        if (job.bounce) faceBudget -= kFaces;
        const int faces = count + (job.bounce ? kFaces : 0);
        p.facesCaptured += faces;
        s.facesCaptured = faces;
        if (job.completes) {
            r.ready = true;
            r.forced = first && !job.bounce;  // one more capture later for the bounce light
            r.facesDone = 0;
            r.captureKey = it.captureKey;
            r.resolution = it.resolution;
            r.lastCapture = frame_;
            r.firstRequest = 0;
            ++r.captures;
            if (inProgress_ == it.entity) inProgress_ = 0;
        } else {
            r.facesDone += count;
            inProgress_ = it.entity;
        }
        p.jobs.push_back(job);
    }

    // --- Shading: ready probes in view, in shading order. ---
    std::vector<int> shaded;
    for (int idx : wanted) {
        ProbeState& s = p.probes[static_cast<size_t>(idx)];
        const Record& r = records_[s.entity];
        s.ready = r.ready;
        s.facesDone = r.facesDone;
        s.captures = r.captures;
        s.lastCapture = r.lastCapture;
        if (!r.ready) {
            s.reason = "pending";
            continue;
        }
        if (!s.inView) {
            s.reason = "out_of_view";
            continue;
        }
        shaded.push_back(idx);
    }
    std::sort(shaded.begin(), shaded.end(), [&](int a, int b) {
        return shadesBefore(frame.probes[static_cast<size_t>(a)], frame.probes[static_cast<size_t>(b)]);
    });
    if (shaded.size() > static_cast<size_t>(kMaxProbes)) shaded.resize(static_cast<size_t>(kMaxProbes));
    for (int idx : shaded) p.probes[static_cast<size_t>(idx)].shaded = true;
    p.shaded = std::move(shaded);
    if (p.overBudget > 0) {
        p.warnings.push_back("the probe atlas is full: " + std::to_string(p.overBudget) + " probe" + (p.overBudget == 1 ? "" : "s") +
                             " got no slot (Environment.probeBudget = " + std::to_string(settings.budget) +
                             ", max " + std::to_string(kMaxProbes) + "); the least important ones fall back to the sky. "
                             "Raise probeBudget or merge small probes");
    }
    if (p.facesDeferred > 0 && !settings.unlimited) {
        p.warnings.push_back(std::to_string(p.facesDeferred) + " capture faces wait for the face budget (Environment.probeUpdates = " +
                             std::to_string(settings.facesPerFrame) + " per frame)");
    }
    last_ = p;
    return last_;
}

Json Planner::info(const FrameData* frame) const {
    const Plan& p = last_;
    Json probes = Json::array();
    int ready = 0, slotsUsed = 0;
    for (EntityId e : slots_) slotsUsed += e != 0 ? 1 : 0;
    for (size_t i = 0; i < p.probes.size(); ++i) {
        const ProbeState& s = p.probes[i];
        Json j = Json::object({{"entity", static_cast<int64_t>(s.entity)}, {"ready", s.ready}, {"shaded", s.shaded}, {"inView", s.inView}});
        ready += s.ready ? 1 : 0;
        if (s.slot >= 0) {
            j["slot"] = s.slot;
            j["baseMip"] = s.baseMip;
            const Vec3 c = debugColor(s.slot);
            char hex[16];
            std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", static_cast<int>(c.x * 255.f), static_cast<int>(c.y * 255.f),
                          static_cast<int>(c.z * 255.f));
            j["debugColor"] = hex;
        }
        if (!s.reason.empty() && !s.shaded) j["reason"] = s.reason;
        j["capturedFaces"] = s.facesCaptured;
        if (s.facesDone > 0) j["facesInProgress"] = s.facesDone;
        j["captures"] = static_cast<int64_t>(s.captures);
        if (s.captures > 0) {
            j["lastCaptureFrame"] = static_cast<int64_t>(s.lastCapture);
            j["framesSinceCapture"] = static_cast<int64_t>(frame_ - s.lastCapture);
        }
        if (auto r = records_.find(s.entity); r != records_.end() && r->second.lastCaptureMs > 0.0) {
            j["lastCaptureGpuMs"] = std::round(r->second.lastCaptureMs * 100.0) / 100.0;
        }
        if (frame && i < frame->probes.size()) {
            const ProbeItem& it = frame->probes[i];
            j["shape"] = it.sphere ? "sphere" : "box";
            j["size"] = it.sphere ? Json(std::round(it.halfExtents.x * 200.f) / 100.f)
                                  : Json::array({std::round(it.halfExtents.x * 200.f) / 100.f, std::round(it.halfExtents.y * 200.f) / 100.f,
                                                 std::round(it.halfExtents.z * 200.f) / 100.f});
            j["center"] = Json::array({it.center.x, it.center.y, it.center.z});
            j["resolution"] = it.resolution;
            j["update"] = updateName(it.update);
            if (it.update == static_cast<int>(Update::Realtime)) j["interval"] = it.interval;
            j["priority"] = it.priority;
            j["interior"] = it.interior;
            j["ambient"] = ambientName(it.ambientMode);
            j["boxProjection"] = it.boxProjection;
            j["captureDistance"] = std::round(it.farPlane * 10.f) / 10.f;
            if (auto r = records_.find(s.entity); r != records_.end() && s.ready && it.update != static_cast<int>(Update::Realtime)) {
                // A cached capture that no longer matches what is in range (once probes wait for probe_bake).
                if (r->second.captureKey != it.captureKey || contentHash(*frame, it) != r->second.contentHash) j["stale"] = true;
            }
        }
        probes.push(std::move(j));
    }
    Json warnings = Json::array();
    for (const auto& w : p.warnings) warnings.push(w);
    if (frame) {
        for (const auto& w : sceneWarnings(*frame)) warnings.push(w);
        for (const Json& j : probes.elements()) {
            if (j.get("stale").asBool(false)) {
                warnings.push(probeRef(static_cast<EntityId>(j.get("entity").asInt())) +
                              " shows an old capture: something in range changed since. Run probe_bake (or use update: on_change)");
            }
        }
    }
    const double mb = slotBytes(p.atlasResolution) * p.atlasSlots / (1024.0 * 1024.0);
    return Json::object({{"atlas", Json::object({{"resolution", p.atlasResolution},
                                                 {"slots", p.atlasSlots},
                                                 {"used", slotsUsed},
                                                 {"mips", kMips},
                                                 {"format", "rgba16float cube array"},
                                                 {"memoryMB", std::round(mb * 10.0) / 10.0}})},
                         {"budget", Json::object({{"probes", p.settings.budget},
                                                  {"maxProbes", kMaxProbes},
                                                  {"facesPerFrame", p.settings.facesPerFrame},
                                                  {"unlimited", p.settings.unlimited}})},
                         {"count", static_cast<int64_t>(p.probes.size())},
                         {"ready", ready},
                         {"shaded", static_cast<int64_t>(p.shaded.size())},
                         {"overBudget", p.overBudget},
                         {"facesCaptured", p.facesCaptured},
                         {"facesDeferred", p.facesDeferred},
                         {"frames", static_cast<int64_t>(frame_)},
                         {"probes", probes},
                         {"warnings", warnings}});
}

}  // namespace sky::probes
