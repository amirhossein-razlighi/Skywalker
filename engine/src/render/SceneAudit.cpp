// Scene audit (see SceneAudit.h): a coarse CPU depth raster of a frame plus scene checks.

#include "skywalker/render/SceneAudit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/AnimationComponents.h"
#include "skywalker/ecs/PhysicsComponents.h"

namespace sky::audit {

namespace fs = std::filesystem;

namespace {

const char* const kPrimitives[] = {"cube", "sphere", "plane", "cylinder", "cone", "quad", "capsule", "torus"};
const char* const kProcedural[] = {"grass", "grass_tall", "fern", "flowers", "pebbles", "shell", "rock"};

float round4(float v) { return std::round(v * 10000.f) / 10000.f; }
float round2(float v) { return std::round(v * 100.f) / 100.f; }

/// "1.23%" for a fraction (0..1), with precision that suits small coverages.
std::string pct(float fraction) {
    char b[32];
    const float v = fraction * 100.f;
    std::snprintf(b, sizeof(b), v >= 10.f ? "%.1f%%" : v >= 0.1f ? "%.2f%%" : "%.3f%%", static_cast<double>(v));
    return b;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------------------

struct ClipVert {
    Vec4 c;
};

class Raster {
public:
    Raster(CoverageBuffer& b, const Mat4& vp) : buf_(b), vp_(vp) {}

    void triangle(Vec3 a, Vec3 b, Vec3 c, int32_t owner) {
        Vec4 ca = vp_ * Vec4(a, 1.f), cb = vp_ * Vec4(b, 1.f), cc = vp_ * Vec4(c, 1.f);
        // Trivial reject against the frustum sides (all three outside one plane).
        auto out = [](const Vec4& v, int plane) {
            switch (plane) {
                case 0: return v.x < -v.w;
                case 1: return v.x > v.w;
                case 2: return v.y < -v.w;
                case 3: return v.y > v.w;
                case 4: return v.z < 0.f;
                default: return v.z > v.w;
            }
        };
        for (int p = 0; p < 6; ++p) {
            if (out(ca, p) && out(cb, p) && out(cc, p)) return;
        }
        ++buf_.triangles;
        if (ca.z >= 0.f && cb.z >= 0.f && cc.z >= 0.f) {
            screenTriangle(ca, cb, cc, owner);
            return;
        }
        // Clip against the near plane (z >= 0) and fan the polygon.
        std::array<Vec4, 4> poly{};
        int n = 0;
        const Vec4 in[3] = {ca, cb, cc};
        for (int i = 0; i < 3; ++i) {
            const Vec4& p = in[i];
            const Vec4& q = in[(i + 1) % 3];
            bool pin = p.z >= 0.f, qin = q.z >= 0.f;
            if (pin) poly[static_cast<size_t>(n++)] = p;
            if (pin != qin && n < 4) {
                float t = p.z / (p.z - q.z);
                poly[static_cast<size_t>(n++)] = p + (q + p * -1.f) * t;
            }
        }
        for (int i = 1; i + 1 < n; ++i) screenTriangle(poly[0], poly[static_cast<size_t>(i)], poly[static_cast<size_t>(i + 1)], owner);
    }

private:
    void screenTriangle(const Vec4& a, const Vec4& b, const Vec4& c, int32_t owner) {
        if (a.w <= 1e-6f || b.w <= 1e-6f || c.w <= 1e-6f) return;
        const float W = static_cast<float>(buf_.width), H = static_cast<float>(buf_.height);
        auto toScreen = [&](const Vec4& v) {
            return Vec3{(v.x / v.w * 0.5f + 0.5f) * W, (0.5f - v.y / v.w * 0.5f) * H, v.z / v.w};
        };
        Vec3 p0 = toScreen(a), p1 = toScreen(b), p2 = toScreen(c);
        float area = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
        if (std::fabs(area) < 1e-9f) return;
        int x0 = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
        int x1 = std::min(buf_.width - 1, static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
        int y0 = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
        int y1 = std::min(buf_.height - 1, static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
        if (x0 > x1 || y0 > y1) return;
        const float inv = 1.f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                float w0 = ((p1.x - px) * (p2.y - py) - (p1.y - py) * (p2.x - px)) * inv;
                float w1 = ((p2.x - px) * (p0.y - py) - (p2.y - py) * (p0.x - px)) * inv;
                float w2 = 1.f - w0 - w1;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                float z = w0 * p0.z + w1 * p1.z + w2 * p2.z;
                if (z < 0.f || z > 1.f) continue;
                size_t i = static_cast<size_t>(y) * static_cast<size_t>(buf_.width) + static_cast<size_t>(x);
                if (z < buf_.depth[i]) {
                    buf_.depth[i] = z;
                    buf_.owner[i] = owner;
                }
            }
        }
    }

    CoverageBuffer& buf_;
    Mat4 vp_;
};

const MeshData* geometryFor(const DrawItem& d, const Sources& src, std::shared_ptr<const MeshData>& keep) {
    // Skinned draws carry a per-instance key ("<mesh>@skin<entity>"); the pose and the mesh are keyed by the mesh.
    std::string key = d.mesh;
    if (auto at = key.find("@skin"); at != std::string::npos) key = key.substr(0, at);
    if (d.skin >= 0 && src.posed) {
        keep = src.posed(d.entity, key);
        if (keep) return keep.get();
    }
    return src.mesh ? src.mesh(key) : nullptr;
}

/// Index list to rasterize: a coarser LOD for very heavy meshes (coverage needs no detail).
const std::vector<uint32_t>& indicesFor(const MeshData& m) {
    constexpr size_t kBudget = 60000 * 3;
    if (m.indices.size() <= kBudget || m.lods.empty()) return m.indices;
    for (const auto& lod : m.lods) {
        if (lod.size() <= kBudget) return lod;
    }
    return m.lods.back();
}

// ---------------------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------------------

struct Finding {
    std::string severity;  // error | warning | info
    std::string code;
    std::string message;
    std::string hint;
    EntityId entity = kNoEntity;
};

bool nearColor(const Vec4& a, const Vec4& b, float eps = 0.012f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

std::string entityName(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    return r ? r->name : std::string();
}

std::string ref(const Scene& s, EntityId e) { return "\"" + entityName(s, e) + "\" (#" + std::to_string(e) + ")"; }

bool hasCharacterComponents(const Scene& s, EntityId e) {
    return s.get<Animator>(e) || s.get<CharacterController>(e) || s.get<NavAgent>(e);
}

/// Hint: the kit prefab whose name matches the entity best.
std::string suggest(const std::string& name, const std::vector<std::string>& candidates,
                    const std::vector<std::pair<std::string, std::string>>& keywords) {
    if (candidates.empty()) return {};
    std::string n = lower(name);
    for (const auto& [kw, target] : keywords) {
        if (n.find(kw) == std::string::npos) continue;
        for (const auto& c : candidates) {
            if (lower(c).find(target) != std::string::npos) return c;
        }
    }
    for (const auto& c : candidates) {
        std::string stem = lower(fs::path(c).stem().stem().string());
        if (!stem.empty() && (n.find(stem) != std::string::npos || stem.find(n) != std::string::npos)) return c;
    }
    return {};
}

const std::vector<std::pair<std::string, std::string>>& characterKeywords() {
    static const std::vector<std::pair<std::string, std::string>> k{
        {"guard", "guard"},      {"soldier", "guard"},      {"knight", "guard"},     {"watch", "guard"},
        {"merchant", "merchant"}, {"vendor", "merchant"},   {"shop", "merchant"},    {"trader", "merchant"},
        {"child", "child"},      {"kid", "child"},          {"boy", "child"},        {"girl", "child"},
        {"monk", "monk"},        {"warrior", "monk"},       {"operative", "operative"}, {"agent", "operative"},
        {"soldier", "operative"}, {"runner", "runner"},     {"player", "runner"},    {"hero", "runner"},
        {"woman", "villager_female"}, {"female", "villager_female"}, {"wife", "villager_female"},
        {"farmer", "villager_male"}, {"villager", "villager"}, {"npc", "villager"}, {"man", "villager_male"},
        {"person", "villager"},  {"citizen", "villager"},  {"peasant", "villager"}};
    return k;
}

const std::vector<std::pair<std::string, std::string>>& propKeywords() {
    static const std::vector<std::pair<std::string, std::string>> k{
        {"crate", "crate"}, {"box", "crate"},   {"barrel", "barrel"}, {"sack", "sack"},  {"bag", "sack"},
        {"pot", "pot"},     {"vase", "pot"},    {"jar", "pot"},       {"lantern", "lantern"}, {"lamp", "lantern"},
        {"rock", "rock"},   {"stone", "rock"},  {"boulder", "rock"},  {"cliff", "cliff"}, {"tree", "tree"},
        {"trunk", "tree"},  {"bush", "plant"},  {"plant", "plant"},   {"chair", "chair"}, {"stool", "chair"},
        {"table", "table"}, {"bench", "bench"}, {"rug", "rug"},       {"carpet", "rug"},  {"cloth", "fabric"},
        {"log", "log"},     {"plank", "plank"}, {"debris", "debris"}, {"rubble", "debris"}, {"shelf", "shelf"},
        {"fruit", "fruit"}, {"basket", "basket"}, {"cart", "cart"},   {"wheel", "cart"}};
    return k;
}

/// A sphere-ish part sitting on a torso-ish part, sized like a body: a placeholder character.
struct Part {
    EntityId id;
    std::string mesh;
    Aabb box;
    std::string name;
};

std::string bodyArrangement(const std::vector<Part>& parts) {
    if (parts.size() < 2 || parts.size() > 32) return {};
    // Distinct body-part words among the part names ("Visitor 3 Head", "Body", "Left Arm"...).
    static const char* const bodyWords[] = {"head", "torso", "body", "arm", "leg", "eye", "hand", "foot", "feet",
                                            "nose", "chest", "belly", "pupil", "snout", "ear", "neck", "hip"};
    std::set<std::string> words;
    for (const auto& p : parts) {
        std::string n = lower(p.name);
        for (const char* w : bodyWords) {
            if (n.find(w) != std::string::npos) words.insert(w);
        }
    }
    const bool namedHead = words.count("head") > 0;
    Aabb all{Vec3(1e30f), Vec3(-1e30f)};
    for (const auto& p : parts) {
        all.min = vmin(all.min, p.box.min);
        all.max = vmax(all.max, p.box.max);
    }
    const float height = all.max.y - all.min.y;
    const float footprint = std::max(all.max.x - all.min.x, all.max.z - all.min.z);
    if (height < 0.2f || height > 4.f || footprint > height * 2.5f) return {};
    for (const auto& head : parts) {
        if (head.mesh != "sphere" && head.mesh != "cube" && head.mesh != "capsule") continue;
        Vec3 hc = head.box.center(), he = head.box.extents();
        const float headSize = std::max({he.x, he.y, he.z}) * 2.f;
        for (const auto& torso : parts) {
            if (&torso == &head) continue;
            const bool upright = torso.mesh == "capsule" || torso.mesh == "cylinder" || torso.mesh == "cone";
            Vec3 tc = torso.box.center(), te = torso.box.extents();
            const float torsoH = te.y * 2.f, torsoW = std::max(te.x, te.z) * 2.f;
            // A cube or sphere counts as a torso only when the parts are named like a body.
            const bool tallBox = (torso.mesh == "cube" || torso.mesh == "sphere") && torsoH > torsoW * 0.9f && words.size() >= 2;
            if (!upright && !tallBox) continue;
            const bool above = head.box.min.y > tc.y && hc.y > torso.box.max.y - 0.3f * torsoH &&
                               head.box.min.y < torso.box.max.y + 0.6f * headSize;  // resting on (or just above) the torso
            const bool centered = std::hypot(hc.x - tc.x, hc.z - tc.z) < torsoW * 0.6f + 0.05f;
            const bool headSized = headSize > torsoW * 0.3f && headSize < torsoW * 1.8f + 0.05f && headSize < torsoH * 1.5f;
            if (above && centered && headSized && (head.mesh == "sphere" || namedHead)) {
                return "a " + head.mesh + " \"" + head.name + "\" on a " + torso.mesh + " \"" + torso.name + "\"";
            }
        }
    }
    if (words.size() >= 3 || (namedHead && words.size() >= 2)) {
        std::string list;
        for (const auto& w : words) list += (list.empty() ? "" : ", ") + w;
        return "primitive parts named like a body (" + list + ")";
    }
    return {};
}

}  // namespace

bool isPrimitiveMesh(std::string_view mesh) {
    for (const char* p : kPrimitives) {
        if (mesh == p) return true;
    }
    return false;
}

bool isProceduralMesh(std::string_view mesh) {
    for (const char* p : kProcedural) {
        if (mesh == p) return true;
    }
    return false;
}

bool imageSize(const std::string& path, int& width, int& height) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    unsigned char h[32] = {};
    f.read(reinterpret_cast<char*>(h), sizeof(h));
    if (f.gcount() < 24) return false;
    if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') {
        width = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
        height = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
        return width > 0 && height > 0;
    }
    if (h[0] == 0xFF && h[1] == 0xD8) {  // JPEG: walk the markers to a start-of-frame
        f.clear();
        f.seekg(2);
        for (int guard = 0; guard < 512 && f; ++guard) {
            int c = f.get();
            if (c != 0xFF) continue;
            int marker = f.get();
            while (marker == 0xFF) marker = f.get();
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
            unsigned char len[2];
            f.read(reinterpret_cast<char*>(len), 2);
            int segment = (len[0] << 8) | len[1];
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                unsigned char sof[5];
                f.read(reinterpret_cast<char*>(sof), 5);
                height = (sof[1] << 8) | sof[2];
                width = (sof[3] << 8) | sof[4];
                return width > 0 && height > 0;
            }
            if (segment < 2) return false;
            f.seekg(segment - 2, std::ios::cur);
        }
        return false;
    }
    if (h[0] == '#' && h[1] == '?') {  // Radiance HDR: "-Y <h> +X <w>" after the header
        f.clear();
        f.seekg(0);
        std::string line;
        for (int i = 0; i < 64 && std::getline(f, line); ++i) {
            if (line.rfind("-Y ", 0) == 0 || line.rfind("+Y ", 0) == 0) {
                char a[3], b[3];
                return std::sscanf(line.c_str(), "%2s %d %2s %d", a, &height, b, &width) == 4 && width > 0 && height > 0;
            }
        }
    }
    return false;
}

CoverageBuffer rasterize(const FrameData& frame, const Sources& src, int width) {
    CoverageBuffer buf;
    const float aspect = frame.height > 0 ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : 16.f / 9.f;
    buf.width = std::clamp(width, 16, 2048);
    buf.height = std::max(8, static_cast<int>(std::lround(static_cast<float>(buf.width) / aspect)));
    const size_t n = static_cast<size_t>(buf.width) * static_cast<size_t>(buf.height);
    buf.depth.assign(n, 2.f);
    buf.owner.assign(n, CoverageBuffer::kSky);
    buf.drawPixels.assign(frame.draws.size(), 0);
    Raster r(buf, frame.viewProjection());

    for (size_t di = 0; di < frame.draws.size(); ++di) {
        const DrawItem& d = frame.draws[di];
        std::shared_ptr<const MeshData> keep;
        const MeshData* m = geometryFor(d, src, keep);
        if (!m) {
            buf.missing.push_back(di);
            continue;
        }
        const auto& idx = indicesFor(*m);
        const auto& v = m->vertices;
        auto P = [&](uint32_t i) {
            const float* p = &v[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
            return d.model.transformPoint(Vec3{p[0], p[1], p[2]});
        };
        // Skinned draws keep their mesh-space model matrix; posed vertices are in mesh space too.
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            if (idx[t] * MeshData::kFloatsPerVertex >= v.size() || idx[t + 1] * MeshData::kFloatsPerVertex >= v.size() ||
                idx[t + 2] * MeshData::kFloatsPerVertex >= v.size()) {
                continue;
            }
            r.triangle(P(idx[t]), P(idx[t + 1]), P(idx[t + 2]), static_cast<int32_t>(di));
        }
    }
    // Terrains: a coarse grid of the height field (at most 160 x 160 cells).
    for (const TerrainItem& t : frame.terrains) {
        if (!t.data) continue;
        const world::TerrainData& td = *t.data;
        const int res = td.resolution();
        if (res < 2) continue;
        const int step = std::max(1, (res - 1) / 160);
        const float half = td.size() * 0.5f, cell = td.cell();
        auto at = [&](int x, int z) {
            x = std::min(x, res - 1);
            z = std::min(z, res - 1);
            return t.origin + Vec3{-half + static_cast<float>(x) * cell, td.h(x, z), -half + static_cast<float>(z) * cell};
        };
        for (int z = 0; z < res - 1; z += step) {
            for (int x = 0; x < res - 1; x += step) {
                Vec3 a = at(x, z), b = at(x + step, z), c = at(x, z + step), e = at(x + step, z + step);
                r.triangle(a, b, e, CoverageBuffer::kTerrain);
                r.triangle(a, e, c, CoverageBuffer::kTerrain);
            }
        }
    }
    // Water bodies: a flat sheet at the water level (opaque enough to hide what is far below).
    for (const WaterItem& w : frame.water) {
        const float half = w.size > 0.f ? w.size * 0.5f : 4000.f;
        Vec3 c{w.center.x, w.level, w.center.y};
        const int tiles = w.size > 0.f ? 1 : 16;  // endless water: tessellate so near-plane clipping stays precise
        const float step = 2.f * half / static_cast<float>(tiles);
        for (int z = 0; z < tiles; ++z) {
            for (int x = 0; x < tiles; ++x) {
                Vec3 a = c + Vec3{-half + step * static_cast<float>(x), 0, -half + step * static_cast<float>(z)};
                Vec3 b = a + Vec3{step, 0, 0}, e = a + Vec3{step, 0, step}, f = a + Vec3{0, 0, step};
                r.triangle(a, b, e, CoverageBuffer::kWater);
                r.triangle(a, e, f, CoverageBuffer::kWater);
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        int32_t o = buf.owner[i];
        if (o >= 0) {
            ++buf.drawPixels[static_cast<size_t>(o)];
        } else if (o == CoverageBuffer::kSky) {
            ++buf.skyPixels;
        } else if (o == CoverageBuffer::kTerrain) {
            ++buf.terrainPixels;
        } else if (o == CoverageBuffer::kWater) {
            ++buf.waterPixels;
        }
    }
    return buf;
}

Json auditFrame(const Scene& scene, const FrameData& frame, const Sources& src, const Options& opts, const std::string& label) {
    CoverageBuffer buf = rasterize(frame, src, opts.resolution);
    const float px = buf.pixelFraction();
    const float maxPrim = opts.maxPrimitiveCoverage >= 0.f ? opts.maxPrimitiveCoverage : (opts.strict ? 0.001f : 0.02f);
    const float maxDefault =
        opts.maxDefaultMaterialCoverage >= 0.f ? opts.maxDefaultMaterialCoverage : (opts.strict ? 0.001f : 0.02f);
    std::vector<Finding> findings;
    auto add = [&](std::string sev, std::string code, std::string msg, std::string hint, EntityId e = kNoEntity) {
        findings.push_back({std::move(sev), std::move(code), std::move(msg), std::move(hint), e});
    };

    // Coverage per entity (an entity may draw several parts).
    std::unordered_map<EntityId, float> entityCoverage;
    std::vector<float> drawCoverage(frame.draws.size(), 0.f);
    for (size_t i = 0; i < frame.draws.size(); ++i) {
        drawCoverage[i] = static_cast<float>(buf.drawPixels[i]) * px;
        if (frame.draws[i].entity) entityCoverage[frame.draws[i].entity] += drawCoverage[i];
    }

    // --- Primitives -----------------------------------------------------------------------
    Json primitives = Json::array(), procedural = Json::array();
    float primitiveCoverage = 0.f, proceduralCoverage = 0.f;
    std::vector<EntityId> visiblePrimitive;
    std::set<EntityId> seenPrim;
    for (size_t i = 0; i < frame.draws.size(); ++i) {
        const DrawItem& d = frame.draws[i];
        const bool prim = isPrimitiveMesh(d.mesh), proc = isProceduralMesh(d.mesh);
        if (!prim && !proc) continue;
        const float cov = drawCoverage[i];
        if (cov < opts.minCoverage) continue;
        Json item = Json::object({{"id", d.entity},
                                  {"name", entityName(scene, d.entity)},
                                  {"mesh", d.mesh},
                                  {"coverage", round4(cov * 100.f)}});
        if (const EntityRecord* r = scene.record(d.entity); r && r->parent) item["parent"] = entityName(scene, r->parent);
        if (prim) {
            primitiveCoverage += cov;
            if (seenPrim.insert(d.entity).second) visiblePrimitive.push_back(d.entity);
            primitives.push(std::move(item));
        } else {
            proceduralCoverage += cov;
            procedural.push(std::move(item));
        }
    }
    auto byCoverage = [](Json& arr) {
        std::sort(arr.elements().begin(), arr.elements().end(),
                  [](const Json& a, const Json& b) { return a.get("coverage").asFloat() > b.get("coverage").asFloat(); });
    };
    byCoverage(primitives);
    byCoverage(procedural);

    // --- Primitive characters ---------------------------------------------------------------
    Json characters = Json::array();
    std::set<EntityId> characterRoots;
    auto primitivePartsUnder = [&](EntityId root) {
        std::vector<Part> parts;
        std::vector<std::pair<EntityId, int>> stack{{root, 0}};
        while (!stack.empty()) {
            auto [e, depth] = stack.back();
            stack.pop_back();
            if (const MeshRenderer* m = scene.get<MeshRenderer>(e); m && m->visible && isPrimitiveMesh(m->mesh)) {
                Aabb local = scene.localBounds(e);
                parts.push_back({e, m->mesh, local.transformed(scene.worldMatrix(e)), entityName(scene, e)});
            }
            if (depth < 3) {
                for (EntityId c : scene.children(e)) stack.push_back({c, depth + 1});
            }
        }
        return parts;
    };
    auto hasRiggedMesh = [&](EntityId root) {
        std::vector<EntityId> stack{root};
        while (!stack.empty()) {
            EntityId e = stack.back();
            stack.pop_back();
            if (const MeshRenderer* m = scene.get<MeshRenderer>(e); m && m->visible && str::startsWith(m->mesh, "asset:")) {
                const MeshData* md = src.mesh ? src.mesh(m->mesh) : nullptr;
                if (md && md->skinned()) return true;
            }
            for (EntityId c : scene.children(e)) stack.push_back(c);
        }
        return false;
    };
    for (EntityId e : visiblePrimitive) {
        // Walk up: the nearest ancestor (or self) with character components owns the parts.
        EntityId root = kNoEntity;
        std::string reason;
        EntityId cur = e;
        for (int depth = 0; cur && depth < 5; ++depth) {
            if (hasCharacterComponents(scene, cur)) {
                root = cur;
                std::string comps;
                auto note = [&](const char* what) { comps += (comps.empty() ? "" : ", ") + std::string(what); };
                if (scene.get<Animator>(cur)) note("an animator");
                if (scene.get<CharacterController>(cur)) note("a character controller");
                if (scene.get<NavAgent>(cur)) note("a nav agent");
                reason = "has " + comps + " but draws primitive meshes";
                break;
            }
            const EntityRecord* r = scene.record(cur);
            cur = r ? r->parent : kNoEntity;
        }
        if (root && hasRiggedMesh(root)) root = kNoEntity;  // a real character holding a primitive prop
        if (!root) {
            const EntityRecord* r = scene.record(e);
            for (EntityId cand : {r ? r->parent : kNoEntity, e}) {
                if (!cand) continue;
                EntityId grand = scene.record(cand) ? scene.record(cand)->parent : kNoEntity;
                for (EntityId c2 : {cand, grand}) {
                    if (!c2 || characterRoots.count(c2)) continue;
                    std::string arrangement = bodyArrangement(primitivePartsUnder(c2));
                    if (!arrangement.empty()) {
                        root = c2;
                        reason = "built from primitives: " + arrangement;
                        break;
                    }
                }
                if (root) break;
            }
        }
        if (!root || !characterRoots.insert(root).second) continue;
        auto parts = primitivePartsUnder(root);
        float cov = 0.f;
        for (const auto& p : parts) cov += entityCoverage.count(p.id) ? entityCoverage[p.id] : 0.f;
        std::string name = entityName(scene, root);
        std::string kit = suggest(name, src.kitCharacters, characterKeywords());
        std::string hint = kit.empty() ? (src.kitCharacters.empty()
                                              ? "replace it with a rigged, skinned character (asset_import a glTF with a skin, or the "
                                                "showcase kit characters: mount the kit in game.json and use kit/characters/*.prefab.json)"
                                              : "replace it with a kit character, e.g. " + src.kitCharacters.front())
                                       : "replace it with the kit character " + kit + " (prefab_instantiate, keep the behavior and vars)";
        Json item = Json::object({{"id", root}, {"name", name}, {"reason", reason}, {"parts", parts.size()},
                                  {"coverage", round4(cov * 100.f)}, {"hint", hint}});
        if (!kit.empty()) item["suggestion"] = kit;
        characters.push(item);
        add("error", "primitive_character", "character " + ref(scene, root) + " " + reason, hint, root);
    }

    // Primitive props summary + per-entity warnings (largest first, at most 12 lines).
    if (primitiveCoverage > maxPrim) {
        add("error", "primitive_coverage",
            "builtin primitive meshes cover " + pct(primitiveCoverage) + " of the image (limit " + pct(maxPrim) + ")",
            "replace the listed primitives with modeled assets (kit props, Poly Haven models, dcc_generate recipes) or hide them");
    }
    {
        size_t shown = 0;
        for (const auto& p : primitives.elements()) {
            if (shown >= 12) break;
            EntityId id = static_cast<EntityId>(p.get("id").asInt());
            bool partOfCharacter = false;
            for (EntityId root : characterRoots) {
                for (EntityId cur = id; cur; cur = scene.record(cur) ? scene.record(cur)->parent : kNoEntity) {
                    if (cur == root) partOfCharacter = true;
                }
            }
            if (partOfCharacter) continue;
            ++shown;
            std::string name = p.get("name").asString();
            std::string kit = suggest(name, src.kitProps, propKeywords());
            std::string hint = kit.empty() ? "replace the " + p.get("mesh").asString() +
                                                 " with a modeled mesh (a kit prop, a Poly Haven model or dcc_generate), "
                                                 "or remove it if it is a placeholder"
                                           : "replace it with the kit prop " + kit;
            add(opts.strict || primitiveCoverage > maxPrim ? "warning" : "info", "primitive_mesh",
                p.get("mesh").asString() + " " + ref(scene, id) + " covers " + pct(p.get("coverage").asFloat() / 100.f) +
                    " of the image",
                hint, id);
        }
    }
    if (proceduralCoverage > 0.01f) {
        add("warning", "procedural_meshes",
            "builtin procedural meshes (grass, rock, fern...) cover " + pct(proceduralCoverage),
            "use foliage layers with scanned models (Poly Haven plants and rocks) for hero shots");
    }

    // --- Meshes that failed to load (on screen by their bounds) ------------------------------
    {
        const Mat4 vp = frame.viewProjection();
        std::map<std::string, std::pair<size_t, float>> byFile;  // mesh file -> (draws, bounds coverage)
        for (size_t di : buf.missing) {
            const DrawItem& d = frame.draws[di];
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            int front = 0;
            for (int c = 0; c < 8; ++c) {
                const Aabb& b = d.worldBounds;
                Vec4 p = vp * Vec4(Vec3{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z}, 1.f);
                if (p.w <= 1e-5f) continue;
                ++front;
                x0 = std::min(x0, p.x / p.w), x1 = std::max(x1, p.x / p.w);
                y0 = std::min(y0, p.y / p.w), y1 = std::max(y1, p.y / p.w);
            }
            if (!front) continue;
            const float w = std::clamp(x1, -1.f, 1.f) - std::clamp(x0, -1.f, 1.f), h = std::clamp(y1, -1.f, 1.f) - std::clamp(y0, -1.f, 1.f);
            if (w <= 0.f || h <= 0.f) continue;
            std::string file = d.mesh;
            if (auto hash = file.find('#'); hash != std::string::npos) file = file.substr(0, hash);
            auto& slot = byFile[file];
            ++slot.first;
            slot.second += w * h / 4.f;
        }
        size_t shown = 0;
        for (const auto& [file, info] : byFile) {
            if (shown++ >= 10) break;
            add("error", "missing_mesh",
                file + " could not be loaded (" + std::to_string(info.first) + " draw(s) on screen, ~" + pct(std::min(1.f, info.second)) +
                    " of the image by bounds)",
                "the file is missing or broken: run the project's build / asset fetch (kit_fetch.py, the showcase build script) or re-import it");
        }
    }

    // --- Materials ------------------------------------------------------------------------
    Json materials = Json::array();
    float defaultCoverage = 0.f;
    const Vec4 kDefaultColor{0.8f, 0.8f, 0.82f, 1.f};
    std::set<std::string> missingFiles;
    auto exists = [&](const std::string& p) {
        if (p.empty()) return true;
        std::error_code ec;
        return fs::exists(p, ec);
    };
    std::map<EntityId, std::pair<std::string, float>> materialIssues;  // entity -> (issue, coverage)
    for (size_t i = 0; i < frame.draws.size(); ++i) {
        const DrawItem& d = frame.draws[i];
        const float cov = drawCoverage[i];
        if (cov < opts.minCoverage) continue;
        const Surface& s = d.surface;
        for (const std::string* tex : {&s.texture, &s.normalMap, &s.ormMap, &s.emissiveMap}) {
            if (!tex->empty() && !exists(*tex) && missingFiles.insert(*tex).second) {
                add("error", "missing_texture", "texture file not found: " + *tex + " (used by " + ref(scene, d.entity) + ")",
                    "fetch the project's downloads (kit_fetch.py / the showcase build) or fix the material path", d.entity);
            }
        }
        if (const MeshRenderer* m = scene.get<MeshRenderer>(d.entity); m && !m->material.empty() && src.resolvePath) {
            std::string abs = src.resolvePath(m->material);
            if (!exists(abs) && missingFiles.insert(abs).second) {
                add("error", "missing_material", "material not found: " + m->material + " (" + ref(scene, d.entity) + ")",
                    "create it with material_create or fix mesh.material", d.entity);
            }
        }
        if (s.shading == Shading::Unlit || s.shading == Shading::Water) continue;
        if (!s.texture.empty() || s.color.w < 0.99f) continue;
        const float glow = std::max({s.emissive.x, s.emissive.y, s.emissive.z}) * s.emissive.w;
        if (glow > 0.5f) continue;  // lamps, screens, neon: emissive surfaces need no texture
        const MeshRenderer* m = scene.get<MeshRenderer>(d.entity);
        const bool isDefault = nearColor(s.color, kDefaultColor) && (!m || m->material.empty()) && s.normalMap.empty();
        auto& slot = materialIssues[d.entity];
        slot.first = isDefault ? "default" : (slot.first == "default" ? "default" : "untextured");
        slot.second += cov;
        if (isDefault) defaultCoverage += cov;
    }
    {
        std::vector<std::pair<EntityId, std::pair<std::string, float>>> sorted(materialIssues.begin(), materialIssues.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        size_t shown = 0;
        for (const auto& [id, issue] : sorted) {
            const MeshRenderer* m = scene.get<MeshRenderer>(id);
            Json item = Json::object({{"id", id}, {"name", entityName(scene, id)}, {"issue", issue.first},
                                      {"mesh", m ? m->mesh : std::string()}, {"coverage", round4(issue.second * 100.f)}});
            if (m && !m->material.empty()) item["material"] = m->material;
            materials.push(item);
            if (shown++ >= 12 || issue.second < 0.0005f) continue;
            if (issue.first == "default") {
                add(defaultCoverage > maxDefault ? "error" : "warning", "default_material",
                    ref(scene, id) + " uses the default grey material", "assign a PBR material (kit/materials/*.mat.json, polyhaven texture sets) with material_assign", id);
            } else {
                add(opts.stylized ? "info" : "warning", "untextured_material",
                    ref(scene, id) + " has a flat color without textures (" + pct(issue.second) + " of the image)",
                    "give it a texture set (albedo + normal + ORM) or a kit material; flat colors only suit a deliberate stylized look", id);
            }
        }
    }

    // --- Geometry: LODs and texel density ---------------------------------------------------
    Json lods = Json::array(), texel = Json::array();
    std::set<std::string> lodChecked;
    for (size_t i = 0; i < frame.draws.size(); ++i) {
        const DrawItem& d = frame.draws[i];
        if (drawCoverage[i] < opts.minCoverage) continue;
        if (!str::startsWith(d.mesh, "asset:") || d.skin >= 0) continue;
        const MeshData* m = src.mesh ? src.mesh(d.mesh) : nullptr;
        if (!m) continue;
        const size_t tris = m->indices.size() / 3;
        if (tris >= 20000 && m->lods.empty() && lodChecked.insert(d.mesh).second) {
            lods.push(Json::object({{"mesh", d.mesh}, {"triangles", tris}, {"id", d.entity}}));
            add("warning", "missing_lods", d.mesh + " has " + std::to_string(tris) + " triangles and no LOD chain",
                "re-import it (LODs are built for meshes of 3,000+ triangles) or decimate it with dcc_edit_asset", d.entity);
        }
        // Texel density: base-color texels per meter on this draw.
        if (d.surface.texture.empty() || drawCoverage[i] < 0.01f) continue;
        int tw = 0, th = 0;
        if (!imageSize(d.surface.texture, tw, th)) continue;
        float density = 0.f;
        if (d.surface.triplanar) {
            density = static_cast<float>(tw) * std::max(d.surface.tiling.x, 1e-3f) / 2.f;
        } else {
            double uvArea = 0, worldArea = 0;
            const auto& v = m->vertices;
            const auto& idx = indicesFor(*m);
            const size_t stride = std::max<size_t>(1, idx.size() / 3 / 4000) * 3;  // sample up to ~4000 triangles
            for (size_t t = 0; t + 2 < idx.size(); t += stride) {
                const float* a = &v[static_cast<size_t>(idx[t]) * MeshData::kFloatsPerVertex];
                const float* b = &v[static_cast<size_t>(idx[t + 1]) * MeshData::kFloatsPerVertex];
                const float* c = &v[static_cast<size_t>(idx[t + 2]) * MeshData::kFloatsPerVertex];
                Vec3 pa = d.model.transformPoint({a[0], a[1], a[2]}), pb = d.model.transformPoint({b[0], b[1], b[2]}),
                     pc = d.model.transformPoint({c[0], c[1], c[2]});
                worldArea += 0.5 * length(cross(pb - pa, pc - pa));
                uvArea += 0.5 * std::fabs((b[6] - a[6]) * (c[7] - a[7]) - (c[6] - a[6]) * (b[7] - a[7]));
            }
            if (worldArea <= 1e-9 || uvArea <= 1e-12) continue;
            density = static_cast<float>(std::sqrt(static_cast<double>(tw) * th * uvArea / worldArea)) *
                      std::max(d.surface.tiling.x, 1e-3f);
        }
        if (density < 64.f || density > 16384.f) {
            texel.push(Json::object({{"id", d.entity}, {"name", entityName(scene, d.entity)}, {"texelsPerMeter", std::round(density)},
                                     {"texture", fs::path(d.surface.texture).filename().string()},
                                     {"coverage", round4(drawCoverage[i] * 100.f)}}));
            if (density < 64.f) {
                add("warning", "low_texel_density",
                    ref(scene, d.entity) + " shows " + std::to_string(static_cast<int>(density)) + " texels/m (blurry up close; target ~512)",
                    "raise the material tiling, use triplanar mapping, or a higher-resolution texture set", d.entity);
            }
        }
    }

    // --- Environment and lights ------------------------------------------------------------
    Json env = Json::object();
    const Environment& e = frame.environment;
    const float skyFraction = static_cast<float>(buf.skyPixels) * px;
    env["skyMode"] = e.skyMode;
    env["skyCoverage"] = round4(skyFraction * 100.f);
    const Environment defaults{};
    if (e.skyMode == "gradient" && nearColor(e.skyTop, defaults.skyTop) && nearColor(e.skyHorizon, defaults.skyHorizon) &&
        skyFraction > 0.02f) {
        add("warning", "default_sky", "the sky is the default gradient (" + pct(skyFraction) + " of the image)",
            "environment_update {skyMode: \"atmosphere\"} or an HDRI (skyMode \"hdri\") with matching sun; add clouds and fog for depth");
    }
    if (e.skyMode == "hdri") {
        std::string abs = src.resolvePath ? src.resolvePath(e.hdri) : e.hdri;
        if (e.hdri.empty() || !exists(abs)) {
            add("error", "missing_hdri", "skyMode is hdri but the panorama is missing: '" + e.hdri + "'",
                "download it (polyhaven HDRIs, kit_fetch.py) or switch skyMode to atmosphere");
        }
    }
    size_t localLights = 0;
    for (const LightItem& l : frame.lights) {
        if (l.kind != LightItem::Kind::Directional) ++localLights;
    }
    if (e.sunIntensity <= 0.01f && localLights == 0 && e.ambient < 0.05f) {
        add("warning", "no_lighting", "no sun, no lights and almost no ambient light", "add a light motivation: sun, lamps or emissive surfaces");
    }
    env["lights"] = static_cast<int64_t>(localLights);

    // Hero meshes: big on screen, or skinned characters.
    std::vector<size_t> heroes;
    for (size_t i = 0; i < frame.draws.size(); ++i) {
        if (drawCoverage[i] >= 0.03f || (frame.draws[i].skin >= 0 && drawCoverage[i] >= 0.004f)) heroes.push_back(i);
    }
    Json lightIssues = Json::array();
    for (const LightItem& l : frame.lights) {
        if (l.kind == LightItem::Kind::Directional || l.shadows || l.intensity <= 0.f || l.negative) continue;
        const EntityId le = static_cast<EntityId>(l.id & 0x00FFFFFFFFFFFFFFull);
        if (!le || !scene.exists(le) || (l.id >> 56) != 0) continue;  // only light components (not fire / particle lights)
        size_t lit = 0;
        for (size_t h : heroes) {
            const Aabb& b = frame.draws[h].worldBounds;
            Vec3 closest = vmax(b.min, vmin(l.position, b.max));
            if (distance(closest, l.position) < l.range * 0.6f) ++lit;
        }
        if (!lit) continue;
        lightIssues.push(Json::object({{"id", le}, {"name", entityName(scene, le)}, {"heroMeshes", lit}}));
        add("warning", "light_without_shadows",
            "light " + ref(scene, le) + " lights " + std::to_string(lit) + " hero mesh(es) without casting shadows",
            "set light.castShadows true (docs/RENDERING.md, point and spot light shadows), or lower its intensity", le);
    }
    for (size_t h : heroes) {
        const DrawItem& d = frame.draws[h];
        if (d.skin >= 0 && !d.castShadows) {
            add("warning", "character_no_shadow", ref(scene, d.entity) + " is an animated character that casts no shadow",
                "set mesh.castShadows true so it is grounded", d.entity);
        }
    }

    // --- Verdict -----------------------------------------------------------------------
    bool pass = true;
    Json warnings = Json::array();
    for (const auto& f : findings) {
        if (f.severity == "error") pass = false;
        Json w = Json::object({{"severity", f.severity}, {"code", f.code}, {"message", f.message}});
        if (!f.hint.empty()) w["hint"] = f.hint;
        if (f.entity) w["entity"] = f.entity;
        warnings.push(std::move(w));
    }
    std::stable_sort(warnings.elements().begin(), warnings.elements().end(), [](const Json& a, const Json& b) {
        auto rank = [](const std::string& s) { return s == "error" ? 0 : s == "warning" ? 1 : 2; };
        return rank(a.get("severity").asString()) < rank(b.get("severity").asString());
    });
    size_t visibleDraws = 0;
    for (float c : drawCoverage) visibleDraws += c > 0.f ? 1 : 0;
    Json out = Json::object();
    if (!label.empty()) out["view"] = label;
    out["pass"] = pass;
    out["camera"] = Json::object({{"eye", Json::array({round2(frame.camera.eye.x), round2(frame.camera.eye.y), round2(frame.camera.eye.z)})},
                                  {"target", Json::array({round2(frame.camera.target.x), round2(frame.camera.target.y),
                                                          round2(frame.camera.target.z)})},
                                  {"fov", round2(frame.camera.fovDeg)}});
    out["primitiveCoverage"] = round4(primitiveCoverage * 100.f);
    out["primitiveLimit"] = round4(maxPrim * 100.f);
    out["defaultMaterialCoverage"] = round4(defaultCoverage * 100.f);
    // Long lists keep the largest entries (token budget); the totals stay exact.
    auto capped = [&](const char* key, Json arr, size_t limit) {
        const size_t total = arr.size();
        if (total > limit) {
            arr.elements().resize(limit);
            out[std::string(key) + "Total"] = total;
        }
        out[key] = std::move(arr);
    };
    out["primitiveCount"] = primitives.size();
    capped("primitives", primitives, 40);
    if (procedural.size()) capped("proceduralMeshes", procedural, 20);
    out["primitiveCharacters"] = characters;
    capped("materials", materials, 40);
    if (lods.size()) out["lods"] = lods;
    if (texel.size()) out["texelDensity"] = texel;
    if (lightIssues.size()) out["lightsWithoutShadows"] = lightIssues;
    out["environment"] = env;
    out["stats"] = Json::object({{"draws", frame.draws.size()},
                                 {"visibleDraws", visibleDraws},
                                 {"rasterTriangles", buf.triangles},
                                 {"raster", Json::array({buf.width, buf.height})},
                                 {"terrainCoverage", round4(static_cast<float>(buf.terrainPixels) * px * 100.f)},
                                 {"waterCoverage", round4(static_cast<float>(buf.waterPixels) * px * 100.f)}});
    out["warnings"] = warnings;
    return out;
}

}  // namespace sky::audit
