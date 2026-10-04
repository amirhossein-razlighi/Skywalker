#include "skywalker/render/ShadowAtlas.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>

#include "skywalker/render/Renderer.h"

namespace sky::shadows {

const char* projectionName(Projection p) {
    switch (p) {
        case Projection::Spot: return "spot";
        case Projection::Cube: return "cube";
        case Projection::DualParaboloid: return "dual_paraboloid";
        default: return "none";
    }
}

// --- Projection math (Shadows.metal mirrors these) ----------------------------------------------

float shadowNear(float range) { return std::clamp(range * 0.004f, 0.02f, 0.2f); }

float perspectiveDepth(float d, float near, float far) { return far * (d - near) / ((far - near) * std::max(d, 1e-6f)); }

Basis lightBasis(Vec3 forward) {
    Basis b;
    b.f = normalize(forward);
    Vec3 up = std::fabs(b.f.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    b.s = normalize(cross(b.f, up));
    b.u = cross(b.s, b.f);
    return b;
}

Basis cubeFaceBasis(int face) {
    static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    return lightBasis(kAxes[std::clamp(face, 0, 5)]);
}

int cubeFaceOf(Vec3 v) {
    float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    if (ax >= ay && ax >= az) return v.x >= 0.f ? 0 : 1;
    if (ay >= az) return v.y >= 0.f ? 2 : 3;
    return v.z >= 0.f ? 4 : 5;
}

Mat4 perspectiveViewProj(Vec3 position, const Basis& b, float tanHalf, float near, float far) {
    Mat4 view;
    view.at(0, 0) = b.s.x, view.at(1, 0) = b.s.y, view.at(2, 0) = b.s.z;
    view.at(0, 1) = b.u.x, view.at(1, 1) = b.u.y, view.at(2, 1) = b.u.z;
    view.at(0, 2) = -b.f.x, view.at(1, 2) = -b.f.y, view.at(2, 2) = -b.f.z;
    view.at(3, 0) = -dot(b.s, position);
    view.at(3, 1) = -dot(b.u, position);
    view.at(3, 2) = dot(b.f, position);
    Mat4 proj;
    const float k = 1.f / std::max(tanHalf, 1e-4f), zs = far / (near - far);
    proj.m[0] = k;
    proj.m[5] = k;
    proj.m[10] = zs;
    proj.m[11] = -1.f;
    proj.m[14] = zs * near;
    proj.m[15] = 0.f;
    return proj * view;
}

Mat4 paraboloidMatrix(Vec3 position, Vec3 forward, int hemisphere, float near, float far, float uvScale) {
    Basis b = lightBasis(forward);
    if (hemisphere != 0) b.s = b.s * -1.f, b.f = b.f * -1.f;
    Mat4 m;
    auto col = [&](int c, Vec3 v, float w) {
        m.at(c, 0) = v.x, m.at(c, 1) = v.y, m.at(c, 2) = v.z, m.at(c, 3) = w;
    };
    col(0, b.s, near);
    col(1, b.u, far);
    col(2, b.f, uvScale);
    col(3, position, kParaboloidSentinel);
    return m;
}

Vec4 shadowClip(const Mat4& m, Vec3 world) {
    if (m.at(3, 3) != kParaboloidSentinel) return m * Vec4(world, 1.f);
    Vec3 s{m.at(0, 0), m.at(0, 1), m.at(0, 2)}, u{m.at(1, 0), m.at(1, 1), m.at(1, 2)}, f{m.at(2, 0), m.at(2, 1), m.at(2, 2)};
    Vec3 pos{m.at(3, 0), m.at(3, 1), m.at(3, 2)};
    float near = m.at(0, 3), far = m.at(1, 3), k = m.at(2, 3);
    Vec3 v = world - pos;
    Vec3 lv{dot(v, s), dot(v, u), dot(v, f)};
    float dist = std::max(length(lv), 1e-6f);
    Vec3 d = lv * (1.f / dist);
    float px = d.x / std::max(1.f + d.z, 0.05f) / k, py = d.y / std::max(1.f + d.z, 0.05f) / k;
    float z = (dist - near) / (far - near);
    return {px, py, d.z > -0.2f ? z : d.z + 0.2f - 1e-3f, 1.f};
}

namespace {

Projected projectPerspective(Vec3 v, const Basis& b, float tanHalf, float near, float far, int face) {
    Projected r;
    r.face = face;
    float d = dot(v, b.f);
    if (d <= near * 0.5f) return r;
    float x = dot(v, b.s) / (d * tanHalf), y = dot(v, b.u) / (d * tanHalf);
    r.inside = std::fabs(x) <= 1.f && std::fabs(y) <= 1.f;
    r.uv = {x * 0.5f + 0.5f, 0.5f - y * 0.5f};
    r.depth = perspectiveDepth(d, near, far);
    return r;
}

}  // namespace

Projected projectSpot(Vec3 p, Vec3 lightPos, Vec3 dir, float tanHalf, float near, float far) {
    return projectPerspective(p - lightPos, lightBasis(dir), tanHalf, near, far, 0);
}

Projected projectCube(Vec3 p, Vec3 lightPos, float tanHalf, float near, float far) {
    Vec3 v = p - lightPos;
    int face = cubeFaceOf(v);
    return projectPerspective(v, cubeFaceBasis(face), tanHalf, near, far, face);
}

Projected projectParaboloid(Vec3 p, Vec3 lightPos, Vec3 dir, float uvScale, float near, float far) {
    Basis b = lightBasis(dir);
    Vec3 v = p - lightPos;
    Vec3 lv{dot(v, b.s), dot(v, b.u), dot(v, b.f)};
    float dist = std::max(length(lv), 1e-6f);
    Vec3 d = lv * (1.f / dist);
    Projected r;
    r.face = d.z >= 0.f ? 0 : 1;
    if (r.face == 1) d.x = -d.x, d.z = -d.z;  // the back hemisphere's basis is (-s, u, -f)
    float px = d.x / (1.f + d.z) / uvScale, py = d.y / (1.f + d.z) / uvScale;
    r.inside = std::fabs(px) <= 1.f && std::fabs(py) <= 1.f;
    r.uv = {px * 0.5f + 0.5f, 0.5f - py * 0.5f};
    r.depth = (dist - near) / (far - near);
    return r;
}

Rect faceRect(Projection p, int face, int slotSize) {
    switch (p) {
        case Projection::Cube: {
            const int w = slotSize / 3, h = slotSize / 2;
            return {(face % 3) * w, (face / 3) * h, w, h};
        }
        case Projection::DualParaboloid: return {face * (slotSize / 2), 0, slotSize / 2, slotSize};
        default: return {0, 0, slotSize, slotSize};
    }
}

int faceCount(Projection p) {
    switch (p) {
        case Projection::Spot: return 1;
        case Projection::Cube: return 6;
        case Projection::DualParaboloid: return 2;
        default: return 0;
    }
}

// --- Atlas -------------------------------------------------------------------------------------

ShadowAtlas::ShadowAtlas(AtlasConfig config) { reset(config); }

void ShadowAtlas::reset(AtlasConfig config) {
    int size = 1024;
    while (size * 2 <= std::clamp(config.size, 1024, 8192)) size *= 2;
    config.size = size;
    for (int& s : config.subdiv) s = std::clamp(s, 1, 16);
    config_ = config;
    for (int q = 0; q < kQuadrants; ++q) {
        cells_[static_cast<size_t>(q)].assign(static_cast<size_t>(config_.subdiv[static_cast<size_t>(q)] * config_.subdiv[static_cast<size_t>(q)]), Cell{});
    }
    byKey_.clear();
}

int ShadowAtlas::slotSize(int q) const { return quadrantSize() / config_.subdiv[static_cast<size_t>(q)]; }
int ShadowAtlas::slotCount(int q) const { return static_cast<int>(cells_[static_cast<size_t>(q)].size()); }

int ShadowAtlas::quadrantFor(int size) const {
    int best = -1, smallest = 0;
    for (int q = 0; q < kQuadrants; ++q) {
        if (slotSize(q) < slotSize(smallest)) smallest = q;
        if (slotSize(q) <= size && (best < 0 || slotSize(q) > slotSize(best))) best = q;
    }
    return best >= 0 ? best : smallest;
}

AtlasSlot ShadowAtlas::slotAt(int q, int i) const {
    AtlasSlot s;
    s.quadrant = q;
    s.index = i;
    s.size = slotSize(q);
    const int n = config_.subdiv[static_cast<size_t>(q)];
    s.x = (i % n) * s.size;
    s.y = (i / n) * s.size;
    return s;
}

std::vector<AtlasSlot> ShadowAtlas::allocate(const std::vector<Request>& requests, uint64_t frame) {
    std::vector<AtlasSlot> out(requests.size());
    std::vector<size_t> order(requests.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (requests[a].priority != requests[b].priority) return requests[a].priority > requests[b].priority;
        return requests[a].key < requests[b].key;
    });
    std::unordered_map<uint64_t, size_t> rank;  // key -> position in `order`
    for (size_t r = 0; r < order.size(); ++r) rank.emplace(requests[order[r]].key, r);
    auto requested = [&](uint64_t key) { return rank.count(key) > 0; };

    // Quadrants by slot size, largest first.
    std::array<int, kQuadrants> bySize{0, 1, 2, 3};
    std::stable_sort(bySize.begin(), bySize.end(), [&](int a, int b) { return slotSize(a) > slotSize(b); });

    auto claim = [&](const AtlasSlot& s, uint64_t key) {
        Cell& c = cell(s);
        if (c.owner != key) {
            if (c.owner) byKey_.erase(c.owner);
            c.owner = key;
            c.valid = false;
            c.hash = 0;
        }
        c.lastUse = frame;
        byKey_[key] = s;
    };
    // A free cell, or the least recently used cell of a light not requested this frame.
    auto findFree = [&](int q) {
        AtlasSlot best;
        uint64_t bestUse = UINT64_MAX;
        const auto& cs = cells_[static_cast<size_t>(q)];
        for (size_t i = 0; i < cs.size(); ++i) {
            const Cell& c = cs[i];
            if (c.owner == 0) return slotAt(q, static_cast<int>(i));
            if (c.lastUse != frame && !requested(c.owner) && c.lastUse < bestUse) {
                bestUse = c.lastUse;
                best = slotAt(q, static_cast<int>(i));
            }
        }
        return best;
    };
    // The cell of the least important light still waiting for its turn (lower priority than `r`).
    auto findSteal = [&](int q, size_t r) {
        AtlasSlot best;
        size_t worst = r;
        const auto& cs = cells_[static_cast<size_t>(q)];
        for (size_t i = 0; i < cs.size(); ++i) {
            const Cell& c = cs[i];
            if (c.owner == 0 || c.lastUse == frame) continue;
            auto it = rank.find(c.owner);
            if (it != rank.end() && it->second > worst) {
                worst = it->second;
                best = slotAt(q, static_cast<int>(i));
            }
        }
        return best;
    };

    for (size_t r = 0; r < order.size(); ++r) {
        const Request& req = requests[order[r]];
        if (req.key == 0) continue;
        AtlasSlot cur;
        if (auto it = byKey_.find(req.key); it != byKey_.end() && cell(it->second).owner == req.key) cur = it->second;
        if (cur.valid() && cell(cur).lastUse == frame) continue;  // duplicate key this frame
        const int tq = quadrantFor(std::max(req.size, 1));
        AtlasSlot got;
        if (cur.valid()) {
            // A light with a slot keeps it while it is within a factor of two of the wanted size (no
            // thrashing around tier boundaries); it moves when a free / stale slot of the right tier
            // exists, or grows into a completely free one. It never takes another light's slot.
            const int want = slotSize(tq);
            const bool close = cur.size * 2 >= want && cur.size <= want * 2;
            if (cur.size != want) {
                AtlasSlot cand = findFree(tq);
                if (cand.valid() && (!close || (cur.size < want && cell(cand).owner == 0))) got = cand;
            }
            if (got.valid()) {
                cell(cur) = Cell{};
                byKey_.erase(req.key);
            } else {
                got = cur;
            }
        } else {
            // The wanted tier, then smaller slots, then bigger ones: free or stale cells first, then
            // the slot of a less important light that has not been served yet.
            std::vector<int> prefs;
            const auto tIt = std::find(bySize.begin(), bySize.end(), tq);
            for (auto it = tIt; it != bySize.end(); ++it) prefs.push_back(*it);
            for (auto it = tIt; it != bySize.begin();) prefs.push_back(*--it);
            for (int q : prefs) {
                if ((got = findFree(q)).valid()) break;
            }
            for (auto it = tIt; it != bySize.end() && !got.valid(); ++it) got = findSteal(*it, r);
        }
        if (got.valid()) {
            claim(got, req.key);
            out[order[r]] = got;
        }
    }
    return out;
}

bool ShadowAtlas::contentValid(const AtlasSlot& s, uint64_t hash) const {
    return s.valid() && cell(s).valid && cell(s).hash == hash;
}
bool ShadowAtlas::hasContent(const AtlasSlot& s) const { return s.valid() && cell(s).valid; }
void ShadowAtlas::markRendered(const AtlasSlot& s, uint64_t hash) {
    if (!s.valid()) return;
    cell(s).valid = true;
    cell(s).hash = hash;
}
void ShadowAtlas::invalidateAll() {
    for (auto& q : cells_) {
        for (Cell& c : q) c.valid = false;
    }
}
uint64_t ShadowAtlas::owner(const AtlasSlot& s) const { return s.valid() ? cell(s).owner : 0; }
int ShadowAtlas::used(int q) const {
    int n = 0;
    for (const Cell& c : cells_[static_cast<size_t>(q)]) n += c.owner != 0 ? 1 : 0;
    return n;
}
AtlasSlot ShadowAtlas::slotOf(uint64_t key) const {
    auto it = byKey_.find(key);
    return it != byKey_.end() ? it->second : AtlasSlot{};
}

// --- Planning ----------------------------------------------------------------------------------

ShadowSettings settingsFor(const Environment& env, int quality, bool still) {
    ShadowSettings s;
    s.atlas.size = std::clamp(env.localShadowAtlas, 1024, 8192);
    s.maxLights = std::clamp(env.localShadowLights, 0, kMaxShadowedLights);
    s.maxFaceUpdates = std::clamp(env.localShadowUpdates, 1, kMaxShadowedLights * 6);
    if (quality >= 2) {  // fast editor tier: half the shadowed lights and updates
        s.maxLights = std::min(s.maxLights, std::max(4, s.maxLights / 2));
        s.maxFaceUpdates = std::max(6, s.maxFaceUpdates / 2);
    }
    s.unlimitedUpdates = still;
    s.softness = env.shadowSoftness;
    return s;
}

std::array<Vec4, 2> gpuShadowParams(const LightShadow& s, const AtlasConfig& atlas) {
    if (s.projection == Projection::None || !s.slot.valid()) return {Vec4{}, Vec4{}};
    const float half = static_cast<float>(atlas.size / 2);
    return {Vec4{static_cast<float>(s.slot.x) / half, static_cast<float>(s.slot.y) / half, static_cast<float>(s.slot.size) / half,
                 static_cast<float>(static_cast<int>(s.projection) + 4 * s.slot.quadrant)},
            Vec4{s.strength, s.bias, s.normalBias, s.tanHalf}};
}

bool sphereTouches(Vec3 c, float r, const Aabb& b) {
    float d2 = 0.f;
    for (int i = 0; i < 3; ++i) {
        float v = c[i] < b.min[i] ? b.min[i] - c[i] : (c[i] > b.max[i] ? c[i] - b.max[i] : 0.f);
        d2 += v * v;
    }
    return d2 <= r * r;
}

bool castsLocalShadow(const DrawItem& d, Vec3 lightPos, float lightRange) {
    if (!d.castShadows || d.surface.color.w < 0.5f || d.surface.shading == Shading::Unlit) return false;
    // A small fixture around or right next to the light (bulb, lamp head with its glass, sign box)
    // would swallow it: the light stands for the glowing fixture, which does not shadow its own light.
    const Aabb& b = d.worldBounds;
    const bool small = length(b.max - b.min) < std::max(1.f, lightRange * 0.15f);
    return !(small && sphereTouches(lightPos, std::max(0.3f, lightRange * 0.03f), b));
}

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

}  // namespace

uint64_t casterHash(const FrameData& f, const LightItem& l, uint64_t frameIndex) {
    Hasher h;
    h.pod(l.position), h.pod(l.direction), h.pod(l.range), h.pod(l.cosCone), h.pod(l.shadowMode), h.pod(l.kind);
    bool dynamic = false;
    for (const DrawItem& d : f.draws) {
        if (!castsLocalShadow(d, l.position, l.range) || !sphereTouches(l.position, l.range, d.worldBounds)) continue;
        h.pod(d.entity);
        h.str(d.mesh);
        h.bytes(d.model.m, sizeof(d.model.m));
        h.pod(d.surface.alphaCutoff);
        if (d.surface.alphaCutoff > 0.f) h.str(d.surface.texture);
        if (d.skin >= 0 && static_cast<size_t>(d.skin) < f.skins.size() && f.skins[static_cast<size_t>(d.skin)].palette) {
            const auto& pal = *f.skins[static_cast<size_t>(d.skin)].palette;
            h.bytes(pal.data(), pal.size() * sizeof(Mat4));
        }
    }
    for (const TerrainItem& t : f.terrains) {
        if (!t.castShadows || !t.data) continue;
        h.pod(t.entity);
        const void* data = t.data.get();
        h.pod(data);
        h.pod(t.data->version());  // sculpting edits the heights in place
        h.pod(t.origin);
    }
    for (const InstanceBatch& b : f.instances) {
        if (b.castShadows && sphereTouches(l.position, l.range, b.bounds)) h.pod(b.id);
    }
    for (const GroomItem& g : f.grooms) {
        if (!g.data) continue;
        Aabb box = g.data->bounds.transformed(g.model);
        dynamic = dynamic || sphereTouches(l.position, l.range, box);
    }
    for (const GpuEmitterItem& e : f.gpuEmitters) {
        if (e.params.facing == "mesh" && distance(e.world.translation(), l.position) < l.range + 10.f) dynamic = true;
    }
    if (dynamic) h.pod(frameIndex);
    return h.h;
}

ShadowPlan LocalShadowPlanner::plan(const FrameData& frame, const ShadowSettings& settings) {
    ShadowPlan p;
    p.settings = settings;
    if (!(settings.atlas == atlas_.config())) {
        AtlasConfig want = settings.atlas;
        atlas_.reset(want);
        p.settings.atlas = atlas_.config();  // sanitized (power of two)
    }
    p.settings.atlas = atlas_.config();
    ++frame_;
    p.lights.resize(frame.lights.size());
    const ViewCamera& cam = frame.camera;
    const Planes view(frame.viewProjection());
    const float tanHalfFov = std::tan(radians(std::clamp(cam.fovDeg, 1.f, 179.f)) * 0.5f);
    const int half = atlas_.quadrantSize();

    struct Candidate {
        int light;
        float priority;
        int size;
    };
    std::vector<Candidate> cands;
    int directional = 0;
    for (size_t i = 0; i < frame.lights.size(); ++i) {
        const LightItem& l = frame.lights[i];
        LightShadow& ls = p.lights[i];
        ls.id = l.id;
        ls.kind = static_cast<int>(l.kind);
        if (!l.shadows) {
            ls.reason = "disabled";
            continue;
        }
        ++p.requested;
        if (l.kind == LightItem::Kind::Directional) {
            ls.reason = "directional";
            ++directional;
            continue;
        }
        if (l.range <= 0.f || l.intensity <= 0.f) {
            ls.reason = "disabled";
            continue;
        }
        const float dist = distance(cam.eye, l.position);
        if (l.shadowMaxDistance > 0.f && dist > l.shadowMaxDistance) {
            ls.reason = "beyond_max_distance";
            continue;
        }
        if (!view.sphere(l.position, l.range)) {
            ls.reason = "out_of_view";
            continue;
        }
        float frac = cam.orthographic ? std::min(1.f, l.range / std::max(cam.orthoSize, 1e-3f))
                                      : (dist <= l.range ? 1.f : std::min(1.f, l.range / (dist * tanHalfFov)));
        const bool spot = l.kind == LightItem::Kind::Spot && l.cosCone >= std::cos(radians(65.f));
        ls.projection = spot ? Projection::Spot : (l.shadowMode == 1 ? Projection::DualParaboloid : Projection::Cube);
        int size = l.shadowResolution > 0 ? (spot ? l.shadowResolution : l.shadowResolution * 2)
                                          : static_cast<int>(frac * static_cast<float>(half) * (spot ? 1.f : 1.5f));
        ls.requestedSize = std::clamp(size, 64, half);
        ls.priority = frac * (0.25f + std::min(l.intensity, 50.f) / 50.f) + (l.shadowResolution > 0 ? 0.01f : 0.f);
        cands.push_back({static_cast<int>(i), ls.priority, ls.requestedSize});
    }
    p.candidates = static_cast<int>(cands.size());
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.priority > b.priority; });
    const size_t keep = std::min(cands.size(), static_cast<size_t>(std::clamp(settings.maxLights, 0, kMaxShadowedLights)));
    for (size_t k = keep; k < cands.size(); ++k) {
        LightShadow& ls = p.lights[static_cast<size_t>(cands[k].light)];
        ls.projection = Projection::None;
        ls.reason = "over_light_budget";
        ++p.overBudget;
    }
    if (p.overBudget > 0) {
        p.warnings.push_back(std::to_string(p.overBudget) + " point/spot lights in view light without shadows: over the budget of " +
                             std::to_string(keep) + " (Environment.localShadowLights). Raise it, or set castShadows=false on "
                             "minor lights so the important ones keep theirs.");
    }
    if (directional > 0) {
        p.warnings.push_back("directional lights other than the sun cast no shadows yet (castShadows is ignored for them)");
    }
    cands.resize(keep);

    std::vector<ShadowAtlas::Request> reqs;
    reqs.reserve(cands.size());
    for (const Candidate& c : cands) {
        const LightItem& l = frame.lights[static_cast<size_t>(c.light)];
        uint64_t key = l.id ? l.id : (0xFFull << 56) | static_cast<uint64_t>(c.light);  // anonymous lights: by index
        reqs.push_back({key, c.size, c.priority});
    }
    const std::vector<AtlasSlot> slots = atlas_.allocate(reqs, frame_);

    struct Update {
        int light;
        bool fresh;
        float priority;
    };
    std::vector<Update> updates;
    int atlasFull = 0;
    for (size_t k = 0; k < cands.size(); ++k) {
        const int i = cands[k].light;
        const LightItem& l = frame.lights[static_cast<size_t>(i)];
        LightShadow& ls = p.lights[static_cast<size_t>(i)];
        if (!slots[k].valid()) {
            ls.projection = Projection::None;
            ls.reason = "atlas_full";
            ++atlasFull;
            continue;
        }
        ls.slot = slots[k];
        ls.near = shadowNear(l.range);
        ls.far = l.range;
        ls.bias = std::max(l.shadowBias, 0.f);
        ls.normalBias = std::max(l.shadowNormalBias, 0.f);
        const Rect fr = faceRect(ls.projection, 0, ls.slot.size);
        ls.faceResolution = std::max(1, std::min(fr.w, fr.h));
        const float guard = 1.f + 8.f / static_cast<float>(ls.faceResolution);  // ~4 texels each side for PCF
        if (ls.projection == Projection::Spot) {
            ls.tanHalf = std::tan(std::min(std::acos(std::clamp(l.cosCone, -1.f, 1.f)), radians(80.f))) * guard;
        } else if (ls.projection == Projection::Cube) {
            ls.tanHalf = guard;
        } else {
            ls.tanHalf = 1.1f;  // paraboloid uv scale: each half reaches ~6 degrees past its hemisphere
        }
        const float dist = distance(cam.eye, l.position);
        ls.strength = 1.f;
        if (l.shadowMaxDistance > 0.f) {
            float t = std::clamp((dist - l.shadowMaxDistance * 0.85f) / (l.shadowMaxDistance * 0.15f), 0.f, 1.f);
            ls.strength = 1.f - t * t * (3.f - 2.f * t);
        }
        ls.hash = casterHash(frame, l, frame_);
        if (ls.projection == Projection::DualParaboloid) {
            // Paraboloid views bend straight edges: big low-poly casters (box walls, planes) come out wrong.
            int coarse = 0;
            for (const DrawItem& d : frame.draws) {
                const bool primitive = d.mesh == "cube" || d.mesh == "plane" || d.mesh == "quad";
                if (primitive && length(d.worldBounds.max - d.worldBounds.min) > 2.f && castsLocalShadow(d, l.position, l.range) &&
                    sphereTouches(l.position, l.range, d.worldBounds)) {
                    ++coarse;
                }
            }
            if (coarse > 0) {
                p.warnings.push_back("a dual_paraboloid light has " + std::to_string(coarse) +
                                     " large box/plane casters in range: paraboloid shadows of big flat polygons are "
                                     "approximate (false shadows on walls); use shadowMode \"cube\" for it");
            }
        }
        if (!atlas_.contentValid(ls.slot, ls.hash)) updates.push_back({i, !atlas_.hasContent(ls.slot), ls.priority});
    }
    if (atlasFull > 0) {
        p.warnings.push_back(std::to_string(atlasFull) + " lights found no free atlas slot (Environment.localShadowAtlas = " +
                             std::to_string(atlas_.config().size) + "); they light without shadows");
    }
    // New slots first (they have no shadow at all yet), then the most important changes.
    std::stable_sort(updates.begin(), updates.end(), [](const Update& a, const Update& b) {
        if (a.fresh != b.fresh) return a.fresh;
        return a.priority > b.priority;
    });
    const int budget = settings.unlimitedUpdates ? INT_MAX : std::max(1, settings.maxFaceUpdates);
    int used = 0;
    for (const Update& u : updates) {
        const LightItem& l = frame.lights[static_cast<size_t>(u.light)];
        LightShadow& ls = p.lights[static_cast<size_t>(u.light)];
        const int n = faceCount(ls.projection);
        if (used > 0 && used + n > budget) {
            p.deferredFaces += n;
            if (u.fresh) {
                ls.projection = Projection::None;
                ls.reason = "pending";
            } else {
                ls.stale = true;
            }
            continue;
        }
        used += n;
        ls.updated = true;
        atlas_.markRendered(ls.slot, ls.hash);
        for (int face = 0; face < n; ++face) {
            ShadowFace sf;
            sf.light = u.light;
            sf.face = face;
            sf.projection = ls.projection;
            sf.quadrant = ls.slot.quadrant;
            Rect r = faceRect(ls.projection, face, ls.slot.size);
            r.x += ls.slot.x;
            r.y += ls.slot.y;
            sf.viewport = r;
            sf.center = l.position;
            sf.radius = l.range;
            if (ls.projection == Projection::Spot) {
                sf.viewProj = perspectiveViewProj(l.position, lightBasis(l.direction), ls.tanHalf, ls.near, ls.far);
            } else if (ls.projection == Projection::Cube) {
                sf.viewProj = perspectiveViewProj(l.position, cubeFaceBasis(face), ls.tanHalf, ls.near, ls.far);
            } else {
                sf.viewProj = paraboloidMatrix(l.position, l.direction, face, ls.near, ls.far, ls.tanHalf);
            }
            p.faces.push_back(sf);
        }
    }
    if (p.deferredFaces > 0 && !settings.unlimitedUpdates) {
        p.warnings.push_back(std::to_string(p.deferredFaces) + " shadow views wait for a later frame (Environment.localShadowUpdates = " +
                             std::to_string(settings.maxFaceUpdates) + " per frame)");
    }
    for (const LightShadow& ls : p.lights) {
        if (ls.projection == Projection::None) continue;
        ++p.shadowed;
        if (!ls.updated) ++p.cachedLights;
    }
    last_ = p;
    return p;
}

Json LocalShadowPlanner::info(const FrameData* frame) const {
    (void)frame;
    const ShadowPlan& p = last_;
    const AtlasConfig& a = atlas_.config();
    Json quads = Json::array();
    for (int q = 0; q < kQuadrants; ++q) {
        quads.push(Json::object({{"quadrant", q},
                                 {"slotSize", atlas_.slotSize(q)},
                                 {"slots", atlas_.slotCount(q)},
                                 {"used", atlas_.used(q)}}));
    }
    static const char* kSources[] = {"light", "fluid", "particles", "gpu_particles"};
    static const char* kKinds[] = {"directional", "point", "spot"};
    Json lights = Json::array();
    for (size_t i = 0; i < p.lights.size(); ++i) {
        const LightShadow& ls = p.lights[i];
        if (ls.reason == "disabled" && ls.projection == Projection::None) continue;
        const int source = static_cast<int>((ls.id >> 56) & 0xFF);
        Json j = Json::object({{"light", static_cast<int64_t>(i)},
                               {"entity", static_cast<int64_t>(ls.id & 0xFFFFFFFFFFFFull)},
                               {"source", source < 4 ? kSources[source] : "other"},
                               {"kind", kKinds[std::clamp(ls.kind, 0, 2)]},
                               {"projection", projectionName(ls.projection)}});
        if (ls.projection != Projection::None) {
            j["quadrant"] = ls.slot.quadrant;
            j["slot"] = Json::array({ls.slot.x, ls.slot.y, ls.slot.size});
            j["faceResolution"] = ls.faceResolution;
            j["updated"] = ls.updated;
            if (ls.stale) j["stale"] = true;
            j["strength"] = std::round(ls.strength * 100.f) / 100.f;
        } else {
            j["reason"] = ls.reason;
        }
        j["priority"] = std::round(ls.priority * 1000.f) / 1000.f;
        lights.push(std::move(j));
    }
    Json warnings = Json::array();
    for (const auto& w : p.warnings) warnings.push(w);
    const double mb = static_cast<double>(a.size) * a.size * 4.0 / (1024.0 * 1024.0);
    return Json::object({{"atlasSize", a.size},
                         {"format", "depth32float, 4 quadrant slices"},
                         {"memoryMB", std::round(mb)},
                         {"quadrants", quads},
                         {"budget", Json::object({{"maxLights", p.settings.maxLights},
                                                  {"maxFaceUpdates", p.settings.maxFaceUpdates},
                                                  {"unlimitedUpdates", p.settings.unlimitedUpdates}})},
                         {"requested", p.requested},
                         {"candidates", p.candidates},
                         {"shadowed", p.shadowed},
                         {"cachedLights", p.cachedLights},
                         {"overBudget", p.overBudget},
                         {"facesRendered", static_cast<int64_t>(p.faces.size())},
                         {"facesDeferred", p.deferredFaces},
                         {"frames", static_cast<int64_t>(frame_)},
                         {"lights", lights},
                         {"warnings", warnings}});
}

}  // namespace sky::shadows
