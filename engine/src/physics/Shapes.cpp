#include "Shapes.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "skywalker/core/Strings.h"
#include "skywalker/render/Hdr.h"
#include "skywalker/render/MeshData.h"

namespace sky::physics {

// ---------------------------------------------------------------------------
// Cache & hashing
// ---------------------------------------------------------------------------

JPH::RefConst<JPH::Shape> ShapeCache::find(const std::string& key) const {
    auto it = shapes_.find(key);
    return it == shapes_.end() ? nullptr : it->second;
}

void ShapeCache::put(const std::string& key, JPH::RefConst<JPH::Shape> shape) {
    if (shapes_.size() > 2048) shapes_.clear();  // bounded: a level rarely has this many unique cooked shapes
    shapes_[key] = std::move(shape);
}

const SurfaceMaterial* ShapeCache::material(float friction, float restitution) {
    Hasher h;
    h.pod(friction);
    h.pod(restitution);
    auto& slot = materials_[h.h];
    if (!slot) slot = new SurfaceMaterial(friction, restitution);
    return slot.GetPtr();
}

void ShapeCache::clear() {
    shapes_.clear();
    materials_.clear();
}

void Hasher::reflected(const void* object, const TypeInfo& type, std::initializer_list<const char*> skip) {
    const auto* base = static_cast<const char*>(object);
    for (const FieldInfo& f : type.fields) {
        bool skipped = false;
        for (const char* s : skip) skipped = skipped || f.name == s;
        if (skipped) continue;
        const char* p = base + f.offset;
        switch (f.type) {
            case FieldType::Float: bytes(p, sizeof(float)); break;
            case FieldType::Int: bytes(p, sizeof(int)); break;
            case FieldType::Bool: pod(*reinterpret_cast<const bool*>(p) ? 1 : 0); break;
            case FieldType::String:
            case FieldType::Enum: str(*reinterpret_cast<const std::string*>(p)); break;
            case FieldType::Vec3: bytes(p, sizeof(Vec3)); break;
            case FieldType::Color: bytes(p, sizeof(Vec4)); break;
            case FieldType::Json: str(reinterpret_cast<const Json*>(p)->dump()); break;
            case FieldType::Vec2: bytes(p, sizeof(Vec2)); break;
            case FieldType::Vec4: bytes(p, sizeof(Vec4)); break;
            case FieldType::Entity: {
                const auto& l = *reinterpret_cast<const EntityLink*>(p);
                pod(l.id);
                str(l.name);
                break;
            }
            case FieldType::EntityList:
                for (const auto& l : *reinterpret_cast<const std::vector<EntityLink>*>(p)) {
                    pod(l.id);
                    str(l.name);
                }
                break;
        }
    }
}

namespace {

void warn(const ShapeContext& ctx, std::string msg) {
    if (ctx.warnings && std::find(ctx.warnings->begin(), ctx.warnings->end(), msg) == ctx.warnings->end()) {
        ctx.warnings->push_back(std::move(msg));
    }
}

std::string keyOf(const char* kind, const std::string& mesh, const MeshData* data, const Mat4& m, const SurfaceMaterial* mat,
                   int extra = 0) {
    Hasher h;
    h.mat(m);
    h.pod(data);
    h.pod(data ? data->vertexCount() : 0);
    h.pod(data ? data->indices.size() : 0);
    h.pod(mat);
    h.pod(extra);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h.h));
    return std::string(kind) + "|" + mesh + "|" + buf;
}

bool nearlyIdentity(Vec3 pos, JPH::QuatArg rot) {
    return length(pos) < 1e-6f && rot.IsClose(JPH::Quat::sIdentity(), 1e-10f);
}

JPH::RefConst<JPH::Shape> placed(JPH::RefConst<JPH::Shape> shape, Vec3 pos, JPH::QuatArg rot) {
    if (!shape || nearlyIdentity(pos, rot)) return shape;
    return new JPH::RotatedTranslatedShape(toJolt(pos), rot, shape);
}

JPH::RefConst<JPH::Shape> boxShape(Vec3 half, const SurfaceMaterial* mat) {
    half = vmax(Vec3{std::fabs(half.x), std::fabs(half.y), std::fabs(half.z)}, Vec3(0.001f));
    float minHalf = std::min({half.x, half.y, half.z});
    float convexRadius = std::min(JPH::cDefaultConvexRadius, minHalf * 0.5f);
    return new JPH::BoxShape(toJolt(half), convexRadius, mat);
}

/// Unit primitive meshes resolved by the "auto" collider (mesh-local units).
struct AutoFit {
    std::string shape;  // box | sphere | capsule | cylinder | convex | mesh
    Vec3 size{1.f};
    float radius = 0.5f, height = 1.f;
    Vec3 offset{0.f};
};

AutoFit autoFit(const ShapePart& part, bool dynamic) {
    AutoFit fit;
    const std::string mesh = part.mesh ? part.mesh->mesh : "";
    if (mesh.empty()) {
        fit.shape = "box";
        if (part.hasFallbackBounds) {
            fit.size = part.fallbackBounds.max - part.fallbackBounds.min;
            fit.offset = part.fallbackBounds.center();
        }
        return fit;
    }
    if (mesh == "cube") {
        fit.shape = "box";
    } else if (mesh == "sphere") {
        fit.shape = "sphere";
    } else if (mesh == "capsule") {
        fit.shape = "capsule";
        fit.radius = 0.25f;
    } else if (mesh == "cylinder") {
        fit.shape = "cylinder";
    } else if (mesh == "plane") {
        // A thick slab under the surface so fast objects cannot tunnel through ground planes.
        fit.shape = "box";
        fit.size = {1.f, 0.2f, 1.f};
        fit.offset = {0.f, -0.1f, 0.f};
    } else if (mesh == "quad") {
        fit.shape = "box";
        fit.size = {1.f, 1.f, 0.02f};
    } else if (mesh == "cone") {
        fit.shape = "convex";
    } else {
        fit.shape = dynamic ? "convex" : "mesh";  // torus, imported models
    }
    return fit;
}

/// Vertices of a mesh transformed into the body frame.
std::vector<JPH::Vec3> transformedPoints(const MeshData& mesh, const Mat4& m) {
    std::vector<JPH::Vec3> out;
    out.reserve(mesh.vertexCount());
    for (size_t i = 0; i < mesh.vertexCount(); ++i) {
        const float* v = &mesh.vertices[i * MeshData::kFloatsPerVertex];
        out.push_back(toJolt(m.transformPoint({v[0], v[1], v[2]})));
    }
    return out;
}

JPH::RefConst<JPH::Shape> boundsBox(const MeshData& mesh, const Mat4& m, const SurfaceMaterial* mat) {
    Aabb b = mesh.bounds.transformed(m);
    return placed(boxShape(b.extents(), mat), b.center(), JPH::Quat::sIdentity());
}

JPH::RefConst<JPH::Shape> convexShape(const ShapeContext& ctx, const ShapePart& part, const std::string& meshKey,
                                      const Mat4& m, const SurfaceMaterial* mat) {
    const MeshData* data = meshKey.empty() ? nullptr : ctx.meshes(meshKey);
    if (!data || data->vertexCount() < 4) {
        warn(ctx, "collider on entity #" + std::to_string(part.entity) + ": no mesh to build a convex hull from; using a box");
        return nullptr;
    }
    std::string key = keyOf("convex", meshKey, data, m, mat);
    if (auto cached = ctx.cache->find(key)) return cached;
    std::vector<JPH::Vec3> pts = transformedPoints(*data, m);
    // Hulls are capped at 256 points by Jolt's builder quality; decimate huge meshes uniformly.
    if (pts.size() > 4096) {
        std::vector<JPH::Vec3> fewer;
        size_t stride = pts.size() / 4096 + 1;
        for (size_t i = 0; i < pts.size(); i += stride) fewer.push_back(pts[i]);
        pts.swap(fewer);
    }
    JPH::ConvexHullShapeSettings settings(pts.data(), static_cast<int>(pts.size()), JPH::cDefaultConvexRadius, mat);
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    JPH::RefConst<JPH::Shape> shape;
    if (r.IsValid()) {
        shape = r.Get();
    } else {
        warn(ctx, "convex hull failed for entity #" + std::to_string(part.entity) + " (" + r.GetError().c_str() +
                      "); using its bounding box");
        shape = boundsBox(*data, m, mat);
    }
    ctx.cache->put(key, shape);
    return shape;
}

JPH::RefConst<JPH::Shape> meshShape(const ShapeContext& ctx, const ShapePart& part, const std::string& meshKey,
                                    const Mat4& m, const SurfaceMaterial* mat) {
    const MeshData* data = meshKey.empty() ? nullptr : ctx.meshes(meshKey);
    if (!data || data->indices.size() < 3) {
        warn(ctx, "mesh collider on entity #" + std::to_string(part.entity) + ": the entity has no mesh; using a box");
        return nullptr;
    }
    std::string key = keyOf("mesh", meshKey, data, m, mat);
    if (auto cached = ctx.cache->find(key)) return cached;
    JPH::VertexList verts;
    verts.reserve(data->vertexCount());
    for (const JPH::Vec3& p : transformedPoints(*data, m)) verts.push_back(JPH::Float3(p.GetX(), p.GetY(), p.GetZ()));
    JPH::IndexedTriangleList tris;
    tris.reserve(data->indices.size() / 3);
    bool mirrored = dot(cross(Vec3{m.at(0, 0), m.at(0, 1), m.at(0, 2)}, Vec3{m.at(1, 0), m.at(1, 1), m.at(1, 2)}),
                        Vec3{m.at(2, 0), m.at(2, 1), m.at(2, 2)}) < 0;
    for (size_t i = 0; i + 2 < data->indices.size(); i += 3) {
        uint32_t a = data->indices[i], b = data->indices[i + 1], c = data->indices[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        if (mirrored) std::swap(b, c);  // keep front faces outward
        tris.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    JPH::PhysicsMaterialList mats;
    mats.push_back(mat);
    JPH::MeshShapeSettings settings(std::move(verts), std::move(tris), std::move(mats));
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    JPH::RefConst<JPH::Shape> shape;
    if (r.IsValid()) {
        shape = r.Get();
    } else {
        warn(ctx, "triangle mesh collider failed for entity #" + std::to_string(part.entity) + " (" + r.GetError().c_str() +
                      "); using its bounding box");
        shape = boundsBox(*data, m, mat);
    }
    ctx.cache->put(key, shape);
    return shape;
}

/// Heights in 0..1 from a 16-bit raw (.r16/.raw) or Radiance (.hdr) heightmap; empty on failure.
std::vector<float> loadHeightmap(const std::string& path, int& side, std::string& error) {
    std::string ext = str::lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
    if (ext == ".hdr") {
        auto img = loadHdr(path);
        if (!img) {
            error = img.error().message;
            return {};
        }
        if (img->width != img->height) {
            error = "heightmap must be square";
            return {};
        }
        side = img->width;
        std::vector<float> h(static_cast<size_t>(side) * side);
        float lo = 1e30f, hi = -1e30f;
        for (size_t i = 0; i < h.size(); ++i) {
            h[i] = img->rgb[i * 3];
            lo = std::min(lo, h[i]);
            hi = std::max(hi, h[i]);
        }
        for (float& v : h) v = hi > lo ? (v - lo) / (hi - lo) : 0.f;
        return h;
    }
    if (ext == ".r16" || ext == ".raw") {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + path;
            return {};
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t samples = bytes.size() / 2;
        side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(samples))));
        if (side < 2 || static_cast<size_t>(side) * side != samples) {
            error = "16-bit heightmap must be square (got " + std::to_string(bytes.size()) + " bytes)";
            return {};
        }
        std::vector<float> h(samples);
        for (size_t i = 0; i < samples; ++i) h[i] = static_cast<float>(bytes[i * 2] | (bytes[i * 2 + 1] << 8)) / 65535.f;
        return h;
    }
    error = "unsupported heightmap format \"" + ext + "\" (use .r16, .raw or .hdr)";
    return {};
}

/// Highest mesh surface over a regular grid (local x/z), by rasterizing triangles; samples
/// without geometry below them become holes.
std::vector<float> sampleMesh(const MeshData& mesh, int n, Aabb& outBounds) {
    outBounds = mesh.bounds;
    std::vector<float> h(static_cast<size_t>(n) * n, JPH::HeightFieldShapeConstants::cNoCollisionValue);
    float x0 = mesh.bounds.min.x, z0 = mesh.bounds.min.z;
    float dx = std::max(mesh.bounds.max.x - x0, 1e-4f) / static_cast<float>(n - 1);
    float dz = std::max(mesh.bounds.max.z - z0, 1e-4f) / static_cast<float>(n - 1);
    auto pos = [&](uint32_t i) {
        const float* v = &mesh.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
        return Vec3{v[0], v[1], v[2]};
    };
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        if (mesh.indices[t] >= mesh.vertexCount() || mesh.indices[t + 1] >= mesh.vertexCount() ||
            mesh.indices[t + 2] >= mesh.vertexCount()) {
            continue;
        }
        Vec3 a = pos(mesh.indices[t]), b = pos(mesh.indices[t + 1]), c = pos(mesh.indices[t + 2]);
        float denom = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::fabs(denom) < 1e-12f) continue;  // vertical triangle
        int ix0 = std::max(0, static_cast<int>(std::floor((std::min({a.x, b.x, c.x}) - x0) / dx)));
        int ix1 = std::min(n - 1, static_cast<int>(std::ceil((std::max({a.x, b.x, c.x}) - x0) / dx)));
        int iz0 = std::max(0, static_cast<int>(std::floor((std::min({a.z, b.z, c.z}) - z0) / dz)));
        int iz1 = std::min(n - 1, static_cast<int>(std::ceil((std::max({a.z, b.z, c.z}) - z0) / dz)));
        for (int iz = iz0; iz <= iz1; ++iz) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                float px = x0 + dx * static_cast<float>(ix), pz = z0 + dz * static_cast<float>(iz);
                float w0 = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / denom;
                float w1 = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / denom;
                float w2 = 1.f - w0 - w1;
                const float eps = -1e-4f;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                float y = w0 * a.y + w1 * b.y + w2 * c.y;
                float& slot = h[static_cast<size_t>(iz) * n + ix];
                if (slot == JPH::HeightFieldShapeConstants::cNoCollisionValue || y > slot) slot = y;
            }
        }
    }
    return h;
}

int powerOfTwoAtLeast(int v) {
    int p = 8;
    while (p < v && p < 1024) p *= 2;
    return p;
}

JPH::RefConst<JPH::Shape> heightfieldShape(const ShapeContext& ctx, const ShapePart& part, const Mat4& m,
                                           const SurfaceMaterial* mat) {
    const Collider& c = *part.collider;
    int n = powerOfTwoAtLeast(std::clamp(c.resolution, 8, 1024));
    Decomposed d = decompose(m);
    Vec3 s{std::fabs(d.scale.x), std::fabs(d.scale.y), std::fabs(d.scale.z)};
    Hasher hk;
    hk.mat(m);
    hk.str(c.heightmap);
    if (!c.heightmap.empty()) {  // edits rewrite the file: its timestamp is part of the key
        std::error_code ec;
        auto t = std::filesystem::last_write_time(ctx.paths(c.heightmap), ec);
        if (!ec) hk.pod(static_cast<long long>(t.time_since_epoch().count()));
    }
    hk.pod(n);
    hk.pod(c.size);
    hk.pod(mat);
    const MeshData* data = nullptr;
    if (c.heightmap.empty() && part.mesh) {
        data = ctx.meshes(part.mesh->mesh);
        hk.pod(data);
        hk.pod(data ? data->vertexCount() : 0);
    }
    char hex[40];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hk.h));
    std::string key = std::string("heightfield|") + hex;
    if (auto cached = ctx.cache->find(key)) return cached;

    std::vector<float> samples;
    JPH::Vec3 offset, scale;
    if (!c.heightmap.empty()) {
        int side = 0;
        std::string error;
        std::vector<float> src = loadHeightmap(ctx.paths(c.heightmap), side, error);
        if (src.empty()) {
            warn(ctx, "heightfield on entity #" + std::to_string(part.entity) + ": " + error);
            return nullptr;
        }
        // Bilinear resample to the power-of-two grid Jolt prefers.
        samples.resize(static_cast<size_t>(n) * n);
        for (int z = 0; z < n; ++z) {
            for (int x = 0; x < n; ++x) {
                float fx = static_cast<float>(x) * static_cast<float>(side - 1) / static_cast<float>(n - 1);
                float fz = static_cast<float>(z) * static_cast<float>(side - 1) / static_cast<float>(n - 1);
                int x0 = static_cast<int>(fx), z0 = static_cast<int>(fz);
                int x1 = std::min(x0 + 1, side - 1), z1 = std::min(z0 + 1, side - 1);
                float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
                auto at = [&](int xx, int zz) { return src[static_cast<size_t>(zz) * side + xx]; };
                float top = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * tx;
                float bottom = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * tx;
                samples[static_cast<size_t>(z) * n + x] = top + (bottom - top) * tz;
            }
        }
        Vec3 ext = c.size * s;
        offset = JPH::Vec3(-ext.x * 0.5f, 0.f, -ext.z * 0.5f) + toJolt(c.offset * s);
        scale = JPH::Vec3(ext.x / static_cast<float>(n - 1), ext.y, ext.z / static_cast<float>(n - 1));
    } else {
        if (!data || data->indices.size() < 3) {
            warn(ctx, "heightfield on entity #" + std::to_string(part.entity) + ": set collider.heightmap or give it a mesh");
            return nullptr;
        }
        Aabb b;
        samples = sampleMesh(*data, n, b);
        offset = JPH::Vec3(b.min.x * s.x, 0.f, b.min.z * s.z);
        scale = JPH::Vec3((b.max.x - b.min.x) * s.x / static_cast<float>(n - 1), s.y,
                          (b.max.z - b.min.z) * s.z / static_cast<float>(n - 1));
    }
    JPH::PhysicsMaterialList mats;
    mats.push_back(mat);
    std::vector<JPH::uint8> materialIndices(static_cast<size_t>(n - 1) * (n - 1), 0);
    JPH::HeightFieldShapeSettings settings(samples.data(), offset, scale, static_cast<JPH::uint32>(n), materialIndices.data(), mats);
    settings.mBlockSize = 4;
    settings.mBitsPerSample = 8;
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (!r.IsValid()) {
        warn(ctx, "heightfield failed for entity #" + std::to_string(part.entity) + ": " + r.GetError().c_str());
        return nullptr;
    }
    JPH::RefConst<JPH::Shape> shape = placed(r.Get(), d.translation, d.rotation);
    ctx.cache->put(key, shape);
    return shape;
}

JPH::RefConst<JPH::Shape> partShape(const ShapeContext& ctx, const ShapePart& part, bool dynamic) {
    const SurfaceMaterial* mat = ctx.cache->material(part.friction, part.restitution);
    Collider defaults;
    const Collider& c = part.collider ? *part.collider : defaults;
    std::string kind = c.shape;
    Vec3 size = c.size, offset = c.offset;
    float radius = c.radius, height = c.height;
    if (kind == "auto") {
        AutoFit fit = autoFit(part, dynamic);
        kind = fit.shape;
        size = fit.size;
        radius = fit.radius;
        height = fit.height;
        offset = offset + fit.offset;
    }
    const std::string meshKey = part.mesh ? part.mesh->mesh : "";
    // Shapes Jolt cannot simulate dynamically are downgraded with a note.
    if (dynamic && kind == "mesh") {
        warn(ctx, "entity #" + std::to_string(part.entity) +
                      ": triangle-mesh colliders cannot move; using a convex hull for the dynamic body");
        kind = "convex";
    }
    if (dynamic && kind == "heightfield") {
        warn(ctx, "entity #" + std::to_string(part.entity) + ": heightfields are static only; using a box for the dynamic body");
        kind = "box";
    }

    Decomposed d = decompose(part.local);
    Vec3 s{std::fabs(d.scale.x), std::fabs(d.scale.y), std::fabs(d.scale.z)};
    JPH::Quat colRot = eulerToQuat(c.rotation);
    Vec3 center = d.translation + fromJolt(d.rotation * toJolt(offset * d.scale));
    JPH::Quat rot = (d.rotation * colRot).Normalized();

    if (kind == "box") return placed(boxShape(size * s * 0.5f, mat), center, rot);
    if (kind == "sphere") {
        float r = std::max(radius * std::max({s.x, s.y, s.z}), 0.001f);
        return placed(new JPH::SphereShape(r, mat), center, rot);
    }
    if (kind == "capsule") {
        float r = std::max(radius * std::max(s.x, s.z), 0.001f);
        float half = height * s.y * 0.5f - r;
        if (half <= 1e-3f) return placed(new JPH::SphereShape(r, mat), center, rot);
        return placed(new JPH::CapsuleShape(half, r, mat), center, rot);
    }
    if (kind == "cylinder") {
        float r = std::max(radius * std::max(s.x, s.z), 0.002f);
        float half = std::max(height * s.y * 0.5f, 0.002f);
        float convexRadius = std::min({JPH::cDefaultConvexRadius, r * 0.5f, half * 0.5f});
        return placed(new JPH::CylinderShape(half, r, convexRadius, mat), center, rot);
    }
    // Mesh-based shapes bake the full part transform (including non-uniform scale) into vertices.
    Mat4 m = part.local * Mat4::translate(offset) * Mat4::rotateEulerDeg(c.rotation);
    JPH::RefConst<JPH::Shape> shape;
    if (kind == "convex") shape = convexShape(ctx, part, meshKey, m, mat);
    else if (kind == "mesh") shape = meshShape(ctx, part, meshKey, m, mat);
    else if (kind == "heightfield") shape = heightfieldShape(ctx, part, part.local, mat);
    if (!shape) {
        // Fall back to a box around whatever we know about the entity.
        Aabb b{Vec3(-0.5f), Vec3(0.5f)};
        if (part.hasFallbackBounds) b = part.fallbackBounds;
        if (const MeshData* data = meshKey.empty() ? nullptr : ctx.meshes(meshKey)) b = data->bounds;
        Aabb wb = b.transformed(part.local);
        shape = placed(boxShape(wb.extents(), mat), wb.center(), JPH::Quat::sIdentity());
    }
    return shape;
}

}  // namespace

JPH::RefConst<JPH::Shape> buildBodyShape(const ShapeContext& ctx, const std::vector<ShapePart>& parts, bool dynamic) {
    std::vector<JPH::RefConst<JPH::Shape>> shapes;
    for (const ShapePart& p : parts) {
        if (auto s = partShape(ctx, p, dynamic)) shapes.push_back(std::move(s));
    }
    if (shapes.empty()) return nullptr;
    if (shapes.size() == 1) return shapes.front();
    JPH::StaticCompoundShapeSettings compound;
    for (const auto& s : shapes) compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), s.GetPtr());
    JPH::ShapeSettings::ShapeResult r = compound.Create();
    if (!r.IsValid()) {
        warn(ctx, std::string("compound collider failed: ") + r.GetError().c_str());
        return shapes.front();
    }
    return r.Get();
}

JPH::RefConst<JPH::Shape> characterShape(float height, float radius) {
    radius = std::max(radius, 0.01f);
    float half = std::max(height * 0.5f - radius, 0.01f);
    JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(half, radius);
    return new JPH::RotatedTranslatedShape(JPH::Vec3(0, half + radius, 0), JPH::Quat::sIdentity(), capsule);
}

}  // namespace sky::physics
