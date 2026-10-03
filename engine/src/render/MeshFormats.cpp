// Mesh file importers beyond the OBJ core: OBJ materials (.mtl) and vertex colors, Stanford
// PLY, STL, and the by-extension dispatcher. glTF lives in Gltf.cpp.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

#include "skywalker/core/Strings.h"
#include "skywalker/render/Gltf.h"
#include "skywalker/render/MeshData.h"

namespace sky::mesh {

namespace fs = std::filesystem;

namespace {

Error bad(const std::string& why, const std::string& hint = {}) { return Error::make("parse_error", why, hint); }

/// Authored 8-bit colors in PLY / OBJ files are sRGB; the shader multiplies in linear space.
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string lowerExt(const std::string& path) {
    std::string l = str::lower(path);
    if (l.size() > 5 && l.compare(l.size() - 5, 5, ".gltf") == 0) return ".gltf";
    auto dot = l.rfind('.');
    return dot == std::string::npos ? "" : l.substr(dot);
}

}  // namespace

void zUpToYUp(MeshData& m) {
    for (size_t i = 0; i < m.vertices.size(); i += MeshData::kFloatsPerVertex) {
        float* v = &m.vertices[i];
        for (int o : {0, 3}) {  // position, normal: (x, y, z) -> (x, z, -y)
            float y = v[o + 1], z = v[o + 2];
            v[o + 1] = z;
            v[o + 2] = -y;
        }
    }
    m.computeBounds();
}

// ---------------------------------------------------------------------------------------
// OBJ + MTL
// ---------------------------------------------------------------------------------------

namespace {

ImportedMaterial parseMtl(const std::string& path) {
    ImportedMaterial mat;
    std::ifstream f(path);
    if (!f) return mat;
    std::string dir = fs::path(path).parent_path().string();
    auto texPath = [&](std::istringstream& ls) {
        // Options like "-bm 1.0" may precede the file name; the name is the last token.
        std::string tok, last;
        while (ls >> tok) last = tok;
        if (last.empty()) return std::string();
        fs::path p = (fs::path(dir) / last).lexically_normal();
        return fs::exists(p) ? p.string() : std::string();
    };
    std::string line;
    int materials = 0;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "newmtl") {
            if (++materials > 1) break;  // first material only (one surface per mesh)
            mat.present = true;
        } else if (tag == "Kd") {
            ls >> mat.color.x >> mat.color.y >> mat.color.z;
        } else if (tag == "d") {
            ls >> mat.color.w;
        } else if (tag == "Tr") {
            float tr = 0;
            ls >> tr;
            mat.color.w = 1.f - tr;
        } else if (tag == "Ns") {
            float ns = 0;
            ls >> ns;  // Phong exponent 0..1000 -> roughness
            mat.roughness = std::clamp(std::sqrt(2.f / (std::max(ns, 0.f) + 2.f)), 0.05f, 1.f);
        } else if (tag == "Pr") {
            ls >> mat.roughness;
        } else if (tag == "Pm") {
            ls >> mat.metallic;
        } else if (tag == "Ke") {
            ls >> mat.emissive.x >> mat.emissive.y >> mat.emissive.z;
        } else if (tag == "map_Kd") {
            mat.texture = texPath(ls);
        } else if (tag == "map_Bump" || tag == "bump" || tag == "norm" || tag == "map_bump") {
            mat.normalMap = texPath(ls);
        }
    }
    return mat;
}

}  // namespace

Result<MeshImport> loadObjWithMaterial(const std::string& path, bool normalizeMesh) {
    std::ifstream f(path);
    if (!f) return Error::make("io_error", "cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    MeshImport out;
    auto m = parseObj(text, normalizeMesh);
    if (!m) return m.error();
    out.mesh = std::move(m.value());
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("mtllib", 0) == 0) {
            std::string name = str::trim(line.substr(6));
            fs::path mtl = (fs::path(path).parent_path() / name).lexically_normal();
            out.material = parseMtl(mtl.string());
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// PLY
// ---------------------------------------------------------------------------------------

namespace {

enum class PlyType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };

PlyType plyType(const std::string& t) {
    if (t == "char" || t == "int8") return PlyType::Int8;
    if (t == "uchar" || t == "uint8") return PlyType::UInt8;
    if (t == "short" || t == "int16") return PlyType::Int16;
    if (t == "ushort" || t == "uint16") return PlyType::UInt16;
    if (t == "int" || t == "int32") return PlyType::Int32;
    if (t == "uint" || t == "uint32") return PlyType::UInt32;
    if (t == "float" || t == "float32") return PlyType::Float32;
    if (t == "double" || t == "float64") return PlyType::Float64;
    return PlyType::Invalid;
}

size_t plySize(PlyType t) {
    switch (t) {
        case PlyType::Int8: case PlyType::UInt8: return 1;
        case PlyType::Int16: case PlyType::UInt16: return 2;
        case PlyType::Int32: case PlyType::UInt32: case PlyType::Float32: return 4;
        case PlyType::Float64: return 8;
        default: return 0;
    }
}

struct PlyProperty {
    std::string name;
    PlyType type = PlyType::Invalid;
    bool list = false;
    PlyType countType = PlyType::Invalid;
};

struct PlyElement {
    std::string name;
    size_t count = 0;
    std::vector<PlyProperty> props;
};

/// Reads values from either the ascii token stream or the binary payload.
class PlyReader {
public:
    PlyReader(const std::vector<uint8_t>& bytes, size_t offset, int format) : b_(bytes), pos_(offset), format_(format) {}

    bool read(PlyType t, double& out) {
        if (format_ == 0) {
            skipSpace();
            if (pos_ >= b_.size()) return false;
            const char* start = reinterpret_cast<const char*>(b_.data() + pos_);
            size_t n = 0;
            while (pos_ + n < b_.size() && !std::isspace(b_[pos_ + n])) ++n;
            std::string tok(start, n);
            pos_ += n;
            return str::parseDouble(tok, out);
        }
        size_t sz = plySize(t);
        if (!sz || pos_ + sz > b_.size()) return false;
        uint8_t raw[8];
        std::memcpy(raw, b_.data() + pos_, sz);
        pos_ += sz;
        if (format_ == 2) std::reverse(raw, raw + sz);  // big endian -> host (little endian)
        switch (t) {
            case PlyType::Int8: { int8_t v; std::memcpy(&v, raw, 1); out = v; break; }
            case PlyType::UInt8: out = raw[0]; break;
            case PlyType::Int16: { int16_t v; std::memcpy(&v, raw, 2); out = v; break; }
            case PlyType::UInt16: { uint16_t v; std::memcpy(&v, raw, 2); out = v; break; }
            case PlyType::Int32: { int32_t v; std::memcpy(&v, raw, 4); out = v; break; }
            case PlyType::UInt32: { uint32_t v; std::memcpy(&v, raw, 4); out = v; break; }
            case PlyType::Float32: { float v; std::memcpy(&v, raw, 4); out = v; break; }
            case PlyType::Float64: { double v; std::memcpy(&v, raw, 8); out = v; break; }
            default: return false;
        }
        return true;
    }

private:
    void skipSpace() {
        while (pos_ < b_.size() && std::isspace(b_[pos_])) ++pos_;
    }
    const std::vector<uint8_t>& b_;
    size_t pos_;
    int format_;  // 0 ascii, 1 binary little endian, 2 binary big endian
};

}  // namespace

Result<MeshData> parsePly(const std::vector<uint8_t>& bytes, bool normalizeMesh) {
    // Header
    std::string head(reinterpret_cast<const char*>(bytes.data()), std::min<size_t>(bytes.size(), 64 * 1024));
    size_t endHeader = head.find("end_header");
    if (head.rfind("ply", 0) != 0 || endHeader == std::string::npos) return bad("not a PLY file (missing 'ply' / 'end_header')");
    size_t dataStart = head.find('\n', endHeader);
    if (dataStart == std::string::npos) return bad("truncated PLY header");
    ++dataStart;
    int format = -1;
    std::vector<PlyElement> elements;
    std::istringstream hs(head.substr(0, endHeader));
    std::string line;
    while (std::getline(hs, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "format") {
            std::string f;
            ls >> f;
            format = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1;
        } else if (tag == "element") {
            PlyElement e;
            ls >> e.name >> e.count;
            elements.push_back(e);
        } else if (tag == "property" && !elements.empty()) {
            PlyProperty p;
            std::string t;
            ls >> t;
            if (t == "list") {
                std::string ct, it;
                ls >> ct >> it >> p.name;
                p.list = true;
                p.countType = plyType(ct);
                p.type = plyType(it);
            } else {
                p.type = plyType(t);
                ls >> p.name;
            }
            if (p.type == PlyType::Invalid || (p.list && p.countType == PlyType::Invalid)) {
                return bad("unsupported PLY property type in: " + line);
            }
            elements.back().props.push_back(p);
        }
    }
    if (format < 0) return bad("unknown PLY format");

    MeshData m;
    std::vector<std::vector<uint32_t>> faces;
    PlyReader rd(bytes, dataStart, format);
    size_t vertexCount = 0;
    for (const PlyElement& e : elements) {
        if (e.count > 50'000'000) return bad("PLY element too large");
        bool isVertex = e.name == "vertex";
        bool isFace = e.name == "face";
        auto has = [&](const char* n) {
            return std::any_of(e.props.begin(), e.props.end(), [&](const PlyProperty& p) { return p.name == n; });
        };
        if (isVertex) {
            vertexCount = e.count;
            m.hasVertexColors = has("red") || has("diffuse_red");
        }
        for (size_t i = 0; i < e.count; ++i) {
            Vec3 p{0, 0, 0}, n{0, 0, 0};
            Vec2 uv{0, 0};
            Vec4 c{1, 1, 1, 1};
            for (const PlyProperty& prop : e.props) {
                if (prop.list) {
                    double cnt = 0;
                    if (!rd.read(prop.countType, cnt) || cnt < 0 || cnt > 1024) return bad("bad PLY list in element " + e.name);
                    std::vector<uint32_t> idx;
                    for (int k = 0; k < static_cast<int>(cnt); ++k) {
                        double v = 0;
                        if (!rd.read(prop.type, v)) return bad("truncated PLY data");
                        idx.push_back(static_cast<uint32_t>(v));
                    }
                    if (isFace && (prop.name == "vertex_indices" || prop.name == "vertex_index")) faces.push_back(std::move(idx));
                    continue;
                }
                double v = 0;
                if (!rd.read(prop.type, v)) return bad("truncated PLY data");
                if (!isVertex) continue;
                const std::string& nm = prop.name;
                float f = static_cast<float>(v);
                bool byte = prop.type == PlyType::UInt8;
                if (nm == "x") p.x = f;
                else if (nm == "y") p.y = f;
                else if (nm == "z") p.z = f;
                else if (nm == "nx") n.x = f;
                else if (nm == "ny") n.y = f;
                else if (nm == "nz") n.z = f;
                else if (nm == "s" || nm == "u" || nm == "texture_u" || nm == "texture_s") uv.x = f;
                else if (nm == "t" || nm == "v" || nm == "texture_v" || nm == "texture_t") uv.y = 1.f - f;
                else if (nm == "red" || nm == "diffuse_red") c.x = srgbToLinear(byte ? f / 255.f : f);
                else if (nm == "green" || nm == "diffuse_green") c.y = srgbToLinear(byte ? f / 255.f : f);
                else if (nm == "blue" || nm == "diffuse_blue") c.z = srgbToLinear(byte ? f / 255.f : f);
                else if (nm == "alpha") c.w = byte ? f / 255.f : f;
            }
            if (isVertex) m.addVertex(p, length(n) > 0.5f ? normalize(n) : Vec3{0, 0, 0}, uv, c);
        }
    }
    if (faces.empty()) {
        return bad("PLY has no faces (point cloud)", "point clouds aren't rendered yet; export a mesh (e.g. Poisson reconstruction)");
    }
    for (const auto& face : faces) {
        for (size_t k = 1; k + 1 < face.size(); ++k) {
            if (face[0] >= vertexCount || face[k] >= vertexCount || face[k + 1] >= vertexCount) return bad("PLY face index out of range");
            m.indices.insert(m.indices.end(), {face[0], face[k], face[k + 1]});
        }
    }
    computeMissingNormals(m);
    m.computeBounds();
    if (normalizeMesh) normalizeToUnit(m);
    return m;
}

// ---------------------------------------------------------------------------------------
// STL
// ---------------------------------------------------------------------------------------

Result<MeshData> parseStl(const std::vector<uint8_t>& bytes, bool normalizeMesh) {
    MeshData m;
    auto facet = [&](Vec3 a, Vec3 b, Vec3 c) {
        Vec3 n = cross(b - a, c - a);
        n = length(n) > 1e-12f ? normalize(n) : Vec3{0, 1, 0};
        auto base = static_cast<uint32_t>(m.vertexCount());
        m.addVertex(a, n, {0, 0});
        m.addVertex(b, n, {0, 0});
        m.addVertex(c, n, {0, 0});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    };
    bool binary = false;
    if (bytes.size() >= 84) {
        uint32_t n = 0;
        std::memcpy(&n, bytes.data() + 80, 4);
        binary = bytes.size() == 84 + static_cast<size_t>(n) * 50;
    }
    if (binary) {
        uint32_t n = 0;
        std::memcpy(&n, bytes.data() + 80, 4);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* f = bytes.data() + 84 + static_cast<size_t>(i) * 50;
            float v[12];
            std::memcpy(v, f, sizeof(v));
            facet({v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {v[9], v[10], v[11]});
        }
    } else {
        std::string text(bytes.begin(), bytes.end());
        if (str::trim(text).rfind("solid", 0) != 0) return bad("not an STL file");
        std::istringstream in(text);
        std::string tok;
        std::vector<Vec3> verts;
        while (in >> tok) {
            if (tok == "vertex") {
                Vec3 p;
                in >> p.x >> p.y >> p.z;
                verts.push_back(p);
            } else if (tok == "endfacet") {
                for (size_t k = 1; k + 1 < verts.size(); ++k) facet(verts[0], verts[k], verts[k + 1]);
                verts.clear();
            }
        }
    }
    if (m.indices.empty()) return bad("STL contains no triangles");
    m.computeBounds();
    if (normalizeMesh) normalizeToUnit(m);
    return m;
}

// ---------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------

bool isMeshFile(const std::string& path) {
    std::string e = lowerExt(path);
    return e == ".obj" || e == ".ply" || e == ".stl" || e == ".glb" || e == ".gltf";
}

Result<MeshData> loadMeshFile(const std::string& path, const LoadOptions& options) {
    std::string e = lowerExt(path);
    Result<MeshData> r = Error::make("unsupported", "unsupported mesh format: " + path, "supported: .glb .gltf .obj .ply .stl");
    if (e == ".glb" || e == ".gltf") {
        auto g = loadGltf(path, false);
        if (!g) return g.error();
        r = std::move(g.value().mesh);
    } else if (e == ".obj") {
        r = loadObj(path, false);
    } else if (e == ".ply" || e == ".stl") {
        std::vector<uint8_t> bytes;
        if (!readBytes(path, bytes)) return Error::make("io_error", "cannot read " + path);
        r = e == ".ply" ? parsePly(bytes, false) : parseStl(bytes, false);
    }
    if (!r) return r;
    if (options.zUp) zUpToYUp(r.value());
    if (options.normalize) normalizeToUnit(r.value());
    return r;
}

Result<MeshData> loadMeshFile(const std::string& path, bool normalizeMesh) {
    LoadOptions o;
    o.normalize = normalizeMesh;
    return loadMeshFile(path, o);
}

}  // namespace sky::mesh
