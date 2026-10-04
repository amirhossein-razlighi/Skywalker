#include "skywalker/fx/GpuParticles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/fx/Particles.h"
#include "skywalker/scene/Scene.h"

namespace sky::fx {

namespace {

constexpr float kPi = 3.14159265358979f;

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
Vec4 linearColor(Vec4 c) { return {toLinear(c.x), toLinear(c.y), toLinear(c.z), c.w}; }

void copyMat(float (&dst)[16], const Mat4& m) { std::memcpy(dst, m.m, sizeof(dst)); }
void set4(float (&dst)[4], float x, float y, float z, float w) {
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

/// Splits "a@t b@t" into (value, t or NaN) tokens.
Result<std::vector<std::pair<std::string, float>>> tokens(std::string_view text) {
    std::vector<std::pair<std::string, float>> out;
    std::string s(text);
    for (char& c : s) {
        if (c == ',' || c == ';' || c == '\n' || c == '\t') c = ' ';
    }
    for (const std::string& raw : str::split(s, ' ')) {
        std::string tok = str::trim(raw);
        if (tok.empty()) continue;
        auto at = tok.find('@');
        float t = std::nanf("");
        std::string value = tok;
        if (at != std::string::npos) {
            value = tok.substr(0, at);
            double d;
            if (!str::parseDouble(tok.substr(at + 1), d)) {
                return Error::make("invalid_curve", "bad position in '" + tok + "'", "write stops as value@t, t in 0..1");
            }
            t = static_cast<float>(d);
        }
        out.emplace_back(value, t);
    }
    if (out.empty()) return Error::make("invalid_curve", "empty curve");
    // Stops without a position are spread evenly.
    for (size_t i = 0; i < out.size(); ++i) {
        if (std::isnan(out[i].second)) {
            out[i].second = out.size() == 1 ? 0.f : static_cast<float>(i) / static_cast<float>(out.size() - 1);
        }
        out[i].second = std::clamp(out[i].second, 0.f, 1.f);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

template <typename T>
T sampleStops(const std::vector<std::pair<float, T>>& stops, float t, T fallback) {
    if (stops.empty()) return fallback;
    if (t <= stops.front().first) return stops.front().second;
    if (t >= stops.back().first) return stops.back().second;
    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].first) {
            float span = stops[i].first - stops[i - 1].first;
            float f = span > 1e-6f ? (t - stops[i - 1].first) / span : 1.f;
            return stops[i - 1].second * (1.f - f) + stops[i].second * f;
        }
    }
    return stops.back().second;
}

}  // namespace

Result<std::vector<std::pair<float, Vec4>>> parseGradient(std::string_view text) {
    auto tk = tokens(text);
    if (!tk) return tk.error();
    std::vector<std::pair<float, Vec4>> out;
    for (const auto& [value, t] : tk.value()) {
        Vec4 c;
        if (!reflect::parseHexColor(value, c)) {
            return Error::make("invalid_gradient", "bad color '" + value + "'", "use #rgb, #rrggbb or #rrggbbaa stops");
        }
        out.emplace_back(t, linearColor(c));
    }
    return out;
}

Result<std::vector<std::pair<float, float>>> parseCurve(std::string_view text) {
    auto tk = tokens(text);
    if (!tk) return tk.error();
    std::vector<std::pair<float, float>> out;
    for (const auto& [value, t] : tk.value()) {
        double d;
        if (!str::parseDouble(value, d)) return Error::make("invalid_curve", "bad value '" + value + "'");
        out.emplace_back(t, static_cast<float>(d));
    }
    return out;
}

GpuCurves bakeCurves(const ParticleEmitter& em, std::string* warning) {
    GpuCurves c;
    std::vector<std::pair<float, Vec4>> grad;
    if (!em.colorGradient.empty()) {
        auto g = parseGradient(em.colorGradient);
        if (g) {
            grad = std::move(g.value());
        } else if (warning) {
            *warning = "colorGradient: " + g.error().message;
        }
    }
    if (grad.empty()) grad = {{0.f, linearColor(em.colorStart)}, {1.f, linearColor(em.colorEnd)}};
    std::vector<std::pair<float, float>> size, opacity;
    if (!em.sizeCurve.empty()) {
        auto s = parseCurve(em.sizeCurve);
        if (s) size = std::move(s.value());
        else if (warning) *warning = "sizeCurve: " + s.error().message;
    }
    if (!em.opacityCurve.empty()) {
        auto o = parseCurve(em.opacityCurve);
        if (o) opacity = std::move(o.value());
        else if (warning) *warning = "opacityCurve: " + o.error().message;
    }
    for (int i = 0; i < GpuCurves::kSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(GpuCurves::kSamples - 1);
        Vec4 col = sampleStops(grad, t, Vec4{1, 1, 1, 1});
        col.w *= sampleStops(opacity, t, 1.f);
        c.color[static_cast<size_t>(i)] = col;
        float base = em.sizeStart + (em.sizeEnd - em.sizeStart) * t;
        c.size[static_cast<size_t>(i)] = base * sampleStops(size, t, 1.f);
    }
    return c;
}

Result<VectorField> parseFga(std::string_view text) {
    std::vector<double> nums;
    std::string tok;
    auto flush = [&]() -> bool {
        std::string t = str::trim(tok);
        tok.clear();
        if (t.empty()) return true;
        double d;
        if (!str::parseDouble(t, d)) return false;
        nums.push_back(d);
        return true;
    };
    for (char ch : text) {
        if (ch == ',' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
            if (!flush()) return Error::make("invalid_fga", "non-numeric value in vector field");
        } else {
            tok.push_back(ch);
        }
    }
    if (!flush()) return Error::make("invalid_fga", "non-numeric value in vector field");
    if (nums.size() < 9) return Error::make("invalid_fga", "vector field header is incomplete");
    VectorField f;
    f.nx = static_cast<int>(nums[0]);
    f.ny = static_cast<int>(nums[1]);
    f.nz = static_cast<int>(nums[2]);
    if (f.nx < 1 || f.ny < 1 || f.nz < 1 || f.nx > 512 || f.ny > 512 || f.nz > 512) {
        return Error::make("invalid_fga", "vector field dimensions must be 1..512");
    }
    f.boundsMin = {static_cast<float>(nums[3]), static_cast<float>(nums[4]), static_cast<float>(nums[5])};
    f.boundsMax = {static_cast<float>(nums[6]), static_cast<float>(nums[7]), static_cast<float>(nums[8])};
    const size_t cells = static_cast<size_t>(f.nx) * f.ny * f.nz;
    if (nums.size() < 9 + cells * 3) {
        return Error::make("invalid_fga", "vector field has " + std::to_string((nums.size() - 9) / 3) + " vectors, expected " +
                                              std::to_string(cells));
    }
    f.data.resize(cells * 4);
    for (size_t i = 0; i < cells; ++i) {
        f.data[i * 4 + 0] = static_cast<float>(nums[9 + i * 3]);
        f.data[i * 4 + 1] = static_cast<float>(nums[9 + i * 3 + 1]);
        f.data[i * 4 + 2] = static_cast<float>(nums[9 + i * 3 + 2]);
        f.data[i * 4 + 3] = 0.f;
    }
    return f;
}

Result<VectorField> loadFga(const std::string& path) {
    std::ifstream in(path);
    if (!in) return Error::make("io_error", "cannot read " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parseFga(ss.str());
}

MeshSurface buildMeshSurface(const MeshData& m) {
    MeshSurface s;
    const size_t tris = m.indices.size() / 3;
    double acc = 0;
    auto pos = [&](uint32_t i) {
        const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
        return Vec3{v[0], v[1], v[2]};
    };
    auto nrm = [&](uint32_t i) {
        const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
        return Vec3{v[3], v[4], v[5]};
    };
    for (size_t t = 0; t < tris; ++t) {
        uint32_t i[3] = {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]};
        Vec3 a = pos(i[0]), b = pos(i[1]), c = pos(i[2]);
        float area = 0.5f * length(cross(b - a, c - a));
        if (!(area > 0.f)) continue;
        acc += area;
        for (uint32_t k : i) {
            s.positions.push_back(pos(k));
            s.normals.push_back(nrm(k));
        }
        s.cdf.push_back(static_cast<float>(acc));
    }
    for (float& c : s.cdf) c = static_cast<float>(c / acc);
    if (!s.cdf.empty()) s.cdf.back() = 1.f;
    s.area = static_cast<float>(acc);
    return s;
}

bool builtinParticleMesh(const std::string& key, MeshData& m) {
    m = MeshData{};
    if (key == "fx:leaf") {
        // A curled leaf: elliptical blade along +Z with a midrib fold and a tip curl.
        // Vertex color darkens toward the stem (the particle color multiplies it).
        const int rows = 9, cols = 5;
        for (int r = 0; r <= rows; ++r) {
            float v = static_cast<float>(r) / rows;                 // 0 stem .. 1 tip
            float halfW = 0.5f * std::sin(kPi * std::pow(v, 0.8f)) * 0.55f + 0.004f;
            for (int c = 0; c <= cols; ++c) {
                float u = static_cast<float>(c) / cols * 2.f - 1.f;  // -1 .. 1 across
                float x = u * halfW;
                float z = v - 0.5f;
                float y = std::fabs(u) * halfW * 0.35f + 0.12f * v * v;  // midrib fold + curl
                Vec3 p{x, y, z};
                Vec3 n = normalize(Vec3{-u * 0.35f, 1.f, -0.24f * v});
                float shade = 0.75f + 0.25f * (1.f - std::fabs(u)) * v;
                m.addVertex(p, n, {u * 0.5f + 0.5f, v}, {shade, shade, shade * 0.95f, 1.f});
            }
        }
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                uint32_t a = static_cast<uint32_t>(r * (cols + 1) + c), b = a + 1, d = a + cols + 1, e = d + 1;
                m.indices.insert(m.indices.end(), {a, d, b, b, d, e});
            }
        }
    } else if (key == "fx:shard") {
        // Thin, sharp triangular fragment (glass, metal, debris).
        const Vec3 p[6] = {{0, 0.02f, 0.5f}, {-0.3f, 0.02f, -0.4f}, {0.35f, 0.02f, -0.3f},
                           {0, -0.02f, 0.5f}, {-0.3f, -0.02f, -0.4f}, {0.35f, -0.02f, -0.3f}};
        auto tri = [&](int a, int b, int c) {
            Vec3 n = normalize(cross(p[b] - p[a], p[c] - p[a]));
            for (int i : {a, b, c}) m.addVertex(p[i], n, {0.5f, 0.5f});
            uint32_t base = static_cast<uint32_t>(m.vertexCount() - 3);
            m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
        };
        tri(0, 2, 1);
        tri(3, 4, 5);
        tri(0, 1, 4), tri(0, 4, 3), tri(1, 2, 5), tri(1, 5, 4), tri(2, 0, 3), tri(2, 3, 5);
    } else if (key == "fx:pebble") {
        // Low-poly irregular rock (a squashed, jittered icosphere-like ball).
        const int seg = 8, rings = 5;
        for (int r = 0; r <= rings; ++r) {
            float th = kPi * static_cast<float>(r) / rings;
            for (int s = 0; s <= seg; ++s) {
                float ph = 2.f * kPi * static_cast<float>(s % seg) / seg;
                float jitter = 0.85f + 0.3f * std::fabs(std::sin(static_cast<float>(r * 7 + (s % seg) * 13) * 1.7f));
                if (r == 0 || r == rings) jitter = 1.f;
                Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
                Vec3 p{n.x * 0.5f * jitter, n.y * 0.32f * jitter, n.z * 0.45f * jitter};
                m.addVertex(p, n, {static_cast<float>(s) / seg, static_cast<float>(r) / rings});
            }
        }
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < seg; ++s) {
                uint32_t a = static_cast<uint32_t>(r * (seg + 1) + s), b = a + 1, c = a + seg + 1, d = c + 1;
                m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
            }
        }
    } else {
        return false;
    }
    m.computeBounds();
    return true;
}

int gpuShapeId(const std::string& shape) {
    if (shape == "sphere") return static_cast<int>(GpuShape::Sphere);
    if (shape == "box") return static_cast<int>(GpuShape::Box);
    if (shape == "disc") return static_cast<int>(GpuShape::Disc);
    if (shape == "cone") return static_cast<int>(GpuShape::Cone);
    if (shape == "mesh") return static_cast<int>(GpuShape::Mesh);
    return static_cast<int>(GpuShape::Point);
}

int gpuFacingId(const std::string& facing) {
    if (facing == "velocity") return static_cast<int>(GpuFacing::Velocity);
    if (facing == "horizontal") return static_cast<int>(GpuFacing::Horizontal);
    if (facing == "ribbon") return static_cast<int>(GpuFacing::Ribbon);
    if (facing == "mesh") return static_cast<int>(GpuFacing::Mesh);
    return static_cast<int>(GpuFacing::Camera);
}

int gpuLookId(const std::string& look) {
    if (look == "flame") return static_cast<int>(ParticleLook::Flame);
    if (look == "smoke") return static_cast<int>(ParticleLook::Smoke);
    if (look == "spark") return static_cast<int>(ParticleLook::Spark);
    if (look == "rain") return static_cast<int>(ParticleLook::Rain);
    if (look == "snow") return static_cast<int>(ParticleLook::Snow);
    if (look == "mist") return static_cast<int>(ParticleLook::Mist);
    if (look == "sprite") return kGpuLookSprite;
    return static_cast<int>(ParticleLook::Glow);
}

GpuEmitterParams packEmitter(const GpuEmitterItem& item) {
    const ParticleEmitter& em = item.params;
    GpuEmitterParams p{};
    copyMat(p.world, item.world);
    copyMat(p.delta, Mat4{});
    copyMat(p.fieldInv, Mat4{});
    float spread = std::clamp(em.spread, 0.f, 180.f);
    set4(p.spawn, static_cast<float>(gpuShapeId(em.shape)), em.speed, em.speedJitter, std::cos(radians(spread)));
    set4(p.shapeSize, em.shapeSize.x * 0.5f, em.shapeSize.y * 0.5f, em.shapeSize.z * 0.5f, std::max(em.lifetime, 0.01f));
    Vec3 dir = item.world.transformDir(length(em.direction) > 1e-6f ? normalize(em.direction) : Vec3{0, 1, 0});
    dir = length(dir) > 1e-6f ? normalize(dir) : Vec3{0, 1, 0};
    set4(p.direction, dir.x, dir.y, dir.z, std::clamp(em.lifetimeJitter, 0.f, 1.f));
    set4(p.forces, em.gravity, std::max(em.drag, 0.f), em.turbulence, 1.f / std::max(em.turbulenceScale, 0.01f));
    set4(p.wind, item.wind.x * em.wind, item.wind.y * em.wind, item.wind.z * em.wind, em.worldSpace ? 1.f : 0.f);
    set4(p.size, em.sizeStart, em.sizeEnd, std::clamp(em.sizeJitter, 0.f, 1.f), em.stretch);
    int look = gpuLookId(em.look);
    bool emissive = look == static_cast<int>(ParticleLook::Glow) || look == static_cast<int>(ParticleLook::Flame) ||
                    look == static_cast<int>(ParticleLook::Spark);
    set4(p.look, static_cast<float>(look), static_cast<float>(gpuFacingId(em.facing)), emissive || look == kGpuLookSprite ? em.intensity : 1.f,
         std::max(em.softness, 0.f));
    set4(p.floorPlane, em.collide ? 1.f : 0.f, em.floorHeight, std::clamp(em.bounce, 0.f, 1.f), std::clamp(em.friction, 0.f, 1.f));
    float subMask = em.subEmitOn == "collision" ? 2.f : (em.subEmitOn == "both" ? 3.f : 1.f);
    size_t nColl = std::min<size_t>(item.colliders.size(), kGpuMaxColliders);
    set4(p.collision, em.depthCollision ? 1.f : 0.f, em.stick ? 1.f : 0.f, static_cast<float>(nColl), subMask);
    for (size_t i = 0; i < nColl; ++i) {
        const FxCollider& c = item.colliders[i];
        float* d = p.colliders[i];
        d[0] = c.a.x, d[1] = c.a.y, d[2] = c.a.z, d[3] = static_cast<float>(c.kind);
        d[4] = c.b.x, d[5] = c.b.y, d[6] = c.b.z, d[7] = c.radius;
    }
    float fieldType = em.field == "vortex" ? 1.f : em.field == "attractor" ? 2.f : (em.field == "texture" && item.field) ? 3.f : 0.f;
    set4(p.field, fieldType, em.fieldStrength, std::max(em.fieldRadius, 0.01f), em.fieldPull);
    Vec3 fc = item.world.transformPoint(em.fieldCenter);
    set4(p.fieldCenter, fc.x, fc.y, fc.z, em.fieldLift);
    Vec3 fa = item.world.transformDir(length(em.fieldAxis) > 1e-6f ? em.fieldAxis : Vec3{0, 1, 0});
    fa = length(fa) > 1e-6f ? normalize(fa) : Vec3{0, 1, 0};
    set4(p.fieldAxis, fa.x, fa.y, fa.z, 0.f);
    if (fieldType == 3.f) {
        // world -> local -> field box [0,1]^3 (centered on fieldCenter, fieldSize meters)
        Vec3 sz{std::max(em.fieldSize.x, 1e-3f), std::max(em.fieldSize.y, 1e-3f), std::max(em.fieldSize.z, 1e-3f)};
        Mat4 boxToLocal = Mat4::translate(em.fieldCenter - sz * 0.5f) * Mat4::scale(sz);
        copyMat(p.fieldInv, (item.world * boxToLocal).inverse());
    }
    int cols = std::max(em.flipbookColumns, 1), rows = std::max(em.flipbookRows, 1);
    set4(p.flipbook, static_cast<float>(cols), static_cast<float>(rows), std::max(em.flipbookFps, 0.f), static_cast<float>(cols * rows));
    set4(p.material, std::clamp(em.roughness, 0.02f, 1.f), std::clamp(em.metallic, 0.f, 1.f), radians(em.spin),
         item.texture.empty() ? 0.f : 1.f);
    set4(p.sub, static_cast<float>(std::clamp(em.subEmitCount, 1, 1024)), em.subEmitInherit, item.subEmitter ? 1.f : 0.f,
         static_cast<float>(static_cast<uint32_t>(em.seed) % 65536u) + static_cast<float>(item.entity % 1024u) * 0.618f);
    int segs = std::clamp(em.trailSegments, 2, 32);
    set4(p.trail, static_cast<float>(segs), std::max(em.trailLength, 0.01f) / static_cast<float>(segs - 1), 0.f, 0.f);
    set4(p.frame, 0.f, 0.f, 0.f, 0.f);
    Vec4 lc = linearColor(em.lightColor);
    set4(p.light, lc.x, lc.y, lc.z, std::max(em.light, 0.f));
    const bool thin = item.particleMesh == "fx:leaf";
    set4(p.extra, std::clamp(em.hueVariation, 0.f, 1.f), em.sort ? 1.f : 0.f, 0.f, thin ? 1.f : 0.f);
    for (int i = 0; i < GpuCurves::kSamples; ++i) {
        Vec4 c = item.curves.color[static_cast<size_t>(i)];
        set4(p.colorTable[i], c.x, c.y, c.z, c.w);
        p.sizeTable[i] = item.curves.size[static_cast<size_t>(i)];
    }
    return p;
}

// ---------------------------------------------------------------------------------------
// ParticleSystem: GPU emitters as renderer items
// ---------------------------------------------------------------------------------------

void ParticleSystem::gatherGpu(const Scene& scene, const MeshProvider& meshes, const PathResolver& resolve,
                               std::vector<GpuEmitterItem>& out) {
    const size_t first = out.size();
    const Environment& env = scene.environment();
    float wa = radians(env.windDirection);
    Vec3 wind = Vec3{std::sin(wa), 0.f, std::cos(wa)} * env.windSpeed;
    for (EntityId e : scene.entities()) {
        const ParticleEmitter* em = scene.get<ParticleEmitter>(e);
        if (!em || em->simulation != "gpu" || !scene.isActive(e)) continue;
        GpuEmitterItem item;
        item.entity = e;
        item.world = scene.worldMatrix(e);
        item.params = *em;
        item.burstSerial = burstSerial(e);
        item.wind = wind;
        if (em->facing == "mesh") item.particleMesh = em->mesh.empty() ? "fx:leaf" : em->mesh;
        if (!em->texture.empty()) item.texture = resolve ? resolve(em->texture) : em->texture;
        if (em->shape == "mesh") {
            std::string key = em->shapeMesh;
            if (key.empty()) {
                const MeshRenderer* mr = scene.get<MeshRenderer>(e);
                key = mr ? mr->mesh : "sphere";
            }
            MeshData builtin;
            const MeshData* md = builtinParticleMesh(key, builtin) ? nullptr : (meshes ? meshes(key) : nullptr);
            CachedSurface& cs = surfaces_[key];
            if (!cs.surface || cs.source != md) {
                cs.source = md;
                if (md) cs.surface = std::make_shared<const MeshSurface>(buildMeshSurface(*md));
                else if (!builtin.indices.empty()) cs.surface = std::make_shared<const MeshSurface>(buildMeshSurface(builtin));
                else cs.surface = nullptr;
            }
            item.surface = cs.surface;
        }
        if (em->field == "texture" && !em->fieldTexture.empty()) {
            std::string path = resolve ? resolve(em->fieldTexture) : em->fieldTexture;
            std::error_code ec;
            auto t = std::filesystem::last_write_time(path, ec);
            int64_t stamp = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
            CachedField& cf = fields_[path];
            if (cf.stamp != stamp) {
                cf.stamp = stamp;
                auto f = loadFga(path);
                if (f) {
                    f->version = static_cast<uint64_t>(stamp) ^ std::hash<std::string>{}(path);
                    cf.field = std::make_shared<const VectorField>(std::move(f.value()));
                } else {
                    cf.field = nullptr;
                }
            }
            item.field = cf.field;
        }
        item.colliders = collidersFromLinks(scene, e, em->colliders);
        if (!em->subEmitter.empty()) {
            EntityId sub = scene.resolve(em->subEmitter, e);
            const ParticleEmitter* se = sub ? scene.get<ParticleEmitter>(sub) : nullptr;
            if (se && se->simulation == "gpu" && sub != e) item.subEmitter = sub;
        }
        item.curves = bakeCurves(*em);
        out.push_back(std::move(item));
    }
    for (size_t i = first; i < out.size(); ++i) {
        for (size_t j = first; j < out.size(); ++j) {
            if (out[j].subEmitter == out[i].entity) out[i].isSubEmitter = true;
        }
    }
}

}  // namespace sky::fx
