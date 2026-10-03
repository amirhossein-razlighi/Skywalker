#include "skywalker/render/MeshData.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

namespace sky {

void MeshData::addVertex(Vec3 p, Vec3 n, Vec2 uv, Vec4 color) {
    vertices.insert(vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y, color.x, color.y, color.z, color.w});
}

void MeshData::computeBounds() {
    bounds = {Vec3(1e30f), Vec3(-1e30f)};
    for (size_t i = 0; i < vertices.size(); i += kFloatsPerVertex) {
        Vec3 p{vertices[i], vertices[i + 1], vertices[i + 2]};
        bounds.min = vmin(bounds.min, p);
        bounds.max = vmax(bounds.max, p);
    }
    if (vertices.empty()) bounds = {};
}

namespace mesh {

namespace {

Vec3 pos(const MeshData& m, uint32_t i) {
    const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
    return {v[0], v[1], v[2]};
}
Vec3 nrm(const MeshData& m, uint32_t i) {
    const float* v = &m.vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex + 3];
    return {v[0], v[1], v[2]};
}

/// Makes every triangle counter-clockwise with respect to its vertex normals. Generating
/// geometry and then fixing winding generically is far less error-prone than getting
/// every index pattern right by hand.
void fixWinding(MeshData& m) {
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        Vec3 face = cross(pos(m, b) - pos(m, a), pos(m, c) - pos(m, a));
        Vec3 avg = nrm(m, a) + nrm(m, b) + nrm(m, c);
        if (dot(face, avg) < 0) std::swap(m.indices[t + 1], m.indices[t + 2]);
    }
}

MeshData finish(MeshData m) {
    fixWinding(m);
    m.computeBounds();
    return m;
}

/// Grid of rows x cols vertices connected into quads.
void gridIndices(MeshData& m, uint32_t base, int rows, int cols) {
    for (int r = 0; r < rows - 1; ++r) {
        for (int c = 0; c < cols - 1; ++c) {
            uint32_t a = base + static_cast<uint32_t>(r * cols + c);
            uint32_t b = a + static_cast<uint32_t>(cols);
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
}

}  // namespace

MeshData cube() {
    MeshData m;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        Vec3 u = std::fabs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 v = cross(n, u);
        auto base = static_cast<uint32_t>(m.vertexCount());
        for (int i = 0; i < 4; ++i) {
            float su = (i & 1) ? 0.5f : -0.5f;
            float sv = (i & 2) ? 0.5f : -0.5f;
            m.addVertex(n * 0.5f + u * su + v * sv, n, {su + 0.5f, sv + 0.5f});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
    return finish(std::move(m));
}

MeshData sphere(int segments, int rings) {
    MeshData m;
    for (int r = 0; r <= rings; ++r) {
        float theta = kPi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            float phi = 2.f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            m.addVertex(n * 0.5f, n, {static_cast<float>(s) / segments, static_cast<float>(r) / rings});
        }
    }
    gridIndices(m, 0, rings + 1, segments + 1);
    return finish(std::move(m));
}

MeshData plane() {
    MeshData m;
    for (int i = 0; i < 4; ++i) {
        float x = (i & 1) ? 0.5f : -0.5f;
        float z = (i & 2) ? 0.5f : -0.5f;
        m.addVertex({x, 0, z}, {0, 1, 0}, {x + 0.5f, z + 0.5f});
    }
    m.indices = {0, 1, 2, 1, 3, 2};
    return finish(std::move(m));
}

MeshData quad() {
    MeshData m;
    for (int i = 0; i < 4; ++i) {
        float x = (i & 1) ? 0.5f : -0.5f;
        float y = (i & 2) ? 0.5f : -0.5f;
        m.addVertex({x, y, 0}, {0, 0, 1}, {x + 0.5f, 0.5f - y});
    }
    m.indices = {0, 1, 2, 1, 3, 2};
    return finish(std::move(m));
}

static void addCap(MeshData& m, float y, float radius, int segments, Vec3 n) {
    auto center = static_cast<uint32_t>(m.vertexCount());
    m.addVertex({0, y, 0}, n, {0.5f, 0.5f});
    for (int s = 0; s <= segments; ++s) {
        float phi = 2.f * kPi * static_cast<float>(s) / static_cast<float>(segments);
        float c = std::cos(phi), sn = std::sin(phi);
        m.addVertex({c * radius, y, sn * radius}, n, {c * 0.5f + 0.5f, sn * 0.5f + 0.5f});
    }
    for (int s = 0; s < segments; ++s) {
        m.indices.insert(m.indices.end(), {center, center + 1 + static_cast<uint32_t>(s), center + 2 + static_cast<uint32_t>(s)});
    }
}

MeshData cylinder(int segments) {
    MeshData m;
    for (int row = 0; row < 2; ++row) {
        float y = row == 0 ? 0.5f : -0.5f;
        for (int s = 0; s <= segments; ++s) {
            float phi = 2.f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            Vec3 n{std::cos(phi), 0, std::sin(phi)};
            m.addVertex({n.x * 0.5f, y, n.z * 0.5f}, n, {static_cast<float>(s) / segments, static_cast<float>(row)});
        }
    }
    gridIndices(m, 0, 2, segments + 1);
    addCap(m, 0.5f, 0.5f, segments, {0, 1, 0});
    addCap(m, -0.5f, 0.5f, segments, {0, -1, 0});
    return finish(std::move(m));
}

MeshData cone(int segments) {
    MeshData m;
    for (int s = 0; s <= segments; ++s) {
        float phi = 2.f * kPi * static_cast<float>(s) / static_cast<float>(segments);
        Vec3 n = normalize(Vec3{std::cos(phi), 0.5f, std::sin(phi)});
        m.addVertex({0, 0.5f, 0}, n, {static_cast<float>(s) / segments, 0});
        m.addVertex({std::cos(phi) * 0.5f, -0.5f, std::sin(phi) * 0.5f}, n, {static_cast<float>(s) / segments, 1});
    }
    for (int s = 0; s < segments; ++s) {
        auto a = static_cast<uint32_t>(s * 2);
        m.indices.insert(m.indices.end(), {a, a + 1, a + 3});
    }
    addCap(m, -0.5f, 0.5f, segments, {0, -1, 0});
    return finish(std::move(m));
}

MeshData capsule(int segments, int rings) {
    MeshData m;
    const float r = 0.25f, half = 0.25f;
    int rows = 0;
    for (int hemi = 0; hemi < 2; ++hemi) {
        for (int i = 0; i <= rings; ++i) {
            float theta = (static_cast<float>(i) / static_cast<float>(rings) + static_cast<float>(hemi)) * kPi * 0.5f;
            float offset = hemi == 0 ? half : -half;
            for (int s = 0; s <= segments; ++s) {
                float phi = 2.f * kPi * static_cast<float>(s) / static_cast<float>(segments);
                Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                Vec3 p = n * r + Vec3{0, offset, 0};
                m.addVertex(p, n, {static_cast<float>(s) / segments, 0.5f - p.y});
            }
            ++rows;
        }
    }
    gridIndices(m, 0, rows, segments + 1);
    return finish(std::move(m));
}

MeshData torus(int segments, int sides, float major, float minor) {
    MeshData m;
    const float R = major, r = minor;
    for (int i = 0; i <= segments; ++i) {
        float u = 2.f * kPi * static_cast<float>(i) / static_cast<float>(segments);
        Vec3 center{std::cos(u) * R, 0, std::sin(u) * R};
        for (int j = 0; j <= sides; ++j) {
            float v = 2.f * kPi * static_cast<float>(j) / static_cast<float>(sides);
            Vec3 n{std::cos(v) * std::cos(u), std::sin(v), std::cos(v) * std::sin(u)};
            m.addVertex(center + n * r, n, {static_cast<float>(i) / segments, static_cast<float>(j) / sides});
        }
    }
    gridIndices(m, 0, segments + 1, sides + 1);
    return finish(std::move(m));
}

Result<MeshData> primitive(const std::string& name) {
    if (name == "cube") return cube();
    if (name == "sphere") return sphere();
    if (name == "plane") return plane();
    if (name == "quad") return quad();
    if (name == "cylinder") return cylinder();
    if (name == "cone") return cone();
    if (name == "capsule") return capsule();
    if (name == "torus") return torus();
    if (name == "gizmo_ring") return torus(96, 8, 0.5f, 0.012f);  // internal: rotate-gizmo ring
    return Error::make("unknown_mesh", "unknown primitive '" + name + "'");
}

Result<MeshData> parseObj(const std::string& text, bool normalizeMesh) {
    std::vector<Vec3> positions, normals;
    std::vector<Vec4> colors;  // optional "v x y z r g b" (sRGB 0..1)
    std::vector<Vec2> uvs;
    MeshData m;
    std::map<std::tuple<int, int, int>, uint32_t> cache;
    bool anyMissingNormal = false;

    auto resolve = [](int idx, size_t count) -> int {
        if (idx < 0) return static_cast<int>(count) + idx;
        return idx - 1;
    };

    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            Vec3 p;
            ls >> p.x >> p.y >> p.z;
            positions.push_back(p);
            Vec3 c;
            if (ls >> c.x >> c.y >> c.z) {
                auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
                colors.resize(positions.size() - 1, Vec4{1, 1, 1, 1});
                colors.push_back({lin(c.x), lin(c.y), lin(c.z), 1.f});
                m.hasVertexColors = true;
            }
        } else if (tag == "vn") {
            Vec3 n;
            ls >> n.x >> n.y >> n.z;
            normals.push_back(normalize(n));
        } else if (tag == "vt") {
            Vec2 t;
            ls >> t.x >> t.y;
            uvs.push_back({t.x, 1.f - t.y});
        } else if (tag == "f") {
            std::vector<uint32_t> face;
            std::string vert;
            while (ls >> vert) {
                int vi = 0, ti = 0, ni = 0;
                char* end = nullptr;
                vi = static_cast<int>(std::strtol(vert.c_str(), &end, 10));
                if (*end == '/') {
                    char* p = end + 1;
                    if (*p != '/') ti = static_cast<int>(std::strtol(p, &end, 10));
                    else end = p;
                    if (*end == '/') ni = static_cast<int>(std::strtol(end + 1, &end, 10));
                }
                int pv = resolve(vi, positions.size());
                int pt = ti ? resolve(ti, uvs.size()) : -1;
                int pn = ni ? resolve(ni, normals.size()) : -1;
                if (pv < 0 || pv >= static_cast<int>(positions.size()) || pt >= static_cast<int>(uvs.size()) ||
                    pn >= static_cast<int>(normals.size())) {
                    return Error::make("parse_error", "OBJ line " + std::to_string(lineNo) + ": index out of range");
                }
                auto key = std::make_tuple(pv, pt, pn);
                auto it = cache.find(key);
                if (it == cache.end()) {
                    auto idx = static_cast<uint32_t>(m.vertexCount());
                    m.addVertex(positions[pv], pn >= 0 ? normals[pn] : Vec3{0, 0, 0}, pt >= 0 ? uvs[pt] : Vec2{0, 0},
                                pv < static_cast<int>(colors.size()) ? colors[pv] : Vec4{1, 1, 1, 1});
                    anyMissingNormal = anyMissingNormal || pn < 0;
                    it = cache.emplace(key, idx).first;
                }
                face.push_back(it->second);
            }
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                m.indices.insert(m.indices.end(), {face[0], face[i], face[i + 1]});
            }
        }
    }
    if (m.indices.empty()) return Error::make("parse_error", "OBJ contains no faces");

    if (anyMissingNormal) computeMissingNormals(m);
    m.computeBounds();
    if (normalizeMesh) normalizeToUnit(m);
    return m;
}

void computeMissingNormals(MeshData& m) {
    std::vector<Vec3> acc(m.vertexCount(), Vec3{0, 0, 0});
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        Vec3 n = cross(pos(m, b) - pos(m, a), pos(m, c) - pos(m, a));
        acc[a] += n;
        acc[b] += n;
        acc[c] += n;
    }
    for (size_t i = 0; i < acc.size(); ++i) {
        float* v = &m.vertices[i * MeshData::kFloatsPerVertex + 3];
        if (length(Vec3{v[0], v[1], v[2]}) < 0.5f) {
            Vec3 n = normalize(acc[i]);
            v[0] = n.x;
            v[1] = n.y;
            v[2] = n.z;
        }
    }
}

void normalizeToUnit(MeshData& m) {
    m.computeBounds();
    normalizeToUnit(m, m.bounds);
}

void normalizeToUnit(MeshData& m, const Aabb& reference) {
    Vec3 c = reference.center();
    Vec3 e = reference.max - reference.min;
    float s = 1.f / std::max({e.x, e.y, e.z, 1e-6f});
    for (size_t i = 0; i < m.vertices.size(); i += MeshData::kFloatsPerVertex) {
        m.vertices[i] = (m.vertices[i] - c.x) * s;
        m.vertices[i + 1] = (m.vertices[i + 1] - c.y) * s;
        m.vertices[i + 2] = (m.vertices[i + 2] - c.z) * s;
    }
    if (m.skinned()) m.skin.transform = Mat4::scale(Vec3(s)) * Mat4::translate(-c) * m.skin.transform;
    m.computeBounds();
}

Result<MeshData> loadObj(const std::string& path, bool normalizeMesh) {
    std::ifstream f(path);
    if (!f) return Error::make("io_error", "cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parseObj(ss.str(), normalizeMesh);
}

}  // namespace mesh
}  // namespace sky
